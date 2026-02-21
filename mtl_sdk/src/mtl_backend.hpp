#pragma once
#include "mtl_sdk/mtl_sdk.hpp"
#include <memory>

namespace mtl_sdk {

class IMtlVideoRxSession {
public:
  virtual ~IMtlVideoRxSession() = default;
  virtual bool poll(VideoFrame& out, int timeout_ms) = 0;
  virtual void release(VideoFrame& frame) = 0;
};

class IMtlAudioRxSession {
public:
  virtual ~IMtlAudioRxSession() = default;
  virtual bool poll(AudioFrame& out, int timeout_ms) = 0;
};

class IMtlVideoTxSession {
public:
  virtual ~IMtlVideoTxSession() = default;
  virtual bool put_video(const VideoFrame& frame) = 0;
};

class IMtlAudioTxSession {
public:
  virtual ~IMtlAudioTxSession() = default;
  virtual bool put_audio(const AudioFrame& frame) = 0;
};

class IMtlBackend {
public:
  virtual ~IMtlBackend() = default;

  virtual int start() = 0;
  virtual int stop() = 0;
  virtual TimestampNs now_ptp_ns() const = 0;

  virtual std::unique_ptr<IMtlVideoRxSession> create_video_rx(const VideoFormat& vf, const St2110Endpoint& ep) = 0;
  virtual std::unique_ptr<IMtlAudioRxSession> create_audio_rx(const AudioFormat& af, const St2110Endpoint& ep) = 0;
  virtual std::unique_ptr<IMtlVideoTxSession> create_video_tx(const VideoFormat& vf, const St2110Endpoint& ep) = 0;
  virtual std::unique_ptr<IMtlAudioTxSession> create_audio_tx(const AudioFormat& af, const St2110Endpoint& ep) = 0;
};

std::unique_ptr<IMtlBackend> create_backend_mtl(const MtlSdkConfig& cfg);

} // namespace mtl_sdk
