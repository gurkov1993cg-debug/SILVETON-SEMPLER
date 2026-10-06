#pragma once

#include "silveton/sampler/SamplerTypes.h"
#include "silveton/sampler/SincInterpolator.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace silveton::sampler {

enum class EventType : std::uint8_t {
    NoteOn,
    NoteOff,
    ControlChange,
    PitchBend,
    ChannelPressure,
    Panic
};

struct RealtimeEvent {
    EventType type{EventType::NoteOn};
    std::uint8_t channel{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::int16_t pitchBend{0};
};

class RealtimeEventQueue {
public:
    static constexpr std::size_t kCapacity = 2048U;

    bool push(const RealtimeEvent& event) noexcept;
    bool pop(RealtimeEvent& event) noexcept;
    [[nodiscard]] std::uint64_t droppedCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    void clear() noexcept;

private:
    std::array<RealtimeEvent, kCapacity> data_{};
    std::atomic<std::size_t> write_{0U};
    std::atomic<std::size_t> read_{0U};
    std::atomic<std::uint64_t> dropped_{0U};
};

struct MeterSnapshot {
    float peakLeft{0.0F};
    float peakRight{0.0F};
    float limiterGainReductionDb{0.0F};
    std::uint32_t activeVoices{0U};
    std::uint64_t droppedEvents{0U};
    std::array<float, kUpperCount> upperPeakLeft{};
    std::array<float, kUpperCount> upperPeakRight{};
};

// Values the TONE page shows for an Upper (its own or the PROGRAM default).
struct UpperToneValues {
    float attackMs{5.0F};
    float decayMs{80.0F};
    float sustain{1.0F};
    float releaseMs{120.0F};
    float cutoffHz{kToneCutoffOpenHz};
    float resonance{0.0F};
};
[[nodiscard]] UpperToneValues effectiveUpperTone(const UpperPart& upper) noexcept;

class SamplerEngine {
public:
    // Bank memory range a playing voice will read soon (published by the audio
    // thread, read by SampleStreamPrefetcher; lock-free).
    struct StreamWindow {
        std::uintptr_t begin{0U};
        std::size_t bytes{0U};
    };
    // Spare voices used only to let a voice that was taken over (voice limit)
    // fade out for 4 ms; they never start new notes, so polyphony stays 128.
    static constexpr std::size_t kTakeoverTailVoices = 16U;
    static constexpr std::size_t kMaxStreamWindows =
        (static_cast<std::size_t>(kMaxVoices) + kTakeoverTailVoices) * 2U;

    SamplerEngine();
    ~SamplerEngine();

    SamplerEngine(const SamplerEngine&) = delete;
    SamplerEngine& operator=(const SamplerEngine&) = delete;

    void prepare(double sampleRate, std::uint32_t maximumBlockFrames);
    void reset() noexcept;

    // PORTA BALANCE of the SILVETON keyboard processor (3..40, default 11):
    // how its Korg glide model turns TIME into a glide for each interval.
    void setPortamentoBalance(float balance) noexcept;
    [[nodiscard]] float portamentoBalance() const noexcept;

    // Control thread publishes a fully prepared immutable graph. The audio thread
    // adopts it at a block boundary; Direct ALSA is never stopped for SOUND/COMBI changes.
    void setState(std::shared_ptr<const EngineState> state) noexcept;
    [[nodiscard]] std::shared_ptr<const EngineState> state() const noexcept {
        return std::atomic_load_explicit(&state_, std::memory_order_acquire);
    }

    void setInterpolationQuality(InterpolationQuality quality) noexcept { interpolator_.setQuality(quality); }
    [[nodiscard]] InterpolationQuality interpolationQuality() const noexcept { return interpolator_.quality(); }

    bool postNoteOn(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity) noexcept;
    bool postNoteOff(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity = 0U) noexcept;
    bool postControlChange(std::uint8_t channel, std::uint8_t cc, std::uint8_t value) noexcept;
    bool postPitchBend(std::uint8_t channel, std::int16_t value) noexcept;
    bool postChannelPressure(std::uint8_t channel, std::uint8_t value) noexcept;
    bool postPanic() noexcept;

    // Stereo interleaved float output. No allocations or locks occur here.
    void render(float* interleavedStereo, std::uint32_t frames) noexcept;

    [[nodiscard]] MeterSnapshot meters() const noexcept;
    [[nodiscard]] std::uint32_t activeVoiceCount() const noexcept;
    [[nodiscard]] std::uint64_t droppedEventCount() const noexcept { return events_.droppedCount(); }

    // Any thread, no locks. false = slot changed while reading (try next pass).
    bool readStreamWindow(std::size_t slot, StreamWindow& out) const noexcept;
    // How far ahead of each voice the bank is brought into memory (seconds).
    void setStreamLookaheadSeconds(double seconds) noexcept;

public:
    struct Impl; // internal DSP state; exposed only for translation-unit helpers

private:
    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const EngineState> state_;
    std::shared_ptr<const EngineState> pendingState_;
    RealtimeEventQueue events_;
    SincInterpolator interpolator_;
};

} // namespace silveton::sampler
