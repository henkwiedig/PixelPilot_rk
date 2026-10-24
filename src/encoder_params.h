#ifndef ENCODER_PARAMS_H
#define ENCODER_PARAMS_H

#include "gstrtpreceiver.h"

// The DVR re-encoder's settings, the same for every platform's encoder.
enum class EncResolution { Res720p = 0, Res1080p = 1 };

struct EncoderParams {
    VideoCodec codec = VideoCodec::H264;
    int fps = 30;
    int bitrate_kbps = 8000;
    EncResolution resolution = EncResolution::Res1080p;
};

#endif // ENCODER_PARAMS_H
