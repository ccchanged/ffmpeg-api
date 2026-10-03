#pragma once

#include "export.hpp"

#include <Geode/Result.hpp>

#include <filesystem>
#include <span>

BEGIN_FFMPEG_NAMESPACE_V

/**
 * Muxes audio into a video file. Both functions are synchronous and block until the
 * output is written, so call them from a worker thread, not from the game thread.
 *
 * On failure a partially written output file is deleted again.
 */
// ABI NOTE: mods built against older versions link to these exact signatures, so they
// must not change (not even `std::span<float>` -> `std::span<float const>`).
class FFMPEG_API_DLL AudioMixer {
public:
    AudioMixer() = delete;
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) = delete;

    /**
     * @brief Mixes a video file and an audio file into a single MP4 output.
     *
     * The first video stream of the input is copied as it is (no re-encoding) and the
     * audio is decoded, converted to 44.1 kHz stereo and encoded as AAC. Audio longer
     * than the video is cut off at the end of the video; shorter audio just ends early.
     *
     * @param videoFile The path to the input video file.
     * @param audioFile The path to the input audio file.
     * @param outputMp4File The path where the output MP4 file will be saved. Must differ
     *                      from `videoFile`.
     *
     * @warning Only the first video stream of `videoFile` is copied, other streams are dropped.
     */
    static geode::Result<> mixVideoAudio(const std::filesystem::path& videoFile, const std::filesystem::path& audioFile, const std::filesystem::path& outputMp4File);

    /**
     * @brief Mixes a video file and raw audio data into a single MP4 output.
     *
     * The raw data does not carry a sample rate. It is assumed to cover the whole video,
     * so the rate is derived from the video's duration (samples / duration) and the audio
     * is resampled to 44.1 kHz stereo and encoded as AAC. If your audio does not span the
     * whole video it will be stretched or squeezed to fit.
     *
     * @param videoFile The path to the input video file.
     * @param raw Interleaved stereo float samples (L, R, L, R, ...), so an even count.
     *            Only read, never modified.
     * @param outputMp4File The path where the output MP4 file will be saved. Must differ
     *                      from `videoFile`.
     *
     * @warning Only the first video stream of `videoFile` is copied, other streams are dropped.
     */
    static geode::Result<> mixVideoRaw(const std::filesystem::path& videoFile, std::span<float> raw, const std::filesystem::path& outputMp4File);
};

END_FFMPEG_NAMESPACE_V
