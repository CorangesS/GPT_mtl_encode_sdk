#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"
#include "frame_store/yuv_file_writer.hpp"
#include "frame_transport/frame_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

static std::string connection_state_path() {
  const char* p = std::getenv("CONNECTION_STATE_FILE");
  return p ? p : "connection_state.json";
}

// Minimal JSON value extraction (no external lib)
static std::string extract_string(const std::string& json, const std::string& key) {
  std::string qkey = "\"" + key + "\"";
  auto pos = json.find(qkey);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) return "";
  auto end = json.find('"', pos + 1);
  if (end == std::string::npos) return "";
  return json.substr(pos + 1, end - pos - 1);
}

static int extract_int(const std::string& json, const std::string& key, int default_val) {
  std::string qkey = "\"" + key + "\"";
  auto pos = json.find(qkey);
  if (pos == std::string::npos) return default_val;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return default_val;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  if (pos >= json.size()) return default_val;
  return std::atoi(json.c_str() + pos);
}

static double extract_double(const std::string& json, const std::string& key, double default_val) {
  std::string qkey = "\"" + key + "\"";
  auto pos = json.find(qkey);
  if (pos == std::string::npos) return default_val;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return default_val;
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  if (pos >= json.size()) return default_val;
  return std::atof(json.c_str() + pos);
}

static std::string video_block(const std::string& json) {
  auto pos = json.find("\"video\"");
  if (pos == std::string::npos) return json;
  pos = json.find('{', pos);
  if (pos == std::string::npos) return json;
  int depth = 1;
  size_t start = pos++;
  while (pos < json.size() && depth > 0) {
    if (json[pos] == '{')
      depth++;
    else if (json[pos] == '}')
      depth--;
    pos++;
  }
  return json.substr(start, pos - start);
}

struct ConnectionState {
  bool master_enable = false;
  std::string video_ip;
  uint16_t video_port = 5004;
  uint8_t video_pt = 96;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  bool valid = false;
};

static ConnectionState read_connection_state(const std::string& path) {
  ConnectionState s;
  std::ifstream f(path);
  if (!f) return s;
  std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  if (json.find("\"master_enable\"") == std::string::npos) return s;
  s.master_enable = (json.find("\"master_enable\": true") != std::string::npos);
  if (!s.master_enable) return s;
  std::string vb = video_block(json);
  if (vb.empty()) return s;
  s.video_ip = extract_string(vb, "ip");
  if (s.video_ip.empty()) return s;
  s.video_port = (uint16_t)extract_int(vb, "udp_port", 5004);
  s.video_pt = (uint8_t)extract_int(vb, "payload_type", 96);
  s.width = extract_int(vb, "width", 1920);
  s.height = extract_int(vb, "height", 1080);
  s.fps = extract_double(vb, "fps", 59.94);
  s.valid = true;
  return s;
}

struct EncodeWorker {
  frame_transport::BoundedFrameQueue queue{64};
  std::thread thread;

  void start(encode_sdk::Session& enc) {
    thread = std::thread([this, &enc]() {
      frame_transport::FramePacket packet;
      while (queue.pop(packet)) {
        mtl_sdk::VideoFrame frame;
        packet.to_video_frame(frame);
        enc.push_video(frame);
      }
    });
  }

  void stop() {
    queue.close();
    if (thread.joinable()) thread.join();
  }
};

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

static encode_sdk::VideoCodec parse_video_codec(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "h265" || v == "hevc") return encode_sdk::VideoCodec::H265;
  return encode_sdk::VideoCodec::H264;
}

static encode_sdk::Container parse_container(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "mxf") return encode_sdk::Container::MXF;
  return encode_sdk::Container::MP4;
}

static encode_sdk::HwAccel parse_hw(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "sw" || v == "software") return encode_sdk::HwAccel::Software;
  return encode_sdk::HwAccel::Auto;
}

