#pragma once

#include "frame_transport/frame_transport.hpp"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

namespace frame_store {

struct YuvWriteMeta {
  int width = 0;
  int height = 0;
  double fps = 0.0;
  std::string pix_fmt;  // e.g. "YUV422_10BIT"
  int linesize_y = 0;
  int linesize_uv = 0;
  uint64_t size_y = 0;
  uint64_t size_u = 0;
  uint64_t size_v = 0;
  uint64_t bytes_per_frame = 0;
  uint64_t frames_written = 0;
  std::string session_id;
};

class YuvFileWriter final {
public:
  YuvFileWriter() = default;
  ~YuvFileWriter();

  YuvFileWriter(const YuvFileWriter&) = delete;
  YuvFileWriter& operator=(const YuvFileWriter&) = delete;

  bool open(const std::string& yuv_path,
            const std::string& meta_path,
            const mtl_sdk::VideoFormat& fmt,
            const std::string& session_id);

  bool write(const frame_transport::FramePacket& packet);

  void close();

  const YuvWriteMeta& meta() const { return meta_; }

private:
  bool write_meta_json(bool force = false);

  std::string yuv_path_;
  std::string meta_path_;
  std::ofstream yuv_out_;
  YuvWriteMeta meta_;
  uint64_t last_meta_flush_ns_ = 0;
  bool opened_ = false;
};

}  // namespace frame_store

