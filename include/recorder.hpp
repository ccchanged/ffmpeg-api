#pragma once

#include "render_settings.hpp"
#include "export.hpp"

#include <Geode/Result.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

BEGIN_FFMPEG_NAMESPACE_V

/**
 * Encodes raw frames into a video file.
 *
 * All FFmpeg state lives behind the pimpl in the FFmpeg API mod, so this header
 * needs no FFmpeg includes and every method is exported from the mod's DLL.
 *
 * Typical use: `init()`, then `writeFrame()` once per frame, then `stop()`.
 * If `stop()` is forgotten the destructor finalizes the file, but calling it
 * yourself is preferred so nothing happens implicitly.
 */
class FFMPEG_API_DLL Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder(Recorder&&) noexcept;
    Recorder& operator=(Recorder&&) noexcept;

    /**
     * @brief Initializes the Recorder with the specified rendering settings.
     *
     * Validates the settings, opens the encoder and the output file and writes the
     * container header. Nothing is created on disk if validation or encoder setup fails.
     *
     * @param settings The rendering settings that define the output characteristics,
     *                 including codec, bitrate, resolution, and pixel format.
     *
     * @return Ok on success, otherwise an error describing what went wrong.
     *         Fails if this Recorder is still recording; call `stop()` first.
     */
    [[nodiscard]] geode::Result<> init(const RenderSettings& settings);

    /**
     * @brief Stops the recording process and finalizes the output file.
     *
     * Flushes the encoder, writes the container trailer and releases every resource.
     * Safe to call more than once, and safe to call on a Recorder that was never
     * initialized.
     */
    void stop();

    /**
     * @brief Writes a single video frame to the output.
     *
     * The data is read immediately and not kept, so the buffer may be reused as soon
     * as this returns.
     *
     * @param frameData The raw frame, tightly packed (no row padding), in the pixel
     *                  format and size given in the settings.
     *
     * @return Ok on success, otherwise an error (for example when the size of
     *         `frameData` does not match the settings).
     */
    [[nodiscard]] geode::Result<> writeFrame(std::span<uint8_t const> frameData);

    /**
     * @brief Retrieves a list of available codecs for video encoding.
     *
     * Returns the names of the supported H.264, HEVC, VP8, VP9, AV1 and MPEG-4
     * encoders of this FFmpeg build, sorted alphabetically.
     */
    [[nodiscard]] static std::vector<std::string> getAvailableCodecs();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

END_FFMPEG_NAMESPACE_V