int main(int argc, char** argv) {
  std::string state_path = connection_state_path();
  std::string port_name = "kernel:lo";
  std::string sip = "127.0.0.1";
  bool enable_builtin_ptp = false;
  bool print_stats = true;
  std::string yuv_out_path;
  std::string meta_out_path;
  std::string encode_out_path;
  std::string decode_arm_path;
  std::string vcodec_str = "h264";
  std::string container_str = "mp4";
  std::string hw_str = "auto";
  int vbitrate_kbps = 2000;
  int gop = 120;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) {
      port_name = argv[++i];
      continue;
    }
    if (a == "--sip" && i + 1 < argc) {
      sip = argv[++i];
      continue;
    }
    if (a == "--state" && i + 1 < argc) {
      state_path = argv[++i];
      continue;
    }
    if (a == "--enable-builtin-ptp") {
      enable_builtin_ptp = true;
      continue;
    }
    if (a == "--quiet") {
      print_stats = false;
      continue;
    }
    if (a == "--yuv-out" && i + 1 < argc) {
      yuv_out_path = argv[++i];
      continue;
    }
    if (a == "--meta-out" && i + 1 < argc) {
      meta_out_path = argv[++i];
      continue;
    }
    if (a == "--encode-out" && i + 1 < argc) {
      encode_out_path = argv[++i];
      continue;
    }
    if (a == "--decode-arm" && i + 1 < argc) {
      decode_arm_path = argv[++i];
      continue;
    }
    if (a == "--vcodec" && i + 1 < argc) {
      vcodec_str = argv[++i];
      continue;
    }
    if (a == "--container" && i + 1 < argc) {
      container_str = argv[++i];
      continue;
    }
    if (a == "--vbitrate" && i + 1 < argc) {
      vbitrate_kbps = std::atoi(argv[++i]);
      continue;
    }
    if (a == "--gop" && i + 1 < argc) {
      gop = std::atoi(argv[++i]);
      continue;
    }
    if (a == "--hw" && i + 1 < argc) {
      hw_str = argv[++i];
      continue;
    }
    if (a == "--help" || a == "-h") {
      std::cerr << "Usage: is05_receiver_only_daemon [--port PORT] [--sip SIP]\n"
                << "                                [--state CONNECTION_STATE_FILE]\n"
                << "                                [--enable-builtin-ptp] [--quiet]\n"
                << "                                [--yuv-out FILE.yuv] [--meta-out FILE.json]\n"
                << "On-demand decode (same process, copies frames; trigger file arms encode):\n"
                << "                                [--encode-out OUT.mp4] [--decode-arm TRIGGER_FILE]\n"
                << "                                [--vcodec h264|h265] [--container mp4|mxf]\n"
                << "                                [--vbitrate KBPS] [--gop N] [--hw auto|sw]\n";
      return 0;
    }
  }

  if (!encode_out_path.empty() && decode_arm_path.empty()) {
    std::cerr << "Error: --encode-out requires --decode-arm <trigger_file>\n";
    return 1;
  }
  if (!decode_arm_path.empty() && encode_out_path.empty()) {
    std::cerr << "Error: --decode-arm requires --encode-out <file>\n";
    return 1;
  }
  if (!yuv_out_path.empty() && !encode_out_path.empty()) {
    std::cerr << "Error: do not combine --yuv-out with --encode-out\n";
    return 1;
  }

  if (!yuv_out_path.empty() && meta_out_path.empty()) {
    meta_out_path = yuv_out_path + ".json";
  }

  const char* env_port = std::getenv("MTL_PORT");
  const char* env_sip = std::getenv("MTL_SIP");
  const char* env_ptp = std::getenv("MTL_BUILTIN_PTP");
  if (env_port) port_name = env_port;
  if (env_sip) sip = env_sip;
  if (env_ptp && std::string(env_ptp) == "1") enable_builtin_ptp = true;

  std::cout << "IS-05 receiver-only daemon: state_file=" << state_path
            << " port=" << port_name << " sip=" << sip
            << " builtin_ptp=" << (enable_builtin_ptp ? "on" : "off") << "\n";
  if (yuv_out_path.empty() && encode_out_path.empty()) {
    std::cout << "Mode: receive-only, no disk write (poll + release only)\n";
  } else if (!yuv_out_path.empty()) {
    std::cout << "Mode: receive + write YUV to " << yuv_out_path << "\n";
  } else {
    std::cout << "Mode: receive + optional on-demand encode -> " << encode_out_path
              << " when trigger exists: " << decode_arm_path << "\n";
  }

  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({port_name, sip});
  mtl_cfg.rx_queues = 1;
  mtl_cfg.enable_builtin_ptp = enable_builtin_ptp;

  auto ctx = mtl_sdk::Context::create(mtl_cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  std::unique_ptr<mtl_sdk::Context::VideoRxSession> video_rx;
  ConnectionState current_state;
  uint64_t total_frames = 0;
  uint64_t total_written = 0;
  auto last_stat = std::chrono::steady_clock::now();
  uint64_t last_stat_frames = 0;
  frame_store::YuvFileWriter yuv_writer;
  bool writer_opened = false;
  std::unique_ptr<encode_sdk::Session> enc;
  std::unique_ptr<EncodeWorker> encode_worker;
  frame_transport::ReceiverToSinkAdapter rx_adapter;

  while (true) {
    ConnectionState state = read_connection_state(state_path);
    bool state_changed =
        (state.valid != current_state.valid) ||
        (state.valid && (state.video_ip != current_state.video_ip || state.video_port != current_state.video_port ||
                         state.video_pt != current_state.video_pt || state.width != current_state.width ||
                         state.height != current_state.height || state.fps != current_state.fps));

    if (state_changed && state.valid) {
      if (encode_worker) {
        encode_worker->stop();
        encode_worker.reset();
      }
      if (enc) {
        enc->close();
        enc.reset();
      }
      video_rx.reset();
      mtl_sdk::VideoFormat vf;
      vf.width = state.width;
      vf.height = state.height;
      vf.fps = state.fps;
      vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
      mtl_sdk::St2110Endpoint ep{state.video_ip, state.video_port, state.video_pt};
      video_rx = ctx->create_video_rx(vf, ep);
      if (!video_rx) {
        std::cerr << "create_video_rx failed for " << state.video_ip << ":" << state.video_port << "\n";
        current_state.valid = false;
      } else {
        current_state = state;
        std::cout << "Activated RX: " << state.video_ip << ":" << state.video_port << " pt=" << (int)state.video_pt
                  << " " << state.width << "x" << state.height << " @" << state.fps << "\n";
        if (!yuv_out_path.empty() && !writer_opened) {
          mtl_sdk::VideoFormat vf;
          vf.width = state.width;
          vf.height = state.height;
          vf.fps = state.fps;
          vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
          if (!yuv_writer.open(yuv_out_path, meta_out_path, vf, "is05_receiver_only")) {
            std::cerr << "Failed to open yuv output: " << yuv_out_path << "\n";
          } else {
            writer_opened = true;
            std::cout << "YUV output enabled: " << yuv_out_path << " (meta: " << meta_out_path << ")\n";
          }
        }
      }
    } else if (state_changed && !state.valid) {
      if (encode_worker) {
        encode_worker->stop();
        encode_worker.reset();
      }
      if (enc) {
        enc->close();
        enc.reset();
      }
      video_rx.reset();
      current_state.valid = false;
      std::cout << "Connection disabled, RX stopped\n";
    }

    const bool arm_decode = !decode_arm_path.empty() && std::filesystem::exists(decode_arm_path);

    if (video_rx && arm_decode && !encode_out_path.empty() && !enc) {
      encode_sdk::EncodeParams ep;
      ep.mux.container = parse_container(container_str);
      ep.mux.output_path = encode_out_path;
      ep.video.codec = parse_video_codec(vcodec_str);
      ep.video.hw = parse_hw(hw_str);
      ep.video.bitrate_kbps = vbitrate_kbps;
      ep.video.gop = gop;
      ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
      ep.video.fps_num = (int)(current_state.fps + 0.5);
      ep.video.fps_den = 1;
      ep.audio = std::nullopt;
      enc = encode_sdk::Session::open(ep);
      if (!enc) {
        std::cerr << "On-demand encode: Session::open failed for " << encode_out_path << "\n";
      } else {
        encode_worker = std::make_unique<EncodeWorker>();
        encode_worker->start(*enc);
        std::cout << "On-demand decode started -> " << encode_out_path << " (delete or rename trigger file to stop: "
                  << decode_arm_path << ")\n";
      }
    }

    if (video_rx && !arm_decode && enc && encode_worker) {
      encode_worker->stop();
      encode_worker.reset();
      enc->close();
      enc.reset();
      std::cout << "On-demand decode stopped, closed encoder output\n";
    }

    if (video_rx && enc && encode_worker && arm_decode) {
      auto result = rx_adapter.pump_once(*video_rx, encode_worker->queue, 10);
      if (result == frame_transport::PumpResult::Forwarded || result == frame_transport::PumpResult::InvalidFrame) {
        total_frames++;
      }
      if (result == frame_transport::PumpResult::InvalidFrame) {
        std::cerr << "is05_receiver_only_daemon: skipped invalid frame\n";
      }
    } else if (video_rx) {
      mtl_sdk::VideoFrame frame;
      if (video_rx->poll(frame, 10)) {
        total_frames++;
        if (writer_opened) {
          auto packet = frame_transport::FramePacket::from(frame);
          if (packet.is_valid() && yuv_writer.write(packet)) {
            total_written++;
          }
        }
        video_rx->release(frame);
      }
    }

    if (print_stats) {
      auto now = std::chrono::steady_clock::now();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stat).count();
      if (ms >= 1000) {
        uint64_t delta = total_frames - last_stat_frames;
        double fps = (ms > 0) ? (1000.0 * (double)delta / (double)ms) : 0.0;
        std::cout << "RX stats: total_frames=" << total_frames << " ingest_fps=" << fps;
        if (writer_opened) std::cout << " yuv_written=" << total_written;
        if (enc) std::cout << " encode_active=1";
        std::cout << "\n";
        last_stat = now;
        last_stat_frames = total_frames;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  yuv_writer.close();
  ctx->stop();
  return 0;
}
