#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Borpa { namespace Sim {

std::string base64_encode(const void* data, size_t size);
// Standard alphabet, padding optional. Returns false on any other character.
bool base64_decode(const std::string& text, std::vector<uint8_t>& out);

}} // namespace Borpa::Sim
