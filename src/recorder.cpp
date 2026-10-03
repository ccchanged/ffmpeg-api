#include "recorder.hpp"
#include "utils.hpp"

#include <Geode/loader/Log.hpp>

#include <algorithm>
#include <cstdio>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavformat/avformat.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
}

BEGIN_FFMPEG_NAMESPACE_V

// PixelFormat mirrors AVPixelFormat numerically (FFmpeg only ever appends new formats).
// Verify that at compile time against whatever FFmpeg headers this is built with.
#define PIXFMT_CHECK(mine, av) \
    static_assert(static_cast<int>(PixelFormat::mine) == static_cast<int>(AV_PIX_FMT_##av), \
                  "PixelFormat::" #mine " no longer matches AVPixelFormat, regenerate pixel_format_check.inc and the enum");
#include "pixel_format_check.inc"
#undef PIXFMT_CHECK

namespace {

/// Pixel formats the encoder lists as supported (empty = the encoder lists none).
std::vector<AVPixelFormat> getSupportedPixelFormats(const AVCodec* codec) {
    std::vector<AVPixelFormat> formats;
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) >= 0 && configs) {
        auto const* list = static_cast<const AVPixelFormat*>(configs);
        formats.assign(list, list + count);
    }
    return formats;
}

bool isHardwareFormat(AVPixelFormat format) {
    auto const* desc = av_pix_fmt_desc_get(format);
    return desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

bool isRgbFormat(AVPixelFormat format) {
    auto const* desc = av_pix_fmt_desc_get(format);
    return desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
}

/// Validated conversion from the public enum to FFmpeg's.
geode::Result<> toAVPixelFormat(PixelFormat format, AVPixelFormat& out) {
    int const value = static_cast<int>(format);
    if (value <= static_cast<int>(AV_PIX_FMT_NONE) || value >= static_cast<int>(AV_PIX_FMT_NB))
        return geode::Err("Invalid input pixel format.");

    auto const avFormat = static_cast<AVPixelFormat>(value);
    auto const* desc = av_pix_fmt_desc_get(avFormat);
    if (!desc)
        return geode::Err("Unknown input pixel format.");
    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)
        return geode::Err("Hardware pixel formats cannot be used for raw input frames.");

    out = avFormat;
    return geode::Ok();
}

/// Maps the public enum to FFmpeg's by name, so it never depends on FFmpeg's numbering.
geode::Result<> toAVHWDeviceType(HardwareAccelerationType type, AVHWDeviceType& out) {
    switch (type) {
        case HardwareAccelerationType::NONE:
            out = AV_HWDEVICE_TYPE_NONE;
            return geode::Ok();
        case HardwareAccelerationType::CUDA:
            out = AV_HWDEVICE_TYPE_CUDA;
            return geode::Ok();
        case HardwareAccelerationType::D3D11VA:
            out = AV_HWDEVICE_TYPE_D3D11VA;
            return geode::Ok();
    }
    return geode::Err("Unknown hardware acceleration type.");
}

}

std::vector<std::string> Recorder::getAvailableCodecs() {
    std::vector<std::string> codecs;

    void* iter = nullptr;
    while (const AVCodec* codec = av_codec_iterate(&iter)) {
        if (codec->type != AVMEDIA_TYPE_VIDEO || !av_codec_is_encoder(codec))
            continue;

        switch (codec->id) {
            case AV_CODEC_ID_H264:
            case AV_CODEC_ID_HEVC:
            case AV_CODEC_ID_VP8:
            case AV_CODEC_ID_VP9:
            case AV_CODEC_ID_AV1:
            case AV_CODEC_ID_MPEG4:
                break;
            default:
                continue;
        }

        if (getSupportedPixelFormats(codec).empty())
            continue;

        codecs.emplace_back(codec->name);
    }

    std::ranges::sort(codecs);
    codecs.erase(std::unique(codecs.begin(), codecs.end()), codecs.end());
    return codecs;
}

// ---------------------------------------------------------------------------
// Recorder::Impl
// ---------------------------------------------------------------------------

geode::Result<> Recorder::Impl::init(const RenderSettings& settings) {
    // Mods built against older versions allocate Impl themselves. Adding or changing a data
    // member would make them write past their allocation, so fail the build instead.
    static_assert(sizeof(Impl) == 18 * sizeof(void*), "Recorder::Impl layout is part of the ABI, see recorder.hpp");

    auto res = setup(settings);
    if (res.isErr())
        release();
    return res;
}

