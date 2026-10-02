#include "audio_mixer.hpp"
#include "utils.hpp"

#include <Geode/loader/Log.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/channel_layout.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/samplefmt.h>
    #include <libswresample/swresample.h>
}

BEGIN_FFMPEG_NAMESPACE_V

namespace {

// The mixed audio is always 44.1 kHz interleaved stereo float before it is encoded.
constexpr int kSampleRate = 44100;
constexpr int kChannels = 2;
constexpr int64_t kAacBitrate = 128000;
constexpr int kDefaultFrameSize = 1024;

// --- RAII helpers: nothing here can leak on an early return -------------------------

struct AvDeleter {
    void operator()(AVFormatContext* p) const { avformat_close_input(&p); }
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
    void operator()(AVFrame* p) const { av_frame_free(&p); }
    void operator()(AVPacket* p) const { av_packet_free(&p); }
    void operator()(SwrContext* p) const { swr_free(&p); }
};

template <class T>
using AvPtr = std::unique_ptr<T, AvDeleter>;

struct OutputContextDeleter {
    void operator()(AVFormatContext* p) const {
        if (!p) return;
        if (p->pb && !(p->oformat->flags & AVFMT_NOFILE))
            avio_closep(&p->pb);
        avformat_free_context(p);
    }
};

using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

AVChannelLayout makeStereoLayout() {
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, kChannels);
    return layout;
}

/// AAC encoders usually want planar float; use it when offered, otherwise the first listed format.
AVSampleFormat pickSampleFormat(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count) >= 0 && configs && count > 0) {
        auto const* formats = static_cast<const AVSampleFormat*>(configs);
        for (int i = 0; i < count; ++i) {
            if (formats[i] == AV_SAMPLE_FMT_FLTP)
                return AV_SAMPLE_FMT_FLTP;
        }
        return formats[0];
    }
    return AV_SAMPLE_FMT_FLTP;
}

/// Duration of the video in seconds, or -1 if the file does not say.
double getVideoDurationSeconds(const AVFormatContext* format, const AVStream* stream) {
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
        return static_cast<double>(format->duration) / AV_TIME_BASE;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
        return static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    return -1.0;
}

/// Runs samples through the resampler and appends the result (interleaved stereo float) to `out`.
/// Pass `in = nullptr, inSamples = 0` to flush what the resampler still holds.
geode::Result<> convertAppend(SwrContext* swr, const uint8_t* const* in, int inSamples, std::vector<float>& out) {
    int const capacity = swr_get_out_samples(swr, inSamples);
    if (capacity < 0)
        return geode::Err("Could not estimate the resampler output size: " + utils::getErrorString(capacity));
    if (capacity == 0 && inSamples == 0)
        return geode::Ok();

    size_t const oldSize = out.size();
    out.resize(oldSize + static_cast<size_t>(capacity) * kChannels);

    uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(out.data() + oldSize)};
    int const converted = swr_convert(swr, dst, capacity, in, inSamples);
    if (converted < 0) {
        out.resize(oldSize);
        return geode::Err("Audio conversion failed: " + utils::getErrorString(converted));
    }

    out.resize(oldSize + static_cast<size_t>(converted) * kChannels);
    return geode::Ok();
}

