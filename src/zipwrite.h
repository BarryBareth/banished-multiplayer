#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal .zip writer for bug reports: deflate (LZ77 + the fixed Huffman codes, RFC 1951) or stored when that
// isn't smaller. No third-party code; any unzip (Explorer, Python zipfile) reads it.
namespace zipwrite {
struct Entry {
    std::string name;            // path inside the zip, '/' separators
    std::vector<uint8_t> data;
};
std::vector<uint8_t> Deflate(const uint8_t* data, size_t size);  // raw deflate stream
uint32_t Crc32(const uint8_t* data, size_t size);
std::vector<uint8_t> Build(const std::vector<Entry>& entries);   // the whole .zip file
}
