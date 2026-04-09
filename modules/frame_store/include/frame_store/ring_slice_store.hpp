#pragma once

#include "frame_transport/frame_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace frame_store {

struct RingStoreConfig {
  std::string root_dir;
  std::string channel_id = "channel_main";
  std::string session_id;
  uint32_t slice_duration_sec = 60;
  uint64_t slice_max_bytes = 0;
  uint64_t retention_bytes_limit = 0;
  uint32_t retention_slice_limit = 10;
  uint32_t min_reserved_slices = 2;
  uint32_t min_unprocessed_slices = 0;
};

class RingSliceStore {
public:
  static std::unique_ptr<RingSliceStore> open(const RingStoreConfig& cfg,
                                              const mtl_sdk::VideoFormat& video_format,
                                              const mtl_sdk::AudioFormat* audio_format);

  virtual ~RingSliceStore() = default;

  virtual bool write_video(const frame_transport::FramePacket& packet) = 0;
  virtual bool write_audio(const mtl_sdk::AudioFrame& frame) = 0;
  virtual void close() = 0;

  virtual size_t video_frames_written() const = 0;
  virtual size_t slices_created() const = 0;
};

}  // namespace frame_store
