#pragma once

#include "render_settings.hpp"

#include <Geode/loader/Event.hpp>
#include <mutex>

namespace ffmpeg::events {
namespace impl {
    constexpr size_t VTABLE_VERSION = 1;
    using CreateRecorder_t       = void*(*)();
    using DeleteRecorder_t       = void(*)(void*);
    using InitRecorder_t         = geode::Result<>(*)(void*, const RenderSettings&);
    using StopRecorder_t         = void(*)(void*);
    using WriteFrame_t           = geode::Result<>(*)(void*, std::span<uint8_t const>);
    using GetAvailableCodecs_t   = std::vector<std::string>(*)();
    using MixVideoAudio_t        = geode::Result<>(*)(const std::filesystem::path&, const std::filesystem::path&, const std::filesystem::path&);
    using MixVideoRaw_t          = geode::Result<>(*)(const std::filesystem::path&, std::span<float>, const std::filesystem::path&);

    struct VTable {
        CreateRecorder_t     createRecorder   = nullptr;
        DeleteRecorder_t     deleteRecorder   = nullptr;
        InitRecorder_t       initRecorder     = nullptr;
        StopRecorder_t       stopRecorder     = nullptr;
        WriteFrame_t         writeFrame       = nullptr;
        GetAvailableCodecs_t getAvailableCodecs = nullptr;
        MixVideoAudio_t      mixVideoAudio    = nullptr;
        MixVideoRaw_t        mixVideoRaw      = nullptr;
    };

    struct FetchVTableEvent : geode::Event<FetchVTableEvent, bool(VTable&, size_t)> {
        using Event::Event;
    };

    inline VTable& getVTable() {
        static VTable vtable;
        static std::once_flag flag;
        std::call_once(flag, [] {
            FetchVTableEvent().send(vtable, VTABLE_VERSION);
        });
        return vtable;
    }
}

class Recorder {
public:
    Recorder() {
        auto& vtable = impl::getVTable();
        if (vtable.createRecorder)
            m_ptr = vtable.createRecorder();
    }

    ~Recorder() {
        if (m_ptr) {
            auto& vtable = impl::getVTable();
            if (vtable.deleteRecorder)
                vtable.deleteRecorder(m_ptr);
        }
    }

    // Raw void* ownership — copying would cause a double-free
    Recorder(const Recorder&)            = delete;
    Recorder& operator=(const Recorder&) = delete;

    Recorder(Recorder&& other) noexcept : m_ptr(other.m_ptr) {
        other.m_ptr = nullptr;
    }
    Recorder& operator=(Recorder&& other) noexcept {
        if (this != &other) {
            if (m_ptr) {
                auto& vtable = impl::getVTable();
                if (vtable.deleteRecorder)
                    vtable.deleteRecorder(m_ptr);
            }
            m_ptr       = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    bool isValid() const { return m_ptr != nullptr; }

    /**
     * @brief Initializes the Recorder with the specified rendering settings.
     *
     * @param settings The rendering settings defining codec, bitrate, resolution, and pixel format.
     * @return Ok on success, Err with a description on failure.
     */
    geode::Result<> init(RenderSettings const& settings) {
        auto& vtable = impl::getVTable();
        if (!vtable.initRecorder)
            return geode::Err("FFmpeg API is not available.");
        return vtable.initRecorder(m_ptr, settings);
    }

    /**
     * @brief Stops the recording process and finalizes the output file.
     *
     * Flushes all buffered frames, releases allocated resources, and closes the output file.
     */
    void stop() {
        auto& vtable = impl::getVTable();
        if (vtable.stopRecorder)
            vtable.stopRecorder(m_ptr);
    }

    /**
     * @brief Writes a single video frame to the output.
     *
     * @param frameData Raw frame bytes. Size must match the dimensions set during init.
     * @return Ok on success, Err with a description on failure.
     */
    geode::Result<> writeFrame(std::span<uint8_t const> frameData) {
        auto& vtable = impl::getVTable();
        if (!vtable.writeFrame)
            return geode::Err("FFmpeg API is not available.");
        return vtable.writeFrame(m_ptr, frameData);
    }

    /**
     * @brief Returns a list of available video encoder names.
     */
    static std::vector<std::string> getAvailableCodecs() {
        auto& vtable = impl::getVTable();
        if (!vtable.getAvailableCodecs)
            return {};
        return vtable.getAvailableCodecs();
    }

private:
    void* m_ptr = nullptr;
};

class AudioMixer {
public:
    AudioMixer()                             = delete;
    AudioMixer(const AudioMixer&)            = delete;
    AudioMixer(AudioMixer&&)                 = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer& operator=(AudioMixer&&)      = delete;

    /**
     * @brief Mixes a video file and an audio file into a single MP4 output.
     *
     * @param videoFile     Path to the input video file.
     * @param audioFile     Path to the input audio file (stereo expected).
     * @param outputMp4File Path where the output MP4 will be saved.
     */
    static geode::Result<> mixVideoAudio(std::filesystem::path const& videoFile, std::filesystem::path const& audioFile, std::filesystem::path const& outputMp4File) {
        auto& vtable = impl::getVTable();
        if (!vtable.mixVideoAudio)
            return geode::Err("FFmpeg API is not available.");
        return vtable.mixVideoAudio(videoFile, audioFile, outputMp4File);
    }

    /**
     * @brief Mixes a video file and raw interleaved stereo float audio into a single MP4 output.
     *
     * @param videoFile     Path to the input video file.
     * @param raw           Raw interleaved stereo float audio samples.
     * @param outputMp4File Path where the output MP4 will be saved.
     */
    static geode::Result<> mixVideoRaw(std::filesystem::path const& videoFile, std::span<float> raw, std::filesystem::path const& outputMp4File) {
        auto& vtable = impl::getVTable();
        if (!vtable.mixVideoRaw)
            return geode::Err("FFmpeg API is not available.");
        return vtable.mixVideoRaw(videoFile, raw, outputMp4File);
    }
};

}
