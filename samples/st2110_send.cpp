// ST2110 send sample: sends video (and optional audio) to a multicast address.
// Use with st2110_record for:
//   - Local two-process test: run st2110_send in one terminal, st2110_record in another (same multicast).
//   - Two-machine test: run st2110_send on machine A, st2110_record on machine B (same multicast, same network).

#include "mtl_sdk/mtl_sdk.hpp"

#include <iostream>
#include <cstring>
#include <cmath>
#include <thread>
#include <chrono>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options]\n"
            << "  --ip <multicast_ip>     Default: 239.0.0.1\n"
            << "  --video-port <port>     Default: 5004\n"
            << "  --audio-port <port>     Default: 5006 (0 = no audio)\n"
            << "  --width <w>             Default: 1280\n"
            << "  --height <h>            Default: 720\n"
            << "  --fps <fps>             Default: 30\n"
            << "  --duration <sec>        Run for N seconds (default: 10)\n"
            << "  --mock                  Use mock backend (no real network)\n";
}

int main(int argc, char** argv) {
  std::string ip = "239.0.0.1";
  uint16_t video_port = 5004;
  uint16_t audio_port = 5006;
  int width = 1280;
  int height = 720;
  double fps = 30.0;
  int duration_sec = 10;
  bool use_mock = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--width" && i + 1 < argc) { width = atoi(argv[++i]); continue; }
    if (a == "--height" && i + 1 < argc) { height = atoi(argv[++i]); continue; }
    if (a == "--fps" && i + 1 < argc) { fps = atof(argv[++i]); continue; }
    if (a == "--duration" && i + 1 < argc) { duration_sec = atoi(argv[++i]); continue; }
    if (a == "--mock") { use_mock = true; continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
  }

  mtl_sdk::MtlSdkConfig cfg;
  cfg.ports.push_back({"kernel:lo", "127.0.0.1"}); // override with real port/sip for real MTL
  cfg.tx_queues = 1;  // required for TX when using real MTL
  cfg.rx_queues = 0;

  // Mock backend is chosen at compile time by default (MTL_SDK_WITH_MTL=OFF).
  // For real MTL two-process test, build with MTL_SDK_WITH_MTL=ON and do not pass --mock.
  (void)use_mock;

  auto ctx = mtl_sdk::Context::create(cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  mtl_sdk::VideoFormat vf;
  vf.width = width;
  vf.height = height;
  vf.fps = fps;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::NV12;

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

  // Allocate one video frame (NV12) and fill with test pattern
  size_t y_size = (size_t)width * height;
  size_t uv_size = y_size / 2;
  std::vector<uint8_t> y_buf(y_size), uv_buf(uv_size);

  mtl_sdk::VideoFrame frame;
  frame.fmt = vf;
  frame.mem_type = mtl_sdk::MemoryType::HostPtr;
  frame.num_planes = 2;
  frame.planes[0].data = y_buf.data();
  frame.planes[0].linesize = width;
  frame.planes[1].data = uv_buf.data();
  frame.planes[1].linesize = width;
  frame.bytes_total = y_size + uv_size;

  const int total_video_frames = (int)(fps * duration_sec);
  const int samples_per_audio_chunk = 480; // 10 ms at 48 kHz
  const int audio_chunk_bytes = samples_per_audio_chunk * 2 * 2; // S16LE, 2 ch
  std::vector<uint8_t> pcm_chunk(audio_chunk_bytes);
  int audio_chunks_sent = 0;
  int audio_chunks_total = (int)(48000 * duration_sec / samples_per_audio_chunk);

  auto start = std::chrono::steady_clock::now();
  double frame_interval = 1.0 / fps;

  for (int i = 0; i < total_video_frames; i++) {
    frame.timestamp_ns = (int64_t)i * (int64_t)(1e9 / fps);

    // Simple moving pattern
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
        y_buf[y * width + x] = (uint8_t)((x + i) % 256);
    std::fill(uv_buf.begin(), uv_buf.end(), 128);

    if (!v_tx->put_video(frame)) {
      std::cerr << "put_video failed at frame " << i << "\n";
      break;
    }

    // Send audio chunks to match video (roughly 10 ms per chunk)
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

    // Pace sending by frame rate
    auto next = start + std::chrono::duration<double>(frame_interval * (i + 1));
    std::this_thread::sleep_until(next);
  }

  ctx->stop();
  std::cout << "Sent " << total_video_frames << " video frames to " << ip << ":" << video_port;
  if (a_tx) std::cout << ", " << audio_chunks_sent << " audio chunks to " << ip << ":" << audio_port;
  std::cout << "\n";
  return 0;
}
