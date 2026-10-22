cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

# Keep the version in sync with mod.json
project(ffmpeg-api VERSION 2.1.0)

# This mod only supports Android: Windows users run the official ffmpeg.exe instead.
if (NOT ANDROID)
    message(FATAL_ERROR "ffmpeg-api only supports Android.")
endif()

if (NOT ANDROID_ABI STREQUAL "arm64-v8a" AND NOT ANDROID_ABI STREQUAL "armeabi-v7a")
    message(FATAL_ERROR "Unsupported Android ABI '${ANDROID_ABI}', expected arm64-v8a or armeabi-v7a.")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# The FFmpeg headers live in include/ as well (libavcodec/, libavutil/, ...)
target_include_directories(${PROJECT_NAME} PRIVATE
    include
)

setup_geode_mod(${PROJECT_NAME})

if (ANDROID_ABI STREQUAL "armeabi-v7a")
    # 64-bit file offsets on 32-bit ARM. This build also needs Android API 24, pass
    # -DANDROID_PLATFORM=24 to geode build (see README).
    target_compile_definitions(${PROJECT_NAME} PRIVATE __USE_FILE_OFFSET64)
endif()

# FFmpeg 8.x static libraries built per ABI, expected in lib/<abi>/ (there is no libpostproc in 8.x)
set(FFMPEG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/${ANDROID_ABI})
set(FFMPEG_LIBS
    avcodec
    avformat
    avutil
    avfilter
    swresample
    swscale
    aom
    vpx
    x264
    x265
)

set(STATIC_LIBS)
foreach (lib IN LISTS FFMPEG_LIBS)
    set(lib_path ${FFMPEG_LIB_DIR}/lib${lib}.a)
    if (NOT EXISTS ${lib_path})
        message(FATAL_ERROR "Missing FFmpeg library: ${lib_path}")
    endif()
    list(APPEND STATIC_LIBS ${lib_path})
endforeach()

# System libraries: zlib (used by libavformat/libavcodec), NDK MediaCodec, Android logging
list(APPEND STATIC_LIBS
    z
    android
    mediandk
    log
)

target_link_libraries(${PROJECT_NAME} ${STATIC_LIBS})
