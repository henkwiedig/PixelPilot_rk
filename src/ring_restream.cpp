#include "ring_restream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "spdlog/spdlog.h"

namespace {

// RTP payload cap: 1400 + RTP/UDP/IP headers fits a 1500-byte MTU, so
// nothing is IP-fragmented on the way to the client.
constexpr uint32_t kMaxPayload = 1400;
constexpr uint32_t kRtpHeader = 12;
constexpr uint8_t kPtH265 = 96;      // as rx sends it (rx/rtp_h265.c)
constexpr uint8_t kNalFu = 49;       // RFC 7798 fragmentation unit
// ~0.8 s of a 20 Mbit/s stream: a WiFi client falls behind for that long
// before anything is dropped.
constexpr size_t kMaxQueueBytes = 2 * 1024 * 1024;
constexpr size_t kBatch = 32;        // packets per sendmmsg()
constexpr uint64_t kReportUs = 30000000;

uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// The next Annex-B NAL unit at or after *from: its bytes without the start
// code (nor the zero byte of the next one's 4-byte start code).
bool next_nal(const uint8_t* d, size_t n, size_t* from, const uint8_t** nal, size_t* nal_len) {
    for (;;) {
        size_t i = *from, start = 0;
        bool found = false;
        for (; i + 2 < n; i++) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                start = i + 3;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        size_t end = n;
        for (size_t j = start; j + 2 < n; j++) {
            if (d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1) {
                end = (j > start && d[j - 1] == 0) ? j - 1 : j;
                break;
            }
        }
        *from = end;
        if (end > start) {
            *nal = d + start;
            *nal_len = end - start;
            return true;
        }
    }
}

}  // namespace

void RingRestream::start() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_run)
        return;
    m_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (m_fd < 0) {
        spdlog::warn("[RESTREAM] socket: {}", strerror(errno));
        return;
    }
    // Blocking sends: an IDR is a burst of ~100 packets, the socket buffer
    // takes it. The timeout only keeps a dead interface from hanging the
    // thread (and stop()).
    struct timeval tv = {0, 200000};
    setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int sndbuf = 1024 * 1024;
    setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    uint32_t rnd[2] = {0, 0};
    if (getrandom(rnd, sizeof(rnd), GRND_NONBLOCK) != (ssize_t)sizeof(rnd)) {
        rnd[0] = (uint32_t)now_us();
        rnd[1] = rnd[0] * 2654435761u;
    }
    m_ssrc = rnd[0];
    m_seq = (uint16_t)rnd[1];
    m_pkts.assign(kBatch, std::vector<uint8_t>());
    for (auto& p : m_pkts)
        p.reserve(kRtpHeader + 3 + kMaxPayload);
    m_run = true;
    m_thread = std::thread(&RingRestream::loop, this);
}

void RingRestream::stop() {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_run)
            return;
        m_run = false;
        m_active.store(false, std::memory_order_relaxed);
        m_q.clear();
        m_q_bytes = 0;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    if (m_fd >= 0)
        close(m_fd);
    m_fd = -1;
    m_target.clear();
    m_connected.clear();
}

void RingRestream::set_target(const std::string& ip) {
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_run || ip == m_target)
            return;
        m_target = ip;
        m_target_changed = true;
        m_skip_to_au = false;
        m_q.clear();
        m_q_bytes = 0;
        m_active.store(!ip.empty(), std::memory_order_relaxed);
    }
    m_cv.notify_one();
}

void RingRestream::push_video(std::shared_ptr<const std::vector<uint8_t>> annexb, uint32_t pts_us, bool au_end) {
    if (!active() || !annexb || annexb->empty())
        return;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_skip_to_au) {  // the rest of an access unit the queue had no room for
            if (au_end)
                m_skip_to_au = false;
            return;
        }
        if (m_q_bytes + annexb->size() > kMaxQueueBytes) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            m_skip_to_au = !au_end;
            return;
        }
        m_q_bytes += annexb->size();
        m_q.push_back({std::move(annexb), pts_us, true, au_end});
    }
    m_cv.notify_one();
}

