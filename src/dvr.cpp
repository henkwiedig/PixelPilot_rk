#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <filesystem>
#include <regex>
#include <unistd.h>
#include <vector>
#include <functional>

#include "spdlog/spdlog.h"

#include "dvr.h"
#include "minimp4.h"

#include "gstrtpreceiver.h"
extern "C" {
#include "osd.h"
}

namespace fs = std::filesystem;

int dvr_enabled = 0;
const int SEQUENCE_PADDING = 4; // Configurable padding for sequence numbers
static uint64_t now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void for_each_nal(const uint8_t* data, size_t size, const std::function<void(const uint8_t*, size_t)>& cb) {
	auto find_start = [&](size_t from, size_t& start_len) -> size_t {
		for (size_t i = from; i + 3 < size; i++) {
			if (data[i] == 0x00 && data[i + 1] == 0x00) {
				if (data[i + 2] == 0x01) {
					start_len = 3;
					return i;
				}
				if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
					start_len = 4;
					return i;
				}
			}
		}
		start_len = 0;
		return size;
	};

	size_t pos = 0;
	while (pos < size) {
		size_t start_len = 0;
		size_t start = find_start(pos, start_len);
		if (start == size) {
			break;
		}
		size_t nal_start = start + start_len;
		size_t next_len = 0;
		size_t next = find_start(nal_start, next_len);
		size_t nal_end = (next == size) ? size : next;
		if (nal_end > nal_start) {
			cb(data + nal_start, nal_end - nal_start);
		}
		pos = nal_end;
	}
}

int write_callback(int64_t offset, const void *buffer, size_t size, void *token){
	FILE *f = (FILE*)token;
	fseek(f, offset, SEEK_SET);
	return fwrite(buffer, 1, size, f) != size;
}

Dvr::Dvr(dvr_thread_params params) {
	filename_template = params.filename_template;
	mp4_fragmentation_mode = params.mp4_fragmentation_mode;
	dvr_filenames_with_sequence = params.dvr_filenames_with_sequence;
	video_framerate = params.video_framerate;
	video_frm_width = params.video_p.video_frm_width;
	video_frm_height = params.video_p.video_frm_height;
	codec = params.video_p.codec;
	dvr_file = NULL;
	mp4wr = (mp4_h26x_writer_t *)calloc(1, sizeof(mp4_h26x_writer_t));
}

Dvr::~Dvr() {}

void Dvr::clear_cached_params() {
	cached_vps.clear();
	cached_sps.clear();
	cached_pps.clear();
	headers_injected = false;
	wait_for_idr = true;
}

void Dvr::frame(std::shared_ptr<std::vector<uint8_t>> frame) {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_FRAME,
		.frame = frame
	};
	enqueue_dvr_command(rpc);
}

void Dvr::set_video_params(uint32_t video_frm_w,
						   uint32_t video_frm_h,
						   VideoCodec codec) {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_SET_PARAMS
	};
	rpc.params.video_frm_width = video_frm_w;
	rpc.params.video_frm_height = video_frm_h;
	rpc.params.codec = codec;
	enqueue_dvr_command(rpc);
}

void Dvr::start_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_START
	};
	enqueue_dvr_command(rpc);
}

void Dvr::stop_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_STOP
	};
	enqueue_dvr_command(rpc);
}

void Dvr::set_video_framerate(int rate) {
	video_framerate = rate;
	spdlog::info("Changeing video framerate to {}",video_framerate);
}

void Dvr::toggle_recording() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_TOGGLE
	};
	enqueue_dvr_command(rpc);
};

void Dvr::shutdown() {
	dvr_rpc rpc = {
		.command = dvr_rpc::RPC_SHUTDOWN
	};
	enqueue_dvr_command(rpc);
};

void Dvr::enqueue_dvr_command(dvr_rpc rpc) {
	{
		std::lock_guard<std::mutex> lock(mtx);
		dvrQueue.push(rpc);
	}
	cv.notify_one();
}

void *Dvr::__THREAD__(void *param) {
	pthread_setname_np(pthread_self(), "__DVR");
	((Dvr *)param)->loop();
	return nullptr;
}

