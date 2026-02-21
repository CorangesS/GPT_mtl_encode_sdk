#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>

namespace mtl_sdk {

// ---- Core time model ----
// All timestamps are "PTP time" expressed as nanoseconds in a single epoch.
// In built-in PTP mode, MTL can supply it; in external mode, application provides it.
using TimestampNs = int64_t;

enum class PtpMode {
  BuiltInMtl,   // Use MTL built-in IEEE1588 client
  ExternalFn,   // Application provides ptp_get_time_fn callback
};

using PtpGetTimeFn = std::function<TimestampNs(void)>;

// ---- Network + session config ----
struct NetPortConfig {
  std::string port;   // e.g., "0000:af:01.0" (DPDK BDF) or "kernel:enp175s0f0" etc.
  std::string sip;    // source IP address (IPv4 dotted)
};

struct MtlSdkConfig {
  std::vector<NetPortConfig> ports;
  bool bind_numa = true;
  int tx_queues = 0;
  int rx_queues = 0;

  PtpMode ptp_mode = PtpMode::BuiltInMtl;
  bool enable_builtin_ptp = true;    // maps to MTL_FLAG_PTP_ENABLE (when supported)
  int utc_offset_seconds = 37;       // help when external ptp source uses UTC (see MTL docs)

  PtpGetTimeFn external_ptp_time_fn; // required if ptp_mode == ExternalFn
};

// Simplified pixel formats expected at the SDK boundary.
// (MTL supports many more; you can extend mapping as needed.)
enum class VideoPixFmt {
  YUV422_10BIT,  // YUV422 planar 10-bit LE, 3 planes (Y, U, V)
  YUV420P10LE,   // YUV420 planar 10-bit LE, 3 planes (user file format)
  NV12,          // 8-bit 4:2:0, 2 planes (Y + interleaved UV)
  P010,          // 10-bit 4:2:0 (little endian), 2 planes
};

struct VideoFormat {
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  VideoPixFmt pix_fmt = VideoPixFmt::YUV422_10BIT;
};

struct AudioFormat {
  int sample_rate = 48000;
  int channels = 2;
  // ST2110-30 is commonly PCM; for simplicity we treat input as PCM S16/S24 in app space.
  int bits_per_sample = 24; // 16 or 24 typical
};

struct St2110Endpoint {
  std::string ip;     // multicast or unicast
  uint16_t udp_port = 5004;
  uint8_t payload_type = 96;
};

// ---- SDP model ----
struct SdpMedia {
  enum class Type { Video, Audio };
  Type type;

  St2110Endpoint endpoint;

  // For raw video, fmtp carries width/height/sampling/etc. We keep a generic kv map.
  std::string rtpmap;                 // e.g. "raw/90000" or "L24/48000/2"
  std::vector<std::string> fmtp_kv;   // raw lines, "k=v" or tokens

  // Timing attributes (important for ST2110)
  std::optional<std::string> ts_refclk;   // e.g. "ptp=IEEE1588-2008:..."
  std::optional<std::string> mediaclk;    // e.g. "direct=0"
};

struct SdpSession {
  std::string session_name = "mtl-sdk";
  std::string origin = "- 0 0 IN IP4 127.0.0.1";
  std::string connection = "IN IP4 0.0.0.0";
  std::vector<SdpMedia> media;
};

SdpSession parse_sdp(const std::string& text);
std::string to_sdp(const SdpSession& sdp);

SdpSession load_sdp_file(const std::string& path);
void save_sdp_file(const std::string& path, const SdpSession& sdp);

// ---- Frame memory model ----
enum class MemoryType {
  HostPtr,     // CPU virtual addresses in addr[]
  DmaBufFd,    // Linux DMABUF (fd) for zero-copy import (optional)
  CudaDevice,  // CUDA device ptr (future)
};

struct PlaneView {
  uint8_t* data = nullptr;
  int linesize = 0;
};

struct VideoFrame {
  VideoFormat fmt{};
  TimestampNs timestamp_ns = 0;

  MemoryType mem_type = MemoryType::HostPtr;
  int dmabuf_fd = -1;  // if mem_type == DmaBufFd

  PlaneView planes[3]{};
  int num_planes = 0;
  size_t bytes_total = 0;

  // internal handle to return buffer to MTL backend
  void* opaque = nullptr;
};

struct AudioFrame {
  AudioFormat fmt{};
  TimestampNs timestamp_ns = 0;
  MemoryType mem_type = MemoryType::HostPtr;

  std::vector<uint8_t> pcm; // interleaved PCM
};

// ---- Public sessions ----
class IMtlBackend; // internal

class Context {
public:
  static std::unique_ptr<Context> create(const MtlSdkConfig& cfg);

  ~Context();

  int start();
  int stop();

  // Read PTP time in ns (best-effort).
  TimestampNs now_ptp_ns() const;

  // Session factories (RX = receive, TX = send)
  class VideoRxSession;
  class AudioRxSession;
  class VideoTxSession;
  class AudioTxSession;

  std::unique_ptr<VideoRxSession> create_video_rx(const VideoFormat& vf, const St2110Endpoint& ep);
  std::unique_ptr<AudioRxSession> create_audio_rx(const AudioFormat& af, const St2110Endpoint& ep);
  std::unique_ptr<VideoTxSession> create_video_tx(const VideoFormat& vf, const St2110Endpoint& ep);
  std::unique_ptr<AudioTxSession> create_audio_tx(const AudioFormat& af, const St2110Endpoint& ep);

private:
  explicit Context(std::unique_ptr<IMtlBackend> backend);
  std::unique_ptr<IMtlBackend> backend_;
};

class Context::VideoRxSession {
public:
  virtual ~VideoRxSession() = default;

  // Poll for a frame (non-blocking-ish). timeout_ms == 0 means immediate.
  // Returns true if a frame is available.
  virtual bool poll(VideoFrame& out, int timeout_ms) = 0;

  // Return the frame back to the RX pool / MTL. Safe to call with last polled frame.
  virtual void release(VideoFrame& frame) = 0;
};

class Context::AudioRxSession {
public:
  virtual ~AudioRxSession() = default;
  virtual bool poll(AudioFrame& out, int timeout_ms) = 0;
};

// ---- TX (send) sessions: ST2110-20 video, ST2110-30 audio ----
class Context::VideoTxSession {
public:
  virtual ~VideoTxSession() = default;
  // Submit one frame for transmission. Blocks if TX queue is full (or returns false if non-blocking).
  virtual bool put_video(const VideoFrame& frame) = 0;
};

class Context::AudioTxSession {
public:
  virtual ~AudioTxSession() = default;
  // Submit one audio frame for transmission.
  virtual bool put_audio(const AudioFrame& frame) = 0;
};

} // namespace mtl_sdk
