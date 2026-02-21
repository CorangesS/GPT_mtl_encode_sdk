#include "ffmpeg_utils.hpp"
#include <array>

namespace encode_sdk {

std::string ff_err2str(int err) {
  std::array<char, 256> buf{};
  av_strerror(err, buf.data(), buf.size());
  return std::string(buf.data());
}

} // namespace encode_sdk
