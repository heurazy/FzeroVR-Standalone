#pragma once

#include <aaudio/AAudio.h>
#include <ship/audio/AudioPlayer.h>

#include <memory>

namespace Ship {

class QuestAAudioPlayer final : public AudioPlayer {
  public:
    explicit QuestAAudioPlayer(AudioSettings settings);
    ~QuestAAudioPlayer();

    int32_t Buffered() override;
    const char* GetBackendName() const override { return "AAudio"; }

  protected:
    bool DoInit() override;
    void DoClose() override;
    void DoPlay(const uint8_t* buf, size_t len) override;

  private:
    AAudioStream* stream_ = nullptr;
};

std::shared_ptr<AudioPlayer> GdxCreateQuestAAudioPlayer(AudioSettings settings);

} // namespace Ship
