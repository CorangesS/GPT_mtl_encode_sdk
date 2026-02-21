#include "mtl_sdk/mtl_sdk.hpp"
#include "mtl_backend.hpp"

#include <chrono>
#include <thread>

namespace mtl_sdk {

std::unique_ptr<Context> Context::create(const MtlSdkConfig& cfg) {
  auto backend = create_backend_mtl(cfg);
  if (!backend) return nullptr;
  return std::unique_ptr<Context>(new Context(std::move(backend)));
}

Context::Context(std::unique_ptr<IMtlBackend> backend) : backend_(std::move(backend)) {}

Context::~Context() { stop(); }

int Context::start() { return backend_ ? backend_->start() : -1; }
int Context::stop() { return backend_ ? backend_->stop() : 0; }

TimestampNs Context::now_ptp_ns() const { return backend_ ? backend_->now_ptp_ns() : 0; }

std::unique_ptr<Context::VideoRxSession> Context::create_video_rx(const VideoFormat& vf,
                                                                  const St2110Endpoint& ep) {
  if (!backend_) return nullptr;
  auto impl = backend_->create_video_rx(vf, ep);
  if (!impl) return nullptr;

  struct Wrapper final : public VideoRxSession {
    std::unique_ptr<IMtlVideoRxSession> inner;
    explicit Wrapper(std::unique_ptr<IMtlVideoRxSession> s) : inner(std::move(s)) {}
    bool poll(VideoFrame& out, int timeout_ms) override { return inner->poll(out, timeout_ms); }
    void release(VideoFrame& frame) override { inner->release(frame); }
  };

  return std::make_unique<Wrapper>(std::move(impl));
}

std::unique_ptr<Context::AudioRxSession> Context::create_audio_rx(const AudioFormat& af,
                                                                  const St2110Endpoint& ep) {
  if (!backend_) return nullptr;
  auto impl = backend_->create_audio_rx(af, ep);
  if (!impl) return nullptr;

  struct Wrapper final : public AudioRxSession {
    std::unique_ptr<IMtlAudioRxSession> inner;
    explicit Wrapper(std::unique_ptr<IMtlAudioRxSession> s) : inner(std::move(s)) {}
    bool poll(AudioFrame& out, int timeout_ms) override { return inner->poll(out, timeout_ms); }
  };

  return std::make_unique<Wrapper>(std::move(impl));
}

std::unique_ptr<Context::VideoTxSession> Context::create_video_tx(const VideoFormat& vf,
                                                                  const St2110Endpoint& ep) {
  if (!backend_) return nullptr;
  auto impl = backend_->create_video_tx(vf, ep);
  if (!impl) return nullptr;

  struct Wrapper final : public VideoTxSession {
    std::unique_ptr<IMtlVideoTxSession> inner;
    explicit Wrapper(std::unique_ptr<IMtlVideoTxSession> s) : inner(std::move(s)) {}
    bool put_video(const VideoFrame& frame) override { return inner->put_video(frame); }
  };

  return std::make_unique<Wrapper>(std::move(impl));
}

std::unique_ptr<Context::AudioTxSession> Context::create_audio_tx(const AudioFormat& af,
                                                                  const St2110Endpoint& ep) {
  if (!backend_) return nullptr;
  auto impl = backend_->create_audio_tx(af, ep);
  if (!impl) return nullptr;

  struct Wrapper final : public AudioTxSession {
    std::unique_ptr<IMtlAudioTxSession> inner;
    explicit Wrapper(std::unique_ptr<IMtlAudioTxSession> s) : inner(std::move(s)) {}
    bool put_audio(const AudioFrame& frame) override { return inner->put_audio(frame); }
  };

  return std::make_unique<Wrapper>(std::move(impl));
}

} // namespace mtl_sdk
