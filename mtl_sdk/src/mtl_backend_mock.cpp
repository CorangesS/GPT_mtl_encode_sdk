#include "mtl_backend.hpp"
#include "ring_spsc.hpp"

#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

namespace mtl_sdk {

// Very small synthetic pipeline useful to validate encode_sdk without real MTL.
// Generates NV12 video + PCM audio with PTP-like timestamps.

static TimestampNs mono_ns() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

class MockVideoRx final : public IMtlVideoRxSession {
public:
  MockVideoRx(VideoFormat vf) : vf_(vf), ring_(256) {
    // Pre-allocate a few frames (host memory)
    for (int i = 0; i < 8; i++) {
      FrameBuf b{};
      b.y.resize(vf_.width * vf_.height);
      b.uv.resize((vf_.width * vf_.height) / 2);
      free_.push_back(i);
      bufs_.push_back(std::move(b));
    }
    start_ns_ = mono_ns();
  }

  bool poll(VideoFrame& out, int timeout_ms) override {
    // generate at vf_.fps
    const double frame_ns = 1e9 / vf_.fps;
    auto now = mono_ns();
    auto target = start_ns_ + (TimestampNs)std::llround(frame_idx_ * frame_ns);
    if (now < target) {
      if (timeout_ms <= 0) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
      return false;
    }

    if (free_.empty()) return false;
    int idx = free_.back();
    free_.pop_back();

    fill_frame(idx, frame_idx_);

    out = {};
    out.fmt = vf_;
    out.timestamp_ns = target;
    out.mem_type = MemoryType::HostPtr;
    out.num_planes = 2;
    out.planes[0].data = bufs_[idx].y.data();
    out.planes[0].linesize = vf_.width;
    out.planes[1].data = bufs_[idx].uv.data();
    out.planes[1].linesize = vf_.width;
    out.bytes_total = bufs_[idx].y.size() + bufs_[idx].uv.size();
    out.opaque = reinterpret_cast<void*>(static_cast<intptr_t>(idx));

    frame_idx_++;
    return true;
  }

  void release(VideoFrame& frame) override {
    int idx = (int)(intptr_t)frame.opaque;
    if (idx >= 0 && idx < (int)bufs_.size()) free_.push_back(idx);
    frame.opaque = nullptr;
  }

private:
  struct FrameBuf { std::vector<uint8_t> y; std::vector<uint8_t> uv; };

  void fill_frame(int idx, int64_t n) {
    auto& y = bufs_[idx].y;
    auto& uv = bufs_[idx].uv;
    const int w = vf_.width;
    const int h = vf_.height;
    // moving gradient
    for (int j = 0; j < h; j++) {
      for (int i = 0; i < w; i++) {
        y[j*w + i] = (uint8_t)((i + n*2) % 256);
      }
    }
    std::fill(uv.begin(), uv.end(), 128);
  }

  VideoFormat vf_;
  SpscRing<int> ring_;
  std::vector<FrameBuf> bufs_;
  std::vector<int> free_;
  TimestampNs start_ns_{0};
  int64_t frame_idx_{0};
};

class MockAudioRx final : public IMtlAudioRxSession {
public:
  explicit MockAudioRx(AudioFormat af) : af_(af) {
    start_ns_ = mono_ns();
  }

  bool poll(AudioFrame& out, int timeout_ms) override {
    (void)timeout_ms;
    // generate 10ms audio chunks
    const int samples_per_chunk = af_.sample_rate / 100;
    const double chunk_ns = 1e9 * (double)samples_per_chunk / (double)af_.sample_rate;
    auto target = start_ns_ + (TimestampNs)std::llround(chunk_idx_ * chunk_ns);

    auto now = mono_ns();
    if (now < target) return false;

    out = {};
    out.fmt = af_;
    out.timestamp_ns = target;
    out.mem_type = MemoryType::HostPtr;

    // interleaved S16LE
    out.pcm.resize(samples_per_chunk * af_.channels * 2);
    int16_t* pcm = (int16_t*)out.pcm.data();
    double freq = 440.0;
    for (int i = 0; i < samples_per_chunk; i++) {
      double t = (chunk_idx_ * samples_per_chunk + i) / (double)af_.sample_rate;
      int16_t s = (int16_t)(std::sin(2.0 * M_PI * freq * t) * 16000);
      for (int ch = 0; ch < af_.channels; ch++) pcm[i*af_.channels + ch] = s;
    }

    chunk_idx_++;
    return true;
  }

private:
  AudioFormat af_;
  TimestampNs start_ns_{0};
  int64_t chunk_idx_{0};
};

// Mock TX: accept frames but do not send over network. For two-process tests use real MTL backend.
class MockVideoTx final : public IMtlVideoTxSession {
public:
  MockVideoTx(VideoFormat vf) : vf_(vf) {}
  bool put_video(const VideoFrame& frame) override {
    (void)frame;
    return true;
  }
private:
  VideoFormat vf_;
};

class MockAudioTx final : public IMtlAudioTxSession {
public:
  explicit MockAudioTx(AudioFormat af) : af_(af) {}
  bool put_audio(const AudioFrame& frame) override {
    (void)frame;
    return true;
  }
private:
  AudioFormat af_;
};

class MockBackend final : public IMtlBackend {
public:
  explicit MockBackend(const MtlSdkConfig& cfg) : cfg_(cfg) {}

  int start() override { started_ = true; return 0; }
  int stop() override { started_ = false; return 0; }
  TimestampNs now_ptp_ns() const override { return mono_ns(); }

  std::unique_ptr<IMtlVideoRxSession> create_video_rx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    (void)ep;
    // mock always provides NV12 frames regardless; keep fmt in vf
    VideoFormat v = vf;
    if (v.pix_fmt != VideoPixFmt::NV12) v.pix_fmt = VideoPixFmt::NV12;
    return std::make_unique<MockVideoRx>(v);
  }

  std::unique_ptr<IMtlAudioRxSession> create_audio_rx(const AudioFormat& af,
                                                      const St2110Endpoint& ep) override {
    (void)ep;
    return std::make_unique<MockAudioRx>(af);
  }

  std::unique_ptr<IMtlVideoTxSession> create_video_tx(const VideoFormat& vf,
                                                      const St2110Endpoint& ep) override {
    (void)ep;
    return std::make_unique<MockVideoTx>(vf);
  }

  std::unique_ptr<IMtlAudioTxSession> create_audio_tx(const AudioFormat& af,
                                                      const St2110Endpoint& ep) override {
    (void)ep;
    return std::make_unique<MockAudioTx>(af);
  }

private:
  MtlSdkConfig cfg_;
  bool started_{false};
};

std::unique_ptr<IMtlBackend> create_backend_mock(const MtlSdkConfig& cfg) {
  return std::make_unique<MockBackend>(cfg);
}

} // namespace mtl_sdk
