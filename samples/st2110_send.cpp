// ST2110 send sample: sends video (and optional audio) to a multicast address.
// Use with st2110_record for:
//   - Local two-process test: run st2110_send in one terminal, st2110_record in another (same multicast).
//   - Two-machine test: run st2110_send on machine A, st2110_record on machine B (same multicast, same network).

#include "mtl_sdk/mtl_sdk.hpp"

#include <iostream>
#include <cstring>
#include <cmath>
#include <fstream>
#include <thread>
#include <chrono>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options]\n"
            << "  --port <port_name>       MTL port, e.g. kernel:enp2s0 or kernel:lo (default: kernel:lo)\n"
            << "  --sip <source_ip>        Source IP for the port (default: 127.0.0.1)\n"
            << "  --url <yuv_file>         Read video from YUV file instead of test pattern\n"
            << "  --fmt <format>           YUV format: yuv420p10le | yuv422p10le (default: yuv420p10le when --url)\n"
            << "  --ip <multicast_ip>      Default: 239.0.0.1\n"
            << "  --video-port <port>      Default: 5004\n"
            << "  --audio-port <port>      Default: 5006 (0 = no audio)\n"
            << "  --width <w>              Default: 1920\n"
            << "  --height <h>             Default: 1080\n"
            << "  --fps <fps>              Default: 59.94\n"
            << "  --duration <sec>         Run for N seconds (default: 10)\n"
            << "  --no-ptp                 Disable PTP, use synthetic timestamps (fallback when NIC lacks PTP)\n"
            << "  --sdp-out <file>         Export SDP describing the video/audio streams to <file>\n";
}

// Convert yuv420p10le (3 planes) to yuv422p10le (3 planes).
// 420: Y w*h*2, U (w/2)*(h/2)*2, V (w/2)*(h/2)*2
// 422: Y w*h*2, U (w/2)*h*2, V (w/2)*h*2
static void yuv420p10le_to_yuv422p10le(int w, int h,
    const uint8_t* y_in, const uint8_t* u_in, const uint8_t* v_in,
    uint8_t* y_out, uint8_t* u_out, uint8_t* v_out) {
  const size_t y_sz = (size_t)w * h * 2;
  memcpy(y_out, y_in, y_sz);

  const int u_w = w / 2, u_h = h / 2;
  const int out_u_stride = u_w * 2;
  for (int row = 0; row < u_h; row++) {
    const uint8_t* u_row = u_in + row * u_w * 2;
    const uint8_t* v_row = v_in + row * u_w * 2;
    memcpy(u_out + (row * 2) * out_u_stride, u_row, (size_t)out_u_stride);
    memcpy(u_out + (row * 2 + 1) * out_u_stride, u_row, (size_t)out_u_stride);
    memcpy(v_out + (row * 2) * out_u_stride, v_row, (size_t)out_u_stride);
    memcpy(v_out + (row * 2 + 1) * out_u_stride, v_row, (size_t)out_u_stride);
  }
}