geode::Result<> Recorder::Impl::setup(const RenderSettings& settings) {
    int ret = 0;

    // --- validate the settings before touching FFmpeg or the disk ---------------
    if (settings.m_codec.empty())
        return geode::Err("No codec was specified.");
    if (settings.m_outputFile.empty())
        return geode::Err("No output file was specified.");
    if (settings.m_fps == 0)
        return geode::Err("The frame rate must be greater than zero.");
    if (av_image_check_size(settings.m_width, settings.m_height, 0, nullptr) < 0) {
        return geode::Err("Invalid video size: " + std::to_string(settings.m_width) + "x" +
                          std::to_string(settings.m_height) + ".");
    }

    AVPixelFormat inputFormat = AV_PIX_FMT_NONE;
    if (auto res = toAVPixelFormat(settings.m_pixelFormat, inputFormat); res.isErr())
        return res;

    AVHWDeviceType hwDeviceType = AV_HWDEVICE_TYPE_NONE;
    if (auto res = toAVHWDeviceType(settings.m_hardwareAccelerationType, hwDeviceType); res.isErr())
        return res;

    m_codec = avcodec_find_encoder_by_name(settings.m_codec.c_str());
    if (!m_codec || m_codec->type != AVMEDIA_TYPE_VIDEO)
        return geode::Err("Could not find video encoder '" + settings.m_codec + "'.");

    // --- pick the pixel format the encoder will receive --------------------------
    std::vector<AVPixelFormat> const supported = getSupportedPixelFormats(m_codec);
    std::vector<AVPixelFormat> softwareFormats;
    bool usesMediaCodecSurfaces = false;
    for (AVPixelFormat format : supported) {
        if (format == AV_PIX_FMT_MEDIACODEC)
            usesMediaCodecSurfaces = true;
        else if (!isHardwareFormat(format))
            softwareFormats.push_back(format);
    }

    AVPixelFormat encoderFormat = AV_PIX_FMT_NONE;
    if (usesMediaCodecSurfaces) {
        // Secretly force NV12: with AV_PIX_FMT_MEDIACODEC the encoder would go into
        // surface mode and expect a surface instead of plain frames.
        encoderFormat = AV_PIX_FMT_NV12;
    } else if (std::ranges::find(softwareFormats, inputFormat) != softwareFormats.end()) {
        encoderFormat = inputFormat;
    } else if (!softwareFormats.empty()) {
        encoderFormat = softwareFormats.front();
    } else {
        return geode::Err("Codec '" + settings.m_codec + "' does not support any pixel format usable with raw frames.");
    }
    geode::log::debug("Encoder {} will receive pixel format {}", settings.m_codec, av_get_pix_fmt_name(encoderFormat));

    // RGB -> YUV conversion done by us is tagged as BT.709 (what players assume for HD).
    // When the user supplies colorspace filters, they own the colour handling instead.
    bool const tagBt709 = settings.m_colorspaceFilters.empty() && isRgbFormat(inputFormat) && !isRgbFormat(encoderFormat);

    // --- container and encoder -----------------------------------------------------
    std::string const outputPath = utils::pathToUtf8(settings.m_outputFile);

    ret = avformat_alloc_output_context2(&m_formatContext, nullptr, nullptr, outputPath.c_str());
    if (!m_formatContext)
        return geode::Err("Could not create output context: " + utils::getErrorString(ret));

    m_videoStream = avformat_new_stream(m_formatContext, m_codec);
    if (!m_videoStream)
        return geode::Err("Could not create video stream.");

    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext)
        return geode::Err("Could not allocate video codec context.");

    if (hwDeviceType != AV_HWDEVICE_TYPE_NONE) {
        ret = av_hwdevice_ctx_create(&m_hwDevice, hwDeviceType, nullptr, nullptr, 0);
        if (ret < 0)
            return geode::Err("Could not create hardware device context: " + utils::getErrorString(ret));

        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDevice);
        if (!m_codecContext->hw_device_ctx)
            return geode::Err("Could not reference the hardware device context.");
    }

    AVRational const frameRate{static_cast<int>(settings.m_fps), 1};
    if (settings.m_bitrate > 0)
        m_codecContext->bit_rate = settings.m_bitrate;
    m_codecContext->width = static_cast<int>(settings.m_width);
    m_codecContext->height = static_cast<int>(settings.m_height);
    m_codecContext->time_base = AVRational{1, static_cast<int>(settings.m_fps)};
    m_codecContext->framerate = frameRate;
    m_codecContext->sample_aspect_ratio = AVRational{1, 1};
    m_codecContext->pix_fmt = encoderFormat;
    if (tagBt709) {
        m_codecContext->colorspace = AVCOL_SPC_BT709;
        m_codecContext->color_primaries = AVCOL_PRI_BT709;
        m_codecContext->color_trc = AVCOL_TRC_BT709;
        m_codecContext->color_range = AVCOL_RANGE_MPEG;
    }

    // Must be set before the encoder is opened, or it will not emit global headers.
    if (m_formatContext->oformat->flags & AVFMT_GLOBALHEADER)
        m_codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_codecContext, m_codec, nullptr);
    if (ret < 0)
        return geode::Err("Could not open codec: " + utils::getErrorString(ret));

    ret = avcodec_parameters_from_context(m_videoStream->codecpar, m_codecContext);
    if (ret < 0)
        return geode::Err("Could not copy codec parameters: " + utils::getErrorString(ret));

    m_videoStream->time_base = m_codecContext->time_base;
    m_videoStream->avg_frame_rate = frameRate;
    m_videoStream->r_frame_rate = frameRate;
    m_videoStream->sample_aspect_ratio = m_codecContext->sample_aspect_ratio;

    // --- frames, packet, converter, filters -------------------------------------------
    // m_frame only points at the caller's data in writeFrame, so no pixel buffer is allocated.
    m_frame = av_frame_alloc();
    m_filteredFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_filteredFrame || !m_packet)
        return geode::Err("Could not allocate frames or packet.");

    m_frame->format = inputFormat;
    m_frame->width = m_codecContext->width;
    m_frame->height = m_codecContext->height;

    int const expectedSize = av_image_get_buffer_size(inputFormat, m_frame->width, m_frame->height, 1);
    if (expectedSize < 0)
        return geode::Err("Could not compute the frame size: " + utils::getErrorString(expectedSize));
    m_expectedSize = static_cast<size_t>(expectedSize);

    // If the codec does not support the input pixel format, frames are converted in writeFrame.
    if (inputFormat != encoderFormat) {
        m_swsCtx = sws_getContext(
            m_frame->width, m_frame->height, inputFormat,
            m_frame->width, m_frame->height, encoderFormat,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_swsCtx)
            return geode::Err("Could not create sws context.");

        if (tagBt709) {
            // RGB input is full range; the output is limited range YUV using BT.709 coefficients.
            if (sws_setColorspaceDetails(m_swsCtx, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                         sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16) < 0) {
                geode::log::warn("The pixel format conversion does not support BT.709 coefficients, colors may be slightly off.");
            }
        }

        m_convertedFrame = av_frame_alloc();
        if (!m_convertedFrame)
            return geode::Err("Could not allocate the converted frame.");

        m_convertedFrame->format = encoderFormat;
        m_convertedFrame->width = m_frame->width;
        m_convertedFrame->height = m_frame->height;
        ret = av_frame_get_buffer(m_convertedFrame, 0);
        if (ret < 0)
            return geode::Err("Could not allocate the converted frame buffer: " + utils::getErrorString(ret));

        if (tagBt709) {
            m_convertedFrame->colorspace = AVCOL_SPC_BT709;
            m_convertedFrame->color_primaries = AVCOL_PRI_BT709;
            m_convertedFrame->color_trc = AVCOL_TRC_BT709;
            m_convertedFrame->color_range = AVCOL_RANGE_MPEG;
        }
    }

    if (!settings.m_colorspaceFilters.empty() || settings.m_doVerticalFlip) {
        if (auto res = setupFilters(settings, static_cast<int>(encoderFormat)); res.isErr())
            return res;
    }

    // --- everything is ready: only now create the file and write the header -----------
    if (!(m_formatContext->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_formatContext->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
            return geode::Err("Could not open output file: " + utils::getErrorString(ret));
    }

    ret = avformat_write_header(m_formatContext, nullptr);
    if (ret < 0)
        return geode::Err("Could not write header: " + utils::getErrorString(ret));

    m_frameCount = 0;
    m_init = true;
    return geode::Ok();
}

