#include "frame_store/yuv_file_writer.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace frame_store {
namespace fs = std::filesystem;

static std::string pix_fmt_name(mtl_sdk::VideoPixFmt fmt) {
  switch (fmt) {
    case mtl_sdk::VideoPixFmt::YUV422_10BIT: return "YUV422_10BIT";
    case mtl_sdk::VideoPixFmt::YUV420P10LE: return "YUV420P10LE";
    case mtl_sdk::VideoPixFmt::NV12: return "NV12";
    case mtl_sdk::VideoPixFmt::P010: return "P010";
  }
  return "UNKNOWN";
}

static uint64_t now_wall_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
}

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

YuvFileWriter::~YuvFileWriter() { close(); }

bool YuvFileWriter::open(const std::string& yuv_path,
                         const std::string& meta_path,
                         const mtl_sdk::VideoFormat& fmt,
                         const std::string& session_id) {
  if (opened_) return false;
  if (yuv_path.empty() || meta_path.empty()) return false;

  yuv_path_ = yuv_path;
  meta_path_ = meta_path;

  try {
    fs::path p(yuv_path_);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    fs::path mp(meta_path_);
    if (mp.has_parent_path()) fs::create_directories(mp.parent_path());
  } catch (...) {
    return false;
  }

  yuv_out_.open(yuv_path_, std::ios::binary | std::ios::trunc);
  if (!yuv_out_) return false;

  meta_ = YuvWriteMeta{};
  meta_.width = fmt.width;
  meta_.height = fmt.height;
  meta_.fps = fmt.fps;
  meta_.pix_fmt = pix_fmt_name(fmt.pix_fmt);
  meta_.session_id = session_id;
  meta_.frames_written = 0;

  opened_ = true;
  last_meta_flush_ns_ = 0;
  return write_meta_json(true);
}

bool YuvFileWriter::write(const frame_transport::FramePacket& packet) {
  if (!opened_ || !yuv_out_) return false;
  if (!packet.is_valid()) return false;

  if (meta_.frames_written == 0) {
    meta_.linesize_y = packet.linesize_y;
    meta_.linesize_uv = packet.linesize_uv;
    meta_.size_y = packet.y.size();
    meta_.size_u = packet.u.size();
    meta_.size_v = packet.v.size();
    meta_.bytes_per_frame = meta_.size_y + meta_.size_u + meta_.size_v;
    write_meta_json(true);
  }

  yuv_out_.write(reinterpret_cast<const char*>(packet.y.data()), static_cast<std::streamsize>(packet.y.size()));
  yuv_out_.write(reinterpret_cast<const char*>(packet.u.data()), static_cast<std::streamsize>(packet.u.size()));
  yuv_out_.write(reinterpret_cast<const char*>(packet.v.data()), static_cast<std::streamsize>(packet.v.size()));
  if (!yuv_out_) return false;

  meta_.frames_written++;
  write_meta_json(false);
  return true;
}

void YuvFileWriter::close() {
  if (!opened_) return;
  write_meta_json(true);
  if (yuv_out_.is_open()) yuv_out_.flush();
  if (yuv_out_.is_open()) yuv_out_.close();
  opened_ = false;
}

bool YuvFileWriter::write_meta_json(bool force) {
  static constexpr uint64_t kFlushIntervalNs = 1000000000ULL;
  const uint64_t now = now_wall_ns();
  if (!force && last_meta_flush_ns_ != 0 && (now - last_meta_flush_ns_) < kFlushIntervalNs) return true;

  std::ofstream out(meta_path_, std::ios::trunc);
  if (!out) return false;
  out << "{\n"
      << "  \"format_version\": 1,\n"
      << "  \"session_id\": \"" << json_escape(meta_.session_id) << "\",\n"
      << "  \"pix_fmt\": \"" << json_escape(meta_.pix_fmt) << "\",\n"
      << "  \"width\": " << meta_.width << ",\n"
      << "  \"height\": " << meta_.height << ",\n"
      << "  \"fps\": " << std::fixed << std::setprecision(3) << meta_.fps << ",\n"
      << "  \"num_planes\": 3,\n"
      << "  \"linesize_y\": " << meta_.linesize_y << ",\n"
      << "  \"linesize_uv\": " << meta_.linesize_uv << ",\n"
      << "  \"size_y\": " << meta_.size_y << ",\n"
      << "  \"size_u\": " << meta_.size_u << ",\n"
      << "  \"size_v\": " << meta_.size_v << ",\n"
      << "  \"bytes_per_frame\": " << meta_.bytes_per_frame << ",\n"
      << "  \"frames_written\": " << meta_.frames_written << "\n"
      << "}\n";
  last_meta_flush_ns_ = now;
  return true;
}

}  // namespace frame_store

