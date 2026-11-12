// Restream in --frame-shm mode. With RTP from rx, the restream branch sends
// rx's RTP packets on as they are; with the ring there are no RTP packets,
// so the video is packed into RTP/H.265 (RFC 7798) again here, the air's
// RTP/Opus packets (the audio ring holds them verbatim) go out unchanged,
// both to the client's UDP port 5600 -- what the restream sent before.
//
// The decoder feed only queues: the packing and the sends run on a thread of
// their own, so a slow WiFi client can't hold up the picture. A full queue
// drops (the rest of that access unit too).
#pragma once

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class RingRestream {
public:
    static constexpr uint16_t kPort = 5600;

    RingRestream() = default;
    ~RingRestream() { stop(); }
    RingRestream(const RingRestream&) = delete;
    RingRestream& operator=(const RingRestream&) = delete;

    void start();
    void stop();

    // The client's IPv4 address, "" = off (the queue is emptied).
    void set_target(const std::string& ip);
    // A target is set: worth handing data over at all.
    bool active() const { return m_active.load(std::memory_order_relaxed); }

    // Annex-B NAL units of one access unit (all of it, or the slices so far);
    // au_end: the access unit ends with them (the RTP marker). pts_us: the
    // capture time from the ring entry (32-bit microseconds).
    void push_video(std::shared_ptr<const std::vector<uint8_t>> annexb, uint32_t pts_us, bool au_end);
    // One RTP packet as the air sent it.
    void push_audio(const uint8_t* rtp, uint32_t len);

private:
    struct Item {
        std::shared_ptr<const std::vector<uint8_t>> data;
        uint32_t pts_us;
        bool video;
        bool au_end;
    };

    void loop();
    void send_video(const Item& it);
    void queue_packet(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen);
    void flush_packets();
    bool connect_target(const std::string& ip);

    std::thread m_thread;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<Item> m_q;
    size_t m_q_bytes = 0;
    bool m_run = false;
    std::string m_target;           // under m_mtx
    bool m_target_changed = false;  // under m_mtx
    bool m_skip_to_au = false;      // under m_mtx: the queue cut an access unit short
    std::atomic<bool> m_active{false};

    // Sender thread only.
    int m_fd = -1;
    std::string m_connected;
    uint32_t m_ssrc = 0;
    uint16_t m_seq = 0;
    bool m_have_pts = false;
    uint32_t m_last_pts = 0;
    uint64_t m_ext_pts = 0;  // m_last_pts with its wraps
    std::vector<std::vector<uint8_t>> m_pkts;  // the batch for sendmmsg()
    size_t m_npkts = 0;
    uint64_t m_sent_pkts = 0, m_sent_frames = 0, m_send_errors = 0;
    uint64_t m_report_us = 0;
    struct Counts {
        uint64_t frames, pkts, dropped, errors;
    };
    Counts m_reported = {};  // at the last 30 s report
    std::atomic<uint64_t> m_dropped{0};
};
