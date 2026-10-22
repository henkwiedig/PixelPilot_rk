#include "vsync_lock.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <spdlog/spdlog.h>
#include <xf86drm.h>

#include "osd.h"

// Phase controller. Trimming the refresh rate by p ppm moves the frames
// through the refresh by p * 1e-3 ms per second, so Kp = 200 ppm per ms of
// phase error closes the loop with a ~5 s time constant, slow against the
// 2 s sample window. The feed-forward carries the rate difference; the
// integral (~40 s) only takes up what it gets wrong (or all of it, without
// capture timestamps).
static const double kKp = 200.0;   // ppm per ms
static const double kKi = 5.0;     // ppm per ms per second
static const int kMaxPpm = 1000;   // the kernel knob's range

// The median frame is held this far ahead of the late ones: the kSpreadQuantile
// of the deviations from the median, kDefaultMarginUs until there are enough.
static constexpr double kSpreadQuantile = 0.99;
static const unsigned kDefaultMarginUs = 6000;
static const unsigned kMaxMarginUs = 12000;

// Locked once the error stays under kLockMs for kLockTicks ticks, lost when
// it exceeds kUnlockMs.
static const double kLockMs = 1.0, kUnlockMs = 3.0;
static const int kLockTicks = 4;

// The commit lead grows by a step whenever a commit misses its vblank (a
// miss repeats a frame), at most one step a second: starting the DVR's
// re-encode missed 11 vblanks within a second in flight, which is load, not
// a lead too short. It comes back a step per 10 s without a miss, to no less
// than where it started: VOP2 made every vblank at 2 ms on the VRX Pro, and
// its first misses came at 1.6 ms.
static const unsigned kMinLeadUs = 2000, kMaxLeadUs = 6000;
static const unsigned kLeadStepUs = 250, kLeadRelaxStepUs = 250;
static const uint64_t kLeadStepGapUs = 1000000, kLeadRelaxUs = 10000000;

// A capture interval off a whole number of camera frames by more than
// kStepUs is a step in the clock mapping: the camera itself is regular to a
// few us. The clock sync moves at most once per probe (500 ms), so one step
// per kStepGapUs is taken out: when the camera changes its rate, only its
// first interval passes for a step, and the median period soon follows.
static const double kStepUs = 300;
static const uint64_t kStepGapUs = 400000;

static uint64_t monotonic_us() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void VsyncLock::init(int drm_fd, uint32_t crtc_index, double period_us, unsigned guard_us) {
	drm_fd_ = drm_fd;
	crtc_index_ = crtc_index;
	nominal_period_us_ = period_us_ = period_us;
	guard_us_ = guard_us;

	// debugfs names the DRM device by its minor, the video port by the
	// CRTC's index (VOP2 registers one CRTC per video port, in order).
	char path[96] = "";
	struct stat st;
	if (fstat(drm_fd, &st) == 0) {
		snprintf(path, sizeof(path), "/sys/kernel/debug/dri/%u/video_port%u/vsync_adjust",
		         minor(st.st_rdev), crtc_index);
		knob_fd_ = open(path, O_WRONLY | O_CLOEXEC);
	}
	if (knob_fd_ < 0) {
		spdlog::warn("vsync lock: no {} ({}): paced flips only, the refresh rate stays nominal",
		             path, strerror(errno));
		return;
	}
	spdlog::info("vsync lock: refresh period {:.3f} us, trimmed through {}", period_us, path);
	// A previous run may have left a trim behind.
	set_ppm(0);
}

void VsyncLock::reset_from_signal() {
	if (knob_fd_ >= 0)
		(void)!write(knob_fd_, "0\n", 2);
}

void VsyncLock::shutdown() {
	if (knob_fd_ < 0)
		return;
	set_ppm(0);
	close(knob_fd_);
	knob_fd_ = -1;
}

