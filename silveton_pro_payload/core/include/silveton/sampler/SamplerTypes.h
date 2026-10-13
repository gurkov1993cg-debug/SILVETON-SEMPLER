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
// SILVETON_POLYSIX_V1: built-in single-cycle waves live at these SOUND slots (not in the bank).
constexpr std::uint16_t kBuiltinWaveFirstSlot = 9001U;
constexpr int kBuiltinWaveCount = 6;
// SILVETON_BOW_V1: the bowed instruments (physical model, no recording) live in slots 9101..9104:
// 9101 violin, 9102 viola, 9103 cello, 9104 double bass.
constexpr std::uint16_t kBowFirstSlot = 9101U;
constexpr int kBowInstrumentCount = 4;

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
    // SILVETON_BOW_V1: 0 = a recording (layers above); 1..4 = a bowed string model (violin, viola,
    // cello, double bass) - such a Program has no layers, the sound is computed while playing.
    std::uint8_t bowInstrument{0U};
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
    FxSend2,
    FxSend3,   // SILVETON_FX_V3
    FxSend4
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

// SILVETON_TONE_V2: the rest of the TONE page of one sound (the basic sound of
// an Upper or one of its layers). Everything here is absolute (no PROGRAM fallback).
constexpr float kToneMaxStartMs = 2000.0F;
struct ToneExtra {
    float startMs{0.0F};          // SAMPLE START offset (ms), snapped to a zero crossing
    float endMs{0.0F};            // SAMPLE END (ms from the sample start); 0 = whole sample
    float filterEnvOctaves{0.0F}; // FILTER ENV amount, octaves (bipolar)
    float filterEnvMs{120.0F};    // FILTER ENV time (decay to zero)
    float velToLevel{1.0F};       // 0 = velocity does not change the level, 1 = fully
    float velToCutoff{0.0F};      // octaves the cutoff drops at velocity 1 (0 = none)
    float highPassHz{0.0F};       // HIGH-PASS of this sound, 0 = off
    float lfoRateHz{5.5F};        // vibrato LFO
    float lfoDepthCents{0.0F};    // 0 = no vibrato
    float lfoDelayMs{0.0F};
    float lfoFadeMs{300.0F};
    bool lfoModWheel{true};       // true = the depth follows the modulation wheel (CC1)
    float eqLowDb{0.0F};          // 3-band EQ of this sound (shelf 120 Hz / peak 1.2 kHz / shelf 7 kHz)
    float eqMidDb{0.0F};
    float eqHighDb{0.0F};
    // SILVETON_POLYSIX_V1: synthesizer functions on the recording (a Polysix whose oscillator is the sample).
    float pwmWidth{0.0F};         // 0 = off; 0.05..0.95 = the recording minus itself shifted by WIDTH x period (PWM)
    float pwmDepth{0.0F};         // LFO movement of the width (0..0.45)
    float pwmRateHz{0.5F};
    float pwmMix{1.0F};           // how much of the shifted copy is subtracted (1 = full pulse)
    float subLevel{0.0F};         // SUB: the same recording one/two octaves down, mixed in (0..1)
    std::int8_t subOctave{1};     // 1 or 2
    std::int8_t unisonVoices{1};  // 1..4 copies per note
    float unisonDetuneCents{8.0F};
    float unisonSpread{0.5F};     // stereo spread of the copies 0..1
    float drive{0.0F};            // soft saturation before the filter 0..1
    std::int8_t filterType{0};    // CUTOFF filter: 0 clean SVF, 1 POLYSIX 4-pole ladder, 2 KORG35 (MS-20)
    float filterKeyTrack{0.0F};   // -1..1: cutoff follows the key (1 = fully)
    std::int8_t filter2Type{0};   // second filter: 0 off, 1 LP, 2 HP, 3 BP, 4 BR (12 dB)
    float filter2Hz{1000.0F};
    float filter2Res{0.0F};
    bool filter2Parallel{false};  // false = after filter 1 (series), true = beside it
    bool analogFeel{false};       // tiny random detune / cutoff per note, like a real analog
    // SILVETON_SYNTH_BYPASS_V1: true = the whole SYNTH + MATRIX section is skipped (sound exactly
    // as before Polysix), knob settings kept. Switches without a click while playing.
    bool synthBypass{false};
    // SILVETON_BOW_V1: the ЛЪК page of a bowed sound (ignored for recordings).
    float bowPressure{0.5F};        // 0..1 bow pressure: 0 = flautando, 1 = edgy
    float bowPosition{0.12F};       // 0.05..0.18 bow distance from the bridge (ponticello .. tasto)
    float bowBrightness{0.5F};      // 0..1 string brightness, 0.5 = as built
    float bowVibratoCents{20.0F};   // 0..100 vibrato depth (the modulation wheel adds on top)
    float bowVibratoHz{5.5F};       // 3..9
    float bowVibratoDelayMs{250.0F};// 0..2000 the vibrato starts this long after the note
    float bowAttackMs{20.0F};       // 5..200 how fast the bow starts a stroke (short = bite)
    float bowReleaseMs{60.0F};      // 10..500 how fast the bow lifts
};

