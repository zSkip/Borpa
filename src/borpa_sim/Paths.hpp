#pragma once

#include <filesystem>
#include <string>

namespace Borpa { namespace Sim {

// Paths cross this library as UTF-8 strings; std::filesystem on Windows needs them widened.
inline std::filesystem::path path_from_utf8(const std::string& s) { return std::filesystem::u8path(s); }
inline std::string           path_to_utf8(const std::filesystem::path& p) { return p.u8string(); }

}} // namespace Borpa::Sim