geode::Result<> Recorder::Impl::setupFilters(const RenderSettings& settings, int pixelFormat) {
    auto const format = static_cast<AVPixelFormat>(pixelFormat);
    m_filterGraph = avfilter_graph_alloc();
    if (!m_filterGraph)
        return geode::Err("Could not allocate filter graph.");

    const AVFilter* buffersrc = avfilter_get_by_name("buffer");
    const AVFilter* buffersink = avfilter_get_by_name("buffersink");
    const AVFilter* formatFilter = avfilter_get_by_name("format");
    if (!buffersrc || !buffersink || !formatFilter)
        return geode::Err("This FFmpeg build is missing the buffer, buffersink or format filter.");

    char args[512];
    std::snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:frame_rate=%d/%d:pixel_aspect=%d/%d",
        m_codecContext->width, m_codecContext->height, static_cast<int>(format),
        m_codecContext->time_base.num, m_codecContext->time_base.den,
        m_codecContext->framerate.num, m_codecContext->framerate.den,
        m_codecContext->sample_aspect_ratio.num, m_codecContext->sample_aspect_ratio.den);

    int ret = avfilter_graph_create_filter(&m_buffersrcCtx, buffersrc, "in", args, nullptr, m_filterGraph);
    if (ret < 0)
        return geode::Err("Could not create input for filter graph: " + utils::getErrorString(ret));

    ret = avfilter_graph_create_filter(&m_buffersinkCtx, buffersink, "out", nullptr, nullptr, m_filterGraph);
    if (ret < 0)
        return geode::Err("Could not create output for filter graph: " + utils::getErrorString(ret));

    // Chain: in -> [colorspace] -> [vflip] -> format -> out. Every filter is linked after
    // the previous one, so both optional filters can be active at the same time.
    AVFilterContext* last = m_buffersrcCtx;
    auto append = [&](const char* filterName, const char* instanceName, const char* filterArgs) -> geode::Result<> {
        const AVFilter* filter = avfilter_get_by_name(filterName);
        if (!filter)
            return geode::Err(std::string("This FFmpeg build is missing the '") + filterName + "' filter.");

        AVFilterContext* ctx = nullptr;
        int r = avfilter_graph_create_filter(&ctx, filter, instanceName, filterArgs, nullptr, m_filterGraph);
        if (r < 0)
            return geode::Err(std::string("Could not create the '") + filterName + "' filter: " + utils::getErrorString(r));

        r = avfilter_link(last, 0, ctx, 0);
        if (r < 0)
            return geode::Err(std::string("Could not link the '") + filterName + "' filter: " + utils::getErrorString(r));

        last = ctx;
        return geode::Ok();
    };

    if (!settings.m_colorspaceFilters.empty()) {
        if (auto res = append("colorspace", "colorspace", settings.m_colorspaceFilters.c_str()); res.isErr())
            return res;
    }

    if (settings.m_doVerticalFlip) {
        if (auto res = append("vflip", "vflip", nullptr); res.isErr())
            return res;
    }

    // Pin the output to the format the encoder was opened with.
    std::string const formatArgs = std::string("pix_fmts=") + av_get_pix_fmt_name(format);
    if (auto res = append("format", "format", formatArgs.c_str()); res.isErr())
        return res;

    ret = avfilter_link(last, 0, m_buffersinkCtx, 0);
    if (ret < 0)
        return geode::Err("Could not link the filter graph output: " + utils::getErrorString(ret));

    ret = avfilter_graph_config(m_filterGraph, nullptr);
    if (ret < 0)
        return geode::Err("Could not configure filter graph: " + utils::getErrorString(ret));

    return geode::Ok();
}

