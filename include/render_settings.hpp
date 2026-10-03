#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "export.hpp"

BEGIN_FFMPEG_NAMESPACE_V

/**
 * Pixel formats for the raw frames passed to `Recorder::writeFrame`.
 *
 * The numbers mirror FFmpeg's `AVPixelFormat`; FFmpeg only ever appends new formats,
 * so they stay valid across releases. They are spelled out so editing this list can
 * never shift the others, and `src/pixel_format_check.inc` verifies every one of them
 * against the FFmpeg headers at compile time.
 *
 * Hardware-surface formats (VAAPI, CUDA, MEDIACODEC, ...) are listed only to keep old
 * code compiling. Frames are always plain CPU memory, so using one is rejected with
 * an error.
 */
enum class PixelFormat : int {
    NONE           = -1,
    YUV420P        = 0,
    YUYV422        = 1,
    RGB24          = 2,
    BGR24          = 3,
    YUV422P        = 4,
    YUV444P        = 5,
    YUV410P        = 6,
    YUV411P        = 7,
    GRAY8          = 8,
    MONOWHITE      = 9,
    MONOBLACK      = 10,
    PAL8           = 11,
    YUVJ420P       = 12,
    YUVJ422P       = 13,
    YUVJ444P       = 14,
    UYVY422        = 15,
    UYYVYY411      = 16,
    BGR8           = 17,
    BGR4           = 18,
    BGR4_BYTE      = 19,
    RGB8           = 20,
    RGB4           = 21,
    RGB4_BYTE      = 22,
    NV12           = 23,
    NV21           = 24,
    ARGB           = 25,
    RGBA           = 26,
    ABGR           = 27,
    BGRA           = 28,
    GRAY16BE       = 29,
    GRAY16LE       = 30,
    YUV440P        = 31,
    YUVJ440P       = 32,
    YUVA420P       = 33,
    RGB48BE        = 34,
    RGB48LE        = 35,
    RGB565BE       = 36,
    RGB565LE       = 37,
    RGB555BE       = 38,
    RGB555LE       = 39,
    BGR565BE       = 40,
    BGR565LE       = 41,
    BGR555BE       = 42,
    BGR555LE       = 43,
    VAAPI          = 44,
    YUV420P16LE    = 45,
    YUV420P16BE    = 46,
    YUV422P16LE    = 47,
    YUV422P16BE    = 48,
    YUV444P16LE    = 49,
    YUV444P16BE    = 50,
    DXVA2_VLD      = 51,
    RGB444LE       = 52,
    RGB444BE       = 53,
    BGR444LE       = 54,
    BGR444BE       = 55,
    YA8            = 56,
    Y400A          = 56,
    GRAY8A         = 56,
    BGR48BE        = 57,
    BGR48LE        = 58,
    YUV420P9BE     = 59,
    YUV420P9LE     = 60,
    YUV420P10BE    = 61,
    YUV420P10LE    = 62,
    YUV422P10BE    = 63,
    YUV422P10LE    = 64,
    YUV444P9BE     = 65,
    YUV444P9LE     = 66,
    YUV444P10BE    = 67,
    YUV444P10LE    = 68,
    YUV422P9BE     = 69,
    YUV422P9LE     = 70,
    GBRP           = 71,
    GBR24P         = 71,
    GBRP9BE        = 72,
    GBRP9LE        = 73,
    GBRP10BE       = 74,
    GBRP10LE       = 75,
    GBRP16BE       = 76,
    GBRP16LE       = 77,
    YUVA422P       = 78,
    YUVA444P       = 79,
    YUVA420P9BE    = 80,
    YUVA420P9LE    = 81,
    YUVA422P9BE    = 82,
    YUVA422P9LE    = 83,
    YUVA444P9BE    = 84,
    YUVA444P9LE    = 85,
    YUVA420P10BE   = 86,
    YUVA420P10LE   = 87,
    YUVA422P10BE   = 88,
    YUVA422P10LE   = 89,
    YUVA444P10BE   = 90,
    YUVA444P10LE   = 91,
    YUVA420P16BE   = 92,
    YUVA420P16LE   = 93,
    YUVA422P16BE   = 94,
    YUVA422P16LE   = 95,
    YUVA444P16BE   = 96,
    YUVA444P16LE   = 97,
    VDPAU          = 98,
    XYZ12LE        = 99,
    XYZ12BE        = 100,
    NV16           = 101,
    NV20LE         = 102,
    NV20BE         = 103,
    RGBA64BE       = 104,
    RGBA64LE       = 105,
    BGRA64BE       = 106,
    BGRA64LE       = 107,
    YVYU422        = 108,
    YA16BE         = 109,
    YA16LE         = 110,
    GBRAP          = 111,
    GBRAP16BE      = 112,
    GBRAP16LE      = 113,
    QSV            = 114,
    MMAL           = 115,
    D3D11VA_VLD    = 116,
    CUDA           = 117,
    _0RGB          = 118,
    RGB0           = 119,
    _0BGR          = 120,
    BGR0           = 121,
    YUV420P12BE    = 122,
    YUV420P12LE    = 123,
    YUV420P14BE    = 124,
    YUV420P14LE    = 125,
    YUV422P12BE    = 126,
    YUV422P12LE    = 127,
    YUV422P14BE    = 128,
    YUV422P14LE    = 129,
    YUV444P12BE    = 130,
    YUV444P12LE    = 131,
    YUV444P14BE    = 132,
    YUV444P14LE    = 133,
    GBRP12BE       = 134,
    GBRP12LE       = 135,
    GBRP14BE       = 136,
    GBRP14LE       = 137,
    YUVJ411P       = 138,
    BAYER_BGGR8    = 139,
    BAYER_RGGB8    = 140,
    BAYER_GBRG8    = 141,
    BAYER_GRBG8    = 142,
    BAYER_BGGR16LE = 143,
    BAYER_BGGR16BE = 144,
    BAYER_RGGB16LE = 145,
    BAYER_RGGB16BE = 146,
    BAYER_GBRG16LE = 147,
    BAYER_GBRG16BE = 148,
    BAYER_GRBG16LE = 149,
    BAYER_GRBG16BE = 150,
    YUV440P10LE    = 151,
    YUV440P10BE    = 152,
    YUV440P12LE    = 153,
    YUV440P12BE    = 154,
    AYUV64LE       = 155,
    AYUV64BE       = 156,
    VIDEOTOOLBOX   = 157,
    P010LE         = 158,
    P010BE         = 159,
    GBRAP12BE      = 160,
    GBRAP12LE      = 161,
    GBRAP10BE      = 162,
    GBRAP10LE      = 163,
    MEDIACODEC     = 164,
    GRAY12BE       = 165,
    GRAY12LE       = 166,
    GRAY10BE       = 167,
    GRAY10LE       = 168,
    P016LE         = 169,
    P016BE         = 170,
    D3D11          = 171,
    GRAY9BE        = 172,
    GRAY9LE        = 173,
    GBRPF32BE      = 174,
    GBRPF32LE      = 175,
    GBRAPF32BE     = 176,
    GBRAPF32LE     = 177,
    DRM_PRIME      = 178,
    OPENCL         = 179,
    GRAY14BE       = 180,
    GRAY14LE       = 181,
    GRAYF32BE      = 182,
    GRAYF32LE      = 183,
    YUVA422P12BE   = 184,
    YUVA422P12LE   = 185,
    YUVA444P12BE   = 186,
    YUVA444P12LE   = 187,
    NV24           = 188,
    NV42           = 189,
    VULKAN         = 190,
    Y210BE         = 191,
    Y210LE         = 192,
    X2RGB10LE      = 193,
    X2RGB10BE      = 194,
    X2BGR10LE      = 195,
    X2BGR10BE      = 196,
    P210BE         = 197,
    P210LE         = 198,
    P410BE         = 199,
    P410LE         = 200,
    P216BE         = 201,
    P216LE         = 202,
    P416BE         = 203,
    P416LE         = 204,
    VUYA           = 205,
    RGBAF16BE      = 206,
    RGBAF16LE      = 207,
    VUYX           = 208,
    P012LE         = 209,
    P012BE         = 210,
    Y212BE         = 211,
    Y212LE         = 212,
    XV30BE         = 213,
    XV30LE         = 214,
    XV36BE         = 215,
    XV36LE         = 216,
    RGBF32BE       = 217,
    RGBF32LE       = 218,
    RGBAF32BE      = 219,
    RGBAF32LE      = 220,
    P212BE         = 221,
    P212LE         = 222,
    P412BE         = 223,
    P412LE         = 224,
    GBRAP14BE      = 225,
    GBRAP14LE      = 226,
    D3D12          = 227,
    NB             = 228,
};

