#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "common.h"  // SampleFormat, ChannelLayout live here (single definition)
#include "elementary_media_track_config.h"
namespace samsung { namespace wasm {
struct ElementaryAudioTrackConfig : ElementaryMediaTrackConfig {
  ElementaryAudioTrackConfig(std::string, std::vector<uint8_t>, SampleFormat, ChannelLayout, uint32_t) {}
};
}}
