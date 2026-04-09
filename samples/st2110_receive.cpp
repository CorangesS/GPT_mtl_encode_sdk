#include "mtl_sdk/mtl_sdk.hpp"
#include "frame_transport/frame_transport.hpp"
#include "frame_store/ring_slice_store.hpp"
#include "frame_store/yuv_file_writer.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options]\n"
            << "  --port <port_name>      MTL port, e.g. kernel:enp2s0 or kernel:lo (default: kernel:lo)\n"
            << "  --sip <source_ip>       Source IP for the port (default: 127.0.0.1)\n"
            << "  --ip <multicast_ip>     Default: 239.0.0.1\n"
            << "  --video-port <port>     Default: 5004\n"
            << "  --audio-port <port>     Default: 5006 (0 = no audio)\n"
            << "  --width <w>             Default: 1920\n"
            << "  --height <h>            Default: 1080\n"
            << "  --fps <fps>             Default: 59.94\n"
            << "  --max-frames <n>        Stop after at most N video frames (default: 600; 0 = no cap)\n"
            << "  --idle-exit-ms <n>      After at least one frame, exit if no new video for this long (default: 2000; 0 = disable)\n"
            << "  --no-ptp                Disable PTP, use synthetic timestamps\n"
            << "  --sdp <file>            Load SDP file to derive IP/ports/format\n"
            << "  --progress              Show receive progress\n"
            << "\n"
            << "  Output mode (choose one):\n"
            << "  --yuv-out <file.yuv>    Write a single raw .yuv file (recommended for offline encode)\n"
            << "  --meta-out <file.json>  Meta json path for --yuv-out (default: <yuv-out>.json)\n"
            << "\n"
            << "  Ring slice store mode:\n"
            << "  --store-root <dir>      Store received raw frames as ring slices under <dir>\n"
            << "  --channel-id <id>       Channel name for ring slice store (default: channel_main)\n"
            << "  --session-id <id>       Session id recorded into each slice/meta (default: auto-generated)\n"
            << "  --slice-seconds <n>     Seal slice every N seconds (default: 60)\n"
            << "  --slice-max-bytes <n>   Seal slice when bytes reach N (default: 0 disabled)\n"
            << "  --retention-bytes <n>   Recycle oldest sealed slices when total bytes exceed N\n"
            << "  --retention-slices <n>  Recycle oldest slices when slice count exceeds N (default: 10)\n"
            << "  --min-reserved-slices <n> Keep at least N newest slices even during recycle\n"
            << "  --min-unprocessed-slices <n> Keep at least N non-processed slices\n";
}

static std::string default_meta_path_for(const std::string& yuv_path) { return yuv_path + ".json"; }

