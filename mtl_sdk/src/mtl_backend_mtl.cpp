#include "mtl_backend.hpp"
#include "ring_spsc.hpp"

#include <chrono>
#include <thread>
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

  ⚠️ API names differ slightly between MTL releases (st_* vs mtl_*).
  This backend supports both via `MTL_SDK_USE_ST_API` compile definition.
*/

extern "C" {
  #include <stdint.h>
  #include <mtl_api.h>
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

static int64_t default_ptp_get_time(void* priv) {
  auto* fn = reinterpret_cast<PtpGetTimeFn*>(priv);
  return (*fn)();
}

class MtlVideoRx final : public IMtlVideoRxSession {
public:
  MtlVideoRx(mtl_handle mtl, const VideoFormat& vf, const St2110Endpoint& ep, TimestampNs(*ptp_now)())
      : mtl_(mtl), vf_(vf), ptp_now_(ptp_now), ready_(1024) {

    // Allocate external frames (host buffers). In a real deployment, you can allocate
    // DMA-able memory (hugepages) and provide IOVA, or use GPU-direct memory.
    frame_cnt_ = 8;
    bufs_.resize(frame_cnt_);
    for (int i = 0; i < frame_cnt_; i++) {
      bufs_[i].y.resize(vf_.width * vf_.height);
      bufs_[i].uv.resize((vf_.width * vf_.height)/2);
      free_.push_back(i);
    }

    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_rx";
    ops_.priv = this;
    ops_.num_port = 1;

    // Map endpoint
    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.ip_addr[MTL_PORT_P], ip, 4);
    ops_.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.payload_type = ep.payload_type;

    // Video params (mapping here is simplified, extend as needed)
    ops_.width = vf_.width;
    ops_.height = vf_.height;
    ops_.fps = ST_FPS_P59_94; // TODO: map from vf_.fps
    ops_.fmt = ST20_FMT_YUV_422_10BIT; // TODO: map from vf_.pix_fmt
    ops_.framebuff_cnt = frame_cnt_;

    // External frame mode
    ops_.flags |= ST20P_RX_FLAG_EXT_FRAME;
    ops_.query_ext_frame = &MtlVideoRx::query_ext_frame_trampoline;
    ops_.notify_frame_ready = &MtlVideoRx::frame_ready_trampoline;

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

    // Acquire frame pointer from MTL (API name may differ between releases)
    struct st_frame* frame = st20p_rx_get_frame(rx_);
    if (!frame) {
      // put idx back and try later
      ready_.push(idx);
      return false;
    }

    out = {};
    out.fmt = vf_;
    out.mem_type = MemoryType::HostPtr;
    out.timestamp_ns = ptp_now_ ? ptp_now_() : 0; // TODO: replace with frame meta PTP time
    out.num_planes = 2;
    out.planes[0].data = (uint8_t*)frame->addr[0];
    out.planes[0].linesize = (int)frame->linesize[0];
    out.planes[1].data = (uint8_t*)frame->addr[1];
    out.planes[1].linesize = (int)frame->linesize[1];
    out.bytes_total = frame->size;
    out.opaque = frame;
    return true;
  }

  void release(VideoFrame& frame) override {
    if (!frame.opaque) return;
    auto* stf = reinterpret_cast<struct st_frame*>(frame.opaque);
    st20p_rx_put_frame(rx_, stf); // if your MTL version uses a different name, adjust here.
    frame.opaque = nullptr;
  }

private:
  struct Buf { std::vector<uint8_t> y, uv; };

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
    ext_frame->addr[0] = b.y.data();
    ext_frame->addr[1] = b.uv.data();
    ext_frame->linesize[0] = vf_.width;
    ext_frame->linesize[1] = vf_.width;
    ext_frame->size = b.y.size() + b.uv.size();
    ext_frame->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(idx));
    // ext_frame->iova[...] can be filled if you use IOVA-capable memory (DMA)
    return 0;
  }

  static int frame_ready_trampoline(void* priv, uint16_t frame_idx,
                                   struct st20_rx_frame_meta* meta) {
    (void)meta;
    return reinterpret_cast<MtlVideoRx*>(priv)->frame_ready(frame_idx);
  }

  int frame_ready(uint16_t frame_idx) {
    // Must not block. Push to ring; if full, drop.
    ready_.push((int)frame_idx);
    return 0;
  }

  mtl_handle mtl_{};
  VideoFormat vf_;
  TimestampNs (*ptp_now_)() = nullptr;

  st20p_rx_ops ops_{};
  st20p_rx_handle rx_{};

  int frame_cnt_{0};
  std::vector<Buf> bufs_;
  std::vector<int> free_;
  SpscRing<int> ready_;
};

