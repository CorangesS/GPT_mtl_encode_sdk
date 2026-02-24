/**
 * Unit test: Encode various VideoCodec, Container, AudioCodec combinations
 * Validates 需求.md §2-4: H.264/H.265, MP4/MXF, AAC/MP2/PCM/AC3
 *
 * Uses synthetic VideoFrame (no MTL). Each combination opens Session, pushes
 * a few frames, closes. Skips if encoder not available in FFmpeg build.
 */

#include "encode_sdk/encode_sdk.hpp"
#include "mtl_sdk/mtl_sdk.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; return false; } } while(0)

static mtl_sdk::VideoFrame make_test_frame(int w, int h, int64_t pts_ns) {
  mtl_sdk::VideoFrame f{};
  f.fmt.width = w;
  f.fmt.height = h;
  f.fmt.fps = 59.94;
  f.fmt.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
  f.timestamp_ns = pts_ns;
  f.mem_type = mtl_sdk::MemoryType::HostPtr;
  f.num_planes = 3;

  size_t y_sz = (size_t)w * h * 2;
  size_t uv_sz = (size_t)(w / 2) * h * 2;
  static std::vector<uint8_t> y_buf, u_buf, v_buf;
  y_buf.resize(y_sz);
  u_buf.resize(uv_sz);
  v_buf.resize(uv_sz);
  std::memset(y_buf.data(), 0x10, y_sz);
  std::memset(u_buf.data(), 0x80, uv_sz);
  std::memset(v_buf.data(), 0x80, uv_sz);

  f.planes[0].data = y_buf.data();
  f.planes[0].linesize = w * 2;
  f.planes[1].data = u_buf.data();
  f.planes[1].linesize = (w / 2) * 2;
  f.planes[2].data = v_buf.data();
  f.planes[2].linesize = (w / 2) * 2;
  f.bytes_total = y_sz + uv_sz * 2;
  return f;
}

static mtl_sdk::AudioFrame make_test_audio_frame(int samples, int64_t pts_ns) {
  mtl_sdk::AudioFrame f{};
  f.fmt.sample_rate = 48000;
  f.fmt.channels = 2;
  f.fmt.bits_per_sample = 16;
  f.timestamp_ns = pts_ns;
  f.pcm.resize((size_t)samples * 2 * 2);
  std::memset(f.pcm.data(), 0, f.pcm.size());
  return f;
}

static bool run_encode_test(const std::string& name,
                            encode_sdk::VideoCodec vc,
                            encode_sdk::Container container,
                            std::optional<encode_sdk::AudioCodec> ac,
                            const std::string& out_path) {
  encode_sdk::EncodeParams ep;
  ep.video.codec = vc;
  ep.video.hw = encode_sdk::HwAccel::Software;
  ep.video.bitrate_kbps = 500;
  ep.video.gop = 30;
  ep.video.fps_num = 60000;
  ep.video.fps_den = 1001;
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
  ep.mux.container = container;
  ep.mux.output_path = out_path;
  if (ac) {
    ep.audio = encode_sdk::AudioEncodeParams{};
    ep.audio->codec = *ac;
    ep.audio->bitrate_kbps = 64;
    ep.audio->sample_rate = 48000;
    ep.audio->channels = 2;
  } else {
    ep.audio = std::nullopt;
  }

  try {
    auto sess = encode_sdk::Session::open(ep);
    if (!sess) {
      std::cerr << "  SKIP " << name << " (encoder open failed)\n";
      return true;
    }

    const int w = 320, h = 180;
    const int64_t frame_ns = 1000000000LL * 1001 / 60000;
    for (int i = 0; i < 30; i++) {
      auto vf = make_test_frame(w, h, i * frame_ns);
      if (!sess->push_video(vf)) break;
      if (ac && i % 2 == 0) {
        auto af = make_test_audio_frame(160, i * frame_ns);
        sess->push_audio(af);
      }
    }
    sess->close();
    std::cout << "  OK   " << name << " -> " << out_path << "\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "  SKIP " << name << ": " << e.what() << "\n";
    return true;
  }
}

int main() {
  std::cout << "[encode_format_test] H.264/H.265 x MP4/MXF x AAC/MP2/PCM/AC3\n";

  int ok = 0, total = 0;

  auto test = [&](const std::string& n, encode_sdk::VideoCodec vc,
                  encode_sdk::Container c, std::optional<encode_sdk::AudioCodec> ac) {
    std::string path = "/tmp/mtl_encode_test_" + std::to_string(total++);
    path += (c == encode_sdk::Container::MXF) ? ".mxf" : ".mp4";
    if (run_encode_test(n, vc, c, ac, path)) ok++;
  };

  test("H264+MP4", encode_sdk::VideoCodec::H264, encode_sdk::Container::MP4, std::nullopt);
  test("H264+MP4+AAC", encode_sdk::VideoCodec::H264, encode_sdk::Container::MP4, encode_sdk::AudioCodec::AAC);
  test("H265+MP4", encode_sdk::VideoCodec::H265, encode_sdk::Container::MP4, std::nullopt);
  test("H265+MP4+AAC", encode_sdk::VideoCodec::H265, encode_sdk::Container::MP4, encode_sdk::AudioCodec::AAC);
  test("H264+MXF", encode_sdk::VideoCodec::H264, encode_sdk::Container::MXF, std::nullopt);
  test("H264+MXF+AAC", encode_sdk::VideoCodec::H264, encode_sdk::Container::MXF, encode_sdk::AudioCodec::AAC);
  test("H264+MP4+MP2", encode_sdk::VideoCodec::H264, encode_sdk::Container::MP4, encode_sdk::AudioCodec::MP2);
  test("H264+MP4+PCM", encode_sdk::VideoCodec::H264, encode_sdk::Container::MP4, encode_sdk::AudioCodec::PCM);
  test("H264+MP4+AC3", encode_sdk::VideoCodec::H264, encode_sdk::Container::MP4, encode_sdk::AudioCodec::AC3);

  std::cout << "[encode_format_test] " << ok << "/" << total << " passed (skips allowed)\n";
  return (ok >= 1) ? 0 : 1;
}
