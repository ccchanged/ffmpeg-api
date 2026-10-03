#pragma once

#include "render_settings.hpp"

#include <Geode/Result.hpp>
#include <Geode/loader/Event.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ffmpeg::events {
namespace impl {
    constexpr size_t VTABLE_VERSION = 1;
    using CreateRecorder_t = void*(*)();
    using DeleteRecorder_t = void(*)(void*);
    using InitRecorder_t = geode::Result<>(*)(void*, const RenderSettings&);
    using StopRecorder_t = void(*)(void*);
    using WriteFrame_t = geode::Result<>(*)(void*, std::span<uint8_t const>);
    using GetAvailableCodecs_t = std::vector<std::string>(*)();
    using MixVideoAudio_t = geode::Result<>(*)(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&);
    using MixVideoRaw_t = geode::Result<>(*)(const std::filesystem::path&, std::span<float>, const std::filesystem::path&);

    struct VTable {
        CreateRecorder_t createRecorder = nullptr;
        DeleteRecorder_t deleteRecorder = nullptr;
        InitRecorder_t initRecorder = nullptr;
        StopRecorder_t stopRecorder = nullptr;
        WriteFrame_t writeFrame = nullptr;
        GetAvailableCodecs_t getAvailableCodecs = nullptr;
        MixVideoAudio_t mixVideoAudio = nullptr;
        MixVideoRaw_t mixVideoRaw = nullptr;
    };

    struct FetchVTableEvent : geode::Event<FetchVTableEvent, bool(VTable&, size_t)> {
        using Event::Event;
    };

    /**
     * The function table of the FFmpeg API mod, or an empty table if the mod is not
     * loaded (or speaks another table version). Thread-safe; asks again on the next call
     * as long as no table was received yet.
     */
    inline const VTable& getVTable() {
        static const VTable empty{};
        static VTable vtable;
        static std::atomic<bool> available{false};
        static std::mutex mutex;

        if (!available.load(std::memory_order_acquire)) {
            std::lock_guard lock(mutex);
            if (!available.load(std::memory_order_relaxed)) {
                VTable fetched;
                if (FetchVTableEvent().send(fetched, VTABLE_VERSION)) {
                    vtable = fetched;
                    available.store(true, std::memory_order_release);
                }
            }
        }
        return available.load(std::memory_order_acquire) ? vtable : empty;
    }
}

/**
 * Encodes raw frames into a video file through the FFmpeg API mod.
 * Move-only. The recording is finalized when the object is destroyed, but call
 * `stop()` yourself when you are done.
 */
class Recorder {
public:
    Recorder() {
        auto const& vtable = impl::getVTable();
        if (vtable.createRecorder) {
            m_ptr = vtable.createRecorder();
        }
    }

    ~Recorder() { destroy(); }

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    Recorder(Recorder&& other) noexcept : m_ptr(std::exchange(other.m_ptr, nullptr)) {}

    Recorder& operator=(Recorder&& other) noexcept {
        if (this != &other) {
            destroy();
            m_ptr = std::exchange(other.m_ptr, nullptr);
        }
        return *this;
    }

    /// False if the FFmpeg API mod is not available.
    bool isValid() const { return m_ptr != nullptr; }

    /**
     * @brief Initializes the Recorder with the specified rendering settings.
     *
     * Validates the settings, opens the encoder and the output file and writes the
     * container header. Nothing is created on disk if this fails.
     *
     * @param settings The rendering settings that define the output characteristics,
     *                 including codec, bitrate, resolution, and pixel format.
     *
     * @return Ok on success, otherwise an error describing what went wrong.
     */
    geode::Result<> init(RenderSettings const& settings) {
        auto const& vtable = impl::getVTable();
        if (!m_ptr || !vtable.initRecorder) {
            return geode::Err("FFmpeg API is not available.");
        }
        return vtable.initRecorder(m_ptr, settings);
    }

    /**
     * @brief Stops the recording process and finalizes the output file.
     *
     * Flushes the encoder, writes the container trailer and releases all resources.
     * Safe to call more than once.
     */
    void stop() {
        auto const& vtable = impl::getVTable();
        if (m_ptr && vtable.stopRecorder) {
            vtable.stopRecorder(m_ptr);
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
    geode::Result<> writeFrame(std::span<uint8_t const> frameData) {
        auto const& vtable = impl::getVTable();
        if (!m_ptr || !vtable.writeFrame) {
            return geode::Err("FFmpeg API is not available.");
        }
        return vtable.writeFrame(m_ptr, frameData);
    }

    /**
     * @brief Retrieves a list of available codecs for video encoding.
     *
     * @return The names of the available encoders, sorted alphabetically. Empty if the
     *         FFmpeg API mod is not available.
     */
    static std::vector<std::string> getAvailableCodecs() {
        auto const& vtable = impl::getVTable();
        if (!vtable.getAvailableCodecs) {
            return {};
        }
        return vtable.getAvailableCodecs();
    }

private:
    void destroy() {
        if (m_ptr) {
            auto const& vtable = impl::getVTable();
            if (vtable.deleteRecorder) {
                vtable.deleteRecorder(m_ptr);
            }
            m_ptr = nullptr;
        }
    }

    void* m_ptr = nullptr;
};

/**
 * Muxes audio into a video file through the FFmpeg API mod. Both functions block until
 * the output is written, so call them from a worker thread, not from the game thread.
 */
class AudioMixer {
public:
    AudioMixer() = delete;

    /**
     * @brief Mixes a video file and an audio file into a single MP4 output.
     *
     * The first video stream is copied as it is (no re-encoding) and the audio is
     * converted to 44.1 kHz stereo AAC. Audio longer than the video is cut off at the
     * end of the video.
     *
     * @param videoFile The path to the input video file.
     * @param audioFile The path to the input audio file.
     * @param outputMp4File The path where the output MP4 file will be saved. Must differ
     *                      from `videoFile`.
     *
     * @warning Only the first video stream of `videoFile` is copied, other streams are dropped.
     */
    static geode::Result<> mixVideoAudio(std::filesystem::path const& videoFile, std::filesystem::path const& audioFile, std::filesystem::path const& outputMp4File) {
        auto const& vtable = impl::getVTable();
        if (!vtable.mixVideoAudio) {
            return geode::Err("FFmpeg API is not available.");
        }
        return vtable.mixVideoAudio(videoFile, audioFile, outputMp4File);
    }

    /**
     * @brief Mixes a video file and raw audio data into a single MP4 output.
     *
     * The raw data has no sample rate. It is assumed to cover the whole video, so the
     * rate is derived from the video's duration and the audio is stretched or squeezed
     * to fit if it does not.
     *
     * @param videoFile The path to the input video file.
     * @param raw Interleaved stereo float samples (L, R, L, R, ...), so an even count.
     * @param outputMp4File The path where the output MP4 file will be saved. Must differ
     *                      from `videoFile`.
     *
     * @warning Only the first video stream of `videoFile` is copied, other streams are dropped.
     */
    static geode::Result<> mixVideoRaw(std::filesystem::path const& videoFile, std::span<float> raw, std::filesystem::path const& outputMp4File) {
        auto const& vtable = impl::getVTable();
        if (!vtable.mixVideoRaw) {
            return geode::Err("FFmpeg API is not available.");
        }
        return vtable.mixVideoRaw(videoFile, raw, outputMp4File);
    }
};

}
