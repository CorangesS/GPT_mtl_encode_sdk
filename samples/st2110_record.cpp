// ST2110 receive + encode sample. Subscribe to multicast and write to MP4/MXF.
// For local two-process test: run st2110_send in one terminal, this in another (same --ip/ports).
// For two-machine test: run st2110_send on machine A, this on machine B with same --ip/ports.
// Async encode pipeline: RX thread copies frames and releases immediately; encode thread runs in parallel.

#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <cstring>
#include <algorithm>

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

static encode_sdk::VideoCodec parse_video_codec(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "h265" || v == "hevc") return encode_sdk::VideoCodec::H265;
  return encode_sdk::VideoCodec::H264;
}

static encode_sdk::AudioCodec parse_audio_codec(const std::string& s) {
  std::string v = to_lower(s);
  if (v == "aac") return encode_sdk::AudioCodec::AAC;
  if (v == "mp2") return encode_sdk::AudioCodec::MP2;
  if (v == "pcm") return encode_sdk::AudioCodec::PCM;
  if (v == "ac3") return encode_sdk::AudioCodec::AC3;
  return encode_sdk::AudioCodec::AAC;
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
            << "  --vcodec <h264|h265>    Video codec (default: h264)\n"
            << "  --acodec <aac|mp2|pcm|ac3>  Audio codec (default: aac)\n"
            << "  --container <mp4|mxf>   Output container (default: mp4)\n"
            << "  --vbitrate <kbps>       Video bitrate in kbps (default: 2000)\n"
            << "  --abitrate <kbps>       Audio bitrate in kbps (default: 128)\n"
            << "  --gop <n>               GOP size in frames (default: 120)\n"
            << "  --hw <auto|sw>          Hardware encode: auto or software (default: auto)\n"
            << "  --lcores <list>         DPDK lcores for MTL, e.g. 0-3 or 2,3,4,5 (faster RX)\n"
            << "  --main-lcore <id>       Main lcore id (default: MTL auto)\n"
            << "  --tasklets <n>          Tasklets per lcore; 0=auto (try 16 if slow)\n"
            << "  --data-quota-mbs <n>    Max data quota MB/s per lcore; 0=auto\n";
}

// Copy of video frame data for async pipeline (release RX buffer immediately after copy)
// MTL backend outputs YUV422_10BIT with 3 planes (Y, U, V).
struct FrameCopy {
  mtl_sdk::VideoFormat fmt;
  int64_t timestamp_ns;
  std::vector<uint8_t> y, u, v;
  int linesize_y, linesize_uv;

  static FrameCopy from(const mtl_sdk::VideoFrame& f) {
    FrameCopy fc;
    fc.fmt = f.fmt;
    fc.timestamp_ns = f.timestamp_ns;
    fc.linesize_y = f.planes[0].linesize;
    fc.linesize_uv = f.planes[1].linesize;
    size_t y_sz = (size_t)f.fmt.height * f.planes[0].linesize;
    size_t uv_sz = (size_t)f.fmt.height * f.planes[1].linesize;
    // MTL backend always provides 3-plane YUV422; validate before copy to avoid bad src pointers
    if (f.num_planes >= 3 && f.planes[0].data && f.planes[1].data && f.planes[2].data &&
        y_sz > 0 && uv_sz > 0) {
      fc.y.assign(f.planes[0].data, f.planes[0].data + y_sz);
      fc.u.assign(f.planes[1].data, f.planes[1].data + uv_sz);
      fc.v.assign(f.planes[2].data, f.planes[2].data + uv_sz);
    }
    return fc;
  }

  bool is_valid() const { return !y.empty() && !u.empty() && !v.empty(); }

