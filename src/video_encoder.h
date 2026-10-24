#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H

// The DVR re-encoder of the platform PixelPilot is built for: a
// FrameProcessor (frame pacing and preparation) feeding a VideoEncoder.
#ifdef PIXELPILOT_V4L2
#include "v4l2_encoder.h"
typedef V4l2Encoder VideoEncoder;
#else
#include "frame_processor.h"
typedef MppEncoder VideoEncoder;
#endif

#endif // VIDEO_ENCODER_H