geode::Result<> Recorder::Impl::writeFrame(std::span<uint8_t const> frameData) {
    if (!m_init || !m_frame)
        return geode::Err("Recorder is not initialized.");

    if (frameData.size() != m_expectedSize) {
        return geode::Err("Frame data size (" + std::to_string(frameData.size()) +
                          " bytes) does not match the expected size (" + std::to_string(m_expectedSize) + " bytes).");
    }

    int ret = av_image_fill_arrays(
        m_frame->data,
        m_frame->linesize,
        frameData.data(),
        static_cast<AVPixelFormat>(m_frame->format),
        m_frame->width,
        m_frame->height,
        1
    );
    if (ret < 0)
        return geode::Err("Failed to fill image arrays: " + utils::getErrorString(ret));

    int64_t const pts = static_cast<int64_t>(m_frameCount++);
    AVFrame* frame = m_frame;
    frame->pts = pts;

    if (m_swsCtx) {
        // The encoder may still hold a reference to the previous converted frame.
        ret = av_frame_make_writable(m_convertedFrame);
        if (ret < 0)
            return geode::Err("Could not make the converted frame writable: " + utils::getErrorString(ret));

        int const scaled = sws_scale(
            m_swsCtx, m_frame->data, m_frame->linesize, 0, m_frame->height,
            m_convertedFrame->data, m_convertedFrame->linesize);
        if (scaled != m_frame->height)
            return geode::Err("Failed to convert the frame to the encoder's pixel format.");

        m_convertedFrame->pts = pts;
        frame = m_convertedFrame;
    }

    if (m_buffersrcCtx) {
        // KEEP_REF makes the filter graph copy/reference the frame instead of moving it.
        ret = av_buffersrc_add_frame_flags(m_buffersrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0)
            return geode::Err("Could not feed frame to filter graph: " + utils::getErrorString(ret));

        return drainFilters();
    }

    // Not reference counted (m_frame): the encoder copies the data during send_frame.
    return encode(frame);
}

