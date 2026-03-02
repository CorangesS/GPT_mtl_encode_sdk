// IS-05 receiver daemon: polls connection_state.json (written by routing/is05_server),
// creates/updates MTL video_rx/audio_rx and encodes to MP4（含可选音频轨）.

#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>

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
  pos++; while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  if (pos >= json.size()) return default_val;
  return std::atoi(json.c_str() + pos);
}

static double extract_double(const std::string& json, const std::string& key, double default_val) {
  std::string qkey = "\"" + key + "\"";
  auto pos = json.find(qkey);
  if (pos == std::string::npos) return default_val;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return default_val;
  pos++; while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
  if (pos >= json.size()) return default_val;
  return std::atof(json.c_str() + pos);
}

// Find "video": { ... } sub-object and extract from that region
static std::string video_block(const std::string& json) {
  auto pos = json.find("\"video\"");
  if (pos == std::string::npos) return json;
  pos = json.find('{', pos);
  if (pos == std::string::npos) return json;
  int depth = 1;
  size_t start = pos++;
  while (pos < json.size() && depth > 0) {
    if (json[pos] == '{') depth++;
    else if (json[pos] == '}') depth--;
    pos++;
  }
  return json.substr(start, pos - start);
}

// Find "audio": { ... } sub-object
static std::string audio_block(const std::string& json) {
  auto pos = json.find("\"audio\"");
  if (pos == std::string::npos) return std::string();
  pos = json.find('{', pos);
  if (pos == std::string::npos) return std::string();
  int depth = 1;
  size_t start = pos++;
  while (pos < json.size() && depth > 0) {
    if (json[pos] == '{') depth++;
    else if (json[pos] == '}') depth--;
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
  std::string audio_ip;
  uint16_t audio_port = 0;
  uint8_t audio_pt = 97;
  int audio_sample_rate = 48000;
  int audio_channels = 2;
  bool has_audio = false;
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
  // optional audio block
  std::string ab = audio_block(json);
  if (!ab.empty()) {
    s.audio_ip = extract_string(ab, "ip");
    if (!s.audio_ip.empty()) {
      s.audio_port = (uint16_t)extract_int(ab, "udp_port", 0);
      s.audio_pt = (uint8_t)extract_int(ab, "payload_type", 97);
      s.audio_sample_rate = extract_int(ab, "sample_rate", 48000);
      s.audio_channels = extract_int(ab, "channels", 2);
      if (s.audio_port != 0) s.has_audio = true;
    }
  }
  s.valid = true;
  return s;
}

int main(int argc, char** argv) {
  std::string state_path = connection_state_path();
  std::string port_name = "kernel:lo";
  std::string sip = "127.0.0.1";
  std::string out_path = "is05_output.mp4";

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) { port_name = argv[++i]; continue; }
    if (a == "--sip" && i + 1 < argc) { sip = argv[++i]; continue; }
    if (a == "--output" && i + 1 < argc) { out_path = argv[++i]; continue; }
    if (a == "--state" && i + 1 < argc) { state_path = argv[++i]; continue; }
    if (a == "--help" || a == "-h") {
      std::cerr << "Usage: is05_receiver_daemon [--port PORT] [--sip SIP] [--output MP4] [--state CONNECTION_STATE_FILE]\n";
      return 0;
    }
  }

  const char* env_port = std::getenv("MTL_PORT");
  const char* env_sip = std::getenv("MTL_SIP");
  if (env_port) port_name = env_port;
  if (env_sip) sip = env_sip;

  std::cout << "IS-05 receiver daemon: state_file=" << state_path << " out=" << out_path << "\n";

  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({port_name, sip});
  mtl_cfg.rx_queues = 1;
  mtl_cfg.enable_builtin_ptp = false;

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
  std::unique_ptr<mtl_sdk::Context::AudioRxSession> audio_rx;
  std::unique_ptr<encode_sdk::Session> enc;
  std::mutex session_mutex;
  ConnectionState current_state;
  int max_frames = 0;
  const int run_frames = 600;

  while (true) {
    ConnectionState state = read_connection_state(state_path);
    bool state_changed = (state.valid != current_state.valid ||
                         (state.valid && (state.video_ip != current_state.video_ip ||
                                          state.video_port != current_state.video_port)));

    if (state_changed && state.valid) {
      std::lock_guard<std::mutex> lock(session_mutex);
      video_rx.reset();
      audio_rx.reset();
      enc.reset();

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
        encode_sdk::EncodeParams ep;
        ep.mux.container = encode_sdk::Container::MP4;
        ep.mux.output_path = out_path;
        ep.video.codec = encode_sdk::VideoCodec::H264;
        ep.video.hw = encode_sdk::HwAccel::Auto;
        ep.video.bitrate_kbps = 2000;
        ep.video.gop = 120;
        ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
        ep.video.fps_num = (int)(state.fps + 0.5);
        ep.video.fps_den = 1;
        if (state.has_audio) {
          mtl_sdk::AudioFormat af;
          af.sample_rate = state.audio_sample_rate;
          af.channels = state.audio_channels;
          af.bits_per_sample = 16;
          mtl_sdk::St2110Endpoint aep{state.audio_ip, state.audio_port, state.audio_pt};
          audio_rx = ctx->create_audio_rx(af, aep);
          if (audio_rx) {
            ep.audio = encode_sdk::AudioEncodeParams{};
            ep.audio->codec = encode_sdk::AudioCodec::AAC;
            ep.audio->bitrate_kbps = 128;
            ep.audio->sample_rate = state.audio_sample_rate;
            ep.audio->channels = state.audio_channels;
          } else {
            std::cerr << "create_audio_rx failed for " << state.audio_ip << ":" << state.audio_port << "\n";
            ep.audio = std::nullopt;
          }
        } else {
          ep.audio = std::nullopt;
        }
        enc = encode_sdk::Session::open(ep);
        if (!enc) {
          std::cerr << "Encoder open failed\n";
          video_rx.reset();
          audio_rx.reset();
        } else {
          current_state = state;
          max_frames = run_frames;
          std::cout << "Activated: " << state.video_ip << ":" << state.video_port
                    << " " << state.width << "x" << state.height << " @" << state.fps << "\n";
        }
      }
    } else if (state_changed && !state.valid) {
      std::lock_guard<std::mutex> lock(session_mutex);
      video_rx.reset();
      audio_rx.reset();
      if (enc) { enc->close(); enc.reset(); }
      current_state.valid = false;
    }

    if (video_rx && enc && max_frames > 0) {
      mtl_sdk::VideoFrame frame;
      if (video_rx->poll(frame, 100)) {
        enc->push_video(frame);
        video_rx->release(frame);
        max_frames--;
        if (max_frames == 0) {
          enc->close();
          enc.reset();
          std::cout << "Wrote " << out_path << "\n";
        }
      }
    }

    if (audio_rx && enc) {
      mtl_sdk::AudioFrame af;
      if (audio_rx->poll(af, 0)) {
        enc->push_audio(af);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  ctx->stop();
  return 0;
}
