#include "mtl_backend.hpp"
#include "ring_spsc.hpp"

#include <chrono>
#include <thread>
#include <functional>
#include <cstring>
#include <stdexcept>

/*
  Real MTL backend.

  This file is compiled only when `MTL_SDK_WITH_MTL=ON`.
  It is intentionally "thin": it maps our stable SDK types to the MTL C API.

  You must have Media-Transport-Library headers and libs available.
  Typical header name is: mtl_api.h (or st_api.h in older trees).

  The code below follows the documented patterns:
  - MTL init + start flow: mtl_init -> create sessions -> mtl_start (Design guide) citeturn17view1
  - PTP: built-in PTP enable flag MTL_FLAG_PTP_ENABLE and external ptp_get_time_fn citeturn17view0
  - External frame API: st20p_rx with ST20P_RX_FLAG_EXT_FRAME and query_ext_frame callback citeturn6view0

  Current MTL uses mtl_init/mtl_start/mtl_stop; st_init_params was removed.
*/

extern "C" {
  #include <stdint.h>
  #include <mtl_api.h>
  #include <st30_api.h>
  #include <st30_pipeline_api.h>  /* st30_frame, st30p_tx_*; pulls in st20/st_pipeline */
}

namespace mtl_sdk {

static TimestampNs steady_ns() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline void parse_ipv4(const std::string& ip, uint8_t out[4]) {
  unsigned a,b,c,d;
  if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a,&b,&c,&d) != 4) throw std::runtime_error("bad ip");
  out[0]=a; out[1]=b; out[2]=c; out[3]=d;
}

static uint64_t default_ptp_get_time(void* priv) {
  auto* fn = reinterpret_cast<PtpGetTimeFn*>(priv);
  return static_cast<uint64_t>((*fn)());
}

static enum st_fps fps_to_st(double fps) {
  if (fps < 24) return ST_FPS_P23_98;
  if (fps < 25) return ST_FPS_P24;
  if (fps < 28) return ST_FPS_P25;
  if (fps < 30) return ST_FPS_P29_97;
  if (fps < 48) return ST_FPS_P30;
  if (fps < 55) return ST_FPS_P50;
  if (fps < 60) return ST_FPS_P59_94;
  if (fps < 95) return ST_FPS_P60;
  if (fps < 110) return ST_FPS_P100;
  if (fps < 120) return ST_FPS_P119_88;
  return ST_FPS_P120;
}

class MtlVideoRx final : public IMtlVideoRxSession {
public:
  MtlVideoRx(mtl_handle mtl, const VideoFormat& vf, const St2110Endpoint& ep,
             const std::string& port_name, std::function<TimestampNs()> ptp_fn)
      : mtl_(mtl), vf_(vf), ptp_fn_(std::move(ptp_fn)), ready_(1024) {

    // Allocate external frames for YUV422PLANAR10LE: Y, U, V planes.
    // 32 buffers give headroom when encode lags; 1024 would use ~8.5GB RAM (not recommended).
    frame_cnt_ = 8;
    bufs_.resize(frame_cnt_);
    const size_t y_sz = (size_t)vf_.width * vf_.height * 2;
    const size_t uv_sz = (size_t)(vf_.width / 2) * vf_.height * 2;
    for (int i = 0; i < frame_cnt_; i++) {
      bufs_[i].y.resize(y_sz);
      bufs_[i].u.resize(uv_sz);
      bufs_[i].v.resize(uv_sz);
      free_.push_back(i);
    }

    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_rx";
    ops_.priv = this;
    ops_.port.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.port.ip_addr[MTL_PORT_P], ip, 4);
    snprintf(ops_.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name.c_str());
    ops_.port.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.port.payload_type = ep.payload_type;

    ops_.width = vf_.width;
    ops_.height = vf_.height;
    ops_.fps = fps_to_st(vf_.fps);
    ops_.interlaced = false;
    ops_.transport_fmt = ST20_FMT_YUV_422_10BIT;
    ops_.output_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
    ops_.device = ST_PLUGIN_DEVICE_AUTO;
    ops_.framebuff_cnt = frame_cnt_;

