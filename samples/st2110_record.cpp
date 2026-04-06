// ST2110 receive + encode sample. Subscribe to multicast and write to MP4/MXF.
// For local two-process test: run st2110_send in one terminal, this in another (same --ip/ports).
// For two-machine test: run st2110_send on machine A, this on machine B with same --ip/ports.
// Async encode pipeline: RX thread copies frames and releases immediately; encode thread runs in parallel.

#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"
#include "frame_transport/frame_transport.hpp"
#include "frame_store/ring_slice_store.hpp"

#include <atomic>
#include <iomanip>
#include <iostream>
#include <thread>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options] [output.mp4]\n"
            << "  --port <port_name>      MTL port, e.g. kernel:enp2s0 or kernel:lo (default: kernel:lo)\n"
            << "  --sip <source_ip>       Source IP for the port (default: 127.0.0.1)\n"
            << "  --ip <multicast_ip>     Default: 239.0.0.1\n"
            << "  --video-port <port>     Default: 5004\n"
            << "  --audio-port <port>     Default: 5006 (0 = no audio)\n"
            << "  --width <w>             Default: 1920 (match sender)\n"
            << "  --height <h>            Default: 1080 (match sender)\n"
            << "  --fps <fps>             Default: 59.94 (match sender)\n"
            << "  --max-frames <n>        Stop after N video frames (default: 600)\n"
            << "  --no-ptp                Disable PTP, use synthetic timestamps (fallback when NIC lacks PTP)\n"
            << "  --sdp <file>            Load SDP file to derive IP/ports/format (overrides --ip/--video-port/--audio-port/--width/--height/--fps)\n"
            << "  --lcores <list>         DPDK lcores for MTL, e.g. 0-3 or 2,3,4,5 (faster RX)\n"
            << "  --main-lcore <id>       Main lcore id (default: MTL auto)\n"
            << "  --tasklets <n>          Tasklets per lcore; 0=auto (try 16 if slow)\n"
            << "  --data-quota-mbs <n>    Max data quota MB/s per lcore; 0=auto\n"
            << "  --decode                Enable decode/encode pipeline and write output.mp4\n"
            << "  --progress              Show non-blocking progress for decode/store/receive\n"
            << "  --store-root <dir>      Store received raw frames as ring slices under <dir>\n"
            << "  --channel-id <id>       Channel name for ring slice store (default: channel_main)\n"
            << "  --slice-seconds <n>     Seal slice every N seconds (default: 60 in store mode)\n"
            << "  --slice-max-bytes <n>   Seal slice when bytes reach N (default: 0 disabled)\n"
            << "  --retention-bytes <n>   Recycle oldest sealed slices when total bytes exceed N\n"
            << "  --retention-slices <n>  Recycle oldest slices when slice count exceeds N\n"
            << "  --min-reserved-slices <n> Keep at least N newest slices even during recycle\n"
            << "  --min-unprocessed-slices <n> Keep at least N non-processed slices\n";
}

