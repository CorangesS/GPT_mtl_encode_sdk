#pragma once
extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

#include <string>

namespace encode_sdk {
std::string ff_err2str(int err);
}