    ops_.flags |= ST20P_RX_FLAG_EXT_FRAME | ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ops_.query_ext_frame = &MtlVideoRx::query_ext_frame_trampoline;
    ops_.notify_frame_available = &MtlVideoRx::frame_ready_trampoline;

    rx_ = st20p_rx_create(mtl_, &ops_);
    if (!rx_) throw std::runtime_error("st20p_rx_create failed");
  }

  ~MtlVideoRx() override {
    if (rx_) st20p_rx_free(rx_);
  }

  bool poll(VideoFrame& out, int timeout_ms) override {
    // wait for ready index; no blocking inside MTL callback threads.
    int idx = -1;
    auto deadline = steady_ns() + (TimestampNs)timeout_ms * 1000000LL;
    while (true) {
      if (ready_.pop(idx)) break;
      if (timeout_ms <= 0) return false;
      if (steady_ns() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    struct st_frame* frame = st20p_rx_get_frame(rx_);
    if (!frame) {
      // put idx back and try later
      ready_.push(idx);
      return false;
    }

    out = {};
    out.fmt = vf_;
    out.mem_type = MemoryType::HostPtr;
    // Use PTP when available; otherwise synthetic timestamp from frame index for monotonic DTS
    if (ptp_fn_ && !ptp_fallback_) {
      int64_t ptp_ns = ptp_fn_();
      if (frame_idx_ == 0 && ptp_ns == 0) {
        ptp_fallback_ = true;  // PTP unavailable, use synthetic
      }
      out.timestamp_ns = ptp_fallback_ ? (frame_idx_ * (int64_t)(1e9 / vf_.fps)) : ptp_ns;
    } else {
      out.timestamp_ns = frame_idx_ * (int64_t)(1e9 / vf_.fps);
    }
    frame_idx_++;
    out.num_planes = 3;  // YUV422PLANAR10LE
    out.planes[0].data = (uint8_t*)frame->addr[0];
    out.planes[0].linesize = (int)frame->linesize[0];
    out.planes[1].data = (uint8_t*)frame->addr[1];
    out.planes[1].linesize = (int)frame->linesize[1];
    out.planes[2].data = (uint8_t*)frame->addr[2];
    out.planes[2].linesize = (int)frame->linesize[2];
    out.bytes_total = frame->buffer_size;
    out.opaque = frame;
    return true;
  }

  void release(VideoFrame& frame) override {
    if (!frame.opaque) return;
    auto* stf = reinterpret_cast<struct st_frame*>(frame.opaque);
    // Return buffer index to free_ so query_ext_frame can reuse it.
    // Without this, free_ is depleted after 8 frames and query_ext_frame returns -1.
    uint8_t* addr0 = frame.planes[0].data;
    for (int i = 0; i < frame_cnt_; i++) {
      if (bufs_[i].y.data() == addr0) {
        free_.push_back(i);
        break;
      }
    }
    st20p_rx_put_frame(rx_, stf);
    frame.opaque = nullptr;
  }

private:
  struct Buf { std::vector<uint8_t> y, u, v; };

  static int query_ext_frame_trampoline(void* priv, struct st_ext_frame* ext_frame,
                                        struct st20_rx_frame_meta* meta) {
    (void)meta;
    return reinterpret_cast<MtlVideoRx*>(priv)->query_ext_frame(ext_frame);
  }

  int query_ext_frame(struct st_ext_frame* ext_frame) {
    // Must not block.
    if (free_.empty()) return -1;
    int idx = free_.back();
    free_.pop_back();

    auto& b = bufs_[idx];
    const int w = vf_.width, h = vf_.height;
    ext_frame->addr[0] = b.y.data();
    ext_frame->addr[1] = b.u.data();
    ext_frame->addr[2] = b.v.data();
    ext_frame->linesize[0] = w * 2;
    ext_frame->linesize[1] = w;
    ext_frame->linesize[2] = w;
    ext_frame->size = b.y.size() + b.u.size() + b.v.size();
    ext_frame->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(idx));
    return 0;
  }

  static int frame_ready_trampoline(void* priv) {
    return reinterpret_cast<MtlVideoRx*>(priv)->frame_ready();
  }

  int frame_ready() {
    // Must not block. Signal that a frame is available; poll will call st20p_rx_get_frame.
    ready_.push(0);
    return 0;
  }

  mtl_handle mtl_{};
  VideoFormat vf_;
  std::function<TimestampNs()> ptp_fn_{};
  bool ptp_fallback_ = false;

  st20p_rx_ops ops_{};
  st20p_rx_handle rx_{};

  int frame_cnt_{0};
  int64_t frame_idx_{0};
  std::vector<Buf> bufs_;
  std::vector<int> free_;
  SpscRing<int> ready_;
};