int main(int argc, char** argv) {
  std::string ip = "239.0.0.1";
  std::string port_name = "kernel:lo";
  std::string sip = "127.0.0.1";
  std::string url;
  std::string fmt_str = "yuv420p10le";
  std::string sdp_out;
  uint16_t video_port = 5004;
  uint16_t audio_port = 5006;
  int width = 1920;
  int height = 1080;
  double fps = 59.94;
  int duration_sec = 10;
  bool use_ptp = true;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) { port_name = argv[++i]; continue; }
    if (a == "--sip" && i + 1 < argc) { sip = argv[++i]; continue; }
    if (a == "--url" && i + 1 < argc) { url = argv[++i]; continue; }
    if (a == "--fmt" && i + 1 < argc) { fmt_str = argv[++i]; continue; }
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--width" && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (a == "--height" && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (a == "--fps" && i + 1 < argc) { fps = atof(argv[++i]); continue; }
    if (a == "--duration" && i + 1 < argc) { duration_sec = atoi(argv[++i]); continue; }
    if (a == "--no-ptp") { use_ptp = false; continue; }
    if (a == "--sdp-out" && i + 1 < argc) { sdp_out = argv[++i]; continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
  }

  mtl_sdk::MtlSdkConfig cfg;
  cfg.ports.push_back({port_name, sip});
  cfg.tx_queues = 1;
  cfg.rx_queues = 0;
  cfg.enable_builtin_ptp = use_ptp;

  auto ctx = mtl_sdk::Context::create(cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  const bool from_file = !url.empty();
  const bool use_yuv420 = (fmt_str == "yuv420p10le");
  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;

  mtl_sdk::St2110Endpoint vep{ip, video_port, 96};
  auto v_tx = ctx->create_video_tx(vf, vep);
  if (!v_tx) {
    std::cerr << "Failed to create video TX\n";
    ctx->stop();
    return 1;
  }

  std::unique_ptr<mtl_sdk::Context::AudioTxSession> a_tx;
  if (audio_port != 0) {
    mtl_sdk::AudioFormat af;
    af.sample_rate = 48000;
    af.channels = 2;
    af.bits_per_sample = 16;
    mtl_sdk::St2110Endpoint aep{ip, audio_port, 97};
    a_tx = ctx->create_audio_tx(af, aep);
  }

  // Optionally export an SDP file that describes the current streams.
  if (!sdp_out.empty()) {
    mtl_sdk::SdpSession sdp;
    sdp.session_name = "st2110_send";
    sdp.origin = "- 0 0 IN IP4 " + sip;
    sdp.connection = "IN IP4 " + ip + "/32";

    // Video media
    {
      mtl_sdk::SdpMedia mv;
      mv.type = mtl_sdk::SdpMedia::Type::Video;
      mv.endpoint = vep;
      mv.rtpmap = "raw/90000";
      mv.fmtp_kv.push_back("sampling=YCbCr-4:2:2");
      mv.fmtp_kv.push_back("width=" + std::to_string(width));
      mv.fmtp_kv.push_back("height=" + std::to_string(height));
      // encode fps as exactframerate=N/1000 for typical fractional rates
      int fps_num = static_cast<int>(fps * 1000.0 + 0.5);
      int fps_den = 1000;
      mv.fmtp_kv.push_back("exactframerate=" + std::to_string(fps_num) + "/" + std::to_string(fps_den));
      sdp.media.push_back(std::move(mv));
    }

    // Audio media (optional)
    if (a_tx) {
      mtl_sdk::SdpMedia ma;
      ma.type = mtl_sdk::SdpMedia::Type::Audio;
      ma.endpoint = {ip, audio_port, 97};
      ma.rtpmap = "L16/48000/2";
      sdp.media.push_back(std::move(ma));
    }

    try {
      mtl_sdk::save_sdp_file(sdp_out, sdp);
      std::cout << "Wrote SDP to " << sdp_out << "\n";
    } catch (...) {
      std::cerr << "Failed to write SDP file: " << sdp_out << "\n";
    }
  }

  // Frame buffers for YUV422 10-bit (3 planes: Y, U, V)
  const size_t y_sz = (size_t)width * height * 2;
  const size_t uv_sz = (size_t)(width / 2) * height * 2;
  std::vector<uint8_t> y_buf(y_sz), u_buf(uv_sz), v_buf(uv_sz);

  std::ifstream yuv_file;
  if (from_file) {
    yuv_file.open(url, std::ios::binary);
    if (!yuv_file) {
      std::cerr << "Failed to open YUV file: " << url << "\n";
      ctx->stop();
      return 1;
    }
    std::cout << "Reading from " << url << " (" << width << "x" << height
              << ", " << fmt_str << ")\n";
  }

  const int total_video_frames = (int)(fps * duration_sec);
  const int samples_per_audio_chunk = 480;
  const int audio_chunk_bytes = samples_per_audio_chunk * 2 * 2;
  std::vector<uint8_t> pcm_chunk(audio_chunk_bytes);
  int audio_chunks_sent = 0;
  int audio_chunks_total = (int)(48000 * duration_sec / samples_per_audio_chunk);

  mtl_sdk::VideoFrame frame;
  frame.fmt = vf;
  frame.mem_type = mtl_sdk::MemoryType::HostPtr;
  frame.num_planes = 3;
  frame.planes[0].data = y_buf.data();
  frame.planes[0].linesize = width * 2;
  frame.planes[1].data = u_buf.data();
  frame.planes[1].linesize = width;
  frame.planes[2].data = v_buf.data();
  frame.planes[2].linesize = width;
  frame.bytes_total = y_sz + uv_sz * 2;

  auto start = std::chrono::steady_clock::now();
  double frame_interval = 1.0 / fps;
  const int64_t frame_ns = (int64_t)(1e9 / fps);
  bool ptp_valid = use_ptp;

  if (!use_ptp) {
    std::cout << "PTP disabled, using synthetic timestamps\n";
  }

  for (int i = 0; i < total_video_frames; i++) {
    if (ptp_valid) {
      int64_t ptp_ns = ctx->now_ptp_ns();
      if (i == 0 && ptp_ns == 0) {
        ptp_valid = false;
        std::cout << "PTP unavailable (NIC/mode may not support it), falling back to synthetic timestamps\n";
      }
      if (ptp_valid) {
        frame.timestamp_ns = ptp_ns;
      }
    }
    if (!ptp_valid) {
      frame.timestamp_ns = (int64_t)i * frame_ns;
    }

    if (from_file) {
      if (use_yuv420) {
        const size_t y_read = y_sz;
        const size_t u420_sz = (size_t)(width / 2) * (height / 2) * 2;
        const size_t v420_sz = u420_sz;
        std::vector<uint8_t> u420(u420_sz), v420(v420_sz);
        yuv_file.read((char*)y_buf.data(), y_read);
        yuv_file.read((char*)u420.data(), u420_sz);
        yuv_file.read((char*)v420.data(), v420_sz);
        if (!yuv_file || (size_t)yuv_file.gcount() < v420_sz)
          break;
        yuv420p10le_to_yuv422p10le(width, height,
            y_buf.data(), u420.data(), v420.data(),
            y_buf.data(), u_buf.data(), v_buf.data());
      } else {
        yuv_file.read((char*)y_buf.data(), y_sz);
        yuv_file.read((char*)u_buf.data(), uv_sz);
        yuv_file.read((char*)v_buf.data(), uv_sz);
        if (!yuv_file) break;
      }
    } else {
      for (size_t j = 0; j < y_sz; j += 2)
        *(uint16_t*)(y_buf.data() + j) = (uint16_t)(((j/2 + i) % 1024) << 6);
      std::fill(u_buf.begin(), u_buf.end(), 0);
      std::fill(v_buf.begin(), v_buf.end(), 0);
    }

    if (!v_tx->put_video(frame)) {
      std::cerr << "put_video failed at frame " << i << "\n";
      break;
    }

    if (a_tx && audio_chunks_sent < audio_chunks_total) {
      int chunks_this_frame = (int)((i + 1) * samples_per_audio_chunk * 2 / 48000.0) - audio_chunks_sent;
      for (int c = 0; c < chunks_this_frame && audio_chunks_sent < audio_chunks_total; c++) {
        mtl_sdk::AudioFrame af;
        af.fmt.sample_rate = 48000;
        af.fmt.channels = 2;
        af.fmt.bits_per_sample = 16;
        af.timestamp_ns = (int64_t)audio_chunks_sent * (int64_t)(1e9 * samples_per_audio_chunk / 48000);
        af.pcm.resize(audio_chunk_bytes);
        int16_t* p = (int16_t*)af.pcm.data();
        for (int s = 0; s < samples_per_audio_chunk * 2; s++)
          p[s] = (int16_t)(32000 * std::sin(2.0 * M_PI * 440.0 * (audio_chunks_sent * samples_per_audio_chunk + s / 2) / 48000.0));
        a_tx->put_audio(af);
        audio_chunks_sent++;
      }
    }

    auto next = start + std::chrono::duration<double>(frame_interval * (i + 1));
    std::this_thread::sleep_until(next);
  }

  ctx->stop();
  std::cout << "Sent " << total_video_frames << " video frames to " << ip << ":" << video_port;
  if (a_tx) std::cout << ", " << audio_chunks_sent << " audio chunks to " << ip << ":" << audio_port;
  std::cout << "\n";
  return 0;
}