int main(int argc, char** argv) {
  std::string out = "out.mp4";
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
  bool use_ptp = true;
  std::string lcores;
  int main_lcore = -1;
  uint32_t tasklets_nb_per_sch = 0;
  uint32_t data_quota_mbs_per_sch = 0;
  std::string store_root;
  std::string channel_id = "channel_main";
  uint32_t slice_seconds = 60;
  uint64_t slice_max_bytes = 0;
  uint64_t retention_bytes = 0;
  uint32_t retention_slices = 0;
  uint32_t min_reserved_slices = 2;
  uint32_t min_unprocessed_slices = 0;
  bool enable_decode = false;
  bool show_progress = false;

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
    if (a == "--no-ptp") { use_ptp = false; continue; }
    if (a == "--decode") { enable_decode = true; continue; }
    if (a == "--progress") { show_progress = true; continue; }
    if (a == "--sdp" && i + 1 < argc) { sdp_path = argv[++i]; continue; }
    if (a == "--lcores" && i + 1 < argc) { lcores = argv[++i]; continue; }
    if (a == "--main-lcore" && i + 1 < argc) { main_lcore = atoi(argv[++i]); continue; }
    if (a == "--tasklets" && i + 1 < argc) { tasklets_nb_per_sch = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--data-quota-mbs" && i + 1 < argc) { data_quota_mbs_per_sch = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--store-root" && i + 1 < argc) { store_root = argv[++i]; continue; }
    if (a == "--channel-id" && i + 1 < argc) { channel_id = argv[++i]; continue; }
    if (a == "--slice-seconds" && i + 1 < argc) { slice_seconds = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--slice-max-bytes" && i + 1 < argc) { slice_max_bytes = (uint64_t)std::stoull(argv[++i]); continue; }
    if (a == "--retention-bytes" && i + 1 < argc) { retention_bytes = (uint64_t)std::stoull(argv[++i]); continue; }
    if (a == "--retention-slices" && i + 1 < argc) { retention_slices = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--min-reserved-slices" && i + 1 < argc) { min_reserved_slices = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--min-unprocessed-slices" && i + 1 < argc) { min_unprocessed_slices = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    if (a.compare(0, 2, "--") != 0) { out = a; continue; }
  }

  // If SDP is provided, override IP/ports and basic format from SDP.
  int audio_sample_rate = 48000;
  int audio_channels = 2;
  int audio_bits = 16;
  if (!sdp_path.empty()) {
    try {
      mtl_sdk::SdpSession sdp = mtl_sdk::load_sdp_file(sdp_path);
      bool has_video = false;
      bool has_audio = false;
      for (const auto& m : sdp.media) {
        if (m.type == mtl_sdk::SdpMedia::Type::Video && !has_video) {
          ip = m.endpoint.ip;
          video_port = m.endpoint.udp_port;
          // default values; may be refined from fmtp
          int w = width;
          int h = height;
          double f = fps;
          for (const auto& kv : m.fmtp_kv) {
            if (kv.rfind("width=", 0) == 0) {
              w = std::stoi(kv.substr(6));
            } else if (kv.rfind("height=", 0) == 0) {
              h = std::stoi(kv.substr(7));
            } else if (kv.rfind("exactframerate=", 0) == 0) {
              std::string fr = kv.substr(std::string("exactframerate=").size());
              auto slash = fr.find('/');
              if (slash != std::string::npos) {
                int num = std::stoi(fr.substr(0, slash));
                int den = std::stoi(fr.substr(slash + 1));
                if (den != 0) f = (double)num / (double)den;
              }
            }
          }
          width = w;
          height = h;
          fps = f;
          has_video = true;
        } else if (m.type == mtl_sdk::SdpMedia::Type::Audio && !has_audio) {
          ip = m.endpoint.ip;
          audio_port = m.endpoint.udp_port;
          // parse rtpmap: encoding/sample_rate/channels
          if (!m.rtpmap.empty()) {
            auto first = m.rtpmap.find('/');
            if (first != std::string::npos) {
              auto second = m.rtpmap.find('/', first + 1);
              if (second != std::string::npos) {
                audio_sample_rate = std::stoi(m.rtpmap.substr(first + 1, second - first - 1));
                audio_channels = std::stoi(m.rtpmap.substr(second + 1));
              } else {
                audio_sample_rate = std::stoi(m.rtpmap.substr(first + 1));
              }
            }
          }
          has_audio = true;
        }
      }
      std::cout << "Loaded SDP from " << sdp_path << ", ip=" << ip
                << " video_port=" << video_port
                << " audio_port=" << audio_port << "\n";
    } catch (...) {
      std::cerr << "Failed to load/parse SDP file: " << sdp_path << "\n";
      return 1;
    }
  }

  // ---- MTL SDK ----
  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({port_name, sip});
  mtl_cfg.rx_queues = 1;
  mtl_cfg.enable_builtin_ptp = use_ptp;
  mtl_cfg.lcores = lcores;
  mtl_cfg.main_lcore = main_lcore;
  mtl_cfg.tasklets_nb_per_sch = tasklets_nb_per_sch;
  mtl_cfg.data_quota_mbs_per_sch = data_quota_mbs_per_sch;

  auto ctx = mtl_sdk::Context::create(mtl_cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  if (!use_ptp) {
    std::cout << "PTP disabled, using synthetic timestamps for received frames\n";
  } else if (ctx->now_ptp_ns() == 0) {
    std::cout << "PTP unavailable (NIC/mode may not support it), receiver will use synthetic timestamps\n";
  }

  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  mtl_sdk::St2110Endpoint vep{ ip, video_port, 96 };
  auto v_rx = ctx->create_video_rx(vf, vep);

  std::unique_ptr<mtl_sdk::Context::AudioRxSession> a_rx;
  std::optional<mtl_sdk::AudioFormat> audio_store_format;
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = audio_sample_rate;
    af.channels = audio_channels;
    af.bits_per_sample = audio_bits;
    mtl_sdk::St2110Endpoint aep{ ip, audio_port, 97 };
    a_rx = ctx->create_audio_rx(af, aep);
    if (a_rx) audio_store_format = af;
  }

  const bool store_mode = !store_root.empty();
  const bool decode_mode = enable_decode && !store_mode;
  std::atomic<int> received_frames{0};
  std::atomic<int> output_frames{0};
  std::atomic<bool> progress_done{false};
  std::thread progress_thread;
  std::unique_ptr<frame_store::RingSliceStore> store;
  if (store_mode) {
    frame_store::RingStoreConfig cfg;
    cfg.root_dir = store_root;
    cfg.channel_id = channel_id;
    cfg.slice_duration_sec = slice_seconds;
    cfg.slice_max_bytes = slice_max_bytes;
    cfg.retention_bytes_limit = retention_bytes;
    cfg.retention_slice_limit = retention_slices;
    cfg.min_reserved_slices = min_reserved_slices;
    cfg.min_unprocessed_slices = min_unprocessed_slices;
    store = frame_store::RingSliceStore::open(cfg, vf, audio_store_format ? &*audio_store_format : nullptr);
  }

  // ---- Encoding SDK: Auto hw (NVENC->QSV->CPU), lower load for stability ----
  std::unique_ptr<encode_sdk::Session> enc;
  frame_transport::ReceiverToSinkAdapter rx_adapter;
  frame_transport::BoundedFrameQueue enc_queue(64);
  std::thread enc_thread;
  if (decode_mode) {
    encode_sdk::EncodeParams ep;
    ep.mux.container = encode_sdk::Container::MP4;
    ep.mux.output_path = out;

    ep.video.codec = encode_sdk::VideoCodec::H264;
    ep.video.hw = encode_sdk::HwAccel::Auto;
    ep.video.bitrate_kbps = 2000;
    ep.video.gop = 120;
    ep.video.profile = "main";
    ep.video.fps_num = (int)(fps + 0.5);
    ep.video.fps_den = 1;
    ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

    if (audio_port != 0) {
      ep.audio = encode_sdk::AudioEncodeParams{};
      ep.audio->codec = encode_sdk::AudioCodec::AAC;
      ep.audio->bitrate_kbps = 128;
      ep.audio->sample_rate = 48000;
      ep.audio->channels = 2;
    } else {
      ep.audio = std::nullopt;
    }

    enc = encode_sdk::Session::open(ep);
    if (!enc) {
      std::cerr << "Failed to open encoder\n";
      return 1;
    }

    enc_thread = std::thread([&]() {
      frame_transport::FramePacket packet;
      while (enc_queue.pop(packet)) {
        mtl_sdk::VideoFrame vf_wrap;
        packet.to_video_frame(vf_wrap);
        enc->push_video(vf_wrap);
        output_frames.fetch_add(1);
      }
    });
  }

  if (show_progress) {
    progress_thread = std::thread([&]() {
      while (!progress_done.load()) {
        const int recv = received_frames.load();
        const int out_frames = output_frames.load();
        if (max_frames > 0) {
          const double recv_pct = std::min(100.0, recv * 100.0 / max_frames);
          const double out_pct = std::min(100.0, out_frames * 100.0 / max_frames);
          std::cerr << "\rprogress recv=" << recv << "/" << max_frames
                    << " (" << std::fixed << std::setprecision(1) << recv_pct << "%)"
                    << " out=" << out_frames << "/" << max_frames
                    << " (" << out_pct << "%)" << std::flush;
        } else {
          std::cerr << "\rprogress recv=" << recv << " out=" << out_frames << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  int got = 0;
  while (got < max_frames) {
    if (store_mode) {
      mtl_sdk::VideoFrame vf_out;
      if (v_rx->poll(vf_out, 0)) {
        auto packet = frame_transport::FramePacket::from(vf_out);
        v_rx->release(vf_out);
        if (packet.is_valid() && store->write_video(packet)) {
          got++;
          received_frames.store(got);
          output_frames.store(static_cast<int>(store->video_frames_written()));
        } else {
          std::cerr << "st2110_record: failed to store video frame\n";
        }
      }
    } else if (decode_mode) {
      auto result = rx_adapter.pump_once(*v_rx, enc_queue, 0);
      if (result == frame_transport::PumpResult::Forwarded) {
        got++;
        received_frames.store(got);
      } else if (result == frame_transport::PumpResult::InvalidFrame) {
        std::cerr << "st2110_record: skipped invalid frame\n";
      }
    } else {
      mtl_sdk::VideoFrame vf_out;
      if (v_rx->poll(vf_out, 0)) {
        v_rx->release(vf_out);
        got++;
        received_frames.store(got);
        output_frames.store(got);
      }
    }

    mtl_sdk::AudioFrame af_out;
    if (a_rx && a_rx->poll(af_out, 0)) {
      if (store_mode) {
        store->write_audio(af_out);
      } else if (decode_mode) {
        enc->push_audio(af_out);
      }
    }
  }

  if (store_mode) {
    store->close();
    output_frames.store(static_cast<int>(store->video_frames_written()));
  } else if (decode_mode) {
    enc_queue.close();
    enc_thread.join();
    enc->close();
    output_frames.store(received_frames.load());
  }
  progress_done.store(true);
  if (progress_thread.joinable()) {
    progress_thread.join();
    std::cerr << "\n";
  }
  ctx->stop();

  if (store_mode) {
    std::cout << "Stored " << got << " frames under " << store_root << "/" << channel_id << "\n";
  } else if (decode_mode) {
    std::cout << "Wrote " << out << " (" << got << " frames)\n";
  } else {
    std::cout << "Received " << got << " frames (decode disabled)\n";
  }
  return 0;
}
