#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "mtl_sdk/mtl_sdk.hpp"

namespace encode_sdk {

enum class VideoCodec { H264, H265 };
enum class AudioCodec { AAC, MP2, PCM, AC3 };
enum class Container  { MP4, MXF };

enum class HwAccel {
  Auto,      // prefer NVENC then QSV then SW
  NVENC,
  QSV,
  Software
};

struct VideoEncodeParams {
  VideoCodec codec = VideoCodec::H264;
  HwAccel hw = HwAccel::Auto;

  int bitrate_kbps = 8000;
  int gop = 60;                // distance between IDR
  std::string profile;         // e.g. "high", "main", "high10"
  int fps_num = 60000;
  int fps_den = 1001;

  // Input pixel format expectation (e.g. NV12/P010)
  mtl_sdk::VideoPixFmt input_fmt = mtl_sdk::VideoPixFmt::NV12;
};

struct AudioEncodeParams {
  AudioCodec codec = AudioCodec::AAC;
  int bitrate_kbps = 192;
  int sample_rate = 48000;
  int channels = 2;
};

struct MuxParams {
  Container container = Container::MP4;
  std::string output_path;
};

struct EncodeParams {
  VideoEncodeParams video;
  std::optional<AudioEncodeParams> audio;
  MuxParams mux;
};

class Session {
public:
  static std::unique_ptr<Session> open(const EncodeParams& params);

  virtual ~Session() = default;

  // push frames in timestamp order. timestamp_ns is PTP-derived.
  virtual bool push_video(const mtl_sdk::VideoFrame& frame) = 0;
  virtual bool push_audio(const mtl_sdk::AudioFrame& frame) = 0;

  // Flush and finalize output
  virtual bool close() = 0;

  // Runtime parameter adjustment: safest portable method is "reopen" (close+open).
  // This helper updates internal config; apply() will reopen under the hood.
  virtual void set_video_bitrate_kbps(int kbps) = 0;
  virtual void set_video_gop(int gop) = 0;
  virtual bool apply_reconfigure() = 0;
};

} // namespace encode_sdk