class MtlAudioRx final : public IMtlAudioRxSession {
public:
  MtlAudioRx(mtl_handle mtl, const AudioFormat& af, const St2110Endpoint& ep,
             const std::string& port_name) : mtl_(mtl), af_(af) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_audio_rx";
    ops_.priv = this;
    ops_.num_port = 1;
    snprintf(ops_.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name.c_str());

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.ip_addr[MTL_PORT_P], ip, 4);
    ops_.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.payload_type = ep.payload_type;

    ops_.type = ST30_TYPE_FRAME_LEVEL;
    ops_.fmt = (af_.bits_per_sample == 16) ? ST30_FMT_PCM16 : ST30_FMT_PCM24;
    ops_.channel = af_.channels;
    ops_.sampling = ST30_SAMPLING_48K;
    ops_.ptime = ST30_PTIME_1MS;
    ops_.framebuff_cnt = 32;
    ops_.framebuff_size = st30_calculate_framebuff_size(
        ops_.fmt, ops_.ptime, ops_.sampling, ops_.channel,
        10 * 1000000,  // 10ms frame
        nullptr);
    if (ops_.framebuff_size <= 0) throw std::runtime_error("st30_calculate_framebuff_size failed");

    ops_.notify_frame_ready = &MtlAudioRx::frame_ready_trampoline;

    rx_ = st30_rx_create(mtl_, &ops_);
    if (!rx_) throw std::runtime_error("st30_rx_create failed");
  }

  ~MtlAudioRx() override {
    if (rx_) st30_rx_free(rx_);
  }

  bool poll(AudioFrame& out, int timeout_ms) override {
    (void)timeout_ms;
    // For brevity, this backend leaves audio pull as an exercise:
    // - in notify_frame_ready, queue frame pointer/index
    // - call st30_rx_get_frame / st30_rx_get_framebuffer and then st30_rx_put_framebuff
    return false;
  }

private:
  static int frame_ready_trampoline(void* priv, void* frame, struct st30_rx_frame_meta* meta) {
    (void)priv; (void)frame; (void)meta;
    return 0;
  }

  mtl_handle mtl_{};
  AudioFormat af_;
  st30_rx_ops ops_{};
  st30_rx_handle rx_{};
};

