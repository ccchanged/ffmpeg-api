#include "events.hpp"
#include "recorder.hpp"
#include "audio_mixer.hpp"

using namespace geode::prelude;

$execute {
    using namespace ffmpeg::events::impl;

    FetchVTableEvent().listen([](VTable& vtable, size_t version) {
        if (version != VTABLE_VERSION)
            return ListenerResult::Propagate;

        vtable.createRecorder    = +[]() -> void* { return new ffmpeg::Recorder; };
        vtable.deleteRecorder    = +[](void* ptr) { delete static_cast<ffmpeg::Recorder*>(ptr); };
        vtable.initRecorder      = +[](void* ptr, const ffmpeg::RenderSettings& settings) -> Result<> {
            return static_cast<ffmpeg::Recorder*>(ptr)->init(settings);
        };
        vtable.stopRecorder      = +[](void* ptr) { static_cast<ffmpeg::Recorder*>(ptr)->stop(); };
        vtable.writeFrame        = +[](void* ptr, std::span<uint8_t const> frameData) -> Result<> {
            return static_cast<ffmpeg::Recorder*>(ptr)->writeFrame(frameData);
        };
        vtable.getAvailableCodecs = &ffmpeg::Recorder::getAvailableCodecs;
        vtable.mixVideoAudio      = &ffmpeg::AudioMixer::mixVideoAudio;
        vtable.mixVideoRaw        = &ffmpeg::AudioMixer::mixVideoRaw;

        return ListenerResult::Stop;
    }).leak();
}