/// Decodes the first audio stream of a file to 44.1 kHz interleaved stereo float.
geode::Result<> decodeAudioFile(const std::filesystem::path& file, std::vector<float>& out) {
    std::string const path = utils::pathToUtf8(file);

    AVFormatContext* rawFormat = nullptr;
    int ret = avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr);
    if (ret < 0)
        return geode::Err("Could not open audio file: " + utils::getErrorString(ret));
    AvPtr<AVFormatContext> format(rawFormat);

    ret = avformat_find_stream_info(format.get(), nullptr);
    if (ret < 0)
        return geode::Err("Could not read audio stream info: " + utils::getErrorString(ret));

    const AVCodec* decoder = nullptr;
    int const streamIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (streamIndex < 0)
        return geode::Err("Could not find a decodable audio stream: " + utils::getErrorString(streamIndex));
    AVStream* stream = format->streams[streamIndex];

    AvPtr<AVCodecContext> context(avcodec_alloc_context3(decoder));
    if (!context)
        return geode::Err("Could not allocate the audio decoder.");

    // Parameters first, then open: the decoder needs the extradata (AAC, Vorbis, Opus, FLAC, ...).
    ret = avcodec_parameters_to_context(context.get(), stream->codecpar);
    if (ret < 0)
        return geode::Err("Could not copy audio parameters: " + utils::getErrorString(ret));

    ret = avcodec_open2(context.get(), decoder, nullptr);
    if (ret < 0)
        return geode::Err("Could not open the audio decoder: " + utils::getErrorString(ret));

    AvPtr<AVFrame> frame(av_frame_alloc());
    AvPtr<AVPacket> packet(av_packet_alloc());
    if (!frame || !packet)
        return geode::Err("Could not allocate audio frame or packet.");

    // Avoid repeated reallocations for long tracks (capped, the metadata might be wrong).
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        double const seconds = static_cast<double>(stream->duration) * av_q2d(stream->time_base);
        if (seconds > 0.0 && seconds < 900.0)
            out.reserve(static_cast<size_t>(seconds * kSampleRate) * kChannels);
    }

    AvPtr<SwrContext> swr; // created from the first decoded frame, which knows the real format

    auto convertFrame = [&]() -> geode::Result<> {
        if (!swr) {
            if (frame->sample_rate <= 0 || frame->ch_layout.nb_channels <= 0)
                return geode::Err("The decoded audio has no valid sample rate or channel layout.");

            AVChannelLayout inLayout{};
            if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&inLayout, frame->ch_layout.nb_channels);
            else
                av_channel_layout_copy(&inLayout, &frame->ch_layout);

            AVChannelLayout outLayout = makeStereoLayout();
            SwrContext* rawSwr = nullptr;
            int r = swr_alloc_set_opts2(&rawSwr,
                &outLayout, AV_SAMPLE_FMT_FLT, kSampleRate,
                &inLayout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate,
                0, nullptr);
            av_channel_layout_uninit(&inLayout);
            av_channel_layout_uninit(&outLayout);
            swr.reset(rawSwr);
            if (r < 0)
                return geode::Err("Could not create the audio resampler: " + utils::getErrorString(r));

            r = swr_init(swr.get());
            if (r < 0)
                return geode::Err("Could not initialize the audio resampler: " + utils::getErrorString(r));
        }
        return convertAppend(swr.get(), frame->extended_data, frame->nb_samples, out);
    };

    // nullptr flushes the decoder.
    auto decodePacket = [&](const AVPacket* pkt) -> geode::Result<> {
        int r = avcodec_send_packet(context.get(), pkt);
        if (r < 0 && r != AVERROR_EOF && r != AVERROR(EAGAIN)) {
            // A corrupt packet: skip it, the decoder resyncs on the next one.
            geode::log::warn("Skipping an undecodable audio packet: {}", utils::getErrorString(r));
            return geode::Ok();
        }

        while (true) {
            r = avcodec_receive_frame(context.get(), frame.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return geode::Ok();
            if (r < 0)
                return geode::Err("Failed to decode an audio frame: " + utils::getErrorString(r));

            auto res = convertFrame();
            av_frame_unref(frame.get());
            if (res.isErr())
                return res;
        }
    };

    while ((ret = av_read_frame(format.get(), packet.get())) >= 0) {
        if (packet->stream_index == streamIndex) {
            if (auto res = decodePacket(packet.get()); res.isErr())
                return res;
        }
        av_packet_unref(packet.get());
    }
    if (ret != AVERROR_EOF)
        geode::log::warn("Audio file ended with a read error: {}", utils::getErrorString(ret));

    if (auto res = decodePacket(nullptr); res.isErr())
        return res;

    // Whatever the resampler still holds at the very end of the file.
    if (swr) {
        if (auto res = convertAppend(swr.get(), nullptr, 0, out); res.isErr())
            return res;
    }

    if (out.empty())
        return geode::Err("The audio file contains no decodable audio.");
    return geode::Ok();
}

/// Resamples interleaved stereo float from `inputRate` to 44.1 kHz.
geode::Result<> resampleStereo(std::span<float const> input, int inputRate, std::vector<float>& out) {
    AVChannelLayout stereo = makeStereoLayout();
    SwrContext* rawSwr = nullptr;
    int ret = swr_alloc_set_opts2(&rawSwr,
        &stereo, AV_SAMPLE_FMT_FLT, kSampleRate,
        &stereo, AV_SAMPLE_FMT_FLT, inputRate,
        0, nullptr);
    AvPtr<SwrContext> swr(rawSwr);
    av_channel_layout_uninit(&stereo);
    if (ret < 0)
        return geode::Err("Could not create the audio resampler: " + utils::getErrorString(ret));

    ret = swr_init(swr.get());
    if (ret < 0)
        return geode::Err("Could not initialize the audio resampler: " + utils::getErrorString(ret));

    size_t const frames = input.size() / kChannels;
    out.clear();
    out.reserve(static_cast<size_t>(static_cast<double>(frames) * kSampleRate / inputRate + 1024) * kChannels);

    constexpr size_t kChunk = 65536;
    for (size_t pos = 0; pos < frames; pos += kChunk) {
        int const count = static_cast<int>(std::min(kChunk, frames - pos));
        const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(input.data() + pos * kChannels)};
        if (auto res = convertAppend(swr.get(), in, count, out); res.isErr())
            return res;
    }

    return convertAppend(swr.get(), nullptr, 0, out);
}

