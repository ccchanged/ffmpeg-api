#pragma once

#include <filesystem>
#include <string>

namespace ffmpeg::utils {

/// Human readable text for an FFmpeg error code.
std::string getErrorString(int errorCode);

/// UTF-8 encoding of a path, which is what FFmpeg expects on every platform.
std::string pathToUtf8(const std::filesystem::path& path);

}