// ---- TX (send): ST2110-20 video, ST2110-30 audio ----
// MTL API used: st20p_tx_create, st20p_tx_get_frame, st20p_tx_put_frame, st20p_tx_free;
//               st30_tx_create, st30_tx_get_frame, st30_tx_put_frame, st30_tx_free.
// If your MTL version uses different names (e.g. mtl_* or callback-based get_frame), adapt below.
class MtlVideoTx final : public IMtlVideoTxSession {
public:
  MtlVideoTx(mtl_handle mtl, const VideoFormat& vf, const St2110Endpoint& ep,
             const std::string& port_name) : mtl_(mtl), vf_(vf) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_tx";
    ops_.priv = this;
    ops_.port.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.port.dip_addr[MTL_PORT_P], ip, 4);
    snprintf(ops_.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name.c_str());
    ops_.port.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.port.payload_type = ep.payload_type;

    ops_.width = vf_.width;
    ops_.height = vf_.height;
    ops_.fps = fps_to_st(vf_.fps);
    ops_.interlaced = false;
    switch (vf_.pix_fmt) {
      case VideoPixFmt::YUV422_10BIT:
      case VideoPixFmt::YUV420P10LE:  // App converts to 422 before put_video
        ops_.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
        ops_.transport_fmt = ST20_FMT_YUV_422_10BIT;
        break;
      case VideoPixFmt::NV12:
      case VideoPixFmt::P010:
      default:
        ops_.input_fmt = ST_FRAME_FMT_YUV422PLANAR10LE;
        ops_.transport_fmt = ST20_FMT_YUV_422_10BIT;
        break;
    }
    ops_.transport_pacing = ST21_PACING_NARROW;
    ops_.transport_packing = ST20_PACKING_BPM;
    ops_.device = ST_PLUGIN_DEVICE_AUTO;
    ops_.framebuff_cnt = 4;

    tx_ = st20p_tx_create(mtl_, &ops_);
    if (!tx_) throw std::runtime_error("st20p_tx_create failed");
  }

  ~MtlVideoTx() override {
    if (tx_) st20p_tx_free(tx_);
  }

  bool put_video(const VideoFrame& frame) override {
    struct st_frame* mf = st20p_tx_get_frame(tx_);
    if (!mf) return false;

    const int planes = frame.num_planes > 0 ? frame.num_planes : 3;
    int h[3] = { vf_.height, vf_.height, vf_.height };
    if (planes == 2) {
      h[0] = vf_.height;
      h[1] = vf_.height / 2;
    }
    for (int i = 0; i < planes && i < 3; i++) {
      if (frame.planes[i].data && mf->addr[i]) {
        int linesize = frame.planes[i].linesize > 0 ? frame.planes[i].linesize
          : (i == 0 ? vf_.width * 2 : vf_.width);
        for (int y = 0; y < h[i]; y++)
          memcpy((uint8_t*)mf->addr[i] + y * mf->linesize[i],
                 frame.planes[i].data + y * linesize,
                 (size_t)linesize);
      }
    }
    st20p_tx_put_frame(tx_, mf);
    return true;
  }

private:
  mtl_handle mtl_{};
  VideoFormat vf_;
  st20p_tx_ops ops_{};
  st20p_tx_handle tx_{};
};

// Audio TX: ST2110-30 (use st30p pipeline API for get_frame/put_frame).
class MtlAudioTx final : public IMtlAudioTxSession {
public:
  MtlAudioTx(mtl_handle mtl, const AudioFormat& af, const St2110Endpoint& ep,
             const std::string& port_name) : mtl_(mtl), af_(af) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_audio_tx";
    ops_.priv = this;
    ops_.port.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.port.dip_addr[MTL_PORT_P], ip, 4);
    snprintf(ops_.port.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name.c_str());
    ops_.port.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.port.payload_type = ep.payload_type;

    ops_.fmt = ST30_FMT_PCM24;
    ops_.channel = af_.channels;
    ops_.sampling = ST30_SAMPLING_48K;
    ops_.ptime = ST30_PTIME_1MS;
    ops_.framebuff_cnt = 32;
    int pkt_len = st30_get_packet_size(ops_.fmt, ops_.ptime, ops_.sampling, ops_.channel);
    ops_.framebuff_size = pkt_len * 64;

    tx_ = st30p_tx_create(mtl_, &ops_);
    if (!tx_) throw std::runtime_error("st30p_tx_create failed");
  }

  ~MtlAudioTx() override {
    if (tx_) st30p_tx_free(tx_);
  }

  bool put_audio(const AudioFrame& frame) override {
    struct st30_frame* mf = st30p_tx_get_frame(tx_);
    if (!mf) return false;
    size_t to_copy = std::min(frame.pcm.size(), mf->buffer_size);
    if (to_copy && frame.pcm.data() && mf->addr)
      memcpy(mf->addr, frame.pcm.data(), to_copy);
    mf->data_size = to_copy;
    st30p_tx_put_frame(tx_, mf);
    return true;
  }