// SILVETON_POLYSIX_V1: modulation matrix of one sound - "everything to everything".
enum class ModSource : std::uint8_t { Off, Velocity, Note, Aftertouch, ModWheel, Breath, Expression, Pedal, Bend, Lfo, FilterEnv, AmpEnv, Random };
enum class ModTarget : std::uint8_t { Cutoff, Resonance, Cutoff2, Level, Pan, Pitch, LfoDepth, LfoRate, PwmWidth, Drive, Attack, Release };
constexpr int kModSourceCount = 13;
constexpr int kModTargetCount = 12;
constexpr int kModRoutes = 8;
struct ModRoute {
    ModSource source{ModSource::Off};
    ModTarget target{ModTarget::Cutoff};
    float amount{0.0F};           // -1..1 of the target's full range
    bool bipolar{false};          // unipolar source mapped to -1..1
};
struct ModMatrix {
    std::array<ModRoute, kModRoutes> route{};
};

// Full TONE of one sound. The six legacy values keep their "-1 = PROGRAM" rule.
struct SoundTone {
    float attackMs{-1.0F};
    float decayMs{-1.0F};
    float sustain{-1.0F};
    float releaseMs{-1.0F};
    float cutoffHz{-1.0F};
    float resonance{-1.0F};
    ToneExtra x{};
    ModMatrix mod{};              // SILVETON_POLYSIX_V1
};

// SILVETON_LAYERS_V1: one of the four extra layers of an Upper. A layer is a
// whole sound of its own (SOUND, level, pan, tuning, range, TONE, 3-band EQ + HP);
// the sum of the basic sound and its layers then goes through the common Upper
// processing (level, pan, ON/SOLO/MUTE, 5-band EQ, POLY/MONO/LEG, portamento, T/Q, split, controllers).
constexpr int kUpperLayers = 4;
struct UpperLayer {
    bool enabled{false};
    std::string name;
    ProgramPtr program;
    std::uint16_t soundSlotId{kNoSoundSlot};
    float levelDb{0.0F};
    float pan{0.0F};
    std::int8_t transposeSemitones{0};
    std::int8_t octave{0};
    float tuneCents{0.0F};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    SoundTone tone{};

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return enabled && program != nullptr && key >= keyLow && key <= keyHigh &&
               velocity >= velocityLow && velocity <= velocityHigh;
    }
};

// SILVETON_EQ5_V1: common EQ of an Upper. Band 1 is a low shelf, band 5 a high
// shelf, 2..4 are peaks; plus HIGH-PASS and LOW-PASS (12 or 24 dB/oct).
// A band at 0 dB (or off) costs nothing.
constexpr int kUpperEqBands = 5;
struct EqBand {
    bool on{true};
    float freqHz{1000.0F};
    float gainDb{0.0F};
    float q{0.7F};
};
struct UpperEq {
    bool on{true};
    std::array<EqBand, kUpperEqBands> band{{
        {true, 120.0F, 0.0F, 0.7F}, {true, 400.0F, 0.0F, 1.0F}, {true, 1200.0F, 0.0F, 0.75F},
        {true, 3500.0F, 0.0F, 1.0F}, {true, 7000.0F, 0.0F, 0.7F}}};
    bool hpOn{false};
    float hpHz{40.0F};
    std::uint8_t hpSlopeDb{12};   // 12 or 24
    bool lpOn{false};
    float lpHz{18000.0F};
    std::uint8_t lpSlopeDb{12};
};

// SILVETON_PORTA_MODES_V1
enum class PortamentoMode : std::uint8_t { Silveton, Kronos };

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
    ToneExtra toneExtra{};                               // SILVETON_TONE_V2: rest of the basic sound's TONE
    ModMatrix toneMod{};                                 // SILVETON_POLYSIX_V1: matrix of the basic sound
    std::array<UpperLayer, kUpperLayers> layers{};       // SILVETON_LAYERS_V1
    UpperEq eq{};                                        // SILVETON_EQ5_V1 (eqLow/Mid/HighDb above = bands 1/3/5)
    // SILVETON_PORTA_MODES_V1: KRONOS portamento (used when portaMode == Kronos)
    PortamentoMode portaMode{PortamentoMode::Silveton};
    bool portaFingered{false};         // glide only when another key is still held
    bool portaRateMode{false};         // false = TIME (same time for any interval), true = RATE (time per octave)
    std::uint8_t portaTime{40};        // 0..127 like Kronos
    float fxSend3{0.0F};               // SILVETON_FX_V3: sends 3 and 4
    float fxSend4{0.0F};
    UpperVoiceMode voiceMode{UpperVoiceMode::Poly};
    // Per-Upper realtime controller matrix. Routes are evaluated in the audio
    // callback without allocations; each Upper can use different sources/destinations.
    std::array<UpperControllerRoute, kMaxUpperControllerRoutes> controllerRoutes{};

    [[nodiscard]] bool hasAnySound() const noexcept {
        if (program != nullptr) return true;
        for (const auto& l : layers) if (l.enabled && l.program != nullptr) return true;
        return false;
    }
    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return enabled && !mute && hasAnySound() && key >= keyLow && key <= keyHigh &&
               velocity >= velocityLow && velocity <= velocityHigh;
    }
};