void Dvr::loop() {
	while (true) {
		std::unique_lock<std::mutex> lock(mtx);
		cv.wait(lock, [this] { return !this->dvrQueue.empty(); });
		if (dvrQueue.empty()) {
			break;
		}
		if (!dvrQueue.empty()) {
			dvr_rpc rpc = dvrQueue.front();
			dvrQueue.pop();
			lock.unlock();
			switch (rpc.command) {
			case dvr_rpc::RPC_SET_PARAMS:
				{
					SPDLOG_DEBUG("got rpc SET_PARAMS");
					video_frm_width = rpc.params.video_frm_width;
					video_frm_height = rpc.params.video_frm_height;
					codec = rpc.params.codec;
					clear_cached_params();
					if (dvr_file != NULL) {
						init();
					}
					break;
				}
			case dvr_rpc::RPC_START:
				{
					SPDLOG_DEBUG("got rpc START");
					if (dvr_file != NULL) {
						break;
					}
					if (start() == 0) {
						idr_request_record_start();
						if (video_frm_width > 0 && video_frm_height > 0) {
							init();
						}
					}
					break;
				}
			case dvr_rpc::RPC_STOP:
				{
					SPDLOG_DEBUG("got rpc STOP");
					if (dvr_file == NULL) {
						break;
					}
					stop();
					break;
				}
			case dvr_rpc::RPC_TOGGLE:
				{
					SPDLOG_DEBUG("got rpc TOGGLE");
					if (dvr_file == NULL) {
						if (start() == 0) {
							idr_request_record_start();
							if (video_frm_width > 0 && video_frm_height > 0) {
								init();
							}
						}
					} else {
						stop();
					}
					break;
				}
			case dvr_rpc::RPC_FRAME:
				{
					std::shared_ptr<std::vector<uint8_t>> frame = rpc.frame;
					cache_param_sets(frame->data(), frame->size());
					if (!_ready_to_write) {
						break;
					}
					if (!headers_injected) {
						if (!inject_param_sets()) {
							break;
						}
						headers_injected = true;
					}
					if (wait_for_idr) {
						if (!has_idr(frame->data(), frame->size())) {
							break;
						}
						wait_for_idr = false;
					}
					auto res = mp4_h26x_write_nal(mp4wr, frame->data(), frame->size(), 90000/video_framerate);
					if (MP4E_STATUS_BAD_ARGUMENTS == res) {
						if (!wait_for_idr) {
							spdlog::warn("mp4_h26x_write_nal rejected NAL (size {})", frame->size());
						}
					} else if (MP4E_STATUS_OK != res) {
						spdlog::warn("mp4_h26x_write_nal failed with error {}", res);
					}
					frames_since_flush++;
					uint64_t now = now_ms();
					if ((flush_ms_interval > 0 && now - last_flush_ms >= flush_ms_interval) ||
						(flush_frame_interval > 0 && frames_since_flush >= flush_frame_interval)) {
						if (dvr_file) {
							fflush(dvr_file);
							fsync(fileno(dvr_file));
							last_flush_ms = now;
							frames_since_flush = 0;
						}
					}
					break;
				}
			case dvr_rpc::RPC_SHUTDOWN:
				goto end;
			}
		}
	}
end:
	if (dvr_file != NULL) {
		stop();
	}
	spdlog::info("DVR thread done.");
}

int Dvr::start() {
	char *fname_tpl = filename_template;
	std::string rec_dir, filename_pattern;
	fs::path pathObj(filename_template);
	rec_dir = pathObj.parent_path().string();
	filename_pattern = pathObj.filename().string();
	std::string paddedNumber = "";

	// Ensure the directory exists
	if (!fs::exists(rec_dir))
	{
		spdlog::error("Error: Directory does not exist: {}", rec_dir);
		return -1;
	}

	if (dvr_filenames_with_sequence) {
		// Get the next file number
		std::regex pattern(R"(^(\d+)_.*)"); // Matches filenames that start with digits followed by '_'
		int maxNumber = -1;
		int nextFileNumber = 0;

		for (const auto &entry : fs::directory_iterator(rec_dir)) {
			if (entry.is_regular_file())
			{
				std::string filename = entry.path().filename().string();
				std::smatch match;

				if (std::regex_match(filename, match, pattern))
				{
					int number = std::stoi(match[1].str());
					maxNumber = std::max(maxNumber, number);
				}
			}
		}
		if (maxNumber == -1) {
			nextFileNumber = 0;
		} else {
			nextFileNumber = maxNumber + 1;
		}

		// Zero-pad the number
		std::ostringstream stream;
		stream << std::setw(SEQUENCE_PADDING) << std::setfill('0') << nextFileNumber;
		paddedNumber = stream.str() + "_";
	}

	// Generate timestamped filename
	std::time_t now = std::time(nullptr);
	std::tm *localTime = std::localtime(&now);

	char formattedFilename[256];
	std::strftime(formattedFilename, sizeof(formattedFilename), filename_pattern.c_str(), localTime);

	// Construct final filename
	std::string finalFilename = rec_dir + "/" + paddedNumber + formattedFilename;

	if (!mp4wr) {
		mp4wr = (mp4_h26x_writer_t *)calloc(1, sizeof(mp4_h26x_writer_t));
		if (!mp4wr) {
			spdlog::error("unable to allocate mp4 writer");
			return -1;
		}
	}

	if ((dvr_file = fopen(finalFilename.c_str(), "w")) == NULL) {
		spdlog::error("unable to open DVR file {}", finalFilename);
		return -1;
	}
	last_flush_ms = now_ms();
	frames_since_flush = 0;
	osd_publish_bool_fact("dvr.recording", NULL, 0, true);
	mux = MP4E_open(0 /*sequential_mode*/, mp4_fragmentation_mode, dvr_file, write_callback);
	if (!mux) {
		spdlog::error("MP4E_open failed; unable to start recording");
		fflush(dvr_file);
		fsync(fileno(dvr_file));
		fclose(dvr_file);
		dvr_file = NULL;
		osd_publish_bool_fact("dvr.recording", NULL, 0, false);
		return -1;
	}
	return 0;
}