private:
  mtl_handle mtl_{};
  AudioFormat af_;
  st30p_tx_ops ops_{};
  st30p_tx_handle tx_{};
};

class MtlBackend final : public IMtlBackend {
public:
  explicit MtlBackend(const MtlSdkConfig& cfg) : cfg_(cfg) {
    if (cfg_.ports.empty()) throw std::runtime_error("Need at least one port");

    // Current MTL uses mtl_init_params (st_init_params was removed in newer MTL)
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));
    p.num_ports = (uint8_t)cfg_.ports.size();
    for (int i = 0; i < p.num_ports; i++) {
      p.pmd[i] = mtl_pmd_by_port_name(cfg_.ports[i].port.c_str());
      snprintf(p.port[i], MTL_PORT_MAX_LEN, "%s", cfg_.ports[i].port.c_str());
      p.net_proto[i] = MTL_PROTO_STATIC;
      uint8_t sip[4]; parse_ipv4(cfg_.ports[i].sip, sip);
      memcpy(p.sip_addr[i], sip, 4);
    }
    p.flags = cfg_.bind_numa ? MTL_FLAG_BIND_NUMA : 0;
    if (cfg_.enable_builtin_ptp) p.flags |= MTL_FLAG_PTP_ENABLE;
    if (cfg_.ptp_mode == PtpMode::ExternalFn) {
      ptp_fn_ = cfg_.external_ptp_time_fn;
      p.ptp_get_time_fn = (uint64_t (*)(void*))default_ptp_get_time;
      p.priv = &ptp_fn_;
    }
    p.tx_queues_cnt[MTL_PORT_P] = cfg_.tx_queues;
    p.rx_queues_cnt[MTL_PORT_P] = cfg_.rx_queues;
    st_ = mtl_init(&p);
    if (!st_) throw std::runtime_error("mtl_init failed");
  }

  ~MtlBackend() override {
    stop();
    if (st_) mtl_uninit(st_);
  }

  int start() override {
    return mtl_start(st_);
  }

  int stop() override {
    return mtl_stop(st_);
  }

  TimestampNs now_ptp_ns() const override {
    if (cfg_.ptp_mode == PtpMode::ExternalFn && cfg_.external_ptp_time_fn)
      return cfg_.external_ptp_time_fn();
    return (TimestampNs)mtl_ptp_read_time(st_);
  }

  std::unique_ptr<IMtlVideoRxSession> create_video_rx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    std::function<TimestampNs()> ptp_fn;
    if (cfg_.enable_builtin_ptp) {
      ptp_fn = [this]() { return now_ptp_ns(); };
    }
    return std::make_unique<MtlVideoRx>(st_, vf, ep, cfg_.ports[0].port, std::move(ptp_fn));
  }

  std::unique_ptr<IMtlAudioRxSession> create_audio_rx(const AudioFormat& af,
                                                      const St2110Endpoint& ep) override {
    return std::make_unique<MtlAudioRx>(st_, af, ep, cfg_.ports[0].port);
  }

  std::unique_ptr<IMtlVideoTxSession> create_video_tx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    return std::make_unique<MtlVideoTx>(st_, vf, ep, cfg_.ports[0].port);
  }

  std::unique_ptr<IMtlAudioTxSession> create_audio_tx(const AudioFormat& af,
                                                        const St2110Endpoint& ep) override {
    return std::make_unique<MtlAudioTx>(st_, af, ep, cfg_.ports[0].port);
  }

private:
  MtlSdkConfig cfg_;
  mtl_handle st_{};
  PtpGetTimeFn ptp_fn_;
};

std::unique_ptr<IMtlBackend> create_backend_mtl(const MtlSdkConfig& cfg) {
  return std::make_unique<MtlBackend>(cfg);
}

} // namespace mtl_sdk
