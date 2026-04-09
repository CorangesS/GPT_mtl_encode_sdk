#include "frame_store/ring_slice_store.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace frame_store {
namespace fs = std::filesystem;

namespace {

struct IndexEntry {
  uint64_t timestamp_ns = 0;
  uint64_t offset_y = 0;
  uint64_t size_y = 0;
  uint64_t offset_u = 0;
  uint64_t size_u = 0;
  uint64_t offset_v = 0;
  uint64_t size_v = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t linesize_y = 0;
  uint32_t linesize_uv = 0;
};

struct AudioIndexEntry {
  uint64_t timestamp_ns = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t bits_per_sample = 0;
};

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

static std::string pix_fmt_name(mtl_sdk::VideoPixFmt fmt) {
  switch (fmt) {
    case mtl_sdk::VideoPixFmt::YUV422_10BIT: return "YUV422_10BIT";
    case mtl_sdk::VideoPixFmt::YUV420P10LE: return "YUV420P10LE";
    case mtl_sdk::VideoPixFmt::NV12: return "NV12";
    case mtl_sdk::VideoPixFmt::P010: return "P010";
  }
  return "UNKNOWN";
}

static std::string utc_compact(int64_t ts_ns) {
  std::time_t seconds = static_cast<std::time_t>(ts_ns / 1000000000LL);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%dT%H%M%SZ");
  return oss.str();
}

static int64_t fallback_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static std::string read_text_file(const fs::path& path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string extract_json_string(std::string_view json, const std::string& key) {
  const std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string_view::npos) return {};
  pos = json.find(':', pos);
  if (pos == std::string_view::npos) return {};
  pos = json.find('"', pos + 1);
  if (pos == std::string_view::npos) return {};
  size_t end = json.find('"', pos + 1);
  if (end == std::string_view::npos) return {};
  return std::string(json.substr(pos + 1, end - pos - 1));
}

static uint64_t extract_json_u64(std::string_view json, const std::string& key, uint64_t fallback = 0) {
  const std::string token = "\"" + key + "\"";
  size_t pos = json.find(token);
  if (pos == std::string_view::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string_view::npos) return fallback;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  size_t end = pos;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end == pos) return fallback;
  return std::stoull(std::string(json.substr(pos, end - pos)));
}

struct SliceInfo {
  std::string slice_id;
  fs::path dir;
  std::string status;
  uint64_t bytes = 0;
  uint64_t video_frames = 0;
};

struct PendingWrite {
  enum class Kind { Video, Audio };

  Kind kind = Kind::Video;
  frame_transport::FramePacket video;
  mtl_sdk::AudioFrame audio;
};

class RingSliceStoreImpl final : public RingSliceStore {
public:
  static constexpr uint64_t kStateFlushIntervalNs = 1000000000ULL;
  static constexpr size_t kQueueMaxSize = 64;
  static constexpr size_t kVideoFlushThresholdBytes = 4 * 1024 * 1024;
  static constexpr size_t kIndexFlushThresholdBytes = 256 * 1024;

  RingSliceStoreImpl(const RingStoreConfig& cfg,
                     const mtl_sdk::VideoFormat& video_format,
                     const mtl_sdk::AudioFormat* audio_format)
      : cfg_(cfg), video_format_(video_format) {
    if (cfg_.root_dir.empty()) {
      throw std::runtime_error("ring store root_dir must not be empty");
    }
    if (cfg_.slice_duration_sec == 0 && cfg_.slice_max_bytes == 0) {
      throw std::runtime_error("ring store requires slice_duration_sec or slice_max_bytes");
    }
    if (audio_format) audio_format_ = *audio_format;

    root_dir_ = fs::path(cfg_.root_dir) / cfg_.channel_id;
    state_dir_ = root_dir_ / "state";
    slices_dir_ = root_dir_ / "slices";
    fs::create_directories(state_dir_);
    fs::create_directories(slices_dir_);

    init_manifest();
    init_state_files();
    writer_thread_ = std::thread([this]() { writer_loop(); });
  }

  ~RingSliceStoreImpl() override { close(); }

