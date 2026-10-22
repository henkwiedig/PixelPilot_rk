#ifndef VSYNC_LOCK_H
#define VSYNC_LOCK_H

#include <cstdint>
#include <cstdio>
#include <mutex>

// --vsync-lock: paced page flips, and the display phase locked to the video.
//
// The display thread commits once per refresh, lead_us() before the vblank
// (the OSD goes in the same commit, so it never takes a vblank from the
// video). Which frame: with capture timestamps (the timing SEI), each frame
// belongs to the vblank at its capture time + target latency (due_us()), so
// one that comes in early waits for its own vblank and only one that misses
// it is dropped -- the link delivers in bursts, a frame can come in before
// the one ahead of it has gone out. Without them, the newest frame.
//
// When the video rate is the refresh rate or an integer fraction of it, the
// HDMI refresh rate is trimmed by up to +-1000 ppm through the kernel's
// debugfs vsync_adjust knob, which puts the vblanks on the frames' due
// times: each frame then goes out after a constant latency, chosen so that
// all but the latest 1% are ready guard_us before their commit deadline.
// The camera and the display run off different clocks, so without the lock
// the frames drift through the refresh and, whenever they sit near the
// commit deadline, the link's jitter decides which vblank each one makes:
// repeated and dropped frames, seen as judder on fast pans.
//
// The trim is the camera's rate against the display's (feed-forward, from
// the capture timestamps) plus a PI loop on the phase of the due times (of
// the median frame, without capture timestamps). The target latency is the
// median plus the 99th percentile of the deviations from it, over the last
// ~10 s.
//
// The capture timestamps come mapped onto this clock by the receiver's
// clock sync, and that mapping steps whenever the sync moves to another
// probe: by a millisecond or so on the bench, more over the AR8030 in
// flight, where the round trip swings. Every step would move all due times
// at once (dropped or repeated frames) and kick the phase off by as much.
// The camera ticks perfectly regularly, so the lock takes the steps out
// (stable_capture()) and works on a step-free capture time.
class VsyncLock {
public:
	// period_us: the display mode's nominal refresh period. guard_us: how
	// long before the commit deadline the late frames should be ready.
	void init(int drm_fd, uint32_t crtc_index, double period_us, unsigned guard_us);
	// Back to the nominal refresh rate.
	void shutdown();
	// The same from a signal handler: async-signal-safe.
	void reset_from_signal();

	// Frame thread: a decoded frame was handed to the display at t_us;
	// capture_us is its capture time on this clock, 0 if unknown. Returns
	// the capture time with the clock mapping's steps taken out, for
	// due_us() (0 if unknown). rx_done_us, fed_us and bytes only go to the
	// dump (0 if unknown).
	uint64_t frame_ready(uint64_t t_us, uint64_t capture_us, uint64_t rx_done_us = 0,
	                     uint64_t fed_us = 0, uint32_t bytes = 0);
	// Frame thread: the display never showed a frame (a newer one replaced it
	// before its commit).
	void frame_dropped();

	// Display thread, from here on.
	// When to commit for the next vblank that can still be made after now_us,
	// that vblank's sequence number and time.
	uint64_t next_deadline(uint64_t now_us, uint32_t *target_seq, uint64_t *vblank_us) const;
	// The vblank time a frame captured at capture_us belongs to (to within
	// half a refresh), 0: no capture-time scheduling, show the newest frame.
	uint64_t due_us(uint64_t capture_us) const {
		return slot_mode_ && capture_us ? capture_us + (uint64_t)target_lat_us_ : 0;
	}
	// A vblank was seen (page flip event or vblank wait).
	void vblank(uint32_t seq, uint64_t t_us);
	// A commit aimed at target_seq went out at landed_seq. woke_us and
	// commit_us: how long after the deadline the thread woke and committed.
	void commit_landed(uint32_t target_seq, uint32_t landed_seq, uint64_t woke_us, uint64_t commit_us);
	// Waits for the next vblank when there is nothing to commit, so the
	// vblank timeline stays fresh. false: no vblank (CRTC off).
	bool wait_vblank();
	// Runs the controller and publishes the facts, every kTickUs.
	void tick(uint64_t now_us);

