/**
 * Unit test: set_video_bitrate_kbps, set_video_gop, apply_reconfigure
 * Validates 需求.md §5: 编码参数可调
 *
 * Uses synthetic VideoFrame (no MTL).
 */

#include "encode_sdk/encode_sdk.hpp"
#include "mtl_sdk/mtl_sdk.hpp"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; return 1; } } while(0)

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

int main() {
  std::cout << "[encode_reconfigure_test] set_video_bitrate_kbps, set_video_gop, apply_reconfigure\n";

  encode_sdk::EncodeParams ep;
  ep.video.codec = encode_sdk::VideoCodec::H264;
  ep.video.hw = encode_sdk::HwAccel::Software;
  ep.video.bitrate_kbps = 1000;
  ep.video.gop = 60;
  ep.video.fps_num = 60000;
  ep.video.fps_den = 1001;
  ep.video.input_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
  ep.mux.container = encode_sdk::Container::MP4;
  ep.mux.output_path = "/tmp/mtl_encode_reconfigure_test.mp4";

  auto sess = encode_sdk::Session::open(ep);
  CHECK(sess);

  const int w = 320, h = 180;
  const int64_t frame_ns = 1000000000LL * 1001 / 60000;

  for (int i = 0; i < 15; i++) {
    auto vf = make_test_frame(w, h, i * frame_ns);
    CHECK(sess->push_video(vf));
  }

  sess->set_video_bitrate_kbps(2000);
  sess->set_video_gop(30);
  CHECK(sess->apply_reconfigure());

  for (int i = 15; i < 30; i++) {
    auto vf = make_test_frame(w, h, i * frame_ns);
    CHECK(sess->push_video(vf));
  }

  sess->close();

  std::cout << "[encode_reconfigure_test] PASS\n";
  return 0;
}