  void to_video_frame(mtl_sdk::VideoFrame& out) const {
    if (!is_valid()) return;
    out = {};
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

// Bounded blocking queue for async encode
class FrameQueue {
public:
  static constexpr size_t MAX_SIZE = 64;

  void push(FrameCopy fc) {
    std::unique_lock<std::mutex> lock(m_);
    while (q_.size() >= MAX_SIZE) {
      cv_full_.wait(lock);
    }
    q_.push(std::move(fc));
    cv_empty_.notify_one();
  }

  bool pop(FrameCopy& out) {
    std::unique_lock<std::mutex> lock(m_);
    while (q_.empty() && !done_) {
      cv_empty_.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop();
    cv_full_.notify_one();
    return true;
  }

  void set_done() {
    std::lock_guard<std::mutex> lock(m_);
    done_ = true;
    cv_empty_.notify_all();
  }

private:
  std::queue<FrameCopy> q_;
  std::mutex m_;
  std::condition_variable cv_empty_, cv_full_;
  std::atomic<bool> done_{false};
};

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
  std::string vcodec_str = "h264";
  std::string acodec_str = "aac";
  std::string container_str = "mp4";
  std::string hw_str = "auto";
  int vbitrate_kbps = 2000;
  int abitrate_kbps = 128;
  int gop = 120;

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
    if (a == "--sdp" && i + 1 < argc) { sdp_path = argv[++i]; continue; }
    if (a == "--vcodec" && i + 1 < argc) { vcodec_str = argv[++i]; continue; }
    if (a == "--acodec" && i + 1 < argc) { acodec_str = argv[++i]; continue; }
    if (a == "--container" && i + 1 < argc) { container_str = argv[++i]; continue; }
    if (a == "--vbitrate" && i + 1 < argc) { vbitrate_kbps = atoi(argv[++i]); continue; }
    if (a == "--abitrate" && i + 1 < argc) { abitrate_kbps = atoi(argv[++i]); continue; }
    if (a == "--gop" && i + 1 < argc) { gop = atoi(argv[++i]); continue; }
    if (a == "--hw" && i + 1 < argc) { hw_str = argv[++i]; continue; }
    if (a == "--lcores" && i + 1 < argc) { lcores = argv[++i]; continue; }
    if (a == "--main-lcore" && i + 1 < argc) { main_lcore = atoi(argv[++i]); continue; }
    if (a == "--tasklets" && i + 1 < argc) { tasklets_nb_per_sch = (uint32_t)atoi(argv[++i]); continue; }
    if (a == "--data-quota-mbs" && i + 1 < argc) { data_quota_mbs_per_sch = (uint32_t)atoi(argv[++i]); continue; }
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
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = audio_sample_rate;
    af.channels = audio_channels;
    af.bits_per_sample = audio_bits;
    mtl_sdk::St2110Endpoint aep{ ip, audio_port, 97 };
    a_rx = ctx->create_audio_rx(af, aep);
  }

  // ---- Encoding SDK: codec/container from CLI (--vcodec, --container, etc.) ----
  encode_sdk::EncodeParams ep;
  ep.mux.container = parse_container(container_str);
  ep.mux.output_path = out;

  ep.video.codec = parse_video_codec(vcodec_str);
  ep.video.hw = parse_hw(hw_str);
  ep.video.bitrate_kbps = vbitrate_kbps;
  ep.video.gop = gop;
  ep.video.profile = "main";
  ep.video.fps_num = (int)(fps + 0.5);
  ep.video.fps_den = 1;
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  if (audio_port != 0) {
    ep.audio = encode_sdk::AudioEncodeParams{};
    ep.audio->codec = parse_audio_codec(acodec_str);
    ep.audio->bitrate_kbps = abitrate_kbps;
    ep.audio->sample_rate = audio_sample_rate;
    ep.audio->channels = audio_channels;
  } else {
    ep.audio = std::nullopt;
  }

  auto enc = encode_sdk::Session::open(ep);
  if (!enc) {
    std::cerr << "Failed to open encoder\n";
    return 1;
  }

  // ---- Async: RX copies & releases; encode thread consumes ----
  FrameQueue enc_queue;

  std::thread enc_thread([&]() {
    FrameCopy fc;
    while (enc_queue.pop(fc)) {
      mtl_sdk::VideoFrame vf_wrap;
      fc.to_video_frame(vf_wrap);
      enc->push_video(vf_wrap);
    }
  });

  int got = 0;
  while (got < max_frames) {
    mtl_sdk::VideoFrame vf_out;
    if (v_rx->poll(vf_out, 0)) {
      auto fc = FrameCopy::from(vf_out);
      v_rx->release(vf_out);  // Release immediately after copy
      if (fc.is_valid()) {
        enc_queue.push(std::move(fc));
        got++;
      } else {
        std::cerr << "st2110_record: skipped invalid frame (num_planes=" << vf_out.num_planes << ")\n";
      }
    }

    mtl_sdk::AudioFrame af_out;
    if (a_rx && a_rx->poll(af_out, 0)) {
      enc->push_audio(af_out);
    }
  }

  enc_queue.set_done();
  enc_thread.join();

  enc->close();
  ctx->stop();

  std::cout << "Wrote " << out << " (" << got << " frames)\n";
  return 0;
}
