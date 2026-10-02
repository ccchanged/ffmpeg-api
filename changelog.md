# 1.0.0
- First release

# 1.0.1
- Android release

# 1.0.2
- Improve filesize

# 1.0.3
- Add more codecs to android
- Fix codec fetching

# 1.0.4
- Mac release

# 1.1.2
- Fixed memory leak

# 1.1.3 (reverted)
- Events API

# 1.2.0
- Fix Events API
- V-flip support
- More error handling
- Performance improvements
- Bug fixes

# 1.2.1
- Proper cleanup on recorder initialization failure

# 1.2.2
- Fix Android/macOS ABI

# 2.1.0
- Android only: Windows and macOS are no longer built (use the official ffmpeg.exe there)
- Updated to FFmpeg 8.1.2
- Rewrote the recorder and the audio mixer: fixed memory leaks, crashes on failed initialization, lost end of audio, wrong sample rate and wrong colors after RGB to YUV conversion
- Vertical flip and the colorspace filter now work together
- Raw audio and audio files are mixed in properly (audio is no longer sped up or stretched to fit)
- The recorder, `stop()` and the destructor can no longer crash or leak when used out of order
- The Events API now checks the version, is thread-safe and its Recorder can no longer be copied by accident
- FFmpeg warnings and errors now show up in the Geode log
