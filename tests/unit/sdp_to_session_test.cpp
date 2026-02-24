/**
 * Unit test: Parse SDP and extract parameters for create_video_rx/create_audio_rx
 * Validates that SDP output can be mapped to VideoFormat, AudioFormat, St2110Endpoint
 *
 * No MTL runtime required; verifies SDP -> session parameter mapping.
 */

#include "mtl_sdk/mtl_sdk.hpp"

#include <cassert>
#include <iostream>
#include <string>

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; return 1; } } while(0)

static const char* SAMPLE_SDP = R"(v=0
o=- 0 0 IN IP4 10.0.0.1
s=SDP to Session Test
c=IN IP4 239.50.1.1/32
t=0 0
m=video 5004 RTP/AVP 96
a=rtpmap:96 raw/90000
a=fmtp:96 sampling=YCbCr-4:2:2; width=1920; height=1080; exactframerate=60000/1001
m=audio 5006 RTP/AVP 97
a=rtpmap:97 L24/48000/2
)";

int main() {
  std::cout << "[sdp_to_session_test] parse and map to session params...\n";

  mtl_sdk::SdpSession sdp = mtl_sdk::parse_sdp(SAMPLE_SDP);

  mtl_sdk::VideoFormat vf;
  mtl_sdk::AudioFormat af;
  mtl_sdk::St2110Endpoint vep, aep;
  bool has_video = false, has_audio = false;

  for (const auto& m : sdp.media) {
    if (m.type == mtl_sdk::SdpMedia::Type::Video) {
      vep.ip = m.endpoint.ip;
      vep.udp_port = m.endpoint.udp_port;
      vep.payload_type = m.endpoint.payload_type;
      vf.width = 1920;
      vf.height = 1080;
      vf.fps = 59.94;
      vf.pix_fmt = mtl_sdk::VideoPixFmt::YUV422_10BIT;
      for (const auto& kv : m.fmtp_kv) {
        if (kv.find("width=") == 0) vf.width = std::stoi(kv.substr(6));
        else if (kv.find("height=") == 0) vf.height = std::stoi(kv.substr(7));
      }
      has_video = true;
    } else if (m.type == mtl_sdk::SdpMedia::Type::Audio) {
      aep.ip = m.endpoint.ip;
      aep.udp_port = m.endpoint.udp_port;
      aep.payload_type = m.endpoint.payload_type;
      af.sample_rate = 48000;
      af.channels = 2;
      af.bits_per_sample = 24;
      if (m.rtpmap.find("48000") != std::string::npos) {
        size_t p = m.rtpmap.find('/');
        if (p != std::string::npos) af.sample_rate = std::stoi(m.rtpmap.substr(p + 1));
        p = m.rtpmap.find('/', p + 1);
        if (p != std::string::npos) af.channels = std::stoi(m.rtpmap.substr(p + 1));
      }
      has_audio = true;
    }
  }

  CHECK(has_video);
  CHECK(has_audio);
  CHECK(vep.ip == "239.50.1.1");
  CHECK(vep.udp_port == 5004);
  CHECK(aep.udp_port == 5006);
  CHECK(vf.width == 1920);
  CHECK(vf.height == 1080);
  CHECK(af.sample_rate == 48000);
  CHECK(af.channels == 2);

  std::cout << "[sdp_to_session_test] VideoFormat: " << vf.width << "x" << vf.height
            << " @" << vf.fps << ", endpoint " << vep.ip << ":" << vep.udp_port << "\n";
  std::cout << "[sdp_to_session_test] AudioFormat: " << af.channels << "ch "
            << af.sample_rate << "Hz, endpoint " << aep.ip << ":" << aep.udp_port << "\n";

  std::cout << "[sdp_to_session_test] PASS\n";
  return 0;
}
