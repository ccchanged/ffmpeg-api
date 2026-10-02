#include "utils.hpp"

#include <Geode/loader/Log.hpp>
#include <Geode/loader/ModEvent.hpp>

#include <cstdarg>
#include <cstring>

extern "C" {
    #include <libavutil/error.h>
    #include <libavutil/log.h>
}

namespace ffmpeg::utils {

std::string getErrorString(int errorCode) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errorCode, errbuf, sizeof(errbuf));
    return std::string(errbuf);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    auto const u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

namespace {

// FFmpeg is statically linked into this mod, so this callback only ever sees this
// mod's own FFmpeg output. Only warnings and errors are forwarded, the encoders are
// very chatty at info level.
void logCallback(void* ptr, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING)
        return;

    static thread_local int printPrefix = 1;
    char line[1024];
    av_log_format_line2(ptr, level, fmt, args, line, sizeof(line), &printPrefix);

    size_t length = std::strlen(line);
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        line[--length] = '\0';
    if (length == 0)
        return;

    if (level <= AV_LOG_ERROR)
        geode::log::error("[FFmpeg] {}", line);
    else
        geode::log::warn("[FFmpeg] {}", line);
}

}

}

$on_mod(Loaded) {
    av_log_set_callback(ffmpeg::utils::logCallback);
}
