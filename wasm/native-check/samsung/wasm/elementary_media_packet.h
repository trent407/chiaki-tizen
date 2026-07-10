#pragma once
#include <cstddef>
#include "common.h"
namespace samsung { namespace wasm {
struct ElementaryMediaPacket {
  Seconds pts, dts, duration;
  bool is_key_frame;
  size_t data_size;
  const void *data;
  unsigned width, height;
  unsigned framerate_num, framerate_den;
  SessionId session_id;
};
}}