  bool write_video(const frame_transport::FramePacket& packet) override {
    if (!packet.is_valid()) return false;
    PendingWrite write;
    write.kind = PendingWrite::Kind::Video;
    write.video = packet;
    return enqueue_write(std::move(write));
  }

  bool write_audio(const mtl_sdk::AudioFrame& frame) override {
    if (!audio_format_ || frame.pcm.empty()) return false;
    PendingWrite write;
    write.kind = PendingWrite::Kind::Audio;
    write.audio = frame;
    return enqueue_write(std::move(write));
  }

  void close() override {
    if (closed_) return;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      stopping_ = true;
    }
    queue_cv_empty_.notify_all();
    queue_cv_full_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    flush_pending_buffers();
    seal_active_slice("close");
    recycle_if_needed();
    write_recycler_state();
    closed_ = true;
  }

  size_t video_frames_written() const override { return total_video_frames_; }
  size_t slices_created() const override { return slice_sequence_; }

private:
  bool enqueue_write(PendingWrite write) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    while (!stopping_ && queue_.size() >= kQueueMaxSize) {
      queue_cv_full_.wait(lock);
    }
    if (stopping_) return false;
    queue_.push(std::move(write));
    queue_cv_empty_.notify_one();
    return true;
  }

  void writer_loop() {
    while (true) {
      PendingWrite write;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        while (queue_.empty() && !stopping_) {
          queue_cv_empty_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (queue_.empty()) {
          if (stopping_) break;
          continue;
        }
        write = std::move(queue_.front());
        queue_.pop();
        queue_cv_full_.notify_one();
      }

      if (write.kind == PendingWrite::Kind::Video) {
        write_video_sync(write.video);
      } else {
        write_audio_sync(write.audio);
      }
    }
  }

  void write_video_sync(const frame_transport::FramePacket& packet) {
    ensure_open_slice(packet.timestamp_ns);

    const uint64_t base_offset = file_video_bytes_written_ + video_buffer_.size();
    const uint64_t offset_y = base_offset;
    append_bytes(video_buffer_, packet.y.data(), packet.y.size());
    const uint64_t offset_u = base_offset + packet.y.size();
    append_bytes(video_buffer_, packet.u.data(), packet.u.size());
    const uint64_t offset_v = base_offset + packet.y.size() + packet.u.size();
    append_bytes(video_buffer_, packet.v.data(), packet.v.size());

    IndexEntry entry;
    entry.timestamp_ns = static_cast<uint64_t>(packet.timestamp_ns);
    entry.offset_y = offset_y;
    entry.size_y = packet.y.size();
    entry.offset_u = offset_u;
    entry.size_u = packet.u.size();
    entry.offset_v = offset_v;
    entry.size_v = packet.v.size();
    entry.width = packet.fmt.width;
    entry.height = packet.fmt.height;
    entry.linesize_y = packet.linesize_y;
    entry.linesize_uv = packet.linesize_uv;
    append_pod(index_buffer_, entry);

    ++slice_video_frames_;
    ++total_video_frames_;
    slice_bytes_written_ += packet.y.size() + packet.u.size() + packet.v.size();
    last_video_ts_ns_ = packet.timestamp_ns;

    maybe_flush_slice_state("writing", "video_frame");
    flush_pending_buffers_if_needed();

    if (should_rotate(packet.timestamp_ns)) seal_active_slice("rotate_threshold");
  }

  void write_audio_sync(const mtl_sdk::AudioFrame& frame) {
    if (!audio_format_ || frame.pcm.empty()) return;
    ensure_open_slice(frame.timestamp_ns);

    const uint64_t offset = file_audio_bytes_written_ + audio_buffer_.size();
    append_bytes(audio_buffer_, frame.pcm.data(), frame.pcm.size());

    AudioIndexEntry entry;
    entry.timestamp_ns = static_cast<uint64_t>(frame.timestamp_ns);
    entry.offset = offset;
    entry.size = frame.pcm.size();
    entry.sample_rate = frame.fmt.sample_rate;
    entry.channels = frame.fmt.channels;
    entry.bits_per_sample = frame.fmt.bits_per_sample;
    append_pod(audio_index_buffer_, entry);

    ++slice_audio_frames_;
    slice_bytes_written_ += frame.pcm.size();
    last_audio_ts_ns_ = frame.timestamp_ns;
    maybe_flush_slice_state("writing", "audio_frame");
    flush_pending_buffers_if_needed();
  }

  static void append_bytes(std::vector<uint8_t>& buffer, const uint8_t* data, size_t size) {
    if (size == 0) return;
    buffer.insert(buffer.end(), data, data + size);
  }

  template <typename T>
  static void append_pod(std::vector<uint8_t>& buffer, const T& value) {
    const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
  }

  void init_manifest() {
    std::ofstream out(root_dir_ / "manifest.json", std::ios::trunc);
    out << "{\n"
        << "  \"channel_id\": \"" << json_escape(cfg_.channel_id) << "\",\n"
        << "  \"session_id\": \"" << json_escape(cfg_.session_id) << "\",\n"
        << "  \"format_version\": 1,\n"
        << "  \"slice_duration_sec\": " << cfg_.slice_duration_sec << ",\n"
        << "  \"slice_max_bytes\": " << cfg_.slice_max_bytes << ",\n"
        << "  \"retention_bytes_limit\": " << cfg_.retention_bytes_limit << ",\n"
        << "  \"retention_slice_limit\": " << cfg_.retention_slice_limit << ",\n"
        << "  \"min_reserved_slices\": " << cfg_.min_reserved_slices << ",\n"
        << "  \"min_unprocessed_slices\": " << cfg_.min_unprocessed_slices << ",\n"
        << "  \"video\": {\n"
        << "    \"width\": " << video_format_.width << ",\n"
        << "    \"height\": " << video_format_.height << ",\n"
        << "    \"fps\": " << std::fixed << std::setprecision(3) << video_format_.fps << ",\n"
        << "    \"pix_fmt\": \"" << pix_fmt_name(video_format_.pix_fmt) << "\"\n"
        << "  }";
    if (audio_format_) {
      out << ",\n  \"audio\": {\n"
          << "    \"sample_rate\": " << audio_format_->sample_rate << ",\n"
          << "    \"channels\": " << audio_format_->channels << ",\n"
          << "    \"bits_per_sample\": " << audio_format_->bits_per_sample << "\n"
          << "  }\n";
    } else {
      out << "\n";
    }
    out << "}\n";
  }

  void init_state_files() {
    update_writer_state("init");
    write_recycler_state();
    std::ofstream out(state_dir_ / "decoder_state.json", std::ios::trunc);
    out << "{\n"
        << "  \"current_slice_id\": \"\",\n"
        << "  \"current_frame_index\": 0,\n"
        << "  \"last_committed_slice_id\": \"\",\n"
        << "  \"mode\": \"idle\",\n"
        << "  \"lease_expires_at_ns\": 0\n"
        << "}\n";
  }

  void ensure_open_slice(int64_t ts_ns) {
    if (slice_open_) return;
    const int64_t start_ts_ns = ts_ns > 0 ? ts_ns : fallback_now_ns();
    ++slice_sequence_;
    active_slice_id_ = utc_compact(start_ts_ns) + "_" + format_sequence(slice_sequence_);
    active_slice_dir_ = slices_dir_ / active_slice_id_;
    fs::create_directories(active_slice_dir_);

    video_frames_.open(active_slice_dir_ / "video.frames", std::ios::binary | std::ios::trunc);
    index_file_.open(active_slice_dir_ / "index.bin", std::ios::binary | std::ios::trunc);
    if (audio_format_) {
      audio_frames_.open(active_slice_dir_ / "audio.frames", std::ios::binary | std::ios::trunc);
      audio_index_file_.open(active_slice_dir_ / "audio.index.bin", std::ios::binary | std::ios::trunc);
    }

    slice_start_ts_ns_ = start_ts_ns;
    slice_video_frames_ = 0;
    slice_audio_frames_ = 0;
    slice_bytes_written_ = 0;
    video_buffer_.clear();
    index_buffer_.clear();
    audio_buffer_.clear();
    audio_index_buffer_.clear();
    file_video_bytes_written_ = 0;
    file_audio_bytes_written_ = 0;
    first_slice_id_ = first_slice_id_.empty() ? active_slice_id_ : first_slice_id_;
    last_slice_id_ = active_slice_id_;
    slice_open_ = true;
    // Wall-clock anchor for slice duration (--slice-seconds). Video timestamps may be 0 on the
    // first frame (--no-ptp) then small PTS values; comparing those to a wall-based slice_start
    // underflows uint64_t and falsely triggers rotate_threshold every couple of frames.
    slice_rotate_anchor_wall_ns_ = static_cast<uint64_t>(fallback_now_ns());
    update_slice_json("writing");
    update_writer_state("open_slice");
    last_state_flush_ns_ = static_cast<uint64_t>(fallback_now_ns());
  }

  bool should_rotate(int64_t /*current_ts_ns*/) const {
    if (!slice_open_) return false;
    if (cfg_.slice_duration_sec > 0) {
      const uint64_t now_wall = static_cast<uint64_t>(fallback_now_ns());
      const uint64_t elapsed_ns = now_wall - slice_rotate_anchor_wall_ns_;
      if (elapsed_ns >= static_cast<uint64_t>(cfg_.slice_duration_sec) * 1000000000ULL) return true;
    }
    if (cfg_.slice_max_bytes > 0 && slice_bytes_written_ >= cfg_.slice_max_bytes) return true;
    return false;
  }

  void seal_active_slice(const std::string& reason) {
    if (!slice_open_) return;
    flush_pending_buffers();
    update_slice_json("sealed", reason);
    video_frames_.close();
    index_file_.close();
    if (audio_frames_.is_open()) audio_frames_.close();
    if (audio_index_file_.is_open()) audio_index_file_.close();
    recycle_if_needed();
    write_recycler_state();
    update_writer_state(reason);
    slice_open_ = false;
    slice_start_ts_ns_ = 0;
    slice_rotate_anchor_wall_ns_ = 0;
    slice_bytes_written_ = 0;
  }

  void flush_pending_buffers_if_needed() {
    const bool video_ready = video_buffer_.size() >= kVideoFlushThresholdBytes;
    const bool index_ready = index_buffer_.size() >= kIndexFlushThresholdBytes;
    const bool audio_ready = audio_buffer_.size() >= kVideoFlushThresholdBytes;
    const bool audio_index_ready = audio_index_buffer_.size() >= kIndexFlushThresholdBytes;
    if (video_ready || index_ready || audio_ready || audio_index_ready) {
      flush_pending_buffers();
    }
  }

  void flush_pending_buffers() {
    if (!video_buffer_.empty()) {
      video_frames_.write(reinterpret_cast<const char*>(video_buffer_.data()),
                          static_cast<std::streamsize>(video_buffer_.size()));
      file_video_bytes_written_ += video_buffer_.size();
      video_buffer_.clear();
    }
    if (!index_buffer_.empty()) {
      index_file_.write(reinterpret_cast<const char*>(index_buffer_.data()),
                        static_cast<std::streamsize>(index_buffer_.size()));
      index_buffer_.clear();
    }
    if (audio_frames_.is_open() && !audio_buffer_.empty()) {
      audio_frames_.write(reinterpret_cast<const char*>(audio_buffer_.data()),
                          static_cast<std::streamsize>(audio_buffer_.size()));
      file_audio_bytes_written_ += audio_buffer_.size();
      audio_buffer_.clear();
    }
    if (audio_index_file_.is_open() && !audio_index_buffer_.empty()) {
      audio_index_file_.write(reinterpret_cast<const char*>(audio_index_buffer_.data()),
                              static_cast<std::streamsize>(audio_index_buffer_.size()));
      audio_index_buffer_.clear();
    }
  }

  void update_slice_json(const std::string& status, const std::string& rotate_reason = "") {
    if (active_slice_dir_.empty()) return;
    std::ofstream out(active_slice_dir_ / "slice.json", std::ios::trunc);
    out << "{\n"
        << "  \"slice_id\": \"" << active_slice_id_ << "\",\n"
        << "  \"session_id\": \"" << json_escape(cfg_.session_id) << "\",\n"
        << "  \"status\": \"" << status << "\",\n"
        << "  \"opened_at_ns\": " << slice_start_ts_ns_ << ",\n"
        << "  \"last_video_ts_ns\": " << last_video_ts_ns_ << ",\n"
        << "  \"last_audio_ts_ns\": " << last_audio_ts_ns_ << ",\n"
        << "  \"video_frames_written\": " << slice_video_frames_ << ",\n"
        << "  \"audio_frames_written\": " << slice_audio_frames_ << ",\n"
        << "  \"bytes_written\": " << slice_bytes_written_ << ",\n"
        << "  \"rotate_reason\": \"" << json_escape(rotate_reason) << "\"\n"
        << "}\n";
  }

  void update_writer_state(const std::string& rotate_reason) {
    std::ofstream out(state_dir_ / "writer_state.json", std::ios::trunc);
    out << "{\n"
        << "  \"active_slice_id\": \"" << json_escape(active_slice_id_) << "\",\n"
        << "  \"session_id\": \"" << json_escape(cfg_.session_id) << "\",\n"
        << "  \"opened_at_ns\": " << slice_start_ts_ns_ << ",\n"
        << "  \"last_video_ts_ns\": " << last_video_ts_ns_ << ",\n"
        << "  \"last_audio_ts_ns\": " << last_audio_ts_ns_ << ",\n"
        << "  \"video_frames_written\": " << slice_video_frames_ << ",\n"
        << "  \"bytes_written\": " << slice_bytes_written_ << ",\n"
        << "  \"rotate_reason\": \"" << json_escape(rotate_reason) << "\"\n"
        << "}\n";
  }

  void maybe_flush_slice_state(const std::string& status, const std::string& reason) {
    const uint64_t now_ns = static_cast<uint64_t>(fallback_now_ns());
    if (last_state_flush_ns_ != 0 && now_ns - last_state_flush_ns_ < kStateFlushIntervalNs) {
      return;
    }
    update_slice_json(status);
    update_writer_state(reason);
    last_state_flush_ns_ = now_ns;
  }

  void write_recycler_state() {
    const auto slices = collect_slices();
    const uint64_t total_bytes = total_bytes_from_slices(slices);
    std::ofstream out(state_dir_ / "recycler_state.json", std::ios::trunc);
    out << "{\n"
        << "  \"total_bytes\": " << total_bytes << ",\n"
        << "  \"retention_bytes_limit\": " << cfg_.retention_bytes_limit << ",\n"
        << "  \"retention_slice_limit\": " << cfg_.retention_slice_limit << ",\n"
        << "  \"min_reserved_slices\": " << cfg_.min_reserved_slices << ",\n"
        << "  \"min_unprocessed_slices\": " << cfg_.min_unprocessed_slices << ",\n"
        << "  \"oldest_slice_id\": \"" << json_escape(slices.empty() ? "" : slices.front().slice_id) << "\",\n"
        << "  \"newest_slice_id\": \"" << json_escape(slices.empty() ? "" : slices.back().slice_id) << "\",\n"
        << "  \"last_recycle_at_ns\": " << last_recycle_at_ns_ << ",\n"
        << "  \"deleted_slices\": " << deleted_slices_ << "\n"
        << "}\n";
  }

  std::vector<SliceInfo> collect_slices() const {
    std::vector<SliceInfo> slices;
    if (!fs::exists(slices_dir_)) return slices;
    for (const auto& entry : fs::directory_iterator(slices_dir_)) {
      if (!entry.is_directory()) continue;
      SliceInfo info;
      info.slice_id = entry.path().filename().string();
      info.dir = entry.path();
      const std::string slice_json = read_text_file(entry.path() / "slice.json");
      info.status = extract_json_string(slice_json, "status");
      info.bytes = total_bytes_under(entry.path());
      info.video_frames = extract_json_u64(slice_json, "video_frames_written", 0);
      slices.push_back(std::move(info));
    }
    std::sort(slices.begin(), slices.end(),
              [](const SliceInfo& a, const SliceInfo& b) { return a.slice_id < b.slice_id; });
    return slices;
  }

  static uint64_t total_bytes_from_slices(const std::vector<SliceInfo>& slices) {
    uint64_t total = 0;
    for (const auto& s : slices) total += s.bytes;
    return total;
  }

  void recycle_if_needed() {
    auto slices = collect_slices();
    if (slices.empty()) return;

    const std::string decoder_json = read_text_file(state_dir_ / "decoder_state.json");
    const std::string decoder_current_slice = extract_json_string(decoder_json, "current_slice_id");
    const uint64_t decoder_lease_expires = extract_json_u64(decoder_json, "lease_expires_at_ns", 0);
    const uint64_t now_ns = static_cast<uint64_t>(fallback_now_ns());

    auto count_unprocessed = [&]() {
      size_t count = 0;
      for (const auto& s : slices) {
        if (s.status != "processed" && s.status != "recyclable") ++count;
      }
      return count;
    };

    auto needs_recycle = [&]() {
      if (cfg_.retention_slice_limit > 0 && slices.size() > cfg_.retention_slice_limit) return true;
      if (cfg_.retention_bytes_limit > 0 && total_bytes_from_slices(slices) > cfg_.retention_bytes_limit) return true;
      return false;
    };

    while (needs_recycle()) {
      if (slices.size() <= cfg_.min_reserved_slices) break;
      bool deleted = false;
      for (size_t i = 0; i < slices.size(); ++i) {
        const auto& s = slices[i];
        if (s.slice_id == active_slice_id_) continue;
        if (s.slice_id == decoder_current_slice && decoder_lease_expires > now_ns) continue;
        if (s.status == "writing" || s.status == "processing" || s.status == "corrupted") continue;
        if (count_unprocessed() <= cfg_.min_unprocessed_slices &&
            s.status != "processed" && s.status != "recyclable") {
          continue;
        }
        fs::remove_all(s.dir);
        slices.erase(slices.begin() + static_cast<long>(i));
        deleted_slices_++;
        last_recycle_at_ns_ = now_ns;
        deleted = true;
        break;
      }
      if (!deleted) break;
    }
  }

  static uint64_t total_bytes_under(const fs::path& root) {
    if (!fs::exists(root)) return 0;
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
      if (entry.is_regular_file()) total += entry.file_size();
    }
    return total;
  }

  static std::string format_sequence(size_t n) {
    std::ostringstream oss;
    oss << std::setw(6) << std::setfill('0') << n;
    return oss.str();
  }

  RingStoreConfig cfg_;
  mtl_sdk::VideoFormat video_format_;
  std::optional<mtl_sdk::AudioFormat> audio_format_;
  fs::path root_dir_;
  fs::path state_dir_;
  fs::path slices_dir_;
  fs::path active_slice_dir_;
  std::ofstream video_frames_;
  std::ofstream index_file_;
  std::ofstream audio_frames_;
  std::ofstream audio_index_file_;
  std::vector<uint8_t> video_buffer_;
  std::vector<uint8_t> index_buffer_;
  std::vector<uint8_t> audio_buffer_;
  std::vector<uint8_t> audio_index_buffer_;
  std::queue<PendingWrite> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_empty_;
  std::condition_variable queue_cv_full_;
  std::thread writer_thread_;
  std::string active_slice_id_;
  std::string first_slice_id_;
  std::string last_slice_id_;
  size_t slice_sequence_ = 0;
  size_t total_video_frames_ = 0;
  size_t slice_video_frames_ = 0;
  size_t slice_audio_frames_ = 0;
  uint64_t slice_bytes_written_ = 0;
  uint64_t file_video_bytes_written_ = 0;
  uint64_t file_audio_bytes_written_ = 0;
  int64_t slice_start_ts_ns_ = 0;
  uint64_t slice_rotate_anchor_wall_ns_ = 0;
  int64_t last_video_ts_ns_ = 0;
  int64_t last_audio_ts_ns_ = 0;
  uint64_t last_recycle_at_ns_ = 0;
  uint64_t last_state_flush_ns_ = 0;
  uint64_t deleted_slices_ = 0;
  bool stopping_ = false;
  bool slice_open_ = false;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<RingSliceStore> RingSliceStore::open(const RingStoreConfig& cfg,
                                                     const mtl_sdk::VideoFormat& video_format,
                                                     const mtl_sdk::AudioFormat* audio_format) {
  return std::unique_ptr<RingSliceStore>(new RingSliceStoreImpl(cfg, video_format, audio_format));
}

}  // namespace frame_store