/**
 * Hardware device handed to the encoder (`NONE` = software only).
 *
 * Kept for API compatibility. CUDA and D3D11VA do not exist on Android, so requesting
 * them fails with an error; leave this at `NONE` (the MediaCodec encoders work without it).
 */
enum class HardwareAccelerationType : int {
    NONE = 0,
    CUDA = 2,
    D3D11VA = 7,
};

// NOTE: the layout of RenderSettings is part of the ABI, mods built against older versions
// pass it to the API. Do not add, remove, reorder or retype members.
struct RenderSettings {
    /// Hardware device for the encoder, see above. Leave at `NONE` on Android.
    HardwareAccelerationType m_hardwareAccelerationType = HardwareAccelerationType::NONE;

    /// Layout of the frames passed to `writeFrame`. Converted automatically when the
    /// codec does not support it.
    PixelFormat m_pixelFormat = PixelFormat::RGB0;

    /// Encoder name, e.g. "libx264" or "h264_mediacodec" (see `Recorder::getAvailableCodecs`).
    std::string m_codec;

    /// Optional arguments for FFmpeg's `colorspace` filter, e.g. "all=bt709:iall=bt601-6-625".
    /// Empty = no filter (RGB input is then converted and tagged as BT.709).
    std::string m_colorspaceFilters;

    /// Flip the image vertically (OpenGL framebuffers are stored upside down). On by default.
    bool m_doVerticalFlip = true;

    /// Target bitrate in bits per second.
    int64_t m_bitrate = 30000000;

    /// Frame size in pixels. Must match the frames passed to `writeFrame`.
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;

    /// Frames per second. Must be greater than zero.
    uint16_t m_fps = 60;

    /// Output file; the container is chosen from its extension.
    std::filesystem::path m_outputFile;
};

END_FFMPEG_NAMESPACE_V