void VsyncLock::set_ppm(int ppm) {
	if (knob_fd_ < 0)
		return;
	// Written even when unchanged: a modeset puts the kernel back on the
	// nominal rate behind our back.
	char buf[16];
	const int n = snprintf(buf, sizeof(buf), "%d\n", ppm);
	if (write(knob_fd_, buf, n) == n) {
		ppm_ = ppm;
		return;
	}
	static uint64_t last_warn_us = 0;
	const uint64_t now = monotonic_us();
	if (now - last_warn_us > 5000000) {
		last_warn_us = now;
		spdlog::warn("vsync lock: setting {} ppm failed: {}", ppm, strerror(errno));
	}
}

// q-quantile, partially reordering v.
static double quantile(std::vector<double> &v, double q) {
	const size_t i = (size_t)(q * (v.size() - 1));
	std::nth_element(v.begin(), v.begin() + i, v.end());
	return v[i];
}

// Under mutex_.
uint64_t VsyncLock::stable_capture(uint64_t capture_us, uint64_t t_us) {
	if (!capture_us)
		return 0;
	if (last_capture_us_) {
		const double d = (double)(int64_t)(capture_us - last_capture_us_);
		if (interval_count_ >= 5) {
			// The camera's frame period: the median interval, which lost
			// frames and the steps themselves don't move.
			std::vector<double> iv(intervals_, intervals_ + interval_count_);
			const double period = quantile(iv, 0.5);
			const double k = std::round(d / period);
			const double off = d - k * period;
			if (k >= 1 && k <= 8 && std::fabs(off) > kStepUs && t_us - last_step_us_ >= kStepGapUs) {
				capture_corr_us_ -= (int64_t)std::llround(off);
				last_step_us_ = t_us;
				steps_++;
				largest_step_us_ = std::max(largest_step_us_, std::fabs(off));
			}
		}
		if (d > 0 && d < 1e6) {
			intervals_[interval_next_] = d;
			interval_next_ = (interval_next_ + 1) % kIntervals;
			if (interval_count_ < kIntervals)
				interval_count_++;
		}
	}
	last_capture_us_ = capture_us;
	return (uint64_t)((int64_t)capture_us + capture_corr_us_);
}

void VsyncLock::open_dump(const char *path_template) {
	char path[256];
	const time_t now = time(nullptr);
	struct tm tm;
	localtime_r(&now, &tm);
	if (!strftime(path, sizeof(path), path_template, &tm))
		return;
	dump_ = fopen(path, "w");
	if (dump_)
		spdlog::info("vsync lock: dumping timings to {}", path);
	else
		spdlog::warn("vsync lock: cannot write {}: {}", path, strerror(errno));
}

void VsyncLock::dump_vblank(uint32_t seq, uint64_t vblank_us, uint64_t capture_us) {
	if (dump_)
		fprintf(dump_, "V %u %llu %llu\n", seq, (unsigned long long)vblank_us,
		        (unsigned long long)capture_us);
}

uint64_t VsyncLock::frame_ready(uint64_t t_us, uint64_t capture_us, uint64_t rx_done_us,
                                uint64_t fed_us, uint32_t bytes) {
	std::lock_guard<std::mutex> lk(mutex_);
	const uint64_t stable_us = stable_capture(capture_us, t_us);
	if (dump_)
		fprintf(dump_, "F %llu %llu %llu %llu %llu %u\n", (unsigned long long)t_us,
		        (unsigned long long)capture_us, (unsigned long long)stable_us,
		        (unsigned long long)rx_done_us, (unsigned long long)fed_us, bytes);
	if (!vblank_known())
		return stable_us;
	double phase = std::fmod((double)(int64_t)(t_us - last_vbl_us_), period_us_);
	if (phase < 0)
		phase += period_us_;
	samples_[sample_next_] = {t_us, stable_us, phase};
	sample_next_ = (sample_next_ + 1) % kSamples;
	if (sample_count_ < kSamples)
		sample_count_++;
	return stable_us;
}

void VsyncLock::frame_dropped() {
	std::lock_guard<std::mutex> lk(mutex_);
	drops_++;
}

