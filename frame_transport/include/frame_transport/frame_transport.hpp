#pragma once

#include "mtl_sdk/mtl_sdk.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <utility>
#include <vector>

namespace frame_transport {

struct FramePacket {
  mtl_sdk::VideoFormat fmt;
  int64_t timestamp_ns = 0;
  std::vector<uint8_t> y, u, v;
  int linesize_y = 0;
  int linesize_uv = 0;

  static FramePacket from(const mtl_sdk::VideoFrame& frame) {
    FramePacket packet;
    packet.fmt = frame.fmt;
    packet.timestamp_ns = frame.timestamp_ns;
    packet.linesize_y = frame.planes[0].linesize;
    packet.linesize_uv = frame.planes[1].linesize;

    const size_t y_sz = static_cast<size_t>(frame.fmt.height) * frame.planes[0].linesize;
    const size_t uv_sz = static_cast<size_t>(frame.fmt.height) * frame.planes[1].linesize;
    if (frame.num_planes >= 3 && frame.planes[0].data && frame.planes[1].data && frame.planes[2].data &&
        y_sz > 0 && uv_sz > 0) {
      packet.y.assign(frame.planes[0].data, frame.planes[0].data + y_sz);
      packet.u.assign(frame.planes[1].data, frame.planes[1].data + uv_sz);
      packet.v.assign(frame.planes[2].data, frame.planes[2].data + uv_sz);
    }
    return packet;
  }

  bool is_valid() const { return !y.empty() && !u.empty() && !v.empty(); }

  void to_video_frame(mtl_sdk::VideoFrame& out) const {
    out = {};
    if (!is_valid()) return;

    out.fmt = fmt;
    out.timestamp_ns = timestamp_ns;
    out.mem_type = mtl_sdk::MemoryType::HostPtr;
    out.num_planes = 3;
    out.planes[0].data = const_cast<uint8_t*>(y.data());
    out.planes[0].linesize = linesize_y;
    out.planes[1].data = const_cast<uint8_t*>(u.data());
    out.planes[1].linesize = linesize_uv;
    out.planes[2].data = const_cast<uint8_t*>(v.data());
    out.planes[2].linesize = linesize_uv;
    out.bytes_total = y.size() + u.size() + v.size();
  }
};

class IFrameSink {
public:
  virtual ~IFrameSink() = default;
  virtual bool push(FramePacket packet) = 0;
};

class BoundedFrameQueue final : public IFrameSink {
public:
  explicit BoundedFrameQueue(size_t max_size = 64) : max_size_(max_size) {}

  bool push(FramePacket packet) override {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!closed_ && queue_.size() >= max_size_) {
      cv_full_.wait(lock);
    }
    if (closed_) return false;
    queue_.push(std::move(packet));
    cv_empty_.notify_one();
    return true;
  }

  bool pop(FramePacket& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (queue_.empty() && !closed_) {
      cv_empty_.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    cv_full_.notify_one();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_empty_.notify_all();
    cv_full_.notify_all();
  }

private:
  size_t max_size_;
  std::queue<FramePacket> queue_;
  std::mutex mutex_;
  std::condition_variable cv_empty_;
  std::condition_variable cv_full_;
  bool closed_ = false;
};

enum class PumpResult {
  Timeout,
  Forwarded,
  InvalidFrame,
  SinkClosed,
};

class ReceiverToSinkAdapter {
public:
  PumpResult pump_once(mtl_sdk::Context::VideoRxSession& rx, IFrameSink& sink, int timeout_ms = 0) const {
    mtl_sdk::VideoFrame frame;
    if (!rx.poll(frame, timeout_ms)) return PumpResult::Timeout;

    FramePacket packet = FramePacket::from(frame);
    rx.release(frame);
    if (!packet.is_valid()) return PumpResult::InvalidFrame;
    return sink.push(std::move(packet)) ? PumpResult::Forwarded : PumpResult::SinkClosed;
  }
};

}  // namespace frame_transport