void RingRestream::push_audio(const uint8_t* rtp, uint32_t len) {
    if (!active() || len < kRtpHeader)
        return;
    auto pkt = std::make_shared<const std::vector<uint8_t>>(rtp, rtp + len);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_q_bytes + len > kMaxQueueBytes) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_q_bytes += len;
        m_q.push_back({std::move(pkt), 0, false, false});
    }
    m_cv.notify_one();
}

bool RingRestream::connect_target(const std::string& ip) {
    if (!m_connected.empty()) {
        spdlog::info("[RESTREAM] stopped streaming to {}: {} frames, {} packets, {} dropped, {} send errors",
                     m_connected, m_sent_frames, m_sent_pkts, m_dropped.load(std::memory_order_relaxed),
                     m_send_errors);
        m_connected.clear();
    }
    m_sent_frames = m_sent_pkts = m_send_errors = 0;
    m_dropped.store(0, std::memory_order_relaxed);
    m_reported = {};
    m_have_pts = false;

    if (ip.empty()) {
        struct sockaddr sa = {};
        sa.sa_family = AF_UNSPEC;
        connect(m_fd, &sa, sizeof(sa));
        return true;
    }
    struct sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    if (inet_pton(AF_INET, ip.c_str(), &to.sin_addr) != 1) {
        spdlog::warn("[RESTREAM] not an IPv4 address: {}", ip);
        return false;
    }
    if (connect(m_fd, (struct sockaddr*)&to, sizeof(to)) != 0) {
        spdlog::warn("[RESTREAM] connect {}: {}", ip, strerror(errno));
        return false;
    }
    m_connected = ip;
    m_report_us = now_us();
    spdlog::info("[RESTREAM] frame-shm: RTP/H.265 (pt {}, payload <= {}) + the air's audio to {}:{}", kPtH265,
                 kMaxPayload, ip, kPort);
    return true;
}

void RingRestream::loop() {
    pthread_setname_np(pthread_self(), "restream");
    for (;;) {
        Item it;
        bool changed = false;
        std::string target;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] { return !m_run || m_target_changed || !m_q.empty(); });
            if (!m_run)
                break;
            if (m_target_changed) {
                changed = true;
                m_target_changed = false;
                target = m_target;
            } else {
                it = std::move(m_q.front());
                m_q.pop_front();
                m_q_bytes -= it.data->size();
            }
        }
        if (changed) {
            connect_target(target);
            continue;
        }
        if (m_connected.empty())
            continue;
        if (it.video) {
            send_video(it);
        } else {
            queue_packet(it.data->data(), (uint32_t)it.data->size(), nullptr, 0);
            flush_packets();
        }
        const uint64_t now = now_us();
        if (now - m_report_us >= kReportUs) {
            const Counts c = {m_sent_frames, m_sent_pkts, m_dropped.load(std::memory_order_relaxed), m_send_errors};
            spdlog::info("[RESTREAM] to {}: {} frames, {} packets, {} dropped, {} send errors in {} s", m_connected,
                         c.frames - m_reported.frames, c.pkts - m_reported.pkts, c.dropped - m_reported.dropped,
                         c.errors - m_reported.errors, (now - m_report_us) / 1000000);
            m_reported = c;
            m_report_us = now;
        }
    }
    m_npkts = 0;
    if (!m_connected.empty())
        connect_target("");
}

