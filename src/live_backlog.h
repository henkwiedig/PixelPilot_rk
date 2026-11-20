#pragma once
// How far the live picture's decoder is behind the radio: the age of the
// oldest frame-ring entry still waiting for it, in microseconds (0: keeping
// up). Written by the ring reader (GstRtpReceiver), read by what has to give
// way to the live picture (the V4L2 re-encoder, which shares the codec).
#include <atomic>
#include <cstdint>

extern std::atomic<uint32_t> g_live_backlog_us;