/// Encodes 44.1 kHz stereo float samples as AAC into a stream of the given output.
/// Audio is produced on demand (`encodeUntil`), so it can be interleaved with the video.
class AacEncoder {
public:
    geode::Result<> init(AVFormatContext* output, std::span<float const> pcm) {
        m_output = output;
        m_pcm = pcm;
        m_totalFrames = pcm.size() / kChannels;

        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec)
            return geode::Err("The AAC encoder is not available in this FFmpeg build.");

        m_stream = avformat_new_stream(output, nullptr);
        if (!m_stream)
            return geode::Err("Could not create the audio stream.");

        m_context.reset(avcodec_alloc_context3(codec));
        if (!m_context)
            return geode::Err("Could not allocate the AAC encoder.");

        m_context->sample_rate = kSampleRate;
        av_channel_layout_default(&m_context->ch_layout, kChannels);
        m_context->sample_fmt = pickSampleFormat(codec);
        m_context->bit_rate = kAacBitrate;
        m_context->time_base = AVRational{1, kSampleRate};

        // Must be set before the encoder is opened.
        if (output->oformat->flags & AVFMT_GLOBALHEADER)
            m_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        int ret = avcodec_open2(m_context.get(), codec, nullptr);
        if (ret < 0)
            return geode::Err("Could not open the AAC encoder: " + utils::getErrorString(ret));

        ret = avcodec_parameters_from_context(m_stream->codecpar, m_context.get());
        if (ret < 0)
            return geode::Err("Could not copy the AAC parameters: " + utils::getErrorString(ret));
        m_stream->time_base = m_context->time_base;

        m_frameSize = m_context->frame_size > 0 ? m_context->frame_size : kDefaultFrameSize;

        // Interleaved float in, whatever sample format the encoder wants out (no rate change).
        AVChannelLayout stereo = makeStereoLayout();
        SwrContext* rawSwr = nullptr;
        ret = swr_alloc_set_opts2(&rawSwr,
            &m_context->ch_layout, m_context->sample_fmt, kSampleRate,
            &stereo, AV_SAMPLE_FMT_FLT, kSampleRate,
            0, nullptr);
        av_channel_layout_uninit(&stereo);
        m_swr.reset(rawSwr);
        if (ret < 0)
            return geode::Err("Could not create the sample format converter: " + utils::getErrorString(ret));
        ret = swr_init(m_swr.get());
        if (ret < 0)
            return geode::Err("Could not initialize the sample format converter: " + utils::getErrorString(ret));

        m_frame.reset(av_frame_alloc());
        m_packet.reset(av_packet_alloc());
        if (!m_frame || !m_packet)
            return geode::Err("Could not allocate the audio frame or packet.");

        return geode::Ok();
    }

    /// Encodes audio until its timestamp passes `seconds` (or the audio is used up).
    geode::Result<> encodeUntil(double seconds) {
        while (m_position < m_totalFrames && static_cast<double>(m_nextPts) / kSampleRate <= seconds) {
            if (auto res = encodeChunk(); res.isErr())
                return res;
        }
        return geode::Ok();
    }

    /// Encodes whatever is left and flushes the encoder.
    geode::Result<> finish() {
        if (auto res = encodeUntil(std::numeric_limits<double>::infinity()); res.isErr())
            return res;

        int const ret = avcodec_send_frame(m_context.get(), nullptr);
        if (ret < 0 && ret != AVERROR_EOF)
            return geode::Err("Could not flush the AAC encoder: " + utils::getErrorString(ret));
        return drainPackets();
    }

