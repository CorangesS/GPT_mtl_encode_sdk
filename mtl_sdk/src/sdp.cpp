#include "mtl_sdk/mtl_sdk.hpp"
#include <sstream>
#include <fstream>
#include <algorithm>

namespace mtl_sdk {

static inline std::string trim(std::string s) {
  auto not_space = [](unsigned char c){ return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

SdpSession parse_sdp(const std::string& text) {
  SdpSession sdp;
  std::istringstream in(text);
  std::string line;
  SdpMedia* cur = nullptr;

  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty()) continue;

    if (line.size() < 2 || line[1] != '=') continue;
    char t = line[0];
    std::string v = line.substr(2);

    if (t == 's') {
      sdp.session_name = v;
    } else if (t == 'o') {
      sdp.origin = v;
    } else if (t == 'c') {
      sdp.connection = v;
    } else if (t == 'm') {
      // m=<media> <port> <proto> <fmt>
      // e.g. m=video 5004 RTP/AVP 96
      std::istringstream ms(v);
      std::string media, proto;
      int port = 0;
      int pt = 0;
      ms >> media >> port >> proto >> pt;

      SdpMedia m{};
      if (media == "video") m.type = SdpMedia::Type::Video;
      else if (media == "audio") m.type = SdpMedia::Type::Audio;
      else continue;

      m.endpoint.udp_port = (uint16_t)port;
      m.endpoint.payload_type = (uint8_t)pt;
      // IP will be learned from c= or a= lines; if c= is multicast, use it as default.
      sdp.media.push_back(std::move(m));
      cur = &sdp.media.back();
    } else if (t == 'a' && cur) {
      // attributes: rtpmap/fmtp/ts-refclk/mediaclk and preserve unknown
      if (v.rfind("rtpmap:", 0) == 0) {
        // rtpmap:<pt> <encoding>/<clock>[/channels]
        auto sp = v.find(' ');
        if (sp != std::string::npos) cur->rtpmap = v.substr(sp + 1);
      } else if (v.rfind("fmtp:", 0) == 0) {
        // fmtp:<pt> <params>
        auto sp = v.find(' ');
        if (sp != std::string::npos) {
          std::string params = v.substr(sp + 1);
          // split by ';'
          std::stringstream ps(params);
          std::string token;
          while (std::getline(ps, token, ';')) {
            token = trim(token);
            if (!token.empty()) cur->fmtp_kv.push_back(token);
          }
        }
      } else if (v.rfind("ts-refclk:", 0) == 0) {
        cur->ts_refclk = v.substr(std::string("ts-refclk:").size());
      } else if (v.rfind("mediaclk:", 0) == 0) {
        cur->mediaclk = v.substr(std::string("mediaclk:").size());
      }
    }
  }

  // try to propagate connection IP to each media if it's multicast/unicast
  // c=IN IP4 239.0.0.1/32
  {
    std::string c = sdp.connection;
    std::istringstream cs(c);
    std::string inTok, ipTok;
    cs >> inTok >> ipTok >> ipTok; // "IN" "IP4" "<ip>"
    if (!ipTok.empty()) {
      auto slash = ipTok.find('/');
      std::string ip = (slash == std::string::npos) ? ipTok : ipTok.substr(0, slash);
      for (auto& m : sdp.media) m.endpoint.ip = ip;
    }
  }
  return sdp;
}

std::string to_sdp(const SdpSession& sdp) {
  std::ostringstream out;
  out << "v=0\r\n";
  out << "o=" << sdp.origin << "\r\n";
  out << "s=" << sdp.session_name << "\r\n";
  out << "c=" << sdp.connection << "\r\n";
  out << "t=0 0\r\n";

  for (const auto& m : sdp.media) {
    const char* media = (m.type == SdpMedia::Type::Video) ? "video" : "audio";
    out << "m=" << media << " " << m.endpoint.udp_port << " RTP/AVP " << int(m.endpoint.payload_type) << "\r\n";
    if (!m.rtpmap.empty()) {
      out << "a=rtpmap:" << int(m.endpoint.payload_type) << " " << m.rtpmap << "\r\n";
    }
    if (!m.fmtp_kv.empty()) {
      out << "a=fmtp:" << int(m.endpoint.payload_type) << " ";
      for (size_t i = 0; i < m.fmtp_kv.size(); i++) {
        if (i) out << "; ";
        out << m.fmtp_kv[i];
      }
      out << "\r\n";
    }
    if (m.ts_refclk) out << "a=ts-refclk:" << *m.ts_refclk << "\r\n";
    if (m.mediaclk) out << "a=mediaclk:" << *m.mediaclk << "\r\n";
  }
  return out.str();
}

SdpSession load_sdp_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_sdp(ss.str());
}

void save_sdp_file(const std::string& path, const SdpSession& sdp) {
  std::ofstream f(path, std::ios::binary);
  auto s = to_sdp(sdp);
  f.write(s.data(), (std::streamsize)s.size());
}

} // namespace mtl_sdk
