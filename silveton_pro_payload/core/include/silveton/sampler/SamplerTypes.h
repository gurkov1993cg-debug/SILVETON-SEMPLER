#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace silveton::sampler {

class SampleStream;

constexpr int kMidiKeys = 128;
constexpr int kUpperCount = 7;
constexpr int kMaxVoices = 128;
constexpr int kMaxProgramLayers = 32;
constexpr int kMaxZonesPerMultisample = 512;
constexpr int kMaxListSlots = 256;
constexpr int kMaxUpperControllerRoutes = 8;
constexpr std::uint16_t kFixedSoundSlotCount = 500U;
constexpr std::uint16_t kFixedCombiSlotCount = 500U;
constexpr std::uint16_t kNoSoundSlot = 0U;

struct LoopRegion {
    bool enabled{false};
    std::uint64_t startFrame{0};
    std::uint64_t endFrame{0}; // exclusive
    std::uint32_t crossfadeFrames{0};
};

struct SampleData {
    std::string name;
    std::string sourcePath;
    std::string sourceIdentity; // stable import identity; backing file path may change after SET packaging
    std::uint32_t sampleRate{48000};
    std::uint16_t channels{1};
    std::uint8_t rootKey{60};
    float fineTuneCents{0.0F};
    std::vector<float> interleaved; // optional full sample or preloaded attack prefix
    std::shared_ptr<const SampleStream> stream; // optional mmap-backed sustain/full source
    std::uint64_t totalFrames{0U};
    LoopRegion loop{};

    [[nodiscard]] std::uint64_t preloadedFrameCount() const noexcept {
        if (channels == 0U) return 0U;
        // Called for every sample of every voice: avoid the division for mono/stereo.
        if (channels == 1U) return static_cast<std::uint64_t>(interleaved.size());
        if (channels == 2U) return static_cast<std::uint64_t>(interleaved.size() >> 1U);
        return static_cast<std::uint64_t>(interleaved.size() / static_cast<std::size_t>(channels));
    }

    [[nodiscard]] std::uint64_t frameCount() const noexcept {
        return totalFrames > 0U ? totalFrames : preloadedFrameCount();
    }

    [[nodiscard]] bool valid() const noexcept {
        return channels >= 1U && channels <= 2U && sampleRate > 0U && frameCount() > 0U;
    }
};

using SamplePtr = std::shared_ptr<const SampleData>;

struct SampleZone {
    SamplePtr sample;
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    std::uint8_t rootKey{60};
    std::int8_t transposeSemitones{0};
    float tuneCents{0.0F};
    float gainDb{0.0F};
    float pan{0.0F}; // -1..+1
    bool oneShot{false};
    bool reverse{false};
    bool nonTranspose{false};
    std::int8_t sourceLevelRaw{0};
    std::int8_t sourceFilterRaw{0};

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return sample != nullptr && key >= keyLow && key <= keyHigh &&
               velocity >= velocityLow && velocity <= velocityHigh;
    }
};

struct Multisample {
    std::string name;
    std::vector<SampleZone> zones;
    std::string sourceIdentity; // stable KMP/KSC import identity
};

using MultisamplePtr = std::shared_ptr<const Multisample>;

enum class FilterMode : std::uint8_t { LowPass, HighPass, BandPass, Bypass };
enum class LfoWave : std::uint8_t { Sine, Triangle, SawUp, SawDown, Square, RandomHold };
enum class UpperVoiceMode : std::uint8_t { Poly, Mono, Legato };

struct EnvelopeParams {
    float attackSec{0.005F};
    float decaySec{0.08F};
    float sustain{1.0F};
    float releaseSec{0.12F};
};

struct FilterParams {
    FilterMode mode{FilterMode::Bypass};
    float cutoffHz{20000.0F};
    float resonance{0.0F}; // 0..1
    float keyTrack{0.0F}; // -1..1
    float velocityAmount{0.0F}; // octaves, bipolar
    float envelopeAmount{0.0F}; // octaves, bipolar
    float lfoAmount{0.0F}; // octaves, bipolar
};