geode::Result<> Recorder::Impl::drainFilters() {
    while (true) {
        int const ret = av_buffersink_get_frame(m_buffersinkCtx, m_filteredFrame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return geode::Ok();
        if (ret < 0)
            return geode::Err("Could not retrieve frame from filter graph: " + utils::getErrorString(ret));

        auto res = encode(m_filteredFrame);
        av_frame_unref(m_filteredFrame);
        if (res.isErr())
            return res;
    }
}

/// Sends a frame to the encoder and writes every packet it produces.
/// Passing nullptr flushes the encoder.
geode::Result<> Recorder::Impl::encode(AVFrame* frame) {
    int ret = avcodec_send_frame(m_codecContext, frame);
    if (ret < 0 && ret != AVERROR_EOF)
        return geode::Err("Error while sending frame: " + utils::getErrorString(ret));

    while (true) {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return geode::Ok();
        if (ret < 0)
            return geode::Err("Error while receiving packet: " + utils::getErrorString(ret));

        av_packet_rescale_ts(m_packet, m_codecContext->time_base, m_videoStream->time_base);
        m_packet->stream_index = m_videoStream->index;

        // The muxer takes ownership of the packet, even when writing fails.
        ret = av_interleaved_write_frame(m_formatContext, m_packet);
        if (ret < 0)
            return geode::Err("Error while writing packet: " + utils::getErrorString(ret));
    }
}

void Recorder::Impl::stop() {
    if (m_init) {
        m_init = false;

        if (m_buffersrcCtx) {
            int const ret = av_buffersrc_add_frame(m_buffersrcCtx, nullptr);
            if (ret < 0)
                geode::log::warn("Could not flush the filter graph: {}", utils::getErrorString(ret));
            else if (auto res = drainFilters(); res.isErr())
                geode::log::warn("Could not drain the filter graph: {}", res.unwrapErr());
        }

        if (auto res = encode(nullptr); res.isErr())
            geode::log::warn("Could not flush the encoder: {}", res.unwrapErr());

        int const ret = av_write_trailer(m_formatContext);
        if (ret < 0)
            geode::log::warn("Could not write the trailer: {}", utils::getErrorString(ret));
    }

    release();
}

/// Frees everything. Safe to call repeatedly and on a partially initialized recorder.
void Recorder::Impl::release() {
    if (m_formatContext) {
        if (!(m_formatContext->oformat->flags & AVFMT_NOFILE))
            avio_closep(&m_formatContext->pb);
        avformat_free_context(m_formatContext);
        m_formatContext = nullptr;
        m_videoStream = nullptr;
    }

    avcodec_free_context(&m_codecContext);
    av_frame_free(&m_frame);
    av_frame_free(&m_convertedFrame);
    av_frame_free(&m_filteredFrame);
    av_packet_free(&m_packet);

    sws_freeContext(m_swsCtx);
    m_swsCtx = nullptr;

    avfilter_graph_free(&m_filterGraph); // also frees the filters it owns
    m_buffersrcCtx = nullptr;
    m_buffersinkCtx = nullptr;

    av_buffer_unref(&m_hwDevice);

    m_codec = nullptr;
    m_init = false;
}

END_FFMPEG_NAMESPACE_V
