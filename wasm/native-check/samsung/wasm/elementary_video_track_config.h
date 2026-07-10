#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "elementary_media_track_config.h"
namespace samsung { namespace wasm {
struct ElementaryVideoTrackConfig : ElementaryMediaTrackConfig {
  ElementaryVideoTrackConfig(std::string, std::vector<uint8_t>, uint32_t, uint32_t, uint32_t, uint32_t) {}
};
}}
