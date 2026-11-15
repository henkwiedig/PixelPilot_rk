// Keeps a recording out of the page cache.
//
// A recording is written once and never read back here, but every byte of it
// passes through the page cache. On the VR04 (128 MB) the raw DVR's ~2.7 MB/s
// turns the ~30-40 MB cache over every 10-15 s, which pushes out the code pages
// of everything else; the next use of such a page is a major fault through the
// xz squashfs rootfs (one 128 KiB block read from NAND + decompressed per
// readahead step, one decompressor for the whole system): the display thread
// once lost 110-140 ms that way, a freeze of 7-9 vblanks while recording.
//
// So a background thread writes back everything but the last MiB of the file a
// muxer is writing and drops it from the cache. When the file stops growing
// (the recording ended, or no video) the rest goes too.
#include "dvr_cache.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr off_t kKeepBytes = 1 << 20;       // the muxer may still rewrite the tail
constexpr auto kPeriod = std::chrono::milliseconds(500);
constexpr int kIdlePasses = 4;              // ~2 s without growth: drop the tail too

std::mutex mu;
std::condition_variable cv;
std::string pending;                        // set by watch(), taken by the thread
bool started = false;

// Write back [from, to) and drop [0, to) from the cache. Returns false if the
// file can't be opened.
bool drop(const std::string &path, off_t from, off_t to) {
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (to > from)
		sync_file_range(fd, from, to - from,
		                SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
	posix_fadvise(fd, 0, to, POSIX_FADV_DONTNEED);
	close(fd);
	return true;
}

off_t file_size(const std::string &path) {
	struct stat sb;
	return stat(path.c_str(), &sb) == 0 ? sb.st_size : -1;
}

void loop() {
	pthread_setname_np(pthread_self(), "dvr-cache");
	// Created from a GStreamer streaming thread: never inherit an RT policy.
	struct sched_param sp = {};
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
	if (nice(10) == -1) {}

	std::string cur;
	off_t done = 0, last_size = -1;
	int idle = 0;
	for (;;) {
		std::string next;
		{
			std::unique_lock<std::mutex> lk(mu);
			cv.wait_for(lk, kPeriod, [] { return !pending.empty(); });
			next.swap(pending);
		}
		if (!next.empty() && next != cur) {
			if (!cur.empty())            // a split: the previous file is complete
				drop(cur, done, file_size(cur));
			cur = next;
			done = 0;
			last_size = -1;
			idle = 0;
		}
		if (cur.empty())
			continue;
		const off_t size = file_size(cur);
		if (size < 0) {                  // not created yet, or gone
			continue;
		}
		if (size == last_size) {
			// Not growing: finished, or just no video for a while. Drop the
			// rest too, but keep watching in case it grows again.
			if (++idle == kIdlePasses && size > done && drop(cur, done, size))
				done = size;
			continue;
		}
		idle = 0;
		last_size = size;
		const off_t to = (size - kKeepBytes) & ~(off_t)(kKeepBytes - 1);
		if (to > done && drop(cur, done, to))
			done = to;
	}
}

} // namespace

namespace dvr_cache {

void watch(const std::string &path) {
	std::lock_guard<std::mutex> lk(mu);
	pending = path;
	if (!started) {
		started = true;
		std::thread(loop).detach();
	}
	cv.notify_one();
}

} // namespace dvr_cache
