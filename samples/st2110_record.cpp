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
            << "  --no-ptp                Disable PTP, use synthetic timestamps (fallback when NIC lacks PTP)\n";
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
  uint16_t video_port = 5004;
  uint16_t audio_port = 5006;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  int max_frames = 600;
  bool use_ptp = true;

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
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    if (a.compare(0, 2, "--") != 0) { out = a; continue; }
  }

  // ---- MTL SDK ----
  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({port_name, sip});
  mtl_cfg.rx_queues = 1;
  mtl_cfg.enable_builtin_ptp = use_ptp;

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
    af.sample_rate = 48000;
    af.channels = 2;
    af.bits_per_sample = 16;
    mtl_sdk::St2110Endpoint aep{ ip, audio_port, 97 };
    a_rx = ctx->create_audio_rx(af, aep);
  }

  // ---- Encoding SDK: Auto hw (NVENC->QSV->CPU), lower load for stability ----
  encode_sdk::EncodeParams ep;
  ep.mux.container = encode_sdk::Container::MP4;
  ep.mux.output_path = out;

  ep.video.codec = encode_sdk::VideoCodec::H264;
  ep.video.hw = encode_sdk::HwAccel::Auto;
  ep.video.bitrate_kbps = 2000;  // Lower for less CPU load
  ep.video.gop = 120;            // Larger GOP for less load
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
