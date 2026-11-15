#pragma once

#include <string>

// Keeps the recording being written out of the page cache (dvr_cache.cpp):
// a background thread writes back all but the last MiB of the file and drops
// it, and the rest whenever the file stops growing for ~2 s.
namespace dvr_cache {

// The file a muxer just started writing; replaces the previous one.
void watch(const std::string &path);

}
