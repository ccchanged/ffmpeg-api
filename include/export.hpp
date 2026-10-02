#pragma once

// This mod is built for Android only. Symbols are hidden by default
// (CMAKE_CXX_VISIBILITY_PRESET hidden), so the API has to be exported explicitly.
// The macro is empty for other compilers, so other mods can still include these
// headers (e.g. the event-based API) on any platform.
#if defined(__GNUC__) || defined(__clang__)
    #define FFMPEG_API_DLL __attribute__((visibility("default")))
#else
    #define FFMPEG_API_DLL
#endif

#define FFMPEG_API_VERSION 2

// Self-contained token pasting, independent of the Geode SDK.
#define FFMPEG_API_CONCAT_IMPL(a, b) a##b
#define FFMPEG_API_CONCAT(a, b) FFMPEG_API_CONCAT_IMPL(a, b)

#define FFMPEG_API_VERSION_NS FFMPEG_API_CONCAT(v, FFMPEG_API_VERSION)
#define BEGIN_FFMPEG_NAMESPACE_V \
    namespace ffmpeg { \
    inline namespace FFMPEG_API_VERSION_NS {
#define END_FFMPEG_NAMESPACE_V }}
