# ffmpeg-api
A mod that lets developers easily interact with ffmpeg to record raw videos, and mix video and audio files.

**Android only** (FFmpeg 8.1.2). On Windows, run the official `ffmpeg.exe` instead.

## Usage

All calls return a `geode::Result<>`, check it. Always call `stop()` when you are done recording, that is what finalizes the video file (the destructor does it too if you forget).

### Record videos

<details>
  <summary>Normal API</summary>

```cpp
#include <eclipse.ffmpeg-api/include/recorder.hpp>

geode::Result<> video() {
    ffmpeg::Recorder recorder;

    ffmpeg::RenderSettings settings;

    // ffmpeg-api will automatically handle conversion between the input pixel
    // format and the codec's pixel format
    settings.m_pixelFormat = ffmpeg::PixelFormat::RGB0;
    settings.m_codec = "libx264"; // fetch codecs using ffmpeg::Recorder::getAvailableCodecs()
    settings.m_bitrate = 30000000;
    settings.m_width = 1920;
    settings.m_height = 1080;
    settings.m_fps = 60;
    settings.m_outputFile = "output_video.mp4";

    // one tightly packed frame: width * height * 4 bytes for RGB0
    std::vector<uint8_t> frame(settings.m_width * settings.m_height * 4);

    if (auto res = recorder.init(settings); res.isErr())
        return geode::Err(res.unwrapErr());

    for (int i = 0; i < 60; i++) {
        // fill `frame` with your raw data here
        if (auto res = recorder.writeFrame(frame); res.isErr()) {
            recorder.stop();
            return geode::Err(res.unwrapErr());
        }
    }

    recorder.stop();
    return geode::Ok();
}
```

</details>

<details>
  <summary>Event-based API</summary>

Header only, so your mod does not have to link against anything. Works the same as the normal API.

```cpp
#include <eclipse.ffmpeg-api/include/events.hpp>

geode::Result<> video() {
    ffmpeg::events::Recorder recorder;
    if (!recorder.isValid())
        return geode::Err("FFmpeg API is not loaded.");

    ffmpeg::RenderSettings settings;
    settings.m_pixelFormat = ffmpeg::PixelFormat::RGB0;
    settings.m_codec = "libx264"; // fetch codecs using ffmpeg::events::Recorder::getAvailableCodecs()
    settings.m_bitrate = 30000000;
    settings.m_width = 1920;
    settings.m_height = 1080;
    settings.m_fps = 60;
    settings.m_outputFile = "output_video.mp4";

    std::vector<uint8_t> frame(settings.m_width * settings.m_height * 4);

    if (auto res = recorder.init(settings); res.isErr())
        return geode::Err(res.unwrapErr());

    for (int i = 0; i < 60; i++) {
        // fill `frame` with your raw data here
        if (auto res = recorder.writeFrame(frame); res.isErr()) {
            recorder.stop();
            return geode::Err(res.unwrapErr());
        }
    }

    recorder.stop();
    return geode::Ok();
}
```

</details>

`RenderSettings::m_doVerticalFlip` is on by default (OpenGL frames are upside down). `m_colorspaceFilters` takes the arguments of FFmpeg's `colorspace` filter and may be left empty.

### Mix audio

Both functions block until the file is written, so call them from a worker thread. Only the first video stream is copied, the audio becomes 44.1 kHz stereo AAC.

* `mixVideoAudio` decodes any audio file FFmpeg understands. Audio longer than the video is cut off at the video's end.
* `mixVideoRaw` takes interleaved stereo `float` samples. Raw data has no sample rate, so it is assumed to cover the whole video and is stretched or squeezed to fit.

<details>
  <summary>Normal API</summary>

```cpp
#include <eclipse.ffmpeg-api/include/audio_mixer.hpp>

void audioFile() {
    auto res = ffmpeg::AudioMixer::mixVideoAudio("video.mp4", "audio.mp3", "output_mp3.mp4");
    if (res.isErr()) { /* res.unwrapErr() */ }
    res = ffmpeg::AudioMixer::mixVideoAudio("video.mp4", "audio.wav", "output_wav.mp4");
}

void audioRaw() {
    // interleaved stereo samples: L, R, L, R, ...
    std::vector<float> raw;
    auto res = ffmpeg::AudioMixer::mixVideoRaw("video.mp4", raw, "output_raw.mp4");
}
```

</details>

<details>
  <summary>Event-based API</summary>

```cpp
#include <eclipse.ffmpeg-api/include/events.hpp>

void audioFile() {
    auto res = ffmpeg::events::AudioMixer::mixVideoAudio("video.mp4", "audio.mp3", "output_mp3.mp4");
    if (res.isErr()) { /* res.unwrapErr() */ }
}

void audioRaw() {
    // interleaved stereo samples: L, R, L, R, ...
    std::vector<float> raw;
    auto res = ffmpeg::events::AudioMixer::mixVideoRaw("video.mp4", raw, "output_raw.mp4");
}
```

</details>

## Build instructions
The mod links FFmpeg 8.1.2 statically. Build the libraries for both ABIs with [ffmpeg-android-maker](https://github.com/ccchanged/ffmpeg-android-maker/) (make sure pkg-config is installed first, and read the README in that repository). You can run it natively on Linux or using WSL on Windows.

```sh
git clone https://github.com/ccchanged/ffmpeg-android-maker/
cd ffmpeg-android-maker
./ffmpeg-android-maker.sh --enable-libaom --enable-libvpx --enable-libx264 --enable-libx265 --android-api-level=24
```

Then copy the result into this repository:

* the headers (`include/<abi>/*`, identical for both ABIs) into `include/`, so you get `include/libavcodec/`, `include/libavutil/`, ...
* the static libraries into `lib/arm64-v8a/` and `lib/armeabi-v7a/` (`libavcodec.a`, `libavformat.a`, `libavutil.a`, `libavfilter.a`, `libswresample.a`, `libswscale.a`, `libaom.a`, `libvpx.a`, `libx264.a`, `libx265.a`). FFmpeg 8 has no libpostproc.

The libraries are not committed (`lib/` is in `.gitignore`), CI downloads them.

When building for android32 you need to set android version 24, you can do so by adding this arg to your geode build command
```sh
geode build --platform android32 --config Release -- -DANDROID_PLATFORM=24
```

x264 and x265 are GPL, so a build that includes them is GPL too.