int main(int argc, char** argv) {
  std::string port_name = "kernel:lo";
  std::string sip = "127.0.0.1";
  std::string ip = "239.0.0.1";
  std::string sdp_path;

  uint16_t video_port = 5004;
  uint16_t audio_port = 5006;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  int max_frames = 600;
  int idle_exit_ms = 2000;
  bool use_ptp = true;
  bool show_progress = false;

  std::string yuv_out_path;
  std::string meta_out_path;

  std::string store_root;
  std::string channel_id = "channel_main";
  std::string session_id;
  uint32_t slice_seconds = 60;
  uint64_t slice_max_bytes = 0;
  uint64_t retention_bytes = 0;
  uint32_t retention_slices = 10;
  uint32_t min_reserved_slices = 2;
  uint32_t min_unprocessed_slices = 0;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) { port_name = argv[++i]; continue; }
    if (a == "--sip" && i + 1 < argc) { sip = argv[++i]; continue; }
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--width" && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (a == "--height" && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (a == "--fps" && i + 1 < argc) { fps = atof(argv[++i]); continue; }
    if (a == "--max-frames" && i + 1 < argc) { max_frames = atoi(argv[++i]); continue; }
    if (a == "--idle-exit-ms" && i + 1 < argc) { idle_exit_ms = atoi(argv[++i]); continue; }
    if (a == "--no-ptp") { use_ptp = false; continue; }
    if (a == "--sdp" && i + 1 < argc) { sdp_path = argv[++i]; continue; }
    if (a == "--progress") { show_progress = true; continue; }

    if (a == "--yuv-out" && i + 1 < argc) { yuv_out_path = argv[++i]; continue; }
    if (a == "--meta-out" && i + 1 < argc) { meta_out_path = argv[++i]; continue; }

    if (a == "--store-root" && i + 1 < argc) { store_root = argv[++i]; continue; }
    if (a == "--channel-id" && i + 1 < argc) { channel_id = argv[++i]; continue; }
    if (a == "--session-id" && i + 1 < argc) { session_id = argv[++i]; continue; }
    if (a == "--slice-seconds" && i + 1 < argc) { slice_seconds = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--slice-max-bytes" && i + 1 < argc) { slice_max_bytes = (uint64_t)std::stoull(argv[++i]); continue; }
    if (a == "--retention-bytes" && i + 1 < argc) { retention_bytes = (uint64_t)std::stoull(argv[++i]); continue; }
    if (a == "--retention-slices" && i + 1 < argc) { retention_slices = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--min-reserved-slices" && i + 1 < argc) { min_reserved_slices = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--min-unprocessed-slices" && i + 1 < argc) { min_unprocessed_slices = (uint32_t)atoi(argv[++i]); continue; }

    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
  }

  const bool yuv_mode = !yuv_out_path.empty();
  if (!yuv_mode && store_root.empty()) {
    std::cerr << "Error: choose an output mode: either --yuv-out <file.yuv> or --store-root <dir>\n";
    usage(argv[0]);
    return 1;
  }
  if (yuv_mode && meta_out_path.empty()) meta_out_path = default_meta_path_for(yuv_out_path);

  if (max_frames == 0 && idle_exit_ms == 0) {
    std::cerr << "Warning: --max-frames 0 and --idle-exit-ms 0 will run until killed.\n";
  }
  if (session_id.empty()) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    std::ostringstream oss;
    oss << "sess_" << now_ns;
    session_id = oss.str();
  }

  if (!sdp_path.empty()) {
    try {
      mtl_sdk::SdpSession sdp = mtl_sdk::load_sdp_file(sdp_path);
      for (const auto& m : sdp.media) {
        if (m.type == mtl_sdk::SdpMedia::Type::Video) {
          ip = m.endpoint.ip;
          video_port = m.endpoint.udp_port;
        } else if (m.type == mtl_sdk::SdpMedia::Type::Audio) {
          ip = m.endpoint.ip;
          audio_port = m.endpoint.udp_port;
        }
      }
    } catch (...) {
      std::cerr << "Failed to load/parse SDP file: " << sdp_path << "\n";
      return 1;
    }
  }

  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({port_name, sip});
  mtl_cfg.rx_queues = 1;
  mtl_cfg.enable_builtin_ptp = use_ptp;

  auto ctx = mtl_sdk::Context::create(mtl_cfg);
  if (!ctx || ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  auto v_rx = ctx->create_video_rx(vf, {ip, video_port, 96});
  if (!v_rx) {
    std::cerr << "Failed to create video RX\n";
    return 1;
  }

  std::unique_ptr<mtl_sdk::Context::AudioRxSession> a_rx;
  std::optional<mtl_sdk::AudioFormat> audio_format;
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = 48000;
    af.channels = 2;
    af.bits_per_sample = 16;
    a_rx = ctx->create_audio_rx(af, {ip, audio_port, 97});
    if (a_rx) audio_format = af;
  }

  std::unique_ptr<frame_store::RingSliceStore> store;
  frame_store::YuvFileWriter yuv_writer;
  if (yuv_mode) {
    if (!yuv_writer.open(yuv_out_path, meta_out_path, vf, session_id)) {
      std::cerr << "Failed to open yuv output: " << yuv_out_path << "\n";
      return 1;
    }
  } else {
    frame_store::RingStoreConfig cfg;
    cfg.root_dir = store_root;
    cfg.channel_id = channel_id;
    cfg.session_id = session_id;
    cfg.slice_duration_sec = slice_seconds;
    cfg.slice_max_bytes = slice_max_bytes;
    cfg.retention_bytes_limit = retention_bytes;
    cfg.retention_slice_limit = retention_slices;
    cfg.min_reserved_slices = min_reserved_slices;
    cfg.min_unprocessed_slices = min_unprocessed_slices;
    store = frame_store::RingSliceStore::open(cfg, vf, audio_format ? &*audio_format : nullptr);
  }

  std::atomic<int> received_frames{0};
  std::atomic<bool> progress_done{false};
  std::thread progress_thread;
  if (show_progress) {
    progress_thread = std::thread([&]() {
      while (!progress_done.load()) {
        const int recv = received_frames.load();
        if (max_frames > 0) {
          const double pct = std::min(100.0, recv * 100.0 / max_frames);
          std::cerr << "\rprogress recv=" << recv << "/" << max_frames
                    << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::flush;
        } else {
          std::cerr << "\rprogress recv=" << recv << " (no max-frames cap)" << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  int got = 0;
  bool stopped_idle = false;
  bool had_good_video = false;
  auto last_good_video = std::chrono::steady_clock::time_point{};
  while (max_frames == 0 || got < max_frames) {
    mtl_sdk::VideoFrame vf_out;
    if (v_rx->poll(vf_out, 0)) {
      auto packet = frame_transport::FramePacket::from(vf_out);
      v_rx->release(vf_out);
      bool ok = false;
      if (packet.is_valid()) {
        if (yuv_mode) {
          ok = yuv_writer.write(packet);
        } else if (store) {
          ok = store->write_video(packet);
        }
      }
      if (ok) {
        got++;
        received_frames.store(got);
        had_good_video = true;
        last_good_video = std::chrono::steady_clock::now();
      }
    } else {
      if (idle_exit_ms > 0 && had_good_video) {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_good_video).count() >= idle_exit_ms) {
          stopped_idle = true;
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    mtl_sdk::AudioFrame af_out;
    if (a_rx && a_rx->poll(af_out, 0)) {
      if (!yuv_mode && store) {
        store->write_audio(af_out);
      }
    }
  }

  if (store) store->close();
  yuv_writer.close();

  progress_done.store(true);
  if (progress_thread.joinable()) {
    progress_thread.join();
    std::cerr << "\n";
  }
  ctx->stop();

  std::cout << "Stored " << got << " frames";
  if (yuv_mode) {
    std::cout << " to " << yuv_out_path << " (meta: " << meta_out_path << ")";
  } else {
    std::cout << " under " << store_root << "/" << channel_id;
  }
  std::cout << " (session: " << session_id << ")";
  if (stopped_idle) {
    std::cout << " (stopped: no video for " << idle_exit_ms << " ms)\n";
  } else if (max_frames > 0 && got >= max_frames) {
    std::cout << " (stopped: max-frames " << max_frames << ")\n";
  } else {
    std::cout << "\n";
  }
  return 0;
}