// SILVETON_FX_V3. The first four values keep their old numbers (old sets):
// Delay = DIGITAL delay, Reverb = PLATE.
enum class EffectType : std::uint8_t {
    Off, Delay, Chorus, Reverb,
    Hall, Room, Chamber, Spring,                  // reverbs
    TapeDelay, PingPong, MultiTap, DuckDelay,     // delays
    Flanger, Phaser, Rotary,                      // modulation
    Compressor, Drive,                            // dynamics / colour (mostly as INSERT)
    Ensemble                                      // SILVETON_POLYSIX_V1: the Polysix / string-machine ensemble
};
constexpr int kEffectTypeCount = 18;
[[nodiscard]] constexpr bool effectIsReverb(EffectType t) noexcept {
    return t == EffectType::Reverb || t == EffectType::Hall || t == EffectType::Room ||
           t == EffectType::Chamber || t == EffectType::Spring;
}
[[nodiscard]] constexpr bool effectIsDelay(EffectType t) noexcept {
    return t == EffectType::Delay || t == EffectType::TapeDelay || t == EffectType::PingPong ||
           t == EffectType::MultiTap || t == EffectType::DuckDelay;
}

// One effect. What the fields mean per type:
//  reverbs:   feedback = DECAY (0..0.95 -> 0.6..12 s), tone = TONE, preDelayMs,
//             damping = DAMPING (high-frequency loss in the tail), size = SIZE, mod = MOD
//  delays:    timeMs = TIME (or tempoBpm x division when tempoSync), feedback, tone;
//             mod = wow/flutter (TAPE), depth = DUCK amount (DUCKING) / tap spread (MULTI-TAP)
//  chorus:    rateHz, depth, tone; param1 = voices (2..4)
//  flanger:   rateHz, depth, feedback
//  phaser:    rateHz, depth, feedback; param1 = stages (4/6/8)
//  rotary:    rateHz = slow speed, param1 = fast speed, depth = horn/drum balance, param2 = 1 fast / 0 slow
//  compressor: param1 = threshold dB (-40..0), param2 = ratio (1..20), timeMs = attack ms, param3 = release ms, tone = makeup dB
//  drive:     depth = drive amount, tone = tone, param1 = output dB
struct EffectSlot {
    EffectType type{EffectType::Off};
    float wet{0.0F};
    float timeMs{320.0F};
    float feedback{0.25F};
    float rateHz{0.8F};
    float depth{0.35F};
    float tone{0.5F};
    float preDelayMs{10.0F};
    float damping{0.4F};
    float size{0.5F};
    float mod{0.3F};
    bool tempoSync{false};
    float tempoBpm{120.0F};
    float division{0.25F};   // of a whole note: 0.25 = quarter, 0.125 = eighth, 0.1875 = dotted eighth ...
    float param1{0.0F};
    float param2{0.0F};
    float param3{0.0F};
};

enum class EffectRouting : std::uint8_t { Parallel, Fx1IntoFx2 };

constexpr int kFxSendSlots = 4;   // SILVETON_FX_V3: four common send effects

struct Combi {
    std::string name;
    std::array<UpperPart, kUpperCount> uppers{};
    std::array<EffectSlot, kFxSendSlots> fx{};        // fx[0] / fx[1] were FX1 / FX2
    std::array<EffectSlot, kUpperCount> insert{};     // one INSERT per Upper (runs before the sends)
    EffectRouting fxRouting{EffectRouting::Parallel}; // FX1 into FX2
};

struct GlobalState {
    float masterTuneHz{440.0F};
    std::int8_t transposeSemitones{0};
    float masterGainDb{-6.0F};       // SILVETON_MASTER_6DB_V1: the program starts at -6 dB, no limiter
    float eqLowDb{0.0F};
    float eqMidDb{0.0F};
    float eqHighDb{0.0F};
    bool limiterEnabled{false};      // SILVETON_MASTER_6DB_V1: kept only for the tests / old sets
    float limiterCeilingDb{-6.0F};
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
    // SILVETON_FX_TAIL_V1: changes only when another COMBI / BLEND is called up
    // (not on live edits), so the old effect can ring out instead of stopping.
    std::uint32_t combiGeneration{0U};
};

} // namespace silveton::sampler
