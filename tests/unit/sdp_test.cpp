/**
 * Unit test: SDP parse, to_sdp, load_sdp_file, save_sdp_file
 * Validates 需求.md §4: SDP 协议解析，SDP 文件导入/导出
 *
 * No MTL runtime required; only mtl_sdk SDP API.
 */

#include "mtl_sdk/mtl_sdk.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; return 1; } } while(0)

static const char* SAMPLE_SDP = R"(v=0
o=- 123456 123456 IN IP4 192.168.1.1
s=ST2110 Test
c=IN IP4 239.100.1.1/32
t=0 0
m=video 5004 RTP/AVP 96
a=rtpmap:96 raw/90000
a=fmtp:96 sampling=YCbCr-4:2:2; width=1920; height=1080
a=ts-refclk:ptp=IEEE1588-2008:08-00-11-FF-FE-21-E1-B0
a=mediaclk:direct=0
m=audio 5006 RTP/AVP 97
a=rtpmap:97 L24/48000/2
a=ts-refclk:ptp=IEEE1588-2008:08-00-11-FF-FE-21-E1-B0
)";

int main() {
  std::cout << "[sdp_test] parse_sdp...\n";

  mtl_sdk::SdpSession sdp = mtl_sdk::parse_sdp(SAMPLE_SDP);

  CHECK(sdp.session_name == "ST2110 Test");
  CHECK(sdp.origin == "- 123456 123456 IN IP4 192.168.1.1");
  CHECK(sdp.connection == "IN IP4 239.100.1.1/32");
  CHECK(sdp.media.size() == 2);

  const auto& m0 = sdp.media[0];
  CHECK(m0.type == mtl_sdk::SdpMedia::Type::Video);
  CHECK(m0.endpoint.udp_port == 5004);
  CHECK(m0.endpoint.payload_type == 96);
  CHECK(m0.endpoint.ip == "239.100.1.1");
  CHECK(m0.rtpmap == "raw/90000");
  CHECK(!m0.fmtp_kv.empty());
  CHECK(m0.ts_refclk && m0.ts_refclk->find("ptp=") != std::string::npos);
  CHECK(m0.mediaclk && *m0.mediaclk == "direct=0");

  const auto& m1 = sdp.media[1];
  CHECK(m1.type == mtl_sdk::SdpMedia::Type::Audio);
  CHECK(m1.endpoint.udp_port == 5006);
  CHECK(m1.endpoint.payload_type == 97);
  CHECK(m1.rtpmap == "L24/48000/2");

  std::cout << "[sdp_test] to_sdp roundtrip...\n";

  std::string roundtrip = mtl_sdk::to_sdp(sdp);
  CHECK(!roundtrip.empty());
  CHECK(roundtrip.find("m=video 5004") != std::string::npos);
  CHECK(roundtrip.find("m=audio 5006") != std::string::npos);

  mtl_sdk::SdpSession sdp2 = mtl_sdk::parse_sdp(roundtrip);
  CHECK(sdp2.session_name == sdp.session_name);
  CHECK(sdp2.media.size() == sdp.media.size());
  CHECK(sdp2.media[0].endpoint.ip == sdp.media[0].endpoint.ip);
  CHECK(sdp2.media[0].endpoint.udp_port == sdp.media[0].endpoint.udp_port);

  std::cout << "[sdp_test] load_sdp_file / save_sdp_file...\n";

  const char* tmp_path = "/tmp/mtl_sdk_sdp_test.sdp";
  mtl_sdk::save_sdp_file(tmp_path, sdp);

  mtl_sdk::SdpSession sdp3 = mtl_sdk::load_sdp_file(tmp_path);
  CHECK(sdp3.session_name == sdp.session_name);
  CHECK(sdp3.media.size() == sdp.media.size());
  CHECK(sdp3.media[0].endpoint.udp_port == 5004);
  CHECK(sdp3.media[1].endpoint.udp_port == 5006);

  std::remove(tmp_path);

  std::cout << "[sdp_test] PASS\n";
  return 0;
}
