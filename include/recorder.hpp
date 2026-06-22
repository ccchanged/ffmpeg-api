#pragma once

#include "render_settings.hpp"
#include "export.hpp"

#include <Geode/Result.hpp>

#include <vector>
#include <string>
#include <memory>

// FFmpeg types are C structs — forward-declaring as 'class' is UB per the standard
struct AVFormatContext;
struct AVCodec;
struct AVStream;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVFilterContext;
struct AVFilter;
struct AVFilterGraph;

BEGIN_FFMPEG_NAMESPACE_V

class FFMPEG_API_DLL Recorder {
private:
    class Impl {
    public:
        AVFormatContext* m_formatContext  = nullptr;
        const AVCodec*   m_codec          = nullptr;
        AVStream*        m_videoStream    = nullptr;
        AVCodecContext*  m_codecContext   = nullptr;
        AVBufferRef*     m_hwDevice       = nullptr;
        AVFrame*         m_frame          = nullptr;
        AVFrame*         m_convertedFrame = nullptr;
        AVFrame*         m_filteredFrame  = nullptr;
        AVPacket*        m_packet         = nullptr;
        SwsContext*      m_swsCtx         = nullptr;
        AVFilterGraph*   m_filterGraph    = nullptr;
        AVFilterContext* m_buffersrcCtx   = nullptr;
        AVFilterContext* m_buffersinkCtx  = nullptr;
        AVFilterContext* m_colorspaceCtx  = nullptr;
        AVFilterContext* m_vflipCtx       = nullptr;

        size_t m_frameCount   = 0;
        size_t m_expectedSize = 0;
        bool   m_init         = false;

        geode::Result<> init(const RenderSettings& settings);
        void            stop();
        geode::Result<> writeFrame(std::span<uint8_t const> frameData);
        geode::Result<> filterFrame(AVFrame* inputFrame, AVFrame* outputFrame);
    };

    std::unique_ptr<Impl> m_impl;

public:
    /**
     * @brief Initializes the Recorder with the specified rendering settings.
     *
     * Configures the recorder with the given render settings, allocates necessary
     * resources, and prepares for video encoding.
     *
     * @param settings The rendering settings defining codec, bitrate, resolution, and pixel format.
     * @return Ok on success, Err with a description on failure.
     */
    geode::Result<> init(const RenderSettings& settings) {
        m_impl = std::make_unique<Impl>();
        return m_impl->init(settings);
    }

    /**
     * @brief Stops the recording process and finalizes the output file.
     *
     * Flushes all buffered frames, releases allocated resources, and closes the output file.
     */
    void stop() const { m_impl->stop(); }

    /**
     * @brief Writes a single video frame to the output.
     *
     * Encodes the raw frame data to the output file. The frame data must match
     * the format and dimensions defined during initialization.
     *
     * @param frameData Raw frame bytes to encode.
     * @return Ok on success, Err with a description on failure.
     *
     * @warning frameData size must match the expected frame dimensions.
     */
    geode::Result<> writeFrame(std::span<uint8_t const> frameData) const {
        return m_impl->writeFrame(frameData);
    }

    /**
     * @brief Returns a list of available video encoder names.
     *
     * Iterates all FFmpeg codecs and returns the names of supported video encoders.
     */
    static std::vector<std::string> getAvailableCodecs();
};

END_FFMPEG_NAMESPACE_V