private:
    geode::Result<> encodeChunk() {
        int const count = static_cast<int>(std::min<size_t>(static_cast<size_t>(m_frameSize), m_totalFrames - m_position));

        av_frame_unref(m_frame.get());
        m_frame->format = m_context->sample_fmt;
        m_frame->sample_rate = kSampleRate;
        m_frame->nb_samples = count;
        int ret = av_channel_layout_copy(&m_frame->ch_layout, &m_context->ch_layout);
        if (ret < 0)
            return geode::Err("Could not set the audio frame layout: " + utils::getErrorString(ret));
        ret = av_frame_get_buffer(m_frame.get(), 0);
        if (ret < 0)
            return geode::Err("Could not allocate the audio frame buffer: " + utils::getErrorString(ret));

        const uint8_t* in[1] = {reinterpret_cast<const uint8_t*>(m_pcm.data() + m_position * kChannels)};
        int const converted = swr_convert(m_swr.get(), m_frame->data, count, in, count);
        if (converted < 0)
            return geode::Err("Could not convert the audio samples: " + utils::getErrorString(converted));

        m_position += static_cast<size_t>(count);
        if (converted == 0)
            return geode::Ok();

        m_frame->nb_samples = converted;
        m_frame->pts = m_nextPts;
        m_nextPts += converted;

        ret = avcodec_send_frame(m_context.get(), m_frame.get());
        if (ret < 0)
            return geode::Err("Error while sending an audio frame: " + utils::getErrorString(ret));
        return drainPackets();
    }

    geode::Result<> drainPackets() {
        while (true) {
            int ret = avcodec_receive_packet(m_context.get(), m_packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return geode::Ok();
            if (ret < 0)
                return geode::Err("Error while receiving an audio packet: " + utils::getErrorString(ret));

            av_packet_rescale_ts(m_packet.get(), m_context->time_base, m_stream->time_base);
            m_packet->stream_index = m_stream->index;

            // The muxer takes ownership of the packet, even when writing fails.
            ret = av_interleaved_write_frame(m_output, m_packet.get());
            if (ret < 0)
                return geode::Err("Error while writing an audio packet: " + utils::getErrorString(ret));
        }
    }

    AVFormatContext* m_output = nullptr;
    AVStream* m_stream = nullptr;
    AvPtr<AVCodecContext> m_context;
    AvPtr<SwrContext> m_swr;
    AvPtr<AVFrame> m_frame;
    AvPtr<AVPacket> m_packet;
    std::span<float const> m_pcm;
    size_t m_totalFrames = 0;
    size_t m_position = 0;
    int64_t m_nextPts = 0;
    int m_frameSize = kDefaultFrameSize;
};