	double period_us() const { return period_us_; }
	unsigned lead_us() const { return lead_us_; }

	// --vsync-lock-dump: every frame, vblank and controller tick as a line
	// of text, to work the controller out from a flight. The path goes
	// through strftime, so each run gets its own file. Lines (times in us):
	//   F <ready> <capture as mapped> <capture, steps taken out> <frame
	//     complete in rx> <fed to the decoder> <access unit bytes>
	//   V <vblank seq> <vblank> <capture of the frame it showed, 0: repeat>
	//   T <now> <target latency> <ppm> <phase error> <margin> <lead> <feed-forward ppm>
	void open_dump(const char *path_template);
	// Display thread: the vblank showed the frame captured at capture_us
	// (frame_ready()'s; 0 if no new frame went out).
	void dump_vblank(uint32_t seq, uint64_t vblank_us, uint64_t capture_us);

private:
	struct Sample {
		uint64_t t_us;        // frame ready
		uint64_t capture_us;  // 0: unknown
		double phase_us;      // since the vblank before it, in [0, period)
	};
	static const int kSamples = 256;
	static const int kDeviations = 600;
	static const uint64_t kTickUs = 500000;
	static const uint64_t kWindowUs = 2000000;
	static const int kRateTicks = 20;  // drops and misses per second over ~10 s

	void set_ppm(int ppm);
	void update_margin();
	uint64_t stable_capture(uint64_t capture_us, uint64_t t_us);
	bool vblank_known() const { return last_vbl_us_ != 0; }

	int drm_fd_ = -1;
	uint32_t crtc_index_ = 0;
	int knob_fd_ = -1;
	double nominal_period_us_ = 0;
	unsigned guard_us_ = 0;

	// Vblank timeline: written by the display thread under mutex_, read
	// under it by the frame thread.
	mutable std::mutex mutex_;
	uint32_t last_seq_ = 0;
	uint64_t last_vbl_us_ = 0;
	double period_us_ = 0;
	uint32_t ref_seq_ = 0;
	uint64_t ref_us_ = 0;

	// Frame ready samples (ring), under mutex_.
	Sample samples_[kSamples];
	int sample_next_ = 0;
	int sample_count_ = 0;
	unsigned drops_ = 0;

	// Clock mapping steps taken out of the capture times, under mutex_.
	static const int kIntervals = 15;
	uint64_t last_capture_us_ = 0;
	double intervals_[kIntervals];  // the last capture intervals (ring)
	int interval_next_ = 0;
	int interval_count_ = 0;
	int64_t capture_corr_us_ = 0;   // added to the mapped capture times
	uint64_t last_step_us_ = 0;
	unsigned steps_ = 0;
	double largest_step_us_ = 0;

	// Each frame's phase against its window's median (ring), for the margin.
	double deviations_[kDeviations];
	int deviation_next_ = 0;
	int deviation_count_ = 0;

	// Commit lead, adapted to missed commits.
	unsigned lead_us_ = 2000;
	uint64_t last_miss_us_ = 0;
	uint64_t last_lead_step_us_ = 0;
	uint64_t last_lead_relax_us_ = 0;
	unsigned misses_ = 0;

	// The last kRateTicks ticks' drops and misses, for their rates.
	struct TickCounts {
		unsigned drops, misses;
		double dt_s;
	};
	TickCounts rate_ticks_[kRateTicks] = {};
	int rate_next_ = 0;

	// Controller.
	int ppm_ = 0;
	double ff_ppm_ = 0;
	bool ff_valid_ = false;
	double integ_ppm_ = 0;
	double margin_us_ = 0;
	bool slot_mode_ = false;
	double target_lat_us_ = 0;  // capture -> vblank
	bool locked_ = false;
	int lock_ticks_ = 0;
	uint64_t last_tick_us_ = 0;
	unsigned last_ratio_ = 0;
	int summary_ticks_ = 0;  // ticks since the last logged summary
	FILE *dump_ = nullptr;
};

#endif