struct LfoParams {
    LfoWave wave{LfoWave::Sine};
    float frequencyHz{5.0F};
    float delaySec{0.0F};
    float fadeSec{0.0F};
    bool keySync{true};
    float pitchCents{0.0F};
    float filterOctaves{0.0F};
    float ampDepth{0.0F};
};

struct ProgramLayer {
    std::string name;
    MultisamplePtr multisample;
    bool enabled{true};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    std::int8_t transposeSemitones{0};
    float tuneCents{0.0F};
    float levelDb{0.0F};
    float pan{0.0F};
    // Velocity crossfade widths in MIDI velocity steps. Zero = hard zone edge.
    std::uint8_t velocityFadeIn{0};
    std::uint8_t velocityFadeOut{0};
    // Delay before this layer starts sounding; envelopes/LFOs wait with it.
    float delaySec{0.0F};
    // Layer-level damper gate, combined with the COMBI Upper damper enable.
    bool damperEnabled{true};
    EnvelopeParams ampEnvelope{};
    EnvelopeParams filterEnvelope{0.001F, 0.15F, 0.0F, 0.15F};
    EnvelopeParams pitchEnvelope{0.001F, 0.05F, 0.0F, 0.05F};
    float pitchEnvelopeCents{0.0F};
    FilterParams filter{};
    LfoParams lfo1{};
    LfoParams lfo2{};

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return enabled && multisample != nullptr && key >= keyLow && key <= keyHigh &&
               velocity >= velocityLow && velocity <= velocityHigh;
    }
};

struct Program {
    std::string name;
    std::vector<ProgramLayer> layers;
    bool mono{false};
    bool legato{false};
    float portamentoSec{0.0F};
};

using ProgramPtr = std::shared_ptr<const Program>;

enum class ControllerSource : std::uint8_t {
    Off,
    Breath,
    Expression,
    Aftertouch,
    JoystickX,   // MIDI pitch-bend axis, normalized bipolar
    JoystickY,   // CC selected by cc field (CC1 by default)
    ControlChange
};

enum class ModDestination : std::uint8_t {
    LevelDb,
    Pan,
    PitchSemitones,
    FilterCutoffOctaves,
    FilterResonance,
    LfoDepth,
    FxSend1,
    FxSend2
};

struct UpperControllerRoute {
    ControllerSource source{ControllerSource::Off};
    ModDestination destination{ModDestination::LevelDb};
    std::uint8_t cc{1U};
    float amount{0.0F};
    bool bipolar{false};
};

// Absolute TONE values of an Upper. A negative value means "not set":
// the PROGRAM layer envelope (times its Easy Edit scale) is used instead.
constexpr float kToneMaxTimeMs = 20000.0F;
constexpr float kToneCutoffOpenHz = 20000.0F; // 20 kHz = tone filter off

