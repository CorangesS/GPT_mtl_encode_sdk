#include "encode_sdk/encode_sdk.hpp"
#include "mtl_sdk/mtl_sdk.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

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

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " --store-root <dir> [options] output.mp4\n"
            << "  --store-root <dir>      Root ring_store directory\n"
            << "  --channel-id <id>       Channel id under store root (default: channel_main)\n"
            << "  --session-id <id>       Decode only this session id\n"
            << "  --latest-session        Decode only latest session (default when --session-id omitted)\n"
            << "  --output <file>         Output file path (or use positional output.mp4)\n"
            << "  --vcodec h264|h265      Video codec (default: h264)\n"
            << "  --container mp4|mxf     Output container (default: mp4)\n"
            << "  --progress              Show decode progress\n";
}

int main(int argc, char** argv) {
  std::string store_root;
  std::string channel_id = "channel_main";
  std::string session_id;
  std::string output = "decoded.mp4";
  std::string vcodec = "h264";
  std::string container = "mp4";
  bool show_progress = false;
  bool latest_session = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--store-root" && i + 1 < argc) { store_root = argv[++i]; continue; }
    if (a == "--channel-id" && i + 1 < argc) { channel_id = argv[++i]; continue; }
    if (a == "--session-id" && i + 1 < argc) { session_id = argv[++i]; continue; }
    if (a == "--latest-session") { latest_session = true; continue; }
    if (a == "--output" && i + 1 < argc) { output = argv[++i]; continue; }
    if (a == "--vcodec" && i + 1 < argc) { vcodec = argv[++i]; continue; }
    if (a == "--container" && i + 1 < argc) { container = argv[++i]; continue; }
    if (a == "--progress") { show_progress = true; continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    if (a.compare(0, 2, "--") != 0) { output = a; continue; }
  }
  if (store_root.empty()) {
    usage(argv[0]);
    return 1;
  }

  const fs::path root = fs::path(store_root) / channel_id;
  const std::string manifest = read_text_file(root / "manifest.json");
  if (manifest.empty()) {
    std::cerr << "Failed to read manifest.json under " << root << "\n";
    return 1;
  }

  mtl_sdk::VideoFormat vf;
  vf.width = static_cast<int>(extract_json_u64(manifest, "width", 1920));
  vf.height = static_cast<int>(extract_json_u64(manifest, "height", 1080));
  vf.fps = extract_json_double(manifest, "fps", 59.94);
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

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
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
  ep.audio = std::nullopt;

  auto enc = encode_sdk::Session::open(ep);
  if (!enc) {
    std::cerr << "Failed to open encoder\n";
    return 1;
  }

  std::vector<fs::path> slices;
  std::map<std::string, uint64_t> session_opened_max_ns;
  std::string selected_session;
  const fs::path slices_root = root / "slices";
  if (fs::exists(slices_root)) {
    for (const auto& entry : fs::directory_iterator(slices_root)) {
      if (!entry.is_directory()) continue;
      const std::string slice_json = read_text_file(entry.path() / "slice.json");
      const std::string status = extract_json_string(slice_json, "status");
      if (!(status == "sealed" || status == "processed" || status == "recyclable")) continue;
      const std::string sid = extract_json_string(slice_json, "session_id");
      const uint64_t opened_at_ns = extract_json_u64(slice_json, "opened_at_ns", 0);
      const std::string key = sid.empty() ? "legacy" : sid;
      auto it = session_opened_max_ns.find(key);
      if (it == session_opened_max_ns.end() || opened_at_ns > it->second) {
        session_opened_max_ns[key] = opened_at_ns;
      }
      slices.push_back(entry.path());
    }
  }
  if (!session_id.empty()) {
    selected_session = session_id;
  } else if (latest_session || session_opened_max_ns.size() > 1) {
    uint64_t max_opened = 0;
    for (const auto& kv : session_opened_max_ns) {
      if (selected_session.empty() || kv.second >= max_opened) {
        selected_session = kv.first;
        max_opened = kv.second;
      }
    }
  }

  std::vector<fs::path> filtered_slices;
  filtered_slices.reserve(slices.size());
  for (const auto& s : slices) {
    const std::string slice_json = read_text_file(s / "slice.json");
    const std::string sid = extract_json_string(slice_json, "session_id");
    const std::string key = sid.empty() ? "legacy" : sid;
    if (!selected_session.empty() && key != selected_session) continue;
    filtered_slices.push_back(s);
  }
  slices.swap(filtered_slices);
  std::sort(slices.begin(), slices.end());

  uint64_t total_frames = 0;
  for (const auto& slice : slices) {
    const std::string slice_json = read_text_file(slice / "slice.json");
    total_frames += extract_json_u64(slice_json, "video_frames_written", 0);
  }

  std::atomic<uint64_t> decoded_frames{0};
  std::atomic<bool> progress_done{false};
  std::thread progress_thread;
  if (show_progress) {
    progress_thread = std::thread([&]() {
      while (!progress_done.load()) {
        const auto done = decoded_frames.load();
        const double pct = total_frames > 0 ? std::min(100.0, done * 100.0 / total_frames) : 0.0;
        std::cerr << "\rprogress decode=" << done << "/" << total_frames
                  << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  for (const auto& slice : slices) {
    std::ifstream video_in(slice / "video.frames", std::ios::binary);
    std::ifstream index_in(slice / "index.bin", std::ios::binary);
    if (!video_in || !index_in) continue;

    IndexEntry entry{};
    while (index_in.read(reinterpret_cast<char*>(&entry), sizeof(entry))) {
      mtl_sdk::VideoFrame frame{};
      frame.fmt = vf;
      frame.fmt.width = static_cast<int>(entry.width);
      frame.fmt.height = static_cast<int>(entry.height);
      frame.timestamp_ns = static_cast<int64_t>(entry.timestamp_ns);
      frame.mem_type = mtl_sdk::MemoryType::HostPtr;
      frame.num_planes = 3;
      frame.planes[0].linesize = static_cast<int>(entry.linesize_y);
      frame.planes[1].linesize = static_cast<int>(entry.linesize_uv);
      frame.planes[2].linesize = static_cast<int>(entry.linesize_uv);

      std::vector<uint8_t> y(entry.size_y), u(entry.size_u), v(entry.size_v);
      video_in.seekg(static_cast<std::streamoff>(entry.offset_y), std::ios::beg);
      video_in.read(reinterpret_cast<char*>(y.data()), static_cast<std::streamsize>(y.size()));
      video_in.seekg(static_cast<std::streamoff>(entry.offset_u), std::ios::beg);
      video_in.read(reinterpret_cast<char*>(u.data()), static_cast<std::streamsize>(u.size()));
      video_in.seekg(static_cast<std::streamoff>(entry.offset_v), std::ios::beg);
      video_in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size()));

      frame.planes[0].data = y.data();
      frame.planes[1].data = u.data();
      frame.planes[2].data = v.data();
      frame.bytes_total = y.size() + u.size() + v.size();

      enc->push_video(frame);
      decoded_frames.fetch_add(1);
    }
  }

  enc->close();
  progress_done.store(true);
  if (progress_thread.joinable()) {
    progress_thread.join();
    std::cerr << "\n";
  }
  if (!selected_session.empty()) {
    std::cout << "Decoded session: " << selected_session << "\n";
  }
  std::cout << "Wrote " << output << " from " << decoded_frames.load() << " stored frames\n";
  return 0;
}