void RingRestream::send_video(const Item& it) {
    // The 90 kHz RTP clock from the capture times, carried past their
    // 32-bit wrap (every 71 minutes).
    if (!m_have_pts) {
        m_ext_pts = it.pts_us;
        m_have_pts = true;
    } else {
        m_ext_pts += (int64_t)(int32_t)(it.pts_us - m_last_pts);
    }
    m_last_pts = it.pts_us;
    const uint32_t ts = (uint32_t)(uint64_t)(m_ext_pts * 9 / 100);

    const uint8_t* d = it.data->data();
    const size_t n = it.data->size();
    // Find the last NAL first, for the marker bit.
    size_t cur = 0, last_start = 0;
    const uint8_t* nal;
    size_t nal_len;
    while (next_nal(d, n, &cur, &nal, &nal_len))
        last_start = (size_t)(nal - d);

    cur = 0;
    while (next_nal(d, n, &cur, &nal, &nal_len)) {
        if (nal_len < 2)
            continue;
        const bool last = it.au_end && (size_t)(nal - d) == last_start;
        uint8_t hdr[kRtpHeader] = {0x80, kPtH265, 0, 0,
                                   (uint8_t)(ts >> 24), (uint8_t)(ts >> 16), (uint8_t)(ts >> 8), (uint8_t)ts,
                                   (uint8_t)(m_ssrc >> 24), (uint8_t)(m_ssrc >> 16), (uint8_t)(m_ssrc >> 8),
                                   (uint8_t)m_ssrc};
        auto put = [&](const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen, bool marker) {
            hdr[1] = (uint8_t)((marker ? 0x80 : 0) | kPtH265);
            hdr[2] = (uint8_t)(m_seq >> 8);
            hdr[3] = (uint8_t)m_seq;
            m_seq++;
            std::vector<uint8_t>& p = m_pkts[m_npkts];
            p.assign(hdr, hdr + kRtpHeader);
            p.insert(p.end(), a, a + alen);
            if (blen)
                p.insert(p.end(), b, b + blen);
            if (++m_npkts == kBatch)
                flush_packets();
        };
        if (nal_len <= kMaxPayload) {
            put(nal, (uint32_t)nal_len, nullptr, 0, last);
            continue;
        }
        // Fragmentation units: the payload header (the NAL header with type
        // 49), then the FU header (start/end bits + the NAL's type).
        const uint8_t type = (nal[0] >> 1) & 0x3f;
        uint8_t pre[3] = {(uint8_t)((nal[0] & 0x81) | (kNalFu << 1)), nal[1], 0};
        const uint8_t* body = nal + 2;
        const size_t body_len = nal_len - 2;
        const size_t frag_max = kMaxPayload - sizeof(pre);
        for (size_t off = 0; off < body_len;) {
            const size_t len = std::min(frag_max, body_len - off);
            const bool end = off + len == body_len;
            pre[2] = (uint8_t)((off == 0 ? 0x80 : 0) | (end ? 0x40 : 0) | type);
            put(pre, sizeof(pre), body + off, (uint32_t)len, last && end);
            off += len;
        }
    }
    flush_packets();
    if (it.au_end)
        m_sent_frames++;
}

void RingRestream::queue_packet(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen) {
    std::vector<uint8_t>& p = m_pkts[m_npkts];
    p.assign(a, a + alen);
    if (blen)
        p.insert(p.end(), b, b + blen);
    if (++m_npkts == kBatch)
        flush_packets();
}

void RingRestream::flush_packets() {
    struct mmsghdr msgs[kBatch];
    struct iovec iov[kBatch];
    memset(msgs, 0, sizeof(msgs));
    for (size_t i = 0; i < m_npkts; i++) {
        iov[i].iov_base = m_pkts[i].data();
        iov[i].iov_len = m_pkts[i].size();
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }
    size_t done = 0;
    bool retried = false;
    while (done < m_npkts) {
        const int r = sendmmsg(m_fd, msgs + done, (unsigned)(m_npkts - done), 0);
        if (r > 0) {
            done += (size_t)r;
            m_sent_pkts += (uint64_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR)
            continue;
        // No listener on the client yet: the ICMP error of an earlier
        // packet comes back once, on this send. Try again once.
        if (r < 0 && errno == ECONNREFUSED && !retried) {
            retried = true;
            continue;
        }
        if (m_send_errors++ == 0)
            spdlog::warn("[RESTREAM] send to {}: {}", m_connected, r < 0 ? strerror(errno) : "nothing sent");
        break;  // the rest of the batch is lost
    }
    m_npkts = 0;
}
