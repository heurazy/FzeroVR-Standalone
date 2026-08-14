#include "quest_aaudio_player.h"

#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Ship {
namespace {
constexpr const char* kTag = "FZeroXVR/Audio";
constexpr int64_t kWriteTimeoutNs = 20'000'000; // 20 ms
}

QuestAAudioPlayer::QuestAAudioPlayer(AudioSettings settings) : AudioPlayer(settings) {
}

QuestAAudioPlayer::~QuestAAudioPlayer() {
    DoClose();
}

bool QuestAAudioPlayer::DoInit() {
    DoClose();

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "AAudio_createStreamBuilder failed: %d", result);
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, GetSampleRate());
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setBufferCapacityInFrames(builder,
                                                  std::max(GetDesiredBuffered() + GetSampleLength(), 4096));

    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "AAudioStreamBuilder_openStream failed: %d", result);
        stream_ = nullptr;
        return false;
    }

    const int32_t actualRate = AAudioStream_getSampleRate(stream_);
    const int32_t actualChannels = AAudioStream_getChannelCount(stream_);
    const aaudio_format_t actualFormat = AAudioStream_getFormat(stream_);
    if (actualChannels != 2 || actualFormat != AAUDIO_FORMAT_PCM_I16) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "AAudio unsupported negotiated format: rate=%d channels=%d format=%d",
                            actualRate, actualChannels, static_cast<int>(actualFormat));
        DoClose();
        return false;
    }

    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "AAudioStream_requestStart failed: %d", result);
        DoClose();
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "AAudio initialized: requested=%dHz actual=%dHz stereo bufferCapacity=%d",
                        GetSampleRate(), actualRate, AAudioStream_getBufferCapacityInFrames(stream_));
    return true;
}

void QuestAAudioPlayer::DoClose() {
    if (stream_ == nullptr) {
        return;
    }
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
}

int32_t QuestAAudioPlayer::Buffered() {
    if (stream_ == nullptr) {
        return 0;
    }
    const int64_t written = AAudioStream_getFramesWritten(stream_);
    const int64_t read = AAudioStream_getFramesRead(stream_);
    const int64_t queued = std::max<int64_t>(written - read, 0);
    return static_cast<int32_t>(std::min<int64_t>(queued, std::numeric_limits<int32_t>::max()));
}

void QuestAAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    if (stream_ == nullptr || buf == nullptr || len < 4) {
        return;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(buf);
    const int32_t totalFrames = static_cast<int32_t>(len / (sizeof(int16_t) * 2));
    int32_t framesRemaining = totalFrames;
    int32_t frameOffset = 0;

    // One-shot PCM diagnostics: prove whether the N64 mixer is actually producing samples before
    // blaming the Android output path. Keep this cheap and bounded so it has no runtime cost after
    // the first few submissions.
    static int sProbeCalls = 0;
    static bool sLoggedFirstNonZero = false;
    ++sProbeCalls;
    int32_t peak = 0;
    int64_t absSum = 0;
    const bool measureProbe = (sProbeCalls <= 12) || !sLoggedFirstNonZero || ((sProbeCalls % 120) == 0);
    if (measureProbe) {
        const int32_t sampleCount = totalFrames * 2;
        for (int32_t i = 0; i < sampleCount; ++i) {
            const int32_t v = samples[i] < 0 ? -static_cast<int32_t>(samples[i]) : static_cast<int32_t>(samples[i]);
            peak = std::max(peak, v);
            absSum += v;
        }
    }

    while (framesRemaining > 0) {
        const aaudio_result_t written = AAudioStream_write(
            stream_, samples + frameOffset * 2, framesRemaining, kWriteTimeoutNs);
        if (written > 0) {
            frameOffset += written;
            framesRemaining -= written;
            continue;
        }
        if (written != AAUDIO_ERROR_TIMEOUT) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "AAudioStream_write failed: %d state=%d",
                                written, static_cast<int>(AAudioStream_getState(stream_)));
        }
        break;
    }

    if (measureProbe && (sProbeCalls <= 12 || peak > 0 || (sProbeCalls % 120) == 0)) {
        const int32_t sampleCount = std::max(totalFrames * 2, 1);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "PCM probe #%d frames=%d peak=%d meanAbs=%lld wrote=%d queued=%d state=%d",
                            sProbeCalls, totalFrames, peak,
                            static_cast<long long>(absSum / sampleCount), frameOffset, Buffered(),
                            static_cast<int>(AAudioStream_getState(stream_)));
        if (peak > 0) {
            sLoggedFirstNonZero = true;
        }
    }
}

std::shared_ptr<AudioPlayer> GdxCreateQuestAAudioPlayer(AudioSettings settings) {
    settings.ChannelSetting = AudioChannelsSetting::audioStereo;
    return std::make_shared<QuestAAudioPlayer>(settings);
}

} // namespace Ship