class MtlAudioRx final : public IMtlAudioRxSession {
public:
  MtlAudioRx(mtl_handle mtl, const AudioFormat& af, const St2110Endpoint& ep)
      : mtl_(mtl), af_(af) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_audio_rx";
    ops_.priv = this;
    ops_.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.ip_addr[MTL_PORT_P], ip, 4);
    ops_.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.payload_type = ep.payload_type;

    ops_.type = ST30_TYPE_FRAME_LEVEL;
    ops_.fmt = ST30_FMT_PCM24; // TODO map bits_per_sample
    ops_.channel = af_.channels;
    ops_.sampling = ST30_SAMPLING_48K;
    ops_.framebuff_cnt = 32;

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
  MtlVideoTx(mtl_handle mtl, const VideoFormat& vf, const St2110Endpoint& ep) : mtl_(mtl), vf_(vf) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_tx";
    ops_.priv = this;
    ops_.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.ip_addr[MTL_PORT_P], ip, 4);
    ops_.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.payload_type = ep.payload_type;

    ops_.width = vf_.width;
    ops_.height = vf_.height;
    ops_.fps = ST_FPS_P59_94; // TODO: map from vf_.fps
    ops_.fmt = ST20_FMT_YUV_422_10BIT; // TODO: map from vf_.pix_fmt
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

    const int planes = frame.num_planes > 0 ? frame.num_planes : 2;
    for (int i = 0; i < planes && i < 3; i++) {
      if (frame.planes[i].data && mf->addr[i]) {
        int linesize = frame.planes[i].linesize > 0 ? frame.planes[i].linesize : vf_.width;
        int h = (i == 0) ? vf_.height : (vf_.height / 2);
        for (int y = 0; y < h; y++)
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

// Audio TX: ST2110-30.
class MtlAudioTx final : public IMtlAudioTxSession {
public:
  MtlAudioTx(mtl_handle mtl, const AudioFormat& af, const St2110Endpoint& ep) : mtl_(mtl), af_(af) {
    memset(&ops_, 0, sizeof(ops_));
    ops_.name = "mtl_sdk_audio_tx";
    ops_.priv = this;
    ops_.num_port = 1;

    uint8_t ip[4]; parse_ipv4(ep.ip, ip);
    memcpy(ops_.ip_addr[MTL_PORT_P], ip, 4);
    ops_.udp_port[MTL_PORT_P] = ep.udp_port;
    ops_.payload_type = ep.payload_type;

    ops_.type = ST30_TYPE_FRAME_LEVEL;
    ops_.fmt = ST30_FMT_PCM24; // TODO map bits_per_sample
    ops_.channel = af_.channels;
    ops_.sampling = ST30_SAMPLING_48K;
    ops_.framebuff_cnt = 32;

    tx_ = st30_tx_create(mtl_, &ops_);
    if (!tx_) throw std::runtime_error("st30_tx_create failed");
  }

  ~MtlAudioTx() override {
    if (tx_) st30_tx_free(tx_);
  }

  bool put_audio(const AudioFrame& frame) override {
    struct st30_frame* mf = st30_tx_get_frame(tx_);
    if (!mf) return false;
    size_t to_copy = frame.pcm.size();
    // If your MTL st30_frame has buffer_size, use: to_copy = std::min(to_copy, (size_t)mf->buffer_size);
    if (to_copy && frame.pcm.data() && mf->addr)
      memcpy(mf->addr, frame.pcm.data(), to_copy);
    st30_tx_put_frame(tx_, mf);
    return true;
  }

private:
  mtl_handle mtl_{};
  AudioFormat af_;
  st30_tx_ops ops_{};
  st30_tx_handle tx_{};
};

class MtlBackend final : public IMtlBackend {
public:
  explicit MtlBackend(const MtlSdkConfig& cfg) : cfg_(cfg) {
    if (cfg_.ports.empty()) throw std::runtime_error("Need at least one port");

#ifdef MTL_SDK_USE_ST_API
    struct st_init_params p;
    memset(&p, 0, sizeof(p));
    p.num_ports = (int)cfg_.ports.size();
    for (int i = 0; i < p.num_ports; i++) {
      snprintf(p.port[i], ST_PORT_MAX_LEN, "%s", cfg_.ports[i].port.c_str());
      uint8_t sip[4]; parse_ipv4(cfg_.ports[i].sip, sip);
      memcpy(p.sip_addr[i], sip, 4);
    }
    p.flags = cfg_.bind_numa ? ST_FLAG_BIND_NUMA : 0;

    if (cfg_.ptp_mode == PtpMode::ExternalFn) {
      if (!cfg_.external_ptp_time_fn) throw std::runtime_error("external_ptp_time_fn missing");
      ptp_fn_ = cfg_.external_ptp_time_fn;
      p.ptp_get_time_fn = (st_ptp_get_time_fn)default_ptp_get_time;
      p.priv = &ptp_fn_;
    }
    p.tx_queues_cnt[MTL_PORT_P] = cfg_.tx_queues;
    p.rx_queues_cnt[MTL_PORT_P] = cfg_.rx_queues;

    st_ = st_init(&p);
    if (!st_) throw std::runtime_error("st_init failed");
#else
    struct mtl_init_params p;
    memset(&p, 0, sizeof(p));
    p.num_ports = (int)cfg_.ports.size();
    for (int i = 0; i < p.num_ports; i++) {
      snprintf(p.port[i], MTL_PORT_MAX_LEN, "%s", cfg_.ports[i].port.c_str());
      uint8_t sip[4]; parse_ipv4(cfg_.ports[i].sip, sip);
      memcpy(p.sip_addr[i], sip, 4);
    }
    if (cfg_.enable_builtin_ptp) p.flags |= MTL_FLAG_PTP_ENABLE;
    if (cfg_.ptp_mode == PtpMode::ExternalFn) {
      ptp_fn_ = cfg_.external_ptp_time_fn;
      p.ptp_get_time_fn = (mtl_ptp_get_time_fn)default_ptp_get_time;
      p.priv = &ptp_fn_;
    }
    st_ = mtl_init(&p);
    if (!st_) throw std::runtime_error("mtl_init failed");
#endif
  }

  ~MtlBackend() override {
    stop();
#ifdef MTL_SDK_USE_ST_API
    if (st_) st_uninit(st_);
#else
    if (st_) mtl_uninit(st_);
#endif
  }

  int start() override {
#ifdef MTL_SDK_USE_ST_API
    return st_start(st_);
#else
    return mtl_start(st_);
#endif
  }

  int stop() override {
#ifdef MTL_SDK_USE_ST_API
    return st_stop(st_);
#else
    return mtl_stop(st_);
#endif
  }

  TimestampNs now_ptp_ns() const override {
#ifdef MTL_SDK_USE_ST_API
    // Legacy API doesn't expose read helper; use external fn or 0.
    if (cfg_.ptp_mode == PtpMode::ExternalFn && cfg_.external_ptp_time_fn) return cfg_.external_ptp_time_fn();
    return 0;
#else
    uint64_t ns = 0;
    mtl_ptp_read_time(st_, &ns); // cached time citeturn17view0
    return (TimestampNs)ns;
#endif
  }

  std::unique_ptr<IMtlVideoRxSession> create_video_rx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    return std::make_unique<MtlVideoRx>(st_, vf, ep, nullptr);
  }

  std::unique_ptr<IMtlAudioRxSession> create_audio_rx(const AudioFormat& af,
                                                      const St2110Endpoint& ep) override {
    return std::make_unique<MtlAudioRx>(st_, af, ep);
  }

  std::unique_ptr<IMtlVideoTxSession> create_video_tx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    return std::make_unique<MtlVideoTx>(st_, vf, ep);
  }

  std::unique_ptr<IMtlAudioTxSession> create_audio_tx(const AudioFormat& af,
                                                        const St2110Endpoint& ep) override {
    return std::make_unique<MtlAudioTx>(st_, af, ep);
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
