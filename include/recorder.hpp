#pragma once

#include "render_settings.hpp"
#include "export.hpp"

#include <Geode/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct AVFormatContext;
struct AVCodec;
struct AVStream;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVFilterContext;
struct AVFilterGraph;

BEGIN_FFMPEG_NAMESPACE_V

/**
 * Encodes raw frames into a video file.
 *
 * Typical use: `init()`, then `writeFrame()` once per frame, then `stop()`.
 * If `stop()` is forgotten, the destructor finalizes the file.
 *
 * ABI NOTE: mods built against older versions of this header allocate `Impl` themselves
 * and call its exported `init`, `stop` and `writeFrame`. The data members of `Impl` and
 * the signatures of those three functions must therefore never change. Only add
 * non-virtual member functions.
 */
class FFMPEG_API_DLL Recorder {
private:
    class FFMPEG_API_DLL Impl {
    public:
        AVFormatContext* m_formatContext = nullptr;
        const AVCodec* m_codec = nullptr;
        AVStream* m_videoStream = nullptr;
        AVCodecContext* m_codecContext = nullptr;
        AVBufferRef* m_hwDevice = nullptr;
        AVFrame* m_frame = nullptr;          // wraps the caller's data, owns no pixel buffer
        AVFrame* m_convertedFrame = nullptr; // only when the pixel format must be converted
        AVFrame* m_filteredFrame = nullptr;  // only when a filter graph is used
        AVPacket* m_packet = nullptr;
        SwsContext* m_swsCtx = nullptr;
        AVFilterGraph* m_filterGraph = nullptr;
        AVFilterContext* m_buffersrcCtx = nullptr;
        AVFilterContext* m_buffersinkCtx = nullptr;
        AVFilterContext* m_colorspaceCtx = nullptr; // unused, kept so the layout stays identical
        AVFilterContext* m_vflipCtx = nullptr;      // unused, kept so the layout stays identical

        size_t m_frameCount = 0;
        size_t m_expectedSize = 0;
        bool m_init = false;

        geode::Result<> init(const RenderSettings& settings);
        void stop();
        geode::Result<> writeFrame(std::span<uint8_t const> frameData);

    private:
        geode::Result<> setup(const RenderSettings& settings);
        geode::Result<> setupFilters(const RenderSettings& settings, int pixelFormat);
        geode::Result<> encode(AVFrame* frame);
        geode::Result<> drainFilters();
        void release();
    };

    std::unique_ptr<Impl> m_impl = nullptr;

public:
    Recorder() = default;
    ~Recorder() { stop(); }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) noexcept = default;

    Recorder& operator=(Recorder&& other) noexcept {
        if (this != &other) {
            stop();
            m_impl = std::move(other.m_impl);
        }
        return *this;
    }

    /**
     * @brief Initializes the Recorder with the specified rendering settings.
     *
     * Validates the settings, opens the encoder and the output file and writes the
     * container header. Nothing is created on disk if this fails. A recording that is
     * still running is finalized first.
     *
     * @param settings The rendering settings that define the output characteristics,
     *                 including codec, bitrate, resolution, and pixel format.
     *
     * @return Ok on success, otherwise an error describing what went wrong.
     */
    geode::Result<> init(const RenderSettings& settings) {
        stop();
        m_impl = std::make_unique<Impl>();
        return m_impl->init(settings);
    }

    /**
     * @brief Stops the recording process and finalizes the output file.
     *
     * Flushes the encoder, writes the container trailer and releases all resources.
     * Safe to call more than once, and on a Recorder that was never initialized.
     */
    void stop() const {
        if (m_impl) {
            m_impl->stop();
        }
    }

    /**
     * @brief Writes a single video frame to the output.
     *
     * @param frameData The raw frame, tightly packed (no row padding), in the pixel
     *                  format and size given in the settings. It is read immediately,
     *                  so the buffer may be reused as soon as this returns.
     *
     * @return Ok on success, otherwise an error (for example when the size of
     *         `frameData` does not match the settings).
     */
    geode::Result<> writeFrame(std::span<uint8_t const> frameData) const {
        if (!m_impl) {
            return geode::Err("Recorder is not initialized.");
        }
        return m_impl->writeFrame(frameData);
    }

    /**
     * @brief Retrieves a list of available codecs for video encoding.
     *
     * @return The names of the available H.264, HEVC, VP8, VP9, AV1 and MPEG-4
     *         encoders of this FFmpeg build, sorted alphabetically.
     */
    static std::vector<std::string> getAvailableCodecs();
};

END_FFMPEG_NAMESPACE_V