uint64_t VsyncLock::next_deadline(uint64_t now_us, uint32_t *target_seq, uint64_t *vblank_us) const {
	if (!vblank_known()) {
		*target_seq = 0;
		*vblank_us = now_us + lead_us_;
		return now_us;
	}
	// The vblanks run at last_vbl_us_ + k * period_us_: aim at the first one
	// whose commit deadline is still ahead.
	double k = std::ceil((double)(int64_t)(now_us + lead_us_ - last_vbl_us_) / period_us_);
	if (k < 1)
		k = 1;
	*target_seq = last_seq_ + (uint32_t)k;
	*vblank_us = last_vbl_us_ + (uint64_t)std::llround(k * period_us_);
	return *vblank_us - lead_us_;
}

void VsyncLock::vblank(uint32_t seq, uint64_t t_us) {
	std::lock_guard<std::mutex> lk(mutex_);
	if (vblank_known() && seq <= last_seq_) {
		// The same vblank twice, unless the counter restarted.
		if (t_us < last_vbl_us_ + (uint64_t)(period_us_ / 2))
			return;
		ref_us_ = 0;
	}
	// The refresh period, measured over ~2 s at a time: it follows the trim.
	if (!ref_us_) {
		ref_seq_ = seq;
		ref_us_ = t_us;
	} else if (seq - ref_seq_ >= 120) {
		const double p = (double)(t_us - ref_us_) / (seq - ref_seq_);
		if (std::fabs(p / nominal_period_us_ - 1.0) < 0.01)
			period_us_ = p;
		ref_seq_ = seq;
		ref_us_ = t_us;
	}
	last_seq_ = seq;
	last_vbl_us_ = t_us;
}

void VsyncLock::commit_landed(uint32_t target_seq, uint32_t landed_seq, uint64_t woke_us, uint64_t commit_us) {
	if (!target_seq || landed_seq <= target_seq)
		return;
	misses_++;
	const uint64_t now = monotonic_us();
	last_miss_us_ = now;
	if (lead_us_ < kMaxLeadUs && now - last_lead_step_us_ >= kLeadStepGapUs) {
		lead_us_ = std::min(lead_us_ + kLeadStepUs, kMaxLeadUs);
		last_lead_step_us_ = now;
		spdlog::info("vsync lock: commit missed its vblank by {} (woke {:.2f} ms, committed {:.2f} ms "
		             "after its deadline), lead now {:.2f} ms",
		             landed_seq - target_seq, woke_us / 1000.0, commit_us / 1000.0, lead_us_ / 1000.0);
	}
}