struct UpperPart {
    std::string name;
    ProgramPtr program;
    // Persistent SOUND position. 1..500 = bank SOUND; 0 = no fixed SOUND.
    std::uint16_t soundSlotId{kNoSoundSlot};
    bool enabled{false};
    bool mute{false};
    bool solo{false};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    float levelDb{0.0F};
    float pan{0.0F};
    std::int8_t transposeSemitones{0};
    float tuneCents{0.0F};
    std::int8_t octave{0};
    std::int8_t pitchBendRange{1};   // SILVETON_TUNE_RANGES_V1: default 1 semitone
    bool damperEnabled{true};
    bool expressionEnabled{true};
    bool breathEnabled{false};
    bool aftertouchEnabled{false};
    std::uint8_t midiChannel{0};
    float eqLowDb{0.0F};
    float eqMidDb{0.0F};
    float eqHighDb{0.0F};
    float fxSend1{0.0F};
    float fxSend2{0.0F};
    // COMBI Easy Edit modifiers. These are intentionally stored on the Upper,
    // so changing a stage setup never mutates the underlying PROGRAM.
    float ampAttackScale{1.0F};
    float ampDecayScale{1.0F};
    float ampReleaseScale{1.0F};
    float ampSustainScale{1.0F};
    float filterCutoffOctaves{0.0F};
    float filterResonanceOffset{0.0F};
    float lfoRateScale{1.0F};
    float lfoDepthScale{1.0F};
    float portamentoSec{0.0F};
    float toneAttackMs{-1.0F};
    float toneDecayMs{-1.0F};
    float toneSustain{-1.0F};
    float toneReleaseMs{-1.0F};
    float toneCutoffHz{-1.0F};
    float toneResonance{-1.0F};
    UpperVoiceMode voiceMode{UpperVoiceMode::Poly};
    // Per-Upper realtime controller matrix. Routes are evaluated in the audio
    // callback without allocations; each Upper can use different sources/destinations.
    std::array<UpperControllerRoute, kMaxUpperControllerRoutes> controllerRoutes{};

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return enabled && !mute && program != nullptr && key >= keyLow && key <= keyHigh &&
               velocity >= velocityLow && velocity <= velocityHigh;
    }
};

enum class EffectType : std::uint8_t { Off, Delay, Chorus, Reverb };

struct EffectSlot {
    EffectType type{EffectType::Off};
    float wet{0.0F};
    float timeMs{320.0F};
    float feedback{0.25F};
    float rateHz{0.8F};
    float depth{0.35F};
    float tone{0.5F};
};

enum class EffectRouting : std::uint8_t { Parallel, Fx1IntoFx2 };

struct Combi {
    std::string name;
    std::array<UpperPart, kUpperCount> uppers{};
    EffectSlot fx1{};
    EffectSlot fx2{};
    EffectRouting fxRouting{EffectRouting::Parallel};
};

struct GlobalState {
    float masterTuneHz{440.0F};
    std::int8_t transposeSemitones{0};
    float masterGainDb{0.0F};
    float eqLowDb{0.0F};
    float eqMidDb{0.0F};
    float eqHighDb{0.0F};
    bool limiterEnabled{true};
    float limiterCeilingDb{-0.3F};
    std::uint8_t velocityCurve{0};
    // Global controller assignment. These apply to the whole instrument;
    // individual Upper enable flags still decide whether that controller affects it.
    std::uint8_t breathCc{2};
    std::uint8_t expressionCc{11};
    std::uint8_t damperCc{64};
    bool pitchBendEnabled{true};
    bool aftertouchEnabled{true};

    // GLOBAL tuning. The 12 offsets are interpreted relative to scaleRoot,
    // allowing quarter-tone/custom scales without modifying PROGRAM data.
    std::uint8_t scaleRoot{0}; // 0=C ... 11=B
    std::array<float, 12> scaleOffsetsCents{};

    // GLOBAL MIDI filtering/routing. Bit 0 = MIDI ch 1, bit 15 = MIDI ch 16.
    std::uint16_t midiInputChannelMask{0xFFFFU};
    bool filterNotes{false};
    bool filterControlChange{false};
    bool filterPitchBend{false};
    bool filterAftertouch{false};

    // GLOBAL controller calibration. Values are normalized 0..1 and then
    // shaped with pow(x, curve); curve=1 is linear.
    float breathMin{0.0F};
    float breathMax{1.0F};
    float breathCurve{1.0F};
    float expressionMin{0.0F};
    float expressionMax{1.0F};
    float expressionCurve{1.0F};
    bool damperPolarityReversed{false};
};

struct ListEntry {
    std::uint16_t number{0};
    std::string name;
    std::uint16_t combiBank{0};
    std::uint16_t combiPage{0};
    std::uint16_t combiSlot{0};
};

struct SetList {
    std::string name;
    std::vector<ListEntry> entries;
};

struct EngineState {
    Combi combi{};
    GlobalState global{};
};

} // namespace silveton::sampler
