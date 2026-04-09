#include "encode_sdk/encode_sdk.hpp"
#include "mtl_sdk/mtl_sdk.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static std::string read_text_file(const fs::path& path) {
  std::ifstream in(path);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string extract_json_string(const std::string& json, const std::string& key) {
  const std::string token = "\"" + key + "\"";
  auto pos = json.find(token);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos);
  if (pos == std::string::npos) return {};
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return {};
  auto end = json.find('"', pos + 1);
  if (end == std::string::npos) return {};
  return json.substr(pos + 1, end - pos - 1);
}

static uint64_t extract_json_u64(const std::string& json, const std::string& key, uint64_t fallback = 0) {
  const std::string token = "\"" + key + "\"";
  auto pos = json.find(token);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  size_t end = pos;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end == pos) return fallback;
  return std::stoull(json.substr(pos, end - pos));
}

static double extract_json_double(const std::string& json, const std::string& key, double fallback = 0.0) {
  const std::string token = "\"" + key + "\"";
  auto pos = json.find(token);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
  return std::atof(json.c_str() + pos);
}

static mtl_sdk::VideoPixFmt parse_pix_fmt(const std::string& s) {
  if (s == "YUV422_10BIT") return mtl_sdk::VideoPixFmt::YUV422_10BIT;
  if (s == "YUV420P10LE") return mtl_sdk::VideoPixFmt::YUV420P10LE;
  if (s == "NV12") return mtl_sdk::VideoPixFmt::NV12;
  if (s == "P010") return mtl_sdk::VideoPixFmt::P010;
  return mtl_sdk::VideoPixFmt::YUV422_10BIT;
}

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " --input-yuv <file.yuv> --meta <file.json> [options] output.mp4\n"
            << "  --input-yuv <file>      Raw yuv file written by st2110_receive --yuv-out\n"
            << "  --meta <file>           Meta json written alongside yuv (default: <input>.json)\n"
            << "  --output <file>         Output file path (or use positional output.mp4)\n"
            << "  --vcodec h264|h265      Video codec (default: h264)\n"
            << "  --container mp4|mxf     Output container (default: mp4)\n"
            << "  --progress              Show encode progress\n";
}

int main(int argc, char** argv) {
  std::string input_yuv;
  std::string meta_path;
  std::string output = "encoded.mp4";
  std::string vcodec = "h264";
  std::string container = "mp4";
  bool show_progress = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--input-yuv" && i + 1 < argc) { input_yuv = argv[++i]; continue; }
    if (a == "--meta" && i + 1 < argc) { meta_path = argv[++i]; continue; }
    if (a == "--output" && i + 1 < argc) { output = argv[++i]; continue; }
    if (a == "--vcodec" && i + 1 < argc) { vcodec = argv[++i]; continue; }
    if (a == "--container" && i + 1 < argc) { container = argv[++i]; continue; }
    if (a == "--progress") { show_progress = true; continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    if (a.compare(0, 2, "--") != 0) { output = a; continue; }
  }

  if (input_yuv.empty()) {
    usage(argv[0]);
    return 1;
  }
  if (meta_path.empty()) meta_path = input_yuv + ".json";

  const std::string meta_json = read_text_file(meta_path);
  if (meta_json.empty()) {
    std::cerr << "Failed to read meta json: " << meta_path << "\n";
    return 1;
  }

  const int width = (int)extract_json_u64(meta_json, "width", 1920);
  const int height = (int)extract_json_u64(meta_json, "height", 1080);
  const double fps = extract_json_double(meta_json, "fps", 59.94);
  const std::string pix_fmt_s = extract_json_string(meta_json, "pix_fmt");
  const int linesize_y = (int)extract_json_u64(meta_json, "linesize_y", 0);
  const int linesize_uv = (int)extract_json_u64(meta_json, "linesize_uv", 0);
  const uint64_t size_y = extract_json_u64(meta_json, "size_y", 0);
  const uint64_t size_u = extract_json_u64(meta_json, "size_u", 0);
  const uint64_t size_v = extract_json_u64(meta_json, "size_v", 0);
  const uint64_t bytes_per_frame = extract_json_u64(meta_json, "bytes_per_frame", 0);
  const uint64_t frames_written = extract_json_u64(meta_json, "frames_written", 0);

  if (bytes_per_frame == 0 || size_y == 0 || size_u == 0 || size_v == 0) {
    std::cerr << "Meta json missing plane sizes/bytes_per_frame\n";
    return 1;
  }

  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = parse_pix_fmt(pix_fmt_s.empty() ? "YUV422_10BIT" : pix_fmt_s);

  encode_sdk::EncodeParams ep;
  ep.mux.container = (container == "mxf") ? encode_sdk::Container::MXF : encode_sdk::Container::MP4;
  ep.mux.output_path = output;
  ep.video.codec = (vcodec == "h265" || vcodec == "hevc") ? encode_sdk::VideoCodec::H265 : encode_sdk::VideoCodec::H264;
  ep.video.hw = encode_sdk::HwAccel::Auto;
  ep.video.bitrate_kbps = 2000;
  ep.video.gop = 120;
  ep.video.profile = "main";
  ep.video.fps_num = static_cast<int>(vf.fps + 0.5);
  ep.video.fps_den = 1;
  ep.video.input_fmt = vf.pix_fmt;
  ep.audio = std::nullopt;

  auto enc = encode_sdk::Session::open(ep);
  if (!enc) {
    std::cerr << "Failed to open encoder\n";
    return 1;
  }

  std::ifstream in(input_yuv, std::ios::binary);
  if (!in) {
    std::cerr << "Failed to open yuv file: " << input_yuv << "\n";
    return 1;
  }

  std::vector<uint8_t> buf(bytes_per_frame);
  std::atomic<uint64_t> done{0};
  std::atomic<bool> progress_done{false};
  std::thread progress_thread;
  if (show_progress) {
    progress_thread = std::thread([&]() {
      while (!progress_done.load()) {
        const auto d = done.load();
        const double pct = frames_written > 0 ? std::min(100.0, d * 100.0 / frames_written) : 0.0;
        std::cerr << "\rprogress encode=" << d << "/" << frames_written
                  << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  const double frame_ns_d = fps > 0.0 ? (1e9 / fps) : (1e9 / 59.94);
  for (uint64_t i = 0; (frames_written == 0) ? (in.good()) : (i < frames_written); ++i) {
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!in) break;

    mtl_sdk::VideoFrame frame{};
    frame.fmt = vf;
    frame.mem_type = mtl_sdk::MemoryType::HostPtr;
    frame.num_planes = 3;
    frame.timestamp_ns = static_cast<int64_t>(std::llround(i * frame_ns_d));
    frame.planes[0].data = buf.data();
    frame.planes[0].linesize = linesize_y;
    frame.planes[1].data = buf.data() + size_y;
    frame.planes[1].linesize = linesize_uv;
    frame.planes[2].data = buf.data() + size_y + size_u;
    frame.planes[2].linesize = linesize_uv;
    frame.bytes_total = buf.size();

    enc->push_video(frame);
    done.fetch_add(1);
  }

  enc->close();
  progress_done.store(true);
  if (progress_thread.joinable()) {
    progress_thread.join();
    std::cerr << "\n";
  }
  std::cout << "Wrote " << output << " from " << done.load() << " yuv frames\n";
  return 0;
}