bool VsyncLock::wait_vblank() {
	unsigned type = DRM_VBLANK_RELATIVE;
	if (crtc_index_ == 1)
		type |= DRM_VBLANK_SECONDARY;
	else if (crtc_index_ > 1)
		type |= (crtc_index_ << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
	drmVBlank vbl = {};
	vbl.request.type = (drmVBlankSeqType)type;
	vbl.request.sequence = 1;
	if (drmWaitVBlank(drm_fd_, &vbl))
		return false;
	const uint64_t t_us = (uint64_t)vbl.reply.tval_sec * 1000000 + vbl.reply.tval_usec;
	vblank(vbl.reply.sequence, t_us);
	dump_vblank(vbl.reply.sequence, t_us, 0);
	return true;
}

// The late frames' margin over the median: kSpreadQuantile of the
// deviations, smoothed.
void VsyncLock::update_margin() {
	double margin = kDefaultMarginUs;
	if (deviation_count_ >= 120) {
		std::vector<double> dev(deviations_, deviations_ + deviation_count_);
		margin = std::max(0.0, std::min((double)kMaxMarginUs, quantile(dev, kSpreadQuantile)));
	}
	margin_us_ = margin_us_ ? margin_us_ + 0.2 * (margin - margin_us_) : margin;
}

void VsyncLock::tick(uint64_t now_us) {
	if (now_us - last_tick_us_ < kTickUs)
		return;
	const double dt_s = last_tick_us_ ? (now_us - last_tick_us_) / 1e6 : 0;
	const uint64_t prev_tick_us = last_tick_us_;
	last_tick_us_ = now_us;
	if (!prev_tick_us)
		last_lead_relax_us_ = now_us;

	std::vector<Sample> win;
	unsigned drops;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		win.reserve(sample_count_);
		for (int i = 0; i < sample_count_; i++) {
			const Sample &s = samples_[(sample_next_ - sample_count_ + i + kSamples) % kSamples];
			if (now_us - s.t_us <= kWindowUs)
				win.push_back(s);
		}
		drops = drops_;
		drops_ = 0;
	}
	const unsigned misses = misses_;
	misses_ = 0;

	if (lead_us_ > kMinLeadUs && now_us - last_miss_us_ > kLeadRelaxUs &&
	    now_us - last_lead_relax_us_ > kLeadRelaxUs) {
		lead_us_ = std::max(lead_us_ - kLeadRelaxStepUs, kMinLeadUs);
		last_lead_relax_us_ = now_us;
	}

	// Lockable when the frames come every `ratio` refreshes (median interval,
	// which lost frames don't move).
	unsigned ratio = 0;
	double refreshes_per_frame = 0;
	if (win.size() >= 20) {
		std::vector<double> iv;
		iv.reserve(win.size() - 1);
		for (size_t i = 1; i < win.size(); i++)
			iv.push_back((double)(win[i].t_us - win[i - 1].t_us) / period_us_);
		refreshes_per_frame = quantile(iv, 0.5);
		const long r = std::lround(refreshes_per_frame);
		if (r >= 1 && r <= 4 && std::fabs(refreshes_per_frame - r) < 0.1)
			ratio = (unsigned)r;
	}

	// How long the median frame waits from ready to its vblank: up to the
	// next commit deadline, then the lead.
	double wait_ms = 0;
	if (!win.empty()) {
		std::vector<double> w;
		w.reserve(win.size());
		for (const Sample &x : win)
			w.push_back(x.phase_us < period_us_ - lead_us_ ? period_us_ - x.phase_us
			                                               : 2 * period_us_ - x.phase_us);
		wait_ms = quantile(w, 0.5) / 1000.0;
	}

	double err_ms = 0;
	double lat_p50_ms = 0, lat_p99_ms = 0, lat_max_ms = 0;
	if (ratio) {
		// The camera's frame period from the capture timestamps: a trimmed
		// mean of the intervals, so a lost frame or a step in the clock
		// mapping only spoils one interval.
		std::vector<double> civ;
		for (size_t i = 1; i < win.size(); i++)
			if (win[i].capture_us && win[i - 1].capture_us)
				civ.push_back((double)(int64_t)(win[i].capture_us - win[i - 1].capture_us));
		if (civ.size() >= win.size() / 2 && civ.size() >= 20) {
			std::sort(civ.begin(), civ.end());
			const size_t cut = civ.size() / 10;
			double sum = 0;
			for (size_t i = cut; i < civ.size() - cut; i++)
				sum += civ[i];
			const double frame_us = sum / (civ.size() - 2 * cut);
			if (std::fabs(frame_us / (ratio * nominal_period_us_) - 1.0) < 0.005) {
				const double ff = (ratio * nominal_period_us_ / frame_us - 1.0) * 1e6;
				ff_ppm_ = ff_valid_ ? ff_ppm_ + 0.3 * (ff - ff_ppm_) : ff;
				ff_valid_ = true;
			}
		}

		// The frames' latency, from the capture timestamps.
		std::vector<double> lat;
		for (const Sample &x : win)
			if (x.capture_us)
				lat.push_back((double)(int64_t)(x.t_us - x.capture_us));
		const bool use_latency = lat.size() >= win.size() / 2 && lat.size() >= 20;
		if (use_latency != slot_mode_)
			deviation_count_ = 0;  // a different kind of deviation

		if (use_latency) {
			// Each frame belongs to the vblank at capture + target latency:
			// the median latency, the late frames' margin over it, the guard,
			// and the commit lead.
			lat_max_ms = *std::max_element(lat.begin(), lat.end()) / 1000.0;
			lat_p99_ms = quantile(lat, 0.99) / 1000.0;
			const double median_lat = quantile(lat, 0.5);
			lat_p50_ms = median_lat / 1000.0;
			for (const Sample &x : win) {
				if (x.t_us <= prev_tick_us || !x.capture_us)
					continue;
				deviations_[deviation_next_] = (double)(int64_t)(x.t_us - x.capture_us) - median_lat;
				deviation_next_ = (deviation_next_ + 1) % kDeviations;
				if (deviation_count_ < kDeviations)
					deviation_count_++;
			}
			update_margin();
			const double target = median_lat + margin_us_ + guard_us_ + lead_us_;
			target_lat_us_ = slot_mode_ ? target_lat_us_ + 0.2 * (target - target_lat_us_) : target;
			slot_mode_ = true;
			wait_ms = (target_lat_us_ - median_lat) / 1000.0;

			// The due times against the vblanks (circular mean): the capture
			// grid is regular, so this hardly needs averaging.
			double s = 0, c = 0;
			for (const Sample &x : win) {
				if (!x.capture_us)
					continue;
				const double a = 2 * M_PI * ((double)(int64_t)(x.capture_us - last_vbl_us_) + target_lat_us_) / period_us_;
				s += std::sin(a);
				c += std::cos(a);
			}
			err_ms = std::atan2(s, c) / (2 * M_PI) * period_us_ / 1000.0;
		} else {
			slot_mode_ = false;
			// The newest frame goes out at each deadline: hold the median
			// frame's phase margin + guard before it. The phase is circular:
			// unwrap it around its circular mean.
			double s = 0, c = 0;
			for (const Sample &x : win) {
				const double a = 2 * M_PI * x.phase_us / period_us_;
				s += std::sin(a);
				c += std::cos(a);
			}
			const double mean = std::atan2(s, c) / (2 * M_PI) * period_us_;
			std::vector<double> u;
			u.reserve(win.size());
			for (const Sample &x : win) {
				double d = x.phase_us - mean;
				d -= period_us_ * std::round(d / period_us_);
				u.push_back(mean + d);
			}
			std::vector<double> sorted_u = u;
			const double median = quantile(sorted_u, 0.5);
			for (size_t i = 0; i < win.size(); i++) {
				if (win[i].t_us <= prev_tick_us)
					continue;
				deviations_[deviation_next_] = u[i] - median;
				deviation_next_ = (deviation_next_ + 1) % kDeviations;
				if (deviation_count_ < kDeviations)
					deviation_count_++;
			}
			update_margin();
			double e = median - (period_us_ - lead_us_ - guard_us_ - margin_us_);
			e -= period_us_ * std::round(e / period_us_);
			err_ms = e / 1000.0;
		}

		// Late frames need later vblanks: a slower refresh, negative ppm.
		const double p = (ff_valid_ ? ff_ppm_ : 0) - kKp * err_ms;
		const double integ = integ_ppm_ - kKi * err_ms * dt_s;
		const double out = p + integ;
		// No integral wind-up while the output is pinned at the limit.
		if (std::fabs(out) <= kMaxPpm || std::fabs(integ) < std::fabs(integ_ppm_))
			integ_ppm_ = integ;
		set_ppm((int)std::lround(std::max(-(double)kMaxPpm, std::min((double)kMaxPpm, out))));

		if (std::fabs(err_ms) < kLockMs) {
			if (++lock_ticks_ >= kLockTicks && !locked_) {
				locked_ = true;
				spdlog::info("vsync lock: locked at {} ppm ({:+.0f} camera), frames every {} refresh(es), "
				             "margin {:.1f} ms, frames wait {:.1f} ms",
				             ppm_, ff_valid_ ? ff_ppm_ : 0.0, ratio, margin_us_ / 1000, wait_ms);
			}
		} else if (std::fabs(err_ms) > kUnlockMs) {
			lock_ticks_ = 0;
			if (locked_) {
				locked_ = false;
				spdlog::info("vsync lock: lost, phase error {:.2f} ms", err_ms);
			}
		}
	} else {
		integ_ppm_ = 0;
		ff_valid_ = false;
		deviation_count_ = 0;
		margin_us_ = 0;
		slot_mode_ = false;
		set_ppm(0);
		lock_ticks_ = 0;
		if (locked_)
			spdlog::info("vsync lock: lost, no lockable video");
		locked_ = false;
	}
	if (ratio != last_ratio_ && !ratio && refreshes_per_frame > 0)
		spdlog::info("vsync lock: video at {:.2f} refreshes per frame, not lockable", refreshes_per_frame);
	last_ratio_ = ratio;

	spdlog::debug("vsync lock: ratio {} {} err {:+.2f} ms margin {:.2f} ms wait {:.2f} ms latency {:.2f} ms "
	              "ppm {} ff {:+.0f} integ {:+.0f} lead {} us period {:.3f} us drops {} misses {}",
	              ratio, slot_mode_ ? "slots" : "newest", err_ms, margin_us_ / 1000, wait_ms,
	              target_lat_us_ / 1000, ppm_, ff_valid_ ? ff_ppm_ : 0.0, integ_ppm_, lead_us_, period_us_,
	              drops, misses);

	rate_ticks_[rate_next_] = {drops, misses, dt_s};
	rate_next_ = (rate_next_ + 1) % kRateTicks;
	double rate_drops = 0, rate_misses = 0, rate_s = 0;
	for (const TickCounts &t : rate_ticks_) {
		rate_drops += t.drops;
		rate_misses += t.misses;
		rate_s += t.dt_s;
	}

	const char *state = locked_ ? "locked" : ratio ? "locking" : win.size() >= 20 ? "free" : "idle";
	// For the log (support packages): a summary every ~10 s.
	if (++summary_ticks_ >= kRateTicks) {
		summary_ticks_ = 0;
		unsigned steps;
		double largest_step_us;
		{
			std::lock_guard<std::mutex> lk(mutex_);
			steps = steps_;
			largest_step_us = largest_step_us_;
			steps_ = 0;
			largest_step_us_ = 0;
		}
		spdlog::info("vsync lock: {} {} ppm, err {:+.1f} ms, margin {:.1f} wait {:.1f} lead {:.2f} ms, "
		             "latency p50 {:.1f} p99 {:.1f} max {:.1f} target {:.1f} ms; "
		             "last 10 s: {:.0f} drops, {:.0f} misses, {} clock steps (largest {:.1f} ms)",
		             state, ppm_, err_ms, margin_us_ / 1000, wait_ms, lead_us_ / 1000.0, lat_p50_ms,
		             lat_p99_ms, lat_max_ms, target_lat_us_ / 1000, rate_drops, rate_misses, steps,
		             largest_step_us / 1000);
	}

	if (dump_) {
		fprintf(dump_, "T %llu %.0f %d %.0f %.0f %u %.0f\n", (unsigned long long)now_us, target_lat_us_, ppm_,
		        err_ms * 1000, margin_us_, lead_us_, ff_valid_ ? ff_ppm_ : 0.0);
		fflush(dump_);
	}

	void *batch = osd_batch_init(9);
	osd_add_str_fact(batch, "video.sync.state", NULL, 0, state);
	osd_add_bool_fact(batch, "video.sync.locked", NULL, 0, locked_);
	osd_add_int_fact(batch, "video.sync.ppm", NULL, 0, ppm_);
	osd_add_double_fact(batch, "video.sync.error_ms", NULL, 0, ratio ? err_ms : 0);
	osd_add_double_fact(batch, "video.sync.margin_ms", NULL, 0, margin_us_ / 1000);
	osd_add_double_fact(batch, "video.sync.wait_ms", NULL, 0, wait_ms);
	osd_add_double_fact(batch, "video.sync.lead_ms", NULL, 0, lead_us_ / 1000.0);
	osd_add_double_fact(batch, "video.sync.drops_per_s", NULL, 0, rate_s > 0 ? rate_drops / rate_s : 0);
	osd_add_double_fact(batch, "video.sync.misses_per_s", NULL, 0, rate_s > 0 ? rate_misses / rate_s : 0);
	osd_publish_batch(batch);
}
