// ST2110 receive + encode sample. Subscribe to multicast and write to MP4/MXF.
// For local two-process test: run st2110_send in one terminal, this in another (same --ip/ports).
// For two-machine test: run st2110_send on machine A, this on machine B with same --ip/ports.

#include "mtl_sdk/mtl_sdk.hpp"
#include "encode_sdk/encode_sdk.hpp"

#include <iostream>

static void usage(const char* prog) {
  std::cerr << "Usage: " << prog << " [options] [output.mp4]\n"
            << "  --ip <multicast_ip>     Default: 239.0.0.1\n"
            << "  --video-port <port>     Default: 5004\n"
            << "  --audio-port <port>     Default: 5006\n"
            << "  --max-frames <n>        Stop after N video frames (default: 150)\n";
}

int main(int argc, char** argv) {
  std::string out = "out.mp4";
  std::string ip = "239.0.0.1";
  uint16_t video_port = 5004;
  uint16_t audio_port = 5006;
  int max_frames = 150;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--ip" && i + 1 < argc) { ip = argv[++i]; continue; }
    if (a == "--video-port" && i + 1 < argc) { video_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--audio-port" && i + 1 < argc) { audio_port = (uint16_t)atoi(argv[++i]); continue; }
    if (a == "--max-frames" && i + 1 < argc) { max_frames = atoi(argv[++i]); continue; }
    if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
    if (a.compare(0, 2, "--") != 0) { out = a; continue; }
  }

  // ---- MTL SDK (mock by default; use MTL_SDK_WITH_MTL=ON for real RX) ----
  mtl_sdk::MtlSdkConfig mtl_cfg;
  mtl_cfg.ports.push_back({"kernel:lo", "127.0.0.1"});
  mtl_cfg.rx_queues = 1;

  auto ctx = mtl_sdk::Context::create(mtl_cfg);
  if (!ctx) {
    std::cerr << "Failed to create MTL context\n";
    return 1;
  }
  if (ctx->start() != 0) {
    std::cerr << "Failed to start MTL context\n";
    return 1;
  }

  mtl_sdk::VideoFormat vf;
  vf.width = 1280;
  vf.height = 720;
  vf.fps = 30.0;
  vf.pix_fmt = mtl_sdk::VideoPixFmt::NV12;

  mtl_sdk::AudioFormat af;
  af.sample_rate = 48000;
  af.channels = 2;
  af.bits_per_sample = 16;

  mtl_sdk::St2110Endpoint vep{ ip, video_port, 96 };
  mtl_sdk::St2110Endpoint aep{ ip, audio_port, 97 };

  auto v_rx = ctx->create_video_rx(vf, vep);
  auto a_rx = ctx->create_audio_rx(af, aep);

  // ---- Encoding SDK ----
  encode_sdk::EncodeParams ep;
  ep.mux.container = encode_sdk::Container::MP4;
  ep.mux.output_path = out;

  ep.video.codec = encode_sdk::VideoCodec::H264;
  ep.video.hw = encode_sdk::HwAccel::Auto;
  ep.video.bitrate_kbps = 4000;
  ep.video.gop = 60;
  ep.video.profile = "high";
  ep.video.fps_num = 30;
  ep.video.fps_den = 1;
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::NV12;

  ep.audio = encode_sdk::AudioEncodeParams{};
  ep.audio->codec = encode_sdk::AudioCodec::AAC;
  ep.audio->bitrate_kbps = 128;
  ep.audio->sample_rate = 48000;
  ep.audio->channels = 2;

  auto enc = encode_sdk::Session::open(ep);
  if (!enc) {
    std::cerr << "Failed to open encoder\n";
    return 1;
  }

  int got = 0;
  while (got < max_frames) {
    mtl_sdk::VideoFrame vf_out;
    if (v_rx->poll(vf_out, 0)) {
      enc->push_video(vf_out);
      v_rx->release(vf_out);
      got++;
    }

    mtl_sdk::AudioFrame af_out;
    if (a_rx && a_rx->poll(af_out, 0)) {
      enc->push_audio(af_out);
    }
  }

  enc->close();
  ctx->stop();

  std::cout << "Wrote " << out << "\n";
  return 0;
}