/// Copies the first video stream of `videoFile` and adds the audio as AAC.
/// `pcmRate` is the sample rate of `pcm`; 0 means "derive it from the video's duration".
/// `outputCreated` becomes true once the output file exists on disk.
geode::Result<> muxVideoWithAudio(
    const std::filesystem::path& videoFile, std::span<float const> pcm, int pcmRate,
    const std::filesystem::path& outputFile, bool& outputCreated
) {
    std::error_code ec;
    if (std::filesystem::equivalent(videoFile, outputFile, ec))
        return geode::Err("The output file must be different from the input video.");

    std::string const videoPath = utils::pathToUtf8(videoFile);
    std::string const outputPath = utils::pathToUtf8(outputFile);

    // --- input video ---------------------------------------------------------------------
    AVFormatContext* rawInput = nullptr;
    int ret = avformat_open_input(&rawInput, videoPath.c_str(), nullptr, nullptr);
    if (ret < 0)
        return geode::Err("Could not open video file: " + utils::getErrorString(ret));
    AvPtr<AVFormatContext> input(rawInput);

    // Before anything is read from the streams, or the parameters would be incomplete.
    ret = avformat_find_stream_info(input.get(), nullptr);
    if (ret < 0)
        return geode::Err("Could not read video stream info: " + utils::getErrorString(ret));

    int const videoIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0)
        return geode::Err("Could not find a video stream in the video file.");
    AVStream* inVideo = input->streams[videoIndex];

    double const videoSeconds = getVideoDurationSeconds(input.get(), inVideo);

    // --- prepare the audio ---------------------------------------------------------------
    std::vector<float> resampled;
    std::span<float const> samples = pcm;

    if (pcmRate <= 0) {
        if (videoSeconds <= 0.0)
            return geode::Err("Could not determine the video duration, which is needed to fit the raw audio to it.");
        pcmRate = static_cast<int>(std::lround(static_cast<double>(pcm.size() / kChannels) / videoSeconds));
        if (pcmRate <= 0)
            return geode::Err("The raw audio is too short for the video.");
        geode::log::debug("Raw audio fitted to the video: assuming {} Hz", pcmRate);
    }

    if (pcmRate != kSampleRate) {
        if (auto res = resampleStereo(samples, pcmRate, resampled); res.isErr())
            return res;
        samples = resampled;
    }

    // Do not let audio run past the end of the video.
    if (videoSeconds > 0.0) {
        size_t const maxFrames = static_cast<size_t>(std::ceil(videoSeconds * kSampleRate));
        if (samples.size() / kChannels > maxFrames)
            samples = samples.first(maxFrames * kChannels);
    }

    // --- output ----------------------------------------------------------------------------
    AVFormatContext* rawOutput = nullptr;
    ret = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, outputPath.c_str());
    OutputContextPtr output(rawOutput);
    if (!output)
        return geode::Err("Could not create the output context: " + utils::getErrorString(ret));

    AVStream* outVideo = avformat_new_stream(output.get(), nullptr);
    if (!outVideo)
        return geode::Err("Could not create the output video stream.");

    ret = avcodec_parameters_copy(outVideo->codecpar, inVideo->codecpar);
    if (ret < 0)
        return geode::Err("Could not copy the video parameters: " + utils::getErrorString(ret));
    outVideo->codecpar->codec_tag = 0;
    outVideo->time_base = inVideo->time_base;
    outVideo->avg_frame_rate = inVideo->avg_frame_rate;
    outVideo->r_frame_rate = inVideo->r_frame_rate;
    outVideo->sample_aspect_ratio = inVideo->sample_aspect_ratio;

    AacEncoder audio;
    if (auto res = audio.init(output.get(), samples); res.isErr())
        return res;

    if (!(output->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
            return geode::Err("Could not open the output file: " + utils::getErrorString(ret));
        outputCreated = true;
    }

    ret = avformat_write_header(output.get(), nullptr);
    if (ret < 0)
        return geode::Err("Could not write the header: " + utils::getErrorString(ret));

    // --- copy video, weaving the audio in at the matching timestamps -----------------------
    AvPtr<AVPacket> packet(av_packet_alloc());
    if (!packet)
        return geode::Err("Could not allocate a packet.");

    while ((ret = av_read_frame(input.get(), packet.get())) >= 0) {
        if (packet->stream_index != videoIndex) {
            av_packet_unref(packet.get()); // only the first video stream is copied
            continue;
        }

        int64_t const ts = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        if (ts != AV_NOPTS_VALUE) {
            if (auto res = audio.encodeUntil(static_cast<double>(ts) * av_q2d(inVideo->time_base)); res.isErr())
                return res;
        }

        av_packet_rescale_ts(packet.get(), inVideo->time_base, outVideo->time_base);
        packet->stream_index = outVideo->index;
        packet->pos = -1;

        // The muxer takes ownership of the packet, even when writing fails.
        ret = av_interleaved_write_frame(output.get(), packet.get());
        if (ret < 0)
            return geode::Err("Error while writing a video packet: " + utils::getErrorString(ret));
    }
    if (ret != AVERROR_EOF)
        geode::log::warn("The video file ended with a read error: {}", utils::getErrorString(ret));

    if (auto res = audio.finish(); res.isErr())
        return res;

    ret = av_write_trailer(output.get());
    if (ret < 0)
        return geode::Err("Could not write the trailer: " + utils::getErrorString(ret));

    // Close explicitly so a failing final flush (e.g. disk full) is reported.
    if (!(output->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_closep(&output->pb);
        if (ret < 0)
            return geode::Err("Could not finish writing the output file: " + utils::getErrorString(ret));
    }

    return geode::Ok();
}

/// Runs the mux and deletes a half-written output file on failure.
geode::Result<> mixSamples(
    const std::filesystem::path& videoFile, std::span<float const> pcm, int pcmRate,
    const std::filesystem::path& outputFile
) {
    bool outputCreated = false;
    auto res = muxVideoWithAudio(videoFile, pcm, pcmRate, outputFile, outputCreated);
    if (res.isErr() && outputCreated) {
        // Everything is closed again by now. Only ever delete what this call created.
        std::error_code ec;
        std::filesystem::remove(outputFile, ec);
    }
    return res;
}

}

geode::Result<> AudioMixer::mixVideoAudio(const std::filesystem::path& videoFile, const std::filesystem::path& audioFile, const std::filesystem::path& outputMp4File) {
    std::vector<float> pcm;
    if (auto res = decodeAudioFile(audioFile, pcm); res.isErr())
        return res;
    return mixSamples(videoFile, pcm, kSampleRate, outputMp4File);
}

geode::Result<> AudioMixer::mixVideoRaw(const std::filesystem::path& videoFile, std::span<float const> raw, const std::filesystem::path& outputMp4File) {
    if (raw.empty())
        return geode::Err("No audio data was provided.");
    if (raw.size() % kChannels != 0)
        return geode::Err("Raw audio must be interleaved stereo (an even number of samples).");
    return mixSamples(videoFile, raw, 0, outputMp4File);
}

END_FFMPEG_NAMESPACE_V
