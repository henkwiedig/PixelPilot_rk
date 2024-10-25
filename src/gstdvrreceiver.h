//
// Created by https://github.com/Consti10 on 09.04.24.
// https://github.com/OpenHD/FPVue_RK3566/tree/openhd
//

#ifndef FPVUE_GSTDVRRECEIVER_H
#define FPVUE_GSTDVRRECEIVER_H

#include <gst/gst.h>
#include <thread>
#include <memory>
#include <vector>
#include <functional>

/**
 * @brief Uses gstreamer and appsink to expose the functionality of receiving and parsing
 * rtp h264 and h265.
 */
class GstDvrReceiver {
public:
    /**
     * The constructor is delayed, remember to use start_receiving()
     */
    explicit GstDvrReceiver(char* dvr_file, const VideoCodec& codec);
    virtual ~GstDvrReceiver();
    // Depending on the codec, these are h264,h265 or mjpeg "frames" / frame buffers
    // The big advantage of gstreamer is that it seems to handle all those parsing quirks the best,
    // e.g. the frames on this cb should be easily passable to whatever decode api is available.
    typedef std::function<void(std::shared_ptr<std::vector<uint8_t>> frame)> NEW_FRAME_CALLBACK;
    void start_receiving(NEW_FRAME_CALLBACK cb);
    void stop_receiving();
private:
    std::string construct_gstreamer_pipeline();
    void loop_pull_samples();
    void on_new_sample(std::shared_ptr<std::vector<uint8_t>> sample);
private:
    // The gstreamer pipeline
    GstElement * m_gst_pipeline=nullptr;
    NEW_FRAME_CALLBACK m_cb;
    VideoCodec m_video_codec;
    char* m_dvr_file;
    // appsink
    GstElement *m_app_sink_element = nullptr;
    bool m_pull_samples_run;
    std::unique_ptr<std::thread> m_pull_samples_thread=nullptr;
};


#endif //FPVUE_GSTRTPRECEIVER_H