void Dvr::init() {
	spdlog::info("setting up dvr and mux to {}x{}", video_frm_width, video_frm_height);
	if (!mp4wr || !mux) {
		spdlog::error("mp4 writer/mux not initialized; cannot start recording");
		return;
	}
	memset(mp4wr, 0, sizeof(*mp4wr));
	clear_cached_params();
	if (MP4E_STATUS_OK != mp4_h26x_write_init(mp4wr, mux,
											  video_frm_width,
											  video_frm_height,
											  codec==VideoCodec::H265)) {
		spdlog::error("mp4_h26x_write_init failed");
		if (mux) {
			MP4E_close(mux);
			mux = NULL;
		}
		if (dvr_file) {
			fclose(dvr_file);
			dvr_file = NULL;
		}
		dvr_enabled = 0;
		_ready_to_write = 0;
		return;
	}
	_ready_to_write = 1;
	dvr_enabled = 1;
}

void Dvr::stop() {
	if (mux) {
		MP4E_close(mux);
		mux = NULL;
	}
	if (mp4wr) {
		mp4_h26x_write_close(mp4wr);
		free(mp4wr);
		mp4wr = NULL;
	}
	if (dvr_file) {
		fflush(dvr_file);
		fsync(fileno(dvr_file));
		fclose(dvr_file);
		dvr_file = NULL;
	}
	osd_publish_bool_fact("dvr.recording", NULL, 0, false);
	dvr_enabled = 0;
	_ready_to_write = 0;
}

void Dvr::cache_param_sets(const uint8_t* data, size_t size) {
	if (!data || size == 0) return;
	for_each_nal(data, size, [&](const uint8_t* nal, size_t nal_size) {
		if (!nal || nal_size == 0) return;
		if (codec == VideoCodec::H265) {
			uint8_t nal_type = (nal[0] >> 1) & 0x3f;
			if (nal_type == 32) {
				cached_vps.assign({0x00, 0x00, 0x00, 0x01});
				cached_vps.insert(cached_vps.end(), nal, nal + nal_size);
			} else if (nal_type == 33) {
				cached_sps.assign({0x00, 0x00, 0x00, 0x01});
				cached_sps.insert(cached_sps.end(), nal, nal + nal_size);
			} else if (nal_type == 34) {
				cached_pps.assign({0x00, 0x00, 0x00, 0x01});
				cached_pps.insert(cached_pps.end(), nal, nal + nal_size);
			}
		} else {
			uint8_t nal_type = nal[0] & 0x1f;
			if (nal_type == 7) {
				cached_sps.assign({0x00, 0x00, 0x00, 0x01});
				cached_sps.insert(cached_sps.end(), nal, nal + nal_size);
			} else if (nal_type == 8) {
				cached_pps.assign({0x00, 0x00, 0x00, 0x01});
				cached_pps.insert(cached_pps.end(), nal, nal + nal_size);
			}
		}
	});
}

bool Dvr::has_idr(const uint8_t* data, size_t size) const {
	bool found = false;
	if (!data || size == 0) return false;
	for_each_nal(data, size, [&](const uint8_t* nal, size_t nal_size) {
		if (found || !nal || nal_size == 0) return;
		if (codec == VideoCodec::H265) {
			uint8_t nal_type = (nal[0] >> 1) & 0x3f;
			if (nal_type >= 16 && nal_type <= 21) {
				found = true;
			}
		} else {
			uint8_t nal_type = nal[0] & 0x1f;
			if (nal_type == 5) {
				found = true;
			}
		}
	});
	return found;
}

bool Dvr::inject_param_sets() {
	if (!mp4wr) return false;
	const int duration = (video_framerate > 0) ? (90000 / video_framerate) : 0;
	if (codec == VideoCodec::H265) {
		if (cached_vps.empty() || cached_sps.empty() || cached_pps.empty()) {
			return false;
		}
		mp4_h26x_write_nal(mp4wr, cached_vps.data(), static_cast<int>(cached_vps.size()), duration);
		mp4_h26x_write_nal(mp4wr, cached_sps.data(), static_cast<int>(cached_sps.size()), duration);
		mp4_h26x_write_nal(mp4wr, cached_pps.data(), static_cast<int>(cached_pps.size()), duration);
		return true;
	}
	if (cached_sps.empty() || cached_pps.empty()) {
		return false;
	}
	mp4_h26x_write_nal(mp4wr, cached_sps.data(), static_cast<int>(cached_sps.size()), duration);
	mp4_h26x_write_nal(mp4wr, cached_pps.data(), static_cast<int>(cached_pps.size()), duration);
	return true;
}


// C-compatible interface
extern "C" {
	void dvr_start_recording(Dvr* dvr) {
		if (dvr) {
			dvr->start_recording();
		}
	}

	void dvr_stop_recording(Dvr* dvr) {
		if (dvr) {
			dvr->stop_recording();
		}
	}

	void dvr_set_video_framerate(Dvr* dvr, int f) {
		if (dvr) {
			dvr->set_video_framerate(f);
		}
	}
}
