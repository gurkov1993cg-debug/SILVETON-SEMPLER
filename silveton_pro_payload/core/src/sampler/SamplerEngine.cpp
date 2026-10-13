#include "silveton/sampler/SamplerEngine.h"
#include "silveton/sampler/SampleStream.h"
// SILVETON_CRACKLE_FIX_V1
// SILVETON_PRO_SOUND_V1

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace silveton::sampler {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr float kInv127 = 1.0F / 127.0F;

float dbToGain(float db) noexcept {
    return std::pow(10.0F, db / 20.0F);
}

float gainToDb(float gain) noexcept {
    const float g = std::max(gain, 1.0e-12F);
    return 20.0F * std::log10(g);
}

float clamp01(float v) noexcept { return std::clamp(v, 0.0F, 1.0F); }

struct Envelope {
    enum class Stage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };

    EnvelopeParams params{};
    Stage stage{Stage::Idle};
    float value{0.0F};
    float releaseStep{0.0F};

    void trigger() noexcept {
        stage = Stage::Attack;
        if (params.attackSec <= 0.0F) {
            value = 1.0F;
            stage = Stage::Decay;
        }
    }

    void release(double sr) noexcept {
        if (stage == Stage::Idle || stage == Stage::Release) return;
        stage = Stage::Release;
        const float frames = static_cast<float>(std::max(1.0, static_cast<double>(params.releaseSec) * sr));
        releaseStep = value / frames;
    }

    void forceOff() noexcept {
        value = 0.0F;
        stage = Stage::Idle;
        releaseStep = 0.0F;
    }

    [[nodiscard]] bool active() const noexcept { return stage != Stage::Idle; }

    float process(double sr) noexcept {
        const float srF = static_cast<float>(std::max(1.0, sr));
        switch (stage) {
            case Stage::Idle:
                value = 0.0F;
                break;
            case Stage::Attack: {
                if (params.attackSec <= 0.0F) {
                    value = 1.0F;
                    stage = Stage::Decay;
                } else {
                    value += 1.0F / (params.attackSec * srF);
                    if (value >= 1.0F) {
                        value = 1.0F;
                        stage = Stage::Decay;
                    }
                }
                break;
            }
            case Stage::Decay: {
                const float sus = clamp01(params.sustain);
                if (params.decaySec <= 0.0F) {
                    value = sus;
                    stage = Stage::Sustain;
                } else {
                    value -= (1.0F - sus) / (params.decaySec * srF);
                    if (value <= sus) {
                        value = sus;
                        stage = Stage::Sustain;
                    }
                }
                break;
            }
            case Stage::Sustain:
                value = clamp01(params.sustain);
                break;
            case Stage::Release:
                value -= releaseStep;
                if (value <= 0.0F || !std::isfinite(value)) {
                    value = 0.0F;
                    stage = Stage::Idle;
                }
                break;
        }
        return value;
    }
};

constexpr float kAttackRatio = 0.3F;   // overshoot ratio of the attack curve
constexpr float kTailRatio = 1.0e-4F;    // decay/release end 80 dB down
constexpr float kMinAttackMs = 0.3F;
constexpr float kMinTimeMs = 1.0F;
constexpr float kMaxFallbackMs = 60000.0F;

// Values of one sound's TONE page for one PROGRAM layer. Unset (negative)
// values fall back to the layer envelope times the Easy Edit scales of the Upper.
UpperToneValues effectiveToneFor(const SoundTone& s, const UpperPart& u, const EnvelopeParams& env) noexcept {
    UpperToneValues t;
    const auto ms = [](float sec, float scale) noexcept {
        const float v = sec * std::clamp(scale, 0.0F, 16.0F) * 1000.0F;
        return std::isfinite(v) ? std::clamp(v, 0.0F, kMaxFallbackMs) : 0.0F;
    };
    t.attackMs = s.attackMs >= 0.0F ? std::min(s.attackMs, kToneMaxTimeMs) : ms(env.attackSec, u.ampAttackScale);
    t.decayMs = s.decayMs >= 0.0F ? std::min(s.decayMs, kToneMaxTimeMs) : ms(env.decaySec, u.ampDecayScale);
    t.releaseMs = s.releaseMs >= 0.0F ? std::min(s.releaseMs, kToneMaxTimeMs) : ms(env.releaseSec, u.ampReleaseScale);
    t.sustain = s.sustain >= 0.0F ? std::min(s.sustain, 1.0F)
                                  : std::clamp(env.sustain * std::clamp(u.ampSustainScale, 0.0F, 2.0F), 0.0F, 1.0F);
    t.cutoffHz = s.cutoffHz >= 0.0F ? std::clamp(s.cutoffHz, 20.0F, kToneCutoffOpenHz) : kToneCutoffOpenHz;
    t.resonance = s.resonance >= 0.0F ? std::min(s.resonance, 1.0F) : 0.0F;
    return t;
}

SoundTone basicToneOf(const UpperPart& u) noexcept {
    SoundTone s;
    s.attackMs = u.toneAttackMs; s.decayMs = u.toneDecayMs; s.sustain = u.toneSustain;
    s.releaseMs = u.toneReleaseMs; s.cutoffHz = u.toneCutoffHz; s.resonance = u.toneResonance;
    s.x = u.toneExtra;
    s.mod = u.toneMod;
    return s;
}

// SILVETON_LAYERS_V1: sound 0 = the basic sound of the Upper, 1..4 = its layers.
constexpr int kSoundsPerUpper = 1 + kUpperLayers;

const UpperLayer* layerOf(const UpperPart& u, int soundIndex) noexcept {
    return (soundIndex >= 1 && soundIndex <= kUpperLayers) ? &u.layers[static_cast<std::size_t>(soundIndex - 1)] : nullptr;
}

SoundTone soundToneOf(const UpperPart& u, int soundIndex) noexcept {
    const UpperLayer* l = layerOf(u, soundIndex);
    SoundTone t = l != nullptr ? l->tone : basicToneOf(u);
    // SILVETON_SYNTH_BYPASS_V1: SYNTH ИЗКЛ = every Polysix function and the matrix read as "off";
    // the stored knob values stay untouched, so SYNTH ВКЛ brings them straight back.
    if (t.x.synthBypass) {
        t.x.pwmWidth = 0.0F; t.x.pwmDepth = 0.0F; t.x.subLevel = 0.0F; t.x.unisonVoices = 1; t.x.drive = 0.0F;
        t.x.filterType = 0; t.x.filterKeyTrack = 0.0F; t.x.filter2Type = 0; t.x.analogFeel = false;
        for (auto& r : t.mod.route) r.source = ModSource::Off;
    }
    return t;
}

const Program* soundProgramOf(const UpperPart& u, int soundIndex) noexcept {
    if (soundIndex == 0) return u.program.get();
    const UpperLayer* l = layerOf(u, soundIndex);
    return (l != nullptr && l->enabled) ? l->program.get() : nullptr;
}

// Professional exponential ADSR (one-pole segments). The release time is exact
// for the level at which the key was released; a SUSTAIN edit glides in ~10 ms.
struct AmpEnv {
    enum class Stage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage stage{Stage::Idle};
    float value{0.0F};
    float attackCoef{0.0F}, attackBase{0.0F};
    float decayCoef{0.0F}, decayBase{0.0F};
    float releaseCoef{0.0F}, releaseBase{0.0F};
    float sustainTarget{1.0F};
    float sustainSmooth{0.0F};
    float releaseMs{120.0F};

    void trigger() noexcept { stage = Stage::Attack; }

    void setBlock(const UpperToneValues& t, double sr) noexcept {
        const double rate = std::max(1.0, sr);
        sustainTarget = std::clamp(t.sustain, 0.0F, 1.0F);
        releaseMs = std::max(kMinTimeMs, t.releaseMs);
        sustainSmooth = static_cast<float>(1.0 - std::exp(-1.0 / (0.010 * rate)));
        const double na = std::max(1.0, static_cast<double>(std::max(kMinAttackMs, t.attackMs)) * 0.001 * rate);
        attackCoef = static_cast<float>(std::exp(-std::log((1.0 + kAttackRatio) / kAttackRatio) / na));
        attackBase = (1.0F + kAttackRatio) * (1.0F - attackCoef);
        const double nd = std::max(1.0, static_cast<double>(std::max(kMinTimeMs, t.decayMs)) * 0.001 * rate);
        decayCoef = static_cast<float>(std::exp(-std::log((1.0 + kTailRatio) / kTailRatio) / nd));
        const float range = std::max(1.0F - sustainTarget, 1.0e-4F);
        decayBase = (sustainTarget - kTailRatio * range) * (1.0F - decayCoef);
    }

    void release(double sr) noexcept {
        if (stage == Stage::Idle || stage == Stage::Release) return;
        if (value <= 1.0e-6F) { value = 0.0F; stage = Stage::Idle; return; }
        stage = Stage::Release;
        const double n = std::max(1.0, static_cast<double>(releaseMs) * 0.001 * std::max(1.0, sr));
        releaseCoef = static_cast<float>(std::exp(-std::log((static_cast<double>(value) + kTailRatio) / kTailRatio) / n));
        releaseBase = -kTailRatio * (1.0F - releaseCoef);
    }

    void forceOff() noexcept { value = 0.0F; stage = Stage::Idle; }
    [[nodiscard]] bool active() const noexcept { return stage != Stage::Idle; }

    float process() noexcept {
        switch (stage) {
            case Stage::Idle: value = 0.0F; break;
            case Stage::Attack:
                value = attackBase + value * attackCoef;
                if (value >= 1.0F) { value = 1.0F; stage = Stage::Decay; }
                break;
            case Stage::Decay:
                value = decayBase + value * decayCoef;
                if (value <= sustainTarget) { value = sustainTarget; stage = Stage::Sustain; }
                break;
            case Stage::Sustain:
                value += (sustainTarget - value) * sustainSmooth;
                break;
            case Stage::Release:
                value = releaseBase + value * releaseCoef;
                if (value <= 0.0F || !std::isfinite(value)) { value = 0.0F; stage = Stage::Idle; }
                break;
        }
        return value;
    }
};

// 12 dB/oct low-pass (TPT state variable) for the Upper TONE CUTOFF.
struct ToneLowPass {
    float a1{0.0F}, a2{0.0F}, a3{0.0F};
    float ic1L{0.0F}, ic2L{0.0F}, ic1R{0.0F}, ic2R{0.0F};
    bool prime{true};

    void set(float cutoffHz, float resonance, double sr) noexcept {
        const double fc = std::clamp(static_cast<double>(cutoffHz), 20.0, 0.45 * std::max(1.0, sr));
        const double q = 0.70710678 * std::pow(2.0, static_cast<double>(std::clamp(resonance, 0.0F, 1.0F)) * 3.8);
        const double g = std::tan(kPi * fc / std::max(1.0, sr));
        const double k = 1.0 / q;
        const double a1d = 1.0 / (1.0 + g * (g + k));
        a1 = static_cast<float>(a1d);
        a2 = static_cast<float>(g * a1d);
        a3 = static_cast<float>(g * g * a1d);
    }
    static float tick(float x, float a1, float a2, float a3, float& ic1, float& ic2) noexcept {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0F * v1 - ic1;
        ic2 = 2.0F * v2 - ic2;
        return v2;
    }
    void process(float& l, float& r) noexcept {
        if (prime) { ic1L = ic1R = 0.0F; ic2L = l; ic2R = r; prime = false; }
        l = tick(l, a1, a2, a3, ic1L, ic2L);
        r = tick(r, a1, a2, a3, ic1R, ic2R);
    }
};

// SILVETON_POLYSIX_V1: 4-pole ladder low-pass (the Polysix / SSM2044 family), zero-delay
// feedback (Zavalishin): four one-pole stages, resonance feeds the output back to the
// input, a soft saturation at the input keeps it musical when it screams.
struct LadderFilter {
    float G{0.0F}, k{0.0F}, comp{1.0F};
    std::array<float, 4> sL{}, sR{};
    bool prime{true};
    void set(float cutoffHz, float resonance, double sr) noexcept {
        const double fc = std::clamp(static_cast<double>(cutoffHz), 20.0, 0.45 * std::max(1.0, sr));
        const double g = std::tan(kPi * fc / std::max(1.0, sr));
        G = static_cast<float>(g / (1.0 + g));
        k = 4.0F * std::clamp(resonance, 0.0F, 1.0F) * 0.98F;
        comp = 1.0F + 0.5F * k;   // keep the bass when the resonance is up
    }
    float tick(float x, std::array<float, 4>& st) const noexcept {
        const float g1 = 1.0F - G;
        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        const float S = g1 * (G3 * st[0] + G2 * st[1] + G * st[2] + st[3]);   // state contribution to the output
        float u = (x * comp - k * S) / (1.0F + k * G4);
        u = std::tanh(u * 0.5F) * 2.0F;   // soft clip
        for (int i = 0; i < 4; ++i) { const float vI = (u - st[static_cast<std::size_t>(i)]) * G; const float y = vI + st[static_cast<std::size_t>(i)]; st[static_cast<std::size_t>(i)] = y + vI; u = y; }
        return u;
    }
    void process(float& l, float& r) noexcept {
        if (prime) { sL.fill(0.0F); sR.fill(0.0F); prime = false; }
        l = tick(l, sL); r = tick(r, sR);
    }
};

// SILVETON_POLYSIX_V1: Korg35 low-pass (the MS-20 filter), after W. Pirkle's zero-delay
// model: LPF1 -> LPF2, a HPF in the feedback with gain K (0..2); K near 2 self-oscillates.
struct Korg35Filter {
    float G{0.0F}, K{0.0F};
    float s1L{0.0F}, s2L{0.0F}, s3L{0.0F}, s1R{0.0F}, s2R{0.0F}, s3R{0.0F};
    bool prime{true};
    void set(float cutoffHz, float resonance, double sr) noexcept {
        const double fc = std::clamp(static_cast<double>(cutoffHz), 20.0, 0.45 * std::max(1.0, sr));
        const double g = std::tan(kPi * fc / std::max(1.0, sr));
        G = static_cast<float>(g / (1.0 + g));
        K = 2.0F * std::clamp(resonance, 0.0F, 1.0F) * 0.985F;
    }
    float tick(float x, float& s1, float& s2, float& s3) const noexcept {
        // LPF1
        const float v1 = (x - s1) * G; const float u = v1 + s1; s1 = u + v1;
        // LPF2 with the HPF1 feedback solved instantly
        const float g1 = 1.0F - G;
        const float den = 1.0F - G * K * g1;
        float y = (G * u - G * K * g1 * s3 + g1 * s2) / std::max(den, 0.05F);
        y = std::tanh(y * 0.8F) * 1.25F;   // the nonlinearity that gives the MS-20 its growl
        const float in2 = u + K * g1 * (y - s3);
        const float v2 = (in2 - s2) * G; const float y2 = v2 + s2; s2 = y2 + v2;
        const float v3 = (y - s3) * G; const float lp3 = v3 + s3; s3 = lp3 + v3;
        return y2;
    }
    void process(float& l, float& r) noexcept {
        if (prime) { s1L = s2L = s3L = s1R = s2R = s3R = 0.0F; prime = false; }
        l = tick(l, s1L, s2L, s3L); r = tick(r, s1R, s2R, s3R);
    }
};

struct LfoState {
    double phase{0.0};
    double ageSec{0.0};
    std::uint32_t randomState{0x12345678U};
    float heldRandom{0.0F};

    void reset(bool keySync) noexcept {
        ageSec = 0.0;
        if (keySync) phase = 0.0;
    }

    float process(const LfoParams& p, double sr) noexcept {
        const double safeSr = std::max(1.0, sr);
        ageSec += 1.0 / safeSr;
        const double oldPhase = phase;
        phase += std::max(0.0F, p.frequencyHz) / safeSr;
        phase -= std::floor(phase);

        float v = 0.0F;
        switch (p.wave) {
            case LfoWave::Sine: v = static_cast<float>(std::sin(2.0 * kPi * phase)); break;
            case LfoWave::Triangle: v = static_cast<float>(1.0 - 4.0 * std::abs(phase - 0.5)); break;
            case LfoWave::SawUp: v = static_cast<float>(2.0 * phase - 1.0); break;
            case LfoWave::SawDown: v = static_cast<float>(1.0 - 2.0 * phase); break;
            case LfoWave::Square: v = phase < 0.5 ? 1.0F : -1.0F; break;
            case LfoWave::RandomHold:
                if (phase < oldPhase) {
                    randomState = randomState * 1664525U + 1013904223U;
                    heldRandom = static_cast<float>((static_cast<double>(randomState) / 4294967295.0) * 2.0 - 1.0);
                }
                v = heldRandom;
                break;
        }

        if (ageSec < static_cast<double>(std::max(0.0F, p.delaySec))) return 0.0F;
        const float fade = p.fadeSec <= 0.0F
            ? 1.0F
            : clamp01(static_cast<float>((ageSec - static_cast<double>(p.delaySec)) / static_cast<double>(p.fadeSec)));
        return v * fade;
    }
};

struct StateVariableFilter {
    float ic1eqL{0.0F};
    float ic2eqL{0.0F};
    float ic1eqR{0.0F};
    float ic2eqR{0.0F};

    void reset() noexcept { ic1eqL = ic2eqL = ic1eqR = ic2eqR = 0.0F; }

    float processOne(float x, float& ic1eq, float& ic2eq, FilterMode mode,
                     float cutoffHz, float resonance, double sampleRate) noexcept {
        if (mode == FilterMode::Bypass) return x;
        const float nyquistSafe = static_cast<float>(std::max(100.0, sampleRate * 0.45));
        const float cutoff = std::clamp(cutoffHz, 20.0F, nyquistSafe);
        const float g = std::tan(static_cast<float>(kPi) * cutoff / static_cast<float>(sampleRate));
        const float q = 0.5F + std::clamp(resonance, 0.0F, 1.0F) * 11.5F;
        const float k = 1.0F / q;
        const float a1 = 1.0F / (1.0F + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = x - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0F * v1 - ic1eq;
        ic2eq = 2.0F * v2 - ic2eq;
        const float low = v2;
        const float band = v1;
        const float high = x - k * band - low;
        switch (mode) {
            case FilterMode::LowPass: return low;
            case FilterMode::HighPass: return high;
            case FilterMode::BandPass: return band;
            case FilterMode::Bypass: return x;
        }
        return x;
    }

    // SILVETON_RENDER_SPEED_V1: the coefficients (two tan() per sample before)
    // are computed only when cutoff / resonance really change. Same formulas,
    // same result.
    float cCut{-1.0F}, cRes{-1.0F};
    double cSr{0.0};
    float cK{0.0F}, cA1{0.0F}, cA2{0.0F}, cA3{0.0F};

    float processCached(float x, float& ic1eq, float& ic2eq, FilterMode mode) const noexcept {
        const float v3 = x - ic2eq;
        const float v1 = cA1 * ic1eq + cA2 * v3;
        const float v2 = ic2eq + cA2 * ic1eq + cA3 * v3;
        ic1eq = 2.0F * v1 - ic1eq;
        ic2eq = 2.0F * v2 - ic2eq;
        switch (mode) {
            case FilterMode::LowPass: return v2;
            case FilterMode::HighPass: return x - cK * v1 - v2;
            case FilterMode::BandPass: return v1;
            case FilterMode::Bypass: return x;
        }
        return x;
    }

    void process(float& l, float& r, FilterMode mode, float cutoffHz, float resonance, double sr) noexcept {
        if (mode == FilterMode::Bypass) return;
        if (cutoffHz != cCut || resonance != cRes || sr != cSr) {
            cCut = cutoffHz; cRes = resonance; cSr = sr;
            const float nyquistSafe = static_cast<float>(std::max(100.0, sr * 0.45));
            const float cutoff = std::clamp(cutoffHz, 20.0F, nyquistSafe);
            const float g = std::tan(static_cast<float>(kPi) * cutoff / static_cast<float>(sr));
            const float q = 0.5F + std::clamp(resonance, 0.0F, 1.0F) * 11.5F;
            cK = 1.0F / q;
            cA1 = 1.0F / (1.0F + g * (g + cK));
            cA2 = g * cA1;
            cA3 = g * cA2;
        }
        l = processCached(l, ic1eqL, ic2eqL, mode);
        r = processCached(r, ic1eqR, ic2eqR, mode);
    }
};

struct Biquad {
    float b0{1.0F}, b1{0.0F}, b2{0.0F}, a1{0.0F}, a2{0.0F};
    float z1L{0.0F}, z2L{0.0F}, z1R{0.0F}, z2R{0.0F};

    void reset() noexcept { z1L = z2L = z1R = z2R = 0.0F; }

    void setPeaking(double sr, float freq, float q, float gainDb) noexcept {
        const double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        const double w0 = 2.0 * kPi * static_cast<double>(freq) / sr;
        const double alpha = std::sin(w0) / (2.0 * std::max(0.1, static_cast<double>(q)));
        const double c = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        b0 = static_cast<float>((1.0 + alpha * A) / a0);
        b1 = static_cast<float>((-2.0 * c) / a0);
        b2 = static_cast<float>((1.0 - alpha * A) / a0);
        a1 = static_cast<float>((-2.0 * c) / a0);
        a2 = static_cast<float>((1.0 - alpha / A) / a0);
    }

    void setShelf(double sr, float freq, float gainDb, bool high) noexcept {
        const double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        const double w0 = 2.0 * kPi * static_cast<double>(freq) / sr;
        const double c = std::cos(w0);
        const double s = std::sin(w0);
        const double alpha = s / std::sqrt(2.0);
        const double beta = 2.0 * std::sqrt(A) * alpha;
        double B0, B1, B2, A0, A1, A2;
        if (high) {
            B0 = A * ((A + 1.0) + (A - 1.0) * c + beta);
            B1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c);
            B2 = A * ((A + 1.0) + (A - 1.0) * c - beta);
            A0 = (A + 1.0) - (A - 1.0) * c + beta;
            A1 = 2.0 * ((A - 1.0) - (A + 1.0) * c);
            A2 = (A + 1.0) - (A - 1.0) * c - beta;
        } else {
            B0 = A * ((A + 1.0) - (A - 1.0) * c + beta);
            B1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c);
            B2 = A * ((A + 1.0) - (A - 1.0) * c - beta);
            A0 = (A + 1.0) + (A - 1.0) * c + beta;
            A1 = -2.0 * ((A - 1.0) + (A + 1.0) * c);
            A2 = (A + 1.0) + (A - 1.0) * c - beta;
        }
        b0 = static_cast<float>(B0 / A0);
        b1 = static_cast<float>(B1 / A0);
        b2 = static_cast<float>(B2 / A0);
        a1 = static_cast<float>(A1 / A0);
        a2 = static_cast<float>(A2 / A0);
    }

    static float tick(float x, float& z1, float& z2, float b0, float b1, float b2, float a1, float a2) noexcept {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void process(float& l, float& r) noexcept {
        l = tick(l, z1L, z2L, b0, b1, b2, a1, a2);
        r = tick(r, z1R, z2R, b0, b1, b2, a1, a2);
    }
};

// SILVETON_EQ5_V1: double-precision biquad (TDF2) for the common Upper EQ.
// Peaks use the Orfanidis design ("Digital Parametric Equalizer Design With
// Prescribed Nyquist-Frequency Gain", 1997): the digital curve matches the analog
// one up to Nyquist instead of cramping towards 20 kHz. Shelves are RBJ (at
// 96 kHz their cramping is negligible); HP/LP are Butterworth sections.
struct DBiquad {
    double b0{1.0}, b1{0.0}, b2{0.0}, a1{0.0}, a2{0.0};
    double z1L{0.0}, z2L{0.0}, z1R{0.0}, z2R{0.0};

    void reset() noexcept { z1L = z2L = z1R = z2R = 0.0; }
    void identity() noexcept { b0 = 1.0; b1 = b2 = a1 = a2 = 0.0; }

    void setPeakingMatched(double sr, double freq, double q, double gainDb) noexcept {
        const double G = std::pow(10.0, gainDb / 20.0);
        if (std::abs(gainDb) < 1.0e-6) { identity(); return; }
        const double w0 = 2.0 * kPi * std::clamp(freq, 10.0, 0.49 * sr) / sr;
        const double Dw = w0 / std::max(0.05, q);
        const double G0 = 1.0;                       // gain at DC
        const double GB = std::sqrt(G);              // bandwidth gain (geometric mean)
        const double G2 = G * G, GB2 = GB * GB, G02 = G0 * G0;
        const double F = std::abs(G2 - GB2);
        const double G00 = std::abs(G2 - G02);
        const double F00 = std::abs(GB2 - G02);
        // Gain of the analog prototype at the Nyquist frequency: that is what the
        // digital filter is made to reach there (instead of returning to 0 dB).
        const double num = G02 * (w0 * w0 - kPi * kPi) * (w0 * w0 - kPi * kPi) + G2 * F00 * kPi * kPi * Dw * Dw / F;
        const double den = (w0 * w0 - kPi * kPi) * (w0 * w0 - kPi * kPi) + F00 * kPi * kPi * Dw * Dw / F;
        const double G1u = std::sqrt(std::max(0.0, num / den));
        const double G12u = G1u * G1u;
        const double G01 = std::abs(G2 - G0 * G1u);
        const double G11 = std::abs(G2 - G12u);
        const double F01 = std::abs(GB2 - G0 * G1u);
        const double F11 = std::abs(GB2 - G12u);
        const double W2 = std::sqrt(G11 / G00) * std::tan(w0 / 2.0) * std::tan(w0 / 2.0);
        const double DW = (1.0 + std::sqrt(F00 / F11) * W2) * std::tan(Dw / 2.0);
        const double C = F11 * DW * DW - 2.0 * W2 * (F01 - std::sqrt(F00 * F11));
        const double D = 2.0 * W2 * (G01 - std::sqrt(G00 * G11));
        const double A = std::sqrt(std::max(0.0, (C + D) / F));
        const double B = std::sqrt(std::max(0.0, (G2 * C + GB2 * D) / F));
        const double a0 = 1.0 + W2 + A;
        b0 = (G1u + G0 * W2 + B) / a0;
        b1 = -2.0 * (G1u - G0 * W2) / a0;
        b2 = (G1u - B + G0 * W2) / a0;
        a1 = -2.0 * (1.0 - W2) / a0;
        a2 = (1.0 + W2 - A) / a0;
        if (!std::isfinite(b0) || !std::isfinite(b1) || !std::isfinite(b2) || !std::isfinite(a1) || !std::isfinite(a2)) {
            setPeakingRbj(sr, freq, q, gainDb);
        }
    }

    void setPeakingRbj(double sr, double freq, double q, double gainDb) noexcept {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * std::clamp(freq, 10.0, 0.49 * sr) / sr;
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));
        const double c = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0; b1 = (-2.0 * c) / a0; b2 = (1.0 - alpha * A) / a0;
        a1 = (-2.0 * c) / a0; a2 = (1.0 - alpha / A) / a0;
    }

    void setShelf(double sr, double freq, double gainDb, double q, bool high) noexcept {
        if (std::abs(gainDb) < 1.0e-6) { identity(); return; }
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * std::clamp(freq, 10.0, 0.49 * sr) / sr;
        const double c = std::cos(w0), s = std::sin(w0);
        const double S = std::clamp(q, 0.3, 1.5);   // shelf slope: 1 = steepest without ripple
        const double alpha = s / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
        const double beta = 2.0 * std::sqrt(A) * alpha;
        double B0, B1, B2, A0, A1, A2;
        if (high) {
            B0 = A * ((A + 1.0) + (A - 1.0) * c + beta); B1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c); B2 = A * ((A + 1.0) + (A - 1.0) * c - beta);
            A0 = (A + 1.0) - (A - 1.0) * c + beta; A1 = 2.0 * ((A - 1.0) - (A + 1.0) * c); A2 = (A + 1.0) - (A - 1.0) * c - beta;
        } else {
            B0 = A * ((A + 1.0) - (A - 1.0) * c + beta); B1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c); B2 = A * ((A + 1.0) - (A - 1.0) * c - beta);
            A0 = (A + 1.0) + (A - 1.0) * c + beta; A1 = -2.0 * ((A - 1.0) + (A + 1.0) * c); A2 = (A + 1.0) + (A - 1.0) * c - beta;
        }
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }

    // One Butterworth section (q = 0.7071 for 12 dB; 0.5412 and 1.3066 for 24 dB).
    void setPass(double sr, double freq, double q, bool high) noexcept {
        const double w0 = 2.0 * kPi * std::clamp(freq, 10.0, 0.49 * sr) / sr;
        const double c = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (high) { b0 = (1.0 + c) / 2.0 / a0; b1 = -(1.0 + c) / a0; b2 = b0; }
        else { b0 = (1.0 - c) / 2.0 / a0; b1 = (1.0 - c) / a0; b2 = b0; }
        a1 = -2.0 * c / a0; a2 = (1.0 - alpha) / a0;
    }

    static double tick(double x, double& z1, double& z2, double b0, double b1, double b2, double a1, double a2) noexcept {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void process(float& l, float& r) noexcept {
        l = static_cast<float>(tick(l, z1L, z2L, b0, b1, b2, a1, a2));
        r = static_cast<float>(tick(r, z1R, z2R, b0, b1, b2, a1, a2));
    }
};

// The running state of one Upper's common EQ: targets move smoothly (about
// 20 ms), coefficients are recomputed only while something moves.
struct UpperEqState {
    std::array<DBiquad, kUpperEqBands> band{};
    std::array<DBiquad, 2U> hp{}, lp{};
    std::array<float, kUpperEqBands> gainNow{}, freqNow{}, qNow{};
    float hpNow{40.0F}, lpNow{18000.0F};
    std::array<bool, kUpperEqBands> bandActive{};
    bool hpActive{false}, lpActive{false};
    std::uint8_t hpSlope{12U}, lpSlope{12U};
    bool primed{false};

    void reset() noexcept {
        for (auto& b : band) b.reset();
        for (auto& b : hp) b.reset();
        for (auto& b : lp) b.reset();
        bandActive.fill(false); hpActive = lpActive = false; primed = false;
    }

    static bool approach(float& now, float target, float alpha, float eps) noexcept {
        if (now == target) return false;
        now += (target - now) * alpha;
        if (std::abs(now - target) <= eps) now = target;
        return true;
    }

    // Once per block. alpha = smoothing fraction for this block length.
    void update(const UpperEq& eq, double sr, float alpha) noexcept {
        if (!primed) {
            for (std::size_t i = 0; i < band.size(); ++i) { gainNow[i] = eq.on && eq.band[i].on ? eq.band[i].gainDb : 0.0F; freqNow[i] = eq.band[i].freqHz; qNow[i] = eq.band[i].q; }
            hpNow = eq.hpHz; lpNow = eq.lpHz;
            primed = true;
            for (std::size_t i = 0; i < band.size(); ++i) coef(i, sr);
            hpSlope = eq.hpSlopeDb; lpSlope = eq.lpSlopeDb;
            coefPass(sr, true); coefPass(sr, false);
        }
        for (std::size_t i = 0; i < band.size(); ++i) {
            const auto& b = eq.band[i];
            const float gTarget = (eq.on && b.on) ? std::clamp(b.gainDb, -18.0F, 18.0F) : 0.0F;
            bool moved = approach(gainNow[i], gTarget, alpha, 0.005F);
            moved |= approach(freqNow[i], std::clamp(b.freqHz, 20.0F, 20000.0F), alpha, 0.05F);
            moved |= approach(qNow[i], std::clamp(b.q, 0.1F, 10.0F), alpha, 0.002F);
            if (moved) coef(i, sr);
        }
        const bool wantHp = eq.on && eq.hpOn;
        const bool wantLp = eq.on && eq.lpOn;
        if (wantHp != hpActive) { hpActive = wantHp; hp[0].reset(); hp[1].reset(); }
        if (wantLp != lpActive) { lpActive = wantLp; lp[0].reset(); lp[1].reset(); }
        bool hpMoved = approach(hpNow, std::clamp(eq.hpHz, 10.0F, 2000.0F), alpha, 0.05F);
        if (hpSlope != eq.hpSlopeDb) { hpSlope = eq.hpSlopeDb; hpMoved = true; }
        if (hpMoved) coefPass(sr, true);
        bool lpMoved = approach(lpNow, std::clamp(eq.lpHz, 1000.0F, 20000.0F), alpha, 0.5F);
        if (lpSlope != eq.lpSlopeDb) { lpSlope = eq.lpSlopeDb; lpMoved = true; }
        if (lpMoved) coefPass(sr, false);
    }

    void coef(std::size_t i, double sr) noexcept {
        const bool active = std::abs(gainNow[i]) >= 0.01F;
        if (active && !bandActive[i]) band[i].reset();
        bandActive[i] = active;
        if (!active) return;
        if (i == 0U) band[i].setShelf(sr, freqNow[i], gainNow[i], 1.0, false);
        else if (i == static_cast<std::size_t>(kUpperEqBands - 1)) band[i].setShelf(sr, freqNow[i], gainNow[i], 1.0, true);
        else band[i].setPeakingMatched(sr, freqNow[i], qNow[i], gainNow[i]);
    }

    void coefPass(double sr, bool high) noexcept {
        auto& sec = high ? hp : lp;
        const double f = high ? hpNow : lpNow;
        const bool steep = (high ? hpSlope : lpSlope) >= 24U;
        if (steep) { sec[0].setPass(sr, f, 0.54119610, high); sec[1].setPass(sr, f, 1.30656296, high); }
        else { sec[0].setPass(sr, f, 0.70710678, high); sec[1].identity(); }
    }

    void process(float& l, float& r) noexcept {
        if (hpActive) { hp[0].process(l, r); if (hpSlope >= 24U) hp[1].process(l, r); }
        for (std::size_t i = 0; i < band.size(); ++i) if (bandActive[i]) band[i].process(l, r);
        if (lpActive) { lp[0].process(l, r); if (lpSlope >= 24U) lp[1].process(l, r); }
    }
};

// SILVETON_TONE_V2: per-sound bus filters (HIGH-PASS + 3-band EQ of the basic
// sound or a layer), set from the TONE values once per block when they change.
struct SoundBusState {
    Biquad hp{}, low{}, mid{}, high{};
    float hpHz{-1.0F}, lowDb{0.0F}, midDb{0.0F}, highDb{0.0F};
    bool hpOn{false}, lowOn{false}, midOn{false}, highOn{false};

    void reset() noexcept { hp.reset(); low.reset(); mid.reset(); high.reset(); hpHz = -1.0F; hpOn = lowOn = midOn = highOn = false; }

    void update(const ToneExtra& x, double sr) noexcept {
        const float hz = std::clamp(x.highPassHz, 0.0F, 4000.0F);
        if (hz != hpHz) {
            hpHz = hz;
            const bool on = hz >= 10.0F;
            if (on && !hpOn) hp.reset();
            hpOn = on;
            if (on) {
                DBiquad d; d.setPass(sr, hz, 0.70710678, true);
                hp.b0 = static_cast<float>(d.b0); hp.b1 = static_cast<float>(d.b1); hp.b2 = static_cast<float>(d.b2);
                hp.a1 = static_cast<float>(d.a1); hp.a2 = static_cast<float>(d.a2);
            }
        }
        const auto upd = [sr](Biquad& q, float& now, bool& on, float target, float freq, bool shelfHigh, bool peak) noexcept {
            const float t = std::clamp(target, -18.0F, 18.0F);
            if (t == now) return;
            now = t;
            const bool want = std::abs(t) >= 0.01F;
            if (want && !on) q.reset();
            on = want;
            if (!want) return;
            if (peak) q.setPeaking(sr, freq, 0.75F, t); else q.setShelf(sr, freq, t, shelfHigh);
        };
        upd(low, lowDb, lowOn, x.eqLowDb, 120.0F, false, false);
        upd(mid, midDb, midOn, x.eqMidDb, 1200.0F, false, true);
        upd(high, highDb, highOn, x.eqHighDb, 7000.0F, true, false);
    }

    [[nodiscard]] bool any() const noexcept { return hpOn || lowOn || midOn || highOn; }
    void process(float& l, float& r) noexcept {
        if (hpOn) hp.process(l, r);
        if (lowOn) low.process(l, r);
        if (midOn) mid.process(l, r);
        if (highOn) high.process(l, r);
    }
};

float reverbSecondsFor(float feedback) noexcept;

// SILVETON_FX_V3: a stereo delay line on the heap (sized once in prepare()).
struct HeapLine {
    std::vector<float> left, right;
    std::size_t size{0U}, write{0U};

    void allocate(std::size_t frames) { size = std::max<std::size_t>(frames, 16U); left.assign(size, 0.0F); right.assign(size, 0.0F); write = 0U; }
    void clear() noexcept { std::fill(left.begin(), left.end(), 0.0F); std::fill(right.begin(), right.end(), 0.0F); write = 0U; }
    [[nodiscard]] double maxDelay() const noexcept { return static_cast<double>(size - 3U); }

    static float tapOne(const std::vector<float>& b, std::size_t size, std::size_t write, double delayFrames) noexcept {
        double rp = static_cast<double>(write) - delayFrames;
        while (rp < 0.0) rp += static_cast<double>(size);
        const auto i0 = static_cast<std::size_t>(rp) % size;
        const auto i1 = (i0 + 1U) % size;
        const float frac = static_cast<float>(rp - std::floor(rp));
        return b[i0] + (b[i1] - b[i0]) * frac;
    }
    [[nodiscard]] float tapL(double d) const noexcept { return tapOne(left, size, write, d); }
    [[nodiscard]] float tapR(double d) const noexcept { return tapOne(right, size, write, d); }
    // Hermite (4-point) read: cleaner than linear for modulated (tape/chorus) taps.
    static float tapHermite(const std::vector<float>& b, std::size_t size, std::size_t write, double delayFrames) noexcept {
        double rp = static_cast<double>(write) - delayFrames;
        while (rp < 0.0) rp += static_cast<double>(size);
        const auto i1 = static_cast<std::size_t>(rp) % size;
        const float t = static_cast<float>(rp - std::floor(rp));
        const float y0 = b[(i1 + size - 1U) % size], y1 = b[i1], y2 = b[(i1 + 1U) % size], y3 = b[(i1 + 2U) % size];
        const float c0 = y1, c1 = 0.5F * (y2 - y0), c2 = y0 - 2.5F * y1 + 2.0F * y2 - 0.5F * y3, c3 = 0.5F * (y3 - y0) + 1.5F * (y1 - y2);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }
    [[nodiscard]] float tapHL(double d) const noexcept { return tapHermite(left, size, write, d); }
    [[nodiscard]] float tapHR(double d) const noexcept { return tapHermite(right, size, write, d); }
    void push(float l, float r) noexcept { left[write] = l; right[write] = r; write = (write + 1U) % size; }
};

// One-pole low-pass used all over the effects (tone, damping).
struct OnePole {
    float z{0.0F};
    float process(float x, float a) noexcept { z += (x - z) * a; return z; }
};

// A 6 dB/oct one-pole coefficient for a cutoff.
float onePoleCoef(double hz, double sr) noexcept {
    return static_cast<float>(1.0 - std::exp(-2.0 * kPi * std::clamp(hz, 5.0, 0.45 * sr) / std::max(1.0, sr)));
}

// SILVETON_FX_V3: DELAYS. DIGITAL (clean, TIME glides), TAPE (wow/flutter,
// saturation and head loss in the loop), PING-PONG (repeats alternate L/R),
// MULTI-TAP (four taps at 1/4 .. 4/4 of TIME), DUCKING (the repeats step back
// while you play and come up when you stop).
struct DelayEngine {
    HeapLine line{};
    double sampleRate{48000.0};
    double currentDelay{-1.0};
    double timeSmooth{0.0};
    OnePole toneL{}, toneR{}, headL{}, headR{};
    double wowPhase{0.0}, flutterPhase{0.0};
    float duckEnv{0.0F}, duckGain{1.0F};

    void allocate(double sr, double maxSec) { sampleRate = sr; line.allocate(static_cast<std::size_t>(maxSec * sr) + 64U); reset(); }
    void reset() noexcept {
        line.clear(); currentDelay = -1.0;
        timeSmooth = 1.0 - std::exp(-1.0 / (0.060 * std::max(1.0, sampleRate)));
        toneL = toneR = headL = headR = OnePole{}; wowPhase = flutterPhase = 0.0; duckEnv = 0.0F; duckGain = 1.0F;
    }

    static double delayMs(const EffectSlot& p) noexcept {
        if (p.tempoSync) {
            const double bpm = std::clamp(static_cast<double>(p.tempoBpm), 30.0, 300.0);
            return std::clamp(60000.0 / bpm * 4.0 * std::clamp(static_cast<double>(p.division), 1.0 / 64.0, 1.0), 1.0, 2500.0);
        }
        return std::clamp(static_cast<double>(p.timeMs), 1.0, 2500.0);
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const double ms = delayMs(p);
        const double target = std::clamp(sampleRate * ms / 1000.0, 1.0, line.maxDelay() - 8.0);
        if (currentDelay < 0.0) currentDelay = target;
        else currentDelay += (target - currentDelay) * timeSmooth;
        const float t = std::clamp(p.tone, 0.0F, 1.0F);
        const float toneA = 0.08F + 0.92F * t * t;
        const float fb = std::clamp(p.feedback, 0.0F, 0.95F);
        float dl = 0.0F, dr = 0.0F;
        switch (p.type) {
            case EffectType::TapeDelay: {
                // wow (slow) + flutter (fast) move the read head; HEAD LOSS darkens, tape saturates the loop
                const double m = std::clamp(static_cast<double>(p.mod), 0.0, 1.0);
                wowPhase += 2.0 * kPi * 0.6 / sampleRate; if (wowPhase > 2.0 * kPi) wowPhase -= 2.0 * kPi;
                flutterPhase += 2.0 * kPi * 7.3 / sampleRate; if (flutterPhase > 2.0 * kPi) flutterPhase -= 2.0 * kPi;
                const double dev = sampleRate * (0.0025 * std::sin(wowPhase) + 0.0004 * std::sin(flutterPhase)) * m;
                dl = line.tapHL(std::max(1.0, currentDelay + dev));
                dr = line.tapHR(std::max(1.0, currentDelay + dev * 0.9));
                break;
            }
            case EffectType::MultiTap: {
                const float spread = 0.3F + 0.7F * std::clamp(p.depth, 0.0F, 1.0F);
                const double d1 = std::max(1.0, currentDelay * 0.25), d2 = std::max(1.0, currentDelay * 0.5), d3 = std::max(1.0, currentDelay * 0.75);
                dl = line.tapL(currentDelay) + 0.7F * line.tapL(d3) * (1.0F - spread) + 0.5F * line.tapL(d2) + 0.35F * line.tapL(d1) * spread;
                dr = line.tapR(currentDelay) + 0.7F * line.tapR(d3) * spread + 0.5F * line.tapR(d2) + 0.35F * line.tapR(d1) * (1.0F - spread);
                break;
            }
            default:
                dl = line.tapL(currentDelay);
                dr = line.tapR(currentDelay);
                break;
        }
        float fl = toneL.process(dl, toneA);
        float fr = toneR.process(dr, toneA);
        if (p.type == EffectType::TapeDelay) {
            const float drive = 1.0F + 1.5F * std::clamp(p.depth, 0.0F, 1.0F);
            fl = std::tanh(fl * drive) / drive;
            fr = std::tanh(fr * drive) / drive;
            const float head = onePoleCoef(4500.0 + 8000.0 * (1.0 - static_cast<double>(std::clamp(p.mod, 0.0F, 1.0F))), sampleRate);
            fl = headL.process(fl, head);
            fr = headR.process(fr, head);
        }
        if (p.type == EffectType::PingPong) {
            // mono in, repeats cross: L feeds R, R feeds L
            const float mono = 0.5F * (inL + inR);
            line.push(mono + fr * fb, fl * fb);
        } else {
            line.push(inL + fl * fb, inR + fr * fb);
        }
        if (p.type == EffectType::DuckDelay) {
            const float level = std::max(std::abs(inL), std::abs(inR));
            const float aAtt = onePoleCoef(60.0, sampleRate), aRel = onePoleCoef(1.2, sampleRate);
            duckEnv += (level - duckEnv) * (level > duckEnv ? aAtt : aRel);
            const float amount = std::clamp(p.depth, 0.0F, 1.0F);
            const float duckTarget = 1.0F - amount * std::clamp(duckEnv * 6.0F, 0.0F, 1.0F);
            duckGain += (duckTarget - duckGain) * onePoleCoef(20.0, sampleRate);
            fl *= duckGain; fr *= duckGain;
        }
        wetL = fl; wetR = fr;
    }
};

// SILVETON_FX_V3: CHORUS (2..4 voices on one line, each with its own LFO phase),
// FLANGER (short delay + feedback, LFO), PHASER (4/6/8 all-pass stages), ROTARY
// (horn and drum: tremolo + Doppler through a modulated delay, slow/fast with inertia).
struct ModEngine {
    HeapLine line{};
    double sampleRate{48000.0};
    double phase{0.0};
    float fbL{0.0F}, fbR{0.0F};
    std::array<float, 8U> apL{}, apR{};
    double fastPhase{0.0};         // ensemble fast LFO
    double rotSpeed{0.0};          // current rotor speed (Hz) with inertia
    double hornPhase{0.0}, drumPhase{0.0};
    float splitL{0.0F}, splitR{0.0F};

    void allocate(double sr) { sampleRate = sr; line.allocate(static_cast<std::size_t>(0.06 * sr) + 64U); reset(); }
    void reset() noexcept { line.clear(); phase = 0.0; fastPhase = 0.0; fbL = fbR = 0.0F; apL.fill(0.0F); apR.fill(0.0F); rotSpeed = 0.0; hornPhase = drumPhase = 0.0; splitL = splitR = 0.0F; }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const double rate = std::clamp(static_cast<double>(p.rateHz), 0.02, 12.0);
        const float depth = std::clamp(p.depth, 0.0F, 1.0F);
        switch (p.type) {
            case EffectType::Chorus: {
                line.push(inL, inR);
                const int voices = std::clamp(static_cast<int>(std::lround(p.param1)), 2, 4);
                const double base = sampleRate * 0.012;
                const double dep = sampleRate * 0.008 * static_cast<double>(depth);
                float l = 0.0F, r = 0.0F;
                for (int v = 0; v < voices; ++v) {
                    const double ph = phase + static_cast<double>(v) / static_cast<double>(voices);
                    const double lfo = 0.5 + 0.5 * std::sin(2.0 * kPi * ph);
                    const double d = base * (1.0 + 0.35 * v) + dep * lfo;
                    if ((v & 1) == 0) { l += line.tapHL(d); r += line.tapHR(d + 0.0007 * sampleRate); }
                    else { r += line.tapHR(d); l += line.tapHL(d + 0.0007 * sampleRate); }
                }
                const float norm = 1.0F / static_cast<float>(voices) * 1.4F;
                wetL = l * norm; wetR = r * norm;
                break;
            }
            case EffectType::Flanger: {
                const float fb = std::clamp(p.feedback, -0.95F, 0.95F);
                line.push(inL + fbL * fb, inR + fbR * fb);
                const double lfoL = 0.5 + 0.5 * std::sin(2.0 * kPi * phase);
                const double lfoR = 0.5 + 0.5 * std::sin(2.0 * kPi * (phase + 0.5 * (1.0 - depth)));
                const double minD = sampleRate * 0.0006, span = sampleRate * 0.0075 * depth;
                fbL = line.tapHL(minD + span * lfoL);
                fbR = line.tapHR(minD + span * lfoR);
                wetL = fbL; wetR = fbR;
                break;
            }
            case EffectType::Phaser: {
                const int stages = std::clamp(static_cast<int>(std::lround(p.param1)) & ~1, 4, 8);
                const double lfo = 0.5 + 0.5 * std::sin(2.0 * kPi * phase);
                const double fMin = 200.0, fMax = 200.0 + 2800.0 * depth;
                const double fc = fMin * std::pow(fMax / fMin, lfo);
                const float a = static_cast<float>((std::tan(kPi * fc / sampleRate) - 1.0) / (std::tan(kPi * fc / sampleRate) + 1.0));
                const float fb = std::clamp(p.feedback, 0.0F, 0.9F);
                float xl = inL + fbL * fb, xr = inR + fbR * fb;
                for (int s = 0; s < stages; ++s) {
                    const auto i = static_cast<std::size_t>(s);
                    const float yl = a * xl + apL[i]; apL[i] = xl - a * yl; xl = yl;
                    const float yr = a * xr + apR[i]; apR[i] = xr - a * yr; xr = yr;
                }
                fbL = xl; fbR = xr;
                wetL = xl; wetR = xr;
                break;
            }
            case EffectType::Rotary: {
                // SLOW = rateHz, FAST = param1 (Hz), param2 >= 0.5 = fast. The rotor speeds up / slows down (inertia).
                const double slow = std::clamp(static_cast<double>(p.rateHz), 0.1, 2.0);
                const double fast = std::clamp(static_cast<double>(p.param1) > 0.0 ? static_cast<double>(p.param1) : 6.5, 2.0, 10.0);
                const double want = p.param2 >= 0.5F ? fast : slow;
                rotSpeed += (want - rotSpeed) * (1.0 - std::exp(-1.0 / (1.2 * sampleRate)));
                hornPhase += 2.0 * kPi * rotSpeed / sampleRate; if (hornPhase > 2.0 * kPi) hornPhase -= 2.0 * kPi;
                drumPhase += 2.0 * kPi * rotSpeed * 0.85 / sampleRate; if (drumPhase > 2.0 * kPi) drumPhase -= 2.0 * kPi;
                // crossover 800 Hz: drum = low, horn = high
                const float xo = onePoleCoef(800.0, sampleRate);
                splitL += (inL - splitL) * xo; splitR += (inR - splitR) * xo;
                const float lowL = splitL, lowR = splitR, highL = inL - splitL, highR = inR - splitR;
                line.push(highL, highR);
                const double dop = sampleRate * 0.0009 * (0.3 + 0.7 * depth);
                const double sh = std::sin(hornPhase), ch = std::cos(hornPhase);
                const float hornL = line.tapHL(sampleRate * 0.0012 + dop * (1.0 + sh)) * static_cast<float>(0.55 + 0.45 * sh);
                const float hornR = line.tapHR(sampleRate * 0.0012 + dop * (1.0 - sh)) * static_cast<float>(0.55 - 0.45 * sh);
                const float drumL = lowL * static_cast<float>(0.7 + 0.3 * std::sin(drumPhase));
                const float drumR = lowR * static_cast<float>(0.7 - 0.3 * std::sin(drumPhase));
                (void)ch;
                wetL = hornL + drumL; wetR = hornR + drumR;
                break;
            }
            case EffectType::Ensemble: {
                // SILVETON_POLYSIX_V1: string-machine ensemble - three taps on one line, each moved
                // by a slow (0.6 Hz) and a fast (6 Hz) triangle 120 degrees apart; DEPTH = width.
                line.push(inL, inR);
                const double slowPh = phase;                       // 'rate' is used as the slow speed scale
                const double fastPh = fastPhase;
                const double base = sampleRate * 0.0045;
                const double dSlow = sampleRate * 0.0028 * static_cast<double>(depth);
                const double dFast = sampleRate * 0.00035 * static_cast<double>(depth);
                float l = 0.0F, r = 0.0F;
                for (int v = 0; v < 3; ++v) {
                    const double off = static_cast<double>(v) / 3.0;
                    const auto triAt = [](double ph) noexcept { ph -= std::floor(ph); return 1.0 - 4.0 * std::abs(ph - 0.5); };
                    const double d = base + dSlow * (0.5 + 0.5 * triAt(slowPh + off)) + dFast * (0.5 + 0.5 * triAt(fastPh + off));
                    const float tl = line.tapHL(d), tr = line.tapHR(d + 0.0003 * sampleRate);
                    if (v == 0) { l += tl; r += tr; } else if (v == 1) { l += tl * 0.8F; r += tr * 0.5F; } else { l += tl * 0.5F; r += tr * 0.8F; }
                }
                wetL = l * 0.55F; wetR = r * 0.55F;
                fastPhase += 6.0 * std::clamp(static_cast<double>(p.rateHz), 0.1, 2.0) / 0.6 / sampleRate; fastPhase -= std::floor(fastPhase);
                break;
            }
            default: wetL = inL; wetR = inR; break;
        }
        phase += rate / sampleRate; phase -= std::floor(phase);
    }
};

// SILVETON_FX_V3: HALL / ROOM / CHAMBER - an 8-line feedback delay network
// (Jot) with a Householder mix, input diffusion, early reflections, damping in
// every line and slow modulation of the line lengths (no metallic ringing).
// SIZE scales every length, DECAY = reverb time, DAMPING = high-frequency loss,
// TONE = output colour, MOD = modulation depth, PRE-DELAY.
struct FdnReverb {
    static constexpr std::size_t kLines = 8U;
    double sampleRate{48000.0};
    std::vector<float> pre;           // pre-delay + early reflections
    std::size_t preSize{0U}, preW{0U};
    std::array<std::vector<float>, kLines> line{};
    std::array<std::size_t, kLines> lineSize{}, lineW{};
    std::array<float, kLines> damp{}, fb{};
    std::array<std::vector<float>, 4U> diff{};
    std::array<std::size_t, 4U> diffSize{}, diffW{};
    std::array<double, kLines> modPhase{};
    OnePole outL{}, outR{};
    float bwState{0.0F};
    EffectType kind{EffectType::Hall};

    static constexpr std::array<double, kLines> kBaseMs{{29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.3, 73.1}};   // hall, SIZE 0.5
    static constexpr std::array<double, 4U> kDiffMs{{4.3, 6.7, 9.1, 12.3}};

    void allocate(double sr) {
        sampleRate = sr;
        preSize = static_cast<std::size_t>(0.25 * sr) + 64U; pre.assign(preSize, 0.0F);
        for (std::size_t i = 0; i < kLines; ++i) { lineSize[i] = static_cast<std::size_t>(kBaseMs[i] * 2.2 * 0.001 * sr) + 64U; line[i].assign(lineSize[i], 0.0F); }
        for (std::size_t i = 0; i < 4U; ++i) { diffSize[i] = static_cast<std::size_t>(kDiffMs[i] * 1.6 * 0.001 * sr) + 16U; diff[i].assign(diffSize[i], 0.0F); }
        reset();
    }
    void reset() noexcept {
        std::fill(pre.begin(), pre.end(), 0.0F); preW = 0U;
        for (std::size_t i = 0; i < kLines; ++i) { std::fill(line[i].begin(), line[i].end(), 0.0F); lineW[i] = 0U; modPhase[i] = static_cast<double>(i) * 0.7; }
        for (std::size_t i = 0; i < 4U; ++i) { std::fill(diff[i].begin(), diff[i].end(), 0.0F); diffW[i] = 0U; }
        damp.fill(0.0F); fb.fill(0.0F); outL = outR = OnePole{}; bwState = 0.0F;
    }

    static float readLine(const std::vector<float>& b, std::size_t size, std::size_t w, double d) noexcept {
        return HeapLine::tapOne(b, size, w, d);
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const float seconds = reverbSecondsFor(p.feedback);
        const double size = 0.35 + 1.3 * std::clamp(static_cast<double>(p.size), 0.0, 1.0);   // 0.35 .. 1.65 x
        double sizeK = size, modDepthMs = 0.0, erGain = 0.0, diffG = 0.0;
        float bright = 1.0F;
        switch (p.type) {
            case EffectType::Room:    sizeK = size * 0.45; modDepthMs = 0.15; erGain = 0.55; diffG = 0.55; bright = 0.9F; break;
            case EffectType::Chamber: sizeK = size * 0.7;  modDepthMs = 0.25; erGain = 0.35; diffG = 0.7;  bright = 0.75F; break;
            default:                  sizeK = size;        modDepthMs = 0.45; erGain = 0.2;  diffG = 0.62; bright = 1.0F; break;   // hall
        }
        modDepthMs *= 0.3 + 1.4 * static_cast<double>(std::clamp(p.mod, 0.0F, 1.0F));
        const double dampHz = 1500.0 + 14000.0 * std::pow(1.0 - static_cast<double>(std::clamp(p.damping, 0.0F, 1.0F)), 1.6);
        const float dampA = onePoleCoef(dampHz, sampleRate);
        const float toneA = onePoleCoef(2500.0 + 15000.0 * static_cast<double>(std::clamp(p.tone, 0.0F, 1.0F)) * static_cast<double>(bright), sampleRate);

        // pre-delay + early reflections from the pre line
        const double preD = std::clamp(static_cast<double>(p.preDelayMs), 0.0, 200.0) * 0.001 * sampleRate;
        const float mono = 0.5F * (inL + inR);
        pre[preW] = mono; preW = (preW + 1U) % preSize;
        float x = HeapLine::tapOne(pre, preSize, preW, preD + 1.0);
        float erL = 0.0F, erR = 0.0F;
        if (erGain > 0.0) {
            static constexpr std::array<double, 6U> erMs{{7.3, 11.9, 17.1, 23.3, 29.7, 38.1}};
            static constexpr std::array<float, 6U> erG{{0.8F, 0.65F, 0.55F, 0.45F, 0.35F, 0.28F}};
            for (std::size_t i = 0; i < erMs.size(); ++i) {
                const float e = HeapLine::tapOne(pre, preSize, preW, preD + 1.0 + erMs[i] * sizeK * 0.001 * sampleRate) * erG[i];
                if ((i & 1U) == 0U) erL += e; else erR += e;
            }
        }
        // input bandwidth + diffusion (four nested all-passes)
        bwState += (x - bwState) * 0.9F;
        x = bwState;
        for (std::size_t i = 0; i < 4U; ++i) {
            const double d = std::max(1.0, kDiffMs[i] * (0.6 + 0.6 * sizeK) * 0.001 * sampleRate);
            const float dOut = HeapLine::tapOne(diff[i], diffSize[i], diffW[i], d);
            const float v = x - static_cast<float>(diffG) * dOut;
            diff[i][diffW[i]] = v; diffW[i] = (diffW[i] + 1U) % diffSize[i];
            x = dOut + static_cast<float>(diffG) * v;
        }
        // read the 8 lines (modulated), damp, mix with a Householder matrix, feed back
        std::array<float, kLines> out{};
        float sum = 0.0F;
        for (std::size_t i = 0; i < kLines; ++i) {
            modPhase[i] += 2.0 * kPi * (0.11 + 0.07 * static_cast<double>(i)) / sampleRate;
            if (modPhase[i] > 2.0 * kPi) modPhase[i] -= 2.0 * kPi;
            const double len = kBaseMs[i] * sizeK * 0.001 * sampleRate + modDepthMs * 0.001 * sampleRate * std::sin(modPhase[i]);
            const double lenC = std::clamp(len, 2.0, static_cast<double>(lineSize[i] - 4U));
            float v = readLine(line[i], lineSize[i], lineW[i], lenC);
            damp[i] += (v - damp[i]) * dampA;
            v = damp[i];
            // gain for this line length so that every line decays in the same reverb time
            const float g = std::pow(10.0F, -3.0F * static_cast<float>(lenC / sampleRate) / std::max(0.05F, seconds));
            out[i] = v * g;
            sum += out[i];
        }
        const float h = -2.0F / static_cast<float>(kLines) * sum;   // Householder: y = x - (2/N) * sum
        for (std::size_t i = 0; i < kLines; ++i) {
            const float in = ((i & 1U) == 0U ? x : -x) * 0.5F;
            line[i][lineW[i]] = out[i] + h + in;
            lineW[i] = (lineW[i] + 1U) % lineSize[i];
        }
        float l = out[0] - out[1] + out[2] - out[3] + out[4] - out[5] + out[6] - out[7];
        float r = out[0] + out[1] - out[2] - out[3] + out[4] + out[5] - out[6] - out[7];
        l = outL.process(l, toneA) * 0.6F + erL * static_cast<float>(erGain);
        r = outR.process(r, toneA) * 0.6F + erR * static_cast<float>(erGain);
        wetL = l; wetR = r;
    }
};

// SILVETON_FX_V3: SPRING reverb - two springs, each a feedback loop whose
// dispersion comes from a chain of 24 stretched all-pass sections (the "boing"
// chirp), with low-pass loss. DECAY = loop gain, TONE = brightness, SIZE = spring length.
struct SpringReverb {
    double sampleRate{48000.0};
    struct Spring {
        std::vector<float> loop; std::size_t size{0U}, w{0U};
        std::array<float, 24U> ap{};
        OnePole lp{};
        float fbState{0.0F};
    };
    std::array<Spring, 2U> s{};
    OnePole outL{}, outR{};
    std::vector<float> pre; std::size_t preSize{0U}, preW{0U};

    void allocate(double sr) {
        sampleRate = sr;
        for (auto& sp : s) { sp.size = static_cast<std::size_t>(0.12 * sr) + 64U; sp.loop.assign(sp.size, 0.0F); }
        preSize = static_cast<std::size_t>(0.2 * sr) + 64U; pre.assign(preSize, 0.0F);
        reset();
    }
    void reset() noexcept {
        for (auto& sp : s) { std::fill(sp.loop.begin(), sp.loop.end(), 0.0F); sp.w = 0U; sp.ap.fill(0.0F); sp.lp = OnePole{}; sp.fbState = 0.0F; }
        std::fill(pre.begin(), pre.end(), 0.0F); preW = 0U; outL = outR = OnePole{};
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const float mono = 0.5F * (inL + inR);
        pre[preW] = mono; preW = (preW + 1U) % preSize;
        const double preD = std::clamp(static_cast<double>(p.preDelayMs), 0.0, 150.0) * 0.001 * sampleRate;
        const float x = HeapLine::tapOne(pre, preSize, preW, preD + 1.0);
        const float decay = 0.55F + 0.42F * std::clamp(p.feedback, 0.0F, 0.95F) / 0.95F;
        const float lossA = onePoleCoef(1800.0 + 5000.0 * static_cast<double>(std::clamp(p.tone, 0.0F, 1.0F)), sampleRate);
        const double sizeK = 0.5 + 1.0 * std::clamp(static_cast<double>(p.size), 0.0, 1.0);
        const float apCoef = -0.62F;
        float outA = 0.0F, outB = 0.0F;
        for (std::size_t k = 0; k < 2U; ++k) {
            auto& sp = s[k];
            const double len = std::clamp((k == 0U ? 41.0 : 53.0) * sizeK * 0.001 * sampleRate, 4.0, static_cast<double>(sp.size - 4U));
            float v = HeapLine::tapOne(sp.loop, sp.size, sp.w, len);
            v = sp.lp.process(v, lossA);
            // dispersion: 24 first-order all-passes with the same coefficient
            for (auto& z : sp.ap) { const float y = apCoef * v + z; z = v - apCoef * y; v = y; }
            v = std::tanh(v * 1.2F) / 1.2F;   // the spring does not grow
            sp.fbState = v;
            sp.loop[sp.w] = x * 0.5F + v * decay; sp.w = (sp.w + 1U) % sp.size;
            if (k == 0U) outA = v; else outB = v;
        }
        wetL = outL.process(outA * 0.8F + outB * 0.3F, 0.9F);
        wetR = outR.process(outB * 0.8F + outA * 0.3F, 0.9F);
    }
};

// SILVETON_FX_V3: COMPRESSOR (peak detector, soft knee) and DRIVE (tanh with tone).
struct DynamicsFx {
    double sampleRate{48000.0};
    float env{0.0F};
    float grDb{0.0F};
    OnePole toneL{}, toneR{};
    void allocate(double sr) { sampleRate = sr; reset(); }
    void reset() noexcept { env = 0.0F; grDb = 0.0F; toneL = toneR = OnePole{}; }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        if (p.type == EffectType::Drive) {
            const float drive = 1.0F + 24.0F * std::clamp(p.depth, 0.0F, 1.0F) * std::clamp(p.depth, 0.0F, 1.0F);
            const float toneA = onePoleCoef(1200.0 + 16000.0 * static_cast<double>(std::clamp(p.tone, 0.0F, 1.0F)), sampleRate);
            const float outG = dbToGain(std::clamp(p.param1, -24.0F, 12.0F)) / std::sqrt(drive);
            wetL = toneL.process(std::tanh(inL * drive), toneA) * outG;
            wetR = toneR.process(std::tanh(inR * drive), toneA) * outG;
            return;
        }
        const float thr = std::clamp(p.param1, -60.0F, 0.0F);
        const float ratio = std::clamp(p.param2 > 1.0F ? p.param2 : 4.0F, 1.0F, 20.0F);
        const float aAtt = onePoleCoef(1000.0 / std::clamp(static_cast<double>(p.timeMs), 0.1, 300.0), sampleRate);
        const float aRel = onePoleCoef(1000.0 / std::clamp(static_cast<double>(p.param3 > 0.0F ? p.param3 : 120.0F), 10.0, 3000.0), sampleRate);
        const float level = std::max(std::abs(inL), std::abs(inR));
        env += (level - env) * (level > env ? aAtt : aRel);
        const float lvlDb = gainToDb(env);
        const float knee = 6.0F;
        float over = lvlDb - thr;
        float gr = 0.0F;
        if (over > knee * 0.5F) gr = over - over / ratio;
        else if (over > -knee * 0.5F) { const float t = over + knee * 0.5F; gr = (1.0F - 1.0F / ratio) * t * t / (2.0F * knee); }
        grDb = gr;
        const float g = dbToGain(-gr + std::clamp(p.tone, 0.0F, 24.0F));   // tone = make-up dB here
        wetL = inL * g; wetR = inR * g;
    }
};

// SILVETON_FX_V2: studio plate REVERB (J. Dattorro, "Effect Design Part 1", 1997):
// four input diffusers and a modulated figure-eight tank, stereo output taps.
// DECAY (the FEEDBACK value 0..0.95) = reverb time 0.6..12 s, TONE = brightness.
// Fixed arrays sized for up to 192 kHz: no allocation in the audio thread.
float reverbSecondsFor(float feedback) noexcept {
    const float v = std::clamp(feedback, 0.0F, 0.95F) / 0.95F;
    return 0.6F * std::pow(20.0F, v);
}

template <std::size_t N>
struct FxLine {
    std::array<float, N> buf{};
    std::size_t w{0U};
    void clear() noexcept { buf.fill(0.0F); w = 0U; }
    // Sample pushed d samples ago (d = 1 is the newest), d < N.
    [[nodiscard]] float at(std::size_t d) const noexcept { return buf[(w + N - d) % N]; }
    [[nodiscard]] float atFrac(double d) const noexcept {
        const auto di = static_cast<std::size_t>(d);
        const float frac = static_cast<float>(d - static_cast<double>(di));
        const float a = at(di);
        const float b = at(di + 1U);
        return a + (b - a) * frac;
    }
    void push(float v) noexcept { buf[w] = v; w = (w + 1U) % N; }
};

struct ReverbFx {
    static constexpr double kBaseRate = 29761.0;
    FxLine<24576U> pre{};      // SILVETON_FX_V3: PRE-DELAY up to 200 ms at 96 kHz
    OnePole toneOutL{}, toneOutR{};
    FxLine<1024U> in1{}, in2{};
    FxLine<2560U> in3{};
    FxLine<2048U> in4{};
    FxLine<4608U> modL{};
    FxLine<28800U> delL1{};
    FxLine<11776U> apL{};
    FxLine<24064U> delL2{};
    FxLine<6016U> modR{};
    FxLine<27264U> delR1{};
    FxLine<17152U> apR{};
    FxLine<20480U> delR2{};
    std::size_t lPre{1U}, l1{1U}, l2{1U}, l3{1U}, l4{1U};
    std::size_t lDelL1{1U}, lApL{1U}, lDelL2{1U}, lDelR1{1U}, lApR{1U}, lDelR2{1U};
    double lModL{1.0}, lModR{1.0}, excursion{1.0};
    std::array<std::size_t, 14U> tapsAt{};
    double scale{1.0};
    double lfoPhase{0.0}, lfoStep{0.0};
    float bandwidthState{0.0F}, dampL{0.0F}, dampR{0.0F};
    float feedL{0.0F}, feedR{0.0F};

    std::size_t sc(double base, std::size_t cap) const noexcept {
        return std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(base * scale)), 1U, cap - 3U);
    }

    void reset(double sr) noexcept {
        scale = std::clamp(std::max(1.0, sr) / kBaseRate, 0.5, 192000.0 / kBaseRate);
        pre.clear(); in1.clear(); in2.clear(); in3.clear(); in4.clear();
        modL.clear(); delL1.clear(); apL.clear(); delL2.clear();
        modR.clear(); delR1.clear(); apR.clear(); delR2.clear();
        lPre = sc(0.010 * kBaseRate, 24576U);
        toneOutL = toneOutR = OnePole{};
        l1 = sc(142.0, 1024U); l2 = sc(107.0, 1024U); l3 = sc(379.0, 2560U); l4 = sc(277.0, 2048U);
        excursion = 16.0 * scale;
        lModL = 672.0 * scale;
        lModR = 908.0 * scale;
        lDelL1 = sc(4453.0, 28800U); lApL = sc(1800.0, 11776U); lDelL2 = sc(3720.0, 24064U);
        lDelR1 = sc(4217.0, 27264U); lApR = sc(2656.0, 17152U); lDelR2 = sc(3163.0, 20480U);
        const std::array<double, 14U> taps{{266.0, 2974.0, 1913.0, 1996.0, 1990.0, 187.0, 1066.0,
                                            353.0, 3627.0, 1228.0, 2673.0, 2111.0, 335.0, 121.0}};
        for (std::size_t i = 0; i < taps.size(); ++i) {
            tapsAt[i] = std::max<std::size_t>(1U, static_cast<std::size_t>(std::lround(taps[i] * scale)));
        }
        lfoPhase = 0.0;
        lfoStep = 2.0 * kPi * 0.9 / std::max(1.0, sr);
        bandwidthState = dampL = dampR = feedL = feedR = 0.0F;
    }

    template <std::size_t N>
    static float allpass(FxLine<N>& line, std::size_t len, float x, float g) noexcept {
        const float d = line.at(len);
        const float v = x - g * d;
        line.push(v);
        return d + g * v;
    }

    template <std::size_t N>
    static float allpassMod(FxLine<N>& line, double len, float x, float g) noexcept {
        const float d = line.atFrac(len);
        const float v = x - g * d;
        line.push(v);
        return d + g * v;
    }

    template <std::size_t N>
    static float delay(FxLine<N>& line, std::size_t len, float x) noexcept {
        const float d = line.at(len);
        line.push(x);
        return d;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const float seconds = reverbSecondsFor(p.feedback);
        // Tank gain per trip for this reverb time (measured T30, any sample rate).
        const float decay = std::clamp(std::pow(10.0F, -0.571F / seconds), 0.0F, 0.95F);
        // SILVETON_FX_V3: DAMPING = loss in the tank, TONE = colour of the output, PRE-DELAY in ms.
        const float dampK = std::clamp(p.damping, 0.0F, 1.0F);
        const float damping = 0.0005F + dampK * 0.70F;
        const float toneA = onePoleCoef(2000.0 + 16000.0 * static_cast<double>(std::clamp(p.tone, 0.0F, 1.0F)), kBaseRate * scale);
        const double sr = kBaseRate * scale;
        const auto preLen = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(std::clamp(static_cast<double>(p.preDelayMs), 0.0, 200.0) * 0.001 * sr)) + 1U, 1U, 24576U - 3U);

        float x = delay(pre, preLen, (inL + inR) * 0.5F);
        bandwidthState += (x - bandwidthState) * 0.9995F;
        x = allpass(in1, l1, bandwidthState, 0.75F);
        x = allpass(in2, l2, x, 0.75F);
        x = allpass(in3, l3, x, 0.625F);
        x = allpass(in4, l4, x, 0.625F);

        lfoPhase += lfoStep;
        if (lfoPhase > 2.0 * kPi) lfoPhase -= 2.0 * kPi;
        const double modA = excursion * (0.5 + 0.5 * std::sin(lfoPhase));
        const double modB = excursion * (0.5 + 0.5 * std::cos(lfoPhase));

        // left half of the tank (fed by the right half)
        float a = x + decay * feedR;
        a = allpassMod(modL, lModL + modA, a, -0.70F);
        a = delay(delL1, lDelL1, a);
        dampL = a * (1.0F - damping) + dampL * damping;
        a = allpass(apL, lApL, dampL * decay, 0.50F);
        const float outLeftHalf = delay(delL2, lDelL2, a);

        // right half of the tank (fed by the left half)
        float b = x + decay * feedL;
        b = allpassMod(modR, lModR + modB, b, -0.70F);
        b = delay(delR1, lDelR1, b);
        dampR = b * (1.0F - damping) + dampR * damping;
        b = allpass(apR, lApR, dampR * decay, 0.50F);
        const float outRightHalf = delay(delR2, lDelR2, b);

        feedL = outLeftHalf;
        feedR = outRightHalf;

        wetL = 1.2F * (delR1.at(tapsAt[0]) + delR1.at(tapsAt[1]) - apR.at(tapsAt[2]) + delR2.at(tapsAt[3])
                       - delL1.at(tapsAt[4]) - apL.at(tapsAt[5]) - delL2.at(tapsAt[6]));
        wetR = 1.2F * (delL1.at(tapsAt[7]) + delL1.at(tapsAt[8]) - apL.at(tapsAt[9]) + delL2.at(tapsAt[10])
                       - delR1.at(tapsAt[11]) - apR.at(tapsAt[12]) - delR2.at(tapsAt[13]));
        wetL = toneOutL.process(wetL, toneA);
        wetR = toneOutR.process(wetR, toneA);
    }
};

// SILVETON_FX_V3: one effect slot engine. Holds every effect type; the memory
// is allocated once (prepare), never in the audio thread. A type change clears
// only the engine that starts.
struct FxUnit {
    double sampleRate{48000.0};
    EffectType built{EffectType::Off};
    DelayEngine delay{};
    ModEngine mod{};
    FdnReverb fdn{};
    SpringReverb spring{};
    DynamicsFx dyn{};
    std::unique_ptr<ReverbFx> plate;   // 600 KB: on the heap

    void prepare(double sr, double maxDelaySec) {
        sampleRate = sr;
        delay.allocate(sr, maxDelaySec);
        mod.allocate(sr);
        fdn.allocate(sr);
        spring.allocate(sr);
        dyn.allocate(sr);
        if (plate == nullptr) plate = std::make_unique<ReverbFx>();
        plate->reset(sr);
        built = EffectType::Off;
    }
    void reset() noexcept { built = EffectType::Off; }

    void clearFor(EffectType t) noexcept {
        if (effectIsDelay(t)) delay.reset();
        else if (t == EffectType::Chorus || t == EffectType::Flanger || t == EffectType::Phaser || t == EffectType::Rotary || t == EffectType::Ensemble) mod.reset();
        else if (t == EffectType::Reverb) { if (plate != nullptr) plate->reset(sampleRate); }
        else if (t == EffectType::Spring) spring.reset();
        else if (effectIsReverb(t)) fdn.reset();
        else dyn.reset();
        built = t;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        if (p.type == EffectType::Off) { wetL = wetR = 0.0F; return; }
        if (p.type != built) clearFor(p.type);
        switch (p.type) {
            case EffectType::Off: wetL = wetR = 0.0F; break;
            case EffectType::Delay: case EffectType::TapeDelay: case EffectType::PingPong:
            case EffectType::MultiTap: case EffectType::DuckDelay:
                delay.process(inL, inR, p, wetL, wetR); break;
            case EffectType::Chorus: case EffectType::Flanger: case EffectType::Phaser: case EffectType::Rotary: case EffectType::Ensemble:
                mod.process(inL, inR, p, wetL, wetR); break;
            case EffectType::Reverb:
                if (plate != nullptr) plate->process(inL, inR, p, wetL, wetR); else wetL = wetR = 0.0F;
                break;
            case EffectType::Hall: case EffectType::Room: case EffectType::Chamber:
                fdn.process(inL, inR, p, wetL, wetR); break;
            case EffectType::Spring:
                spring.process(inL, inR, p, wetL, wetR); break;
            case EffectType::Compressor: case EffectType::Drive:
                dyn.process(inL, inR, p, wetL, wetR); break;
        }
        if (!std::isfinite(wetL) || !std::isfinite(wetR)) { wetL = wetR = 0.0F; clearFor(p.type); }
    }
};

} // namespace

bool RealtimeEventQueue::push(const RealtimeEvent& event) noexcept {
    const std::size_t w = write_.load(std::memory_order_relaxed);
    const std::size_t next = (w + 1U) % kCapacity;
    if (next == read_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    data_[w] = event;
    write_.store(next, std::memory_order_release);
    return true;
}

bool RealtimeEventQueue::pop(RealtimeEvent& event) noexcept {
    const std::size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    event = data_[r];
    read_.store((r + 1U) % kCapacity, std::memory_order_release);
    return true;
}

void RealtimeEventQueue::clear() noexcept {
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
}

// ---------------------------------------------------------------- SILVETON_BOW_V1 - the bowed string model
// Behaviour model of a bowed string instrument (violin / viola / cello / double bass).
// Original work, written from the public physics of the bowed string:
//   * the string is a bidirectional digital waveguide (two delay lines, nut side and bridge side)
//   * the bow is a stick/slip friction junction solved exactly every sample
//     (Friedlander construction with the hyperbolic friction curve and the hysteresis rule)
//   * the body is a bank of resonances (modal synthesis) fed by the bridge force
// Calibrated on regime maps at 96 kHz: Helmholtz motion over the whole range of each instrument,
// pitch within a few cents, a "bite" attack like a real stroke, Schelleng's playable force band.
// prepare() allocates (control thread); everything else is realtime-safe.
namespace bow {


constexpr double kBowPi = 3.14159265358979323846;
inline double clampd(double v, double lo, double hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
inline double midiToHz(double n) noexcept { return 440.0 * std::exp2((n - 69.0) / 12.0); }

// ---------------------------------------------------------------- fractional delay line (4-point Lagrange)
class DelayLine {
public:
    void prepare(int maxSamples) { buf_.assign(static_cast<std::size_t>(maxSamples + 8), 0.0F); size_ = static_cast<int>(buf_.size()); write_ = 0; }
    void clear() noexcept { std::fill(buf_.begin(), buf_.end(), 0.0F); }
    void write(float x) noexcept { buf_[static_cast<std::size_t>(write_)] = x; if (++write_ >= size_) write_ = 0; }
    // the sample written `delay` samples ago (delay >= 2)
    float read(double delay) const noexcept {
        delay = clampd(delay, 2.0, static_cast<double>(size_ - 4));
        double pos = static_cast<double>(write_) - delay;
        if (pos < 0.0) pos += size_;
        const int i = static_cast<int>(pos);
        const double t = pos - i;
        const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const double wm1 = -t * (t - 1.0) * (t - 2.0) / 6.0;
        const double w0 = (t + 1.0) * (t - 1.0) * (t - 2.0) / 2.0;
        const double w1 = -(t + 1.0) * t * (t - 2.0) / 2.0;
        const double w2 = (t + 1.0) * t * (t - 1.0) / 6.0;
        return static_cast<float>(wm1 * xm1 + w0 * x0 + w1 * x1 + w2 * x2);
    }
    int capacity() const noexcept { return size_ - 8; }
private:
    float at(int i) const noexcept { if (i < 0) i += size_; else if (i >= size_) i -= size_; return buf_[static_cast<std::size_t>(i)]; }
    std::vector<float> buf_;
    int size_{0}, write_{0};
};

// ---------------------------------------------------------------- one resonance of the body
struct Mode { double freqHz{440.0}, q{10.0}, gainDb{0.0}; };

class Resonator {
public:
    void set(double sr, const Mode& m) noexcept {
        const double w0 = 2.0 * kBowPi * clampd(m.freqHz, 20.0, sr * 0.45) / sr;
        const double alpha = std::sin(w0) / (2.0 * m.q);
        const double a0 = 1.0 + alpha;
        b0_ = alpha / a0; a1_ = -2.0 * std::cos(w0) / a0; a2_ = (1.0 - alpha) / a0;
        gain_ = std::pow(10.0, m.gainDb / 20.0);
    }
    double tick(double x) noexcept {
        const double y = b0_ * (x - x2_) - a1_ * y1_ - a2_ * y2_;
        x2_ = x1_; x1_ = x; y2_ = y1_; y1_ = y;
        return gain_ * y;
    }
    void clear() noexcept { x1_ = x2_ = y1_ = y2_ = 0.0; }
private:
    double b0_{0}, a1_{0}, a2_{0}, gain_{1};
    double x1_{0}, x2_{0}, y1_{0}, y2_{0};
};

// ---------------------------------------------------------------- instrument description
struct Instrument {
    const char* name{"violin"};
    double lowestHz{190.0};         // for the delay line size
    double bodyScale{1.0};          // violin = 1, viola ~0.72, cello ~0.38, bass ~0.25
    double stringLoss{0.9965};      // gain per round trip at the bridge (string damping)
    double fingerLoss{0.994};       // gain per round trip at the finger / nut
    double bridgeCutoffHz{12000.0}; // loss filter of the bridge reflection (lower = darker string)
    double bodyDry{-14.0};          // direct bridge force in the output, dB
    double frictionV0{0.085};       // width of the friction curve (how softly the hair lets go)
    double muStick{0.85}, muSlip{0.25};
    double outGain{0.09};           // a full stroke peaks about like a recording at full velocity
};

inline Instrument instrumentFor(int index) noexcept {
    Instrument i;
    switch (index) {
        case 2: i.name = "viola"; i.lowestHz = 125.0; i.bodyScale = 0.72; i.bridgeCutoffHz = 10000.0; i.outGain = 0.085; break;
        case 3: i.name = "cello"; i.lowestHz = 60.0; i.bodyScale = 0.38; i.bridgeCutoffHz = 8000.0; i.stringLoss = 0.998; i.fingerLoss = 0.996; i.outGain = 0.05; break;
        case 4: i.name = "double bass"; i.lowestHz = 38.0; i.bodyScale = 0.25; i.bridgeCutoffHz = 6500.0; i.stringLoss = 0.9985; i.fingerLoss = 0.997; i.bodyDry = -12.0; i.outGain = 0.045; break;
        default: break;
    }
    return i;
}
constexpr double kLowestHzOfAll = 38.0;

// ---------------------------------------------------------------- player controls (what the musician does)
struct Controls {
    double expression{0.6};     // 0..1  dynamics: bow speed (and a part of the pressure) - the main "breath"
    double pressure{0.5};       // 0..1  bow pressure inside the playable band: 0 = flautando, 1 = edgy
    double bowPosition{0.12};   // 0.05..0.18 distance bow-bridge as a fraction of the string (ponticello .. tasto)
    double brightness{0.5};     // 0..1  string / bridge brightness (0.5 = the instrument as built)
    double vibratoCents{20.0};  // depth (peak), 0 = none
    double vibratoHz{5.5};
    double vibratoDelaySec{0.25};
    double vibratoFadeSec{0.35};
    double glideSec{0.03};      // time of the pitch change on a legato note
    double attackSec{0.02};     // how fast the bow reaches its speed on a new stroke (short = with a "bite")
    double releaseSec{0.06};    // how fast the bow lifts
    double bendSemis{0.0};      // live pitch offset (pitch bend)
    double extraVibratoCents{0.0}; // added by the modulation wheel
};

// ---------------------------------------------------------------- the instrument
class BowedString {
public:
    // Control thread: allocates the delay lines for the lowest note of any instrument.
    void prepare(double sr) {
        sr_ = sr;
        const int maxLen = static_cast<int>(sr / kLowestHzOfAll) + 64;
        nut_.prepare(maxLen); bridge_.prepare(maxLen);
        setInstrument(1);
        reset();
    }
    // Realtime-safe: no allocation.
    void setInstrument(int index) noexcept {
        instrumentIndex_ = index;
        inst_ = instrumentFor(index);
        buildBody();
    }
    int instrument() const noexcept { return instrumentIndex_; }
    void reset() noexcept {
        nut_.clear(); bridge_.clear(); lp_ = 0.0; slipping_ = true; speed_ = 0.0; force_ = 0.0;
        noteHz_ = 440.0; baseHz_ = 440.0; hzStep_ = 0.0; glideFrom_ = glideTo_ = 69.0; glidePos_ = 1.0; vibPhase_ = 0.0; vibAmt_ = 0.0;
        heldTime_ = 0.0; playing_ = false; noise_ = 0.0; damp_ = 0.0; bite_ = 0.0; hairLp_ = 0.0;
        dcX_ = dcY_ = 0.0; outLp_ = 0.0; lastForce_ = 0.0; level_ = 0.0;
        for (auto& r : body_) r.clear();
    }
    void setControls(const Controls& c) noexcept { ctl_ = c; ctl_.bowPosition = clampd(c.bowPosition, 0.05, 0.18); }
    Controls& controls() noexcept { return ctl_; }
    const Controls& controls() const noexcept { return ctl_; }

    // legato = true: the bow keeps moving, the pitch glides to the new note (no new attack).
    // fromNote >= 0 on a new stroke: start there and glide to `midi` (portamento on a re-struck note).
    void noteOn(double midi, bool legato, double fromNote = -1.0) noexcept {
        if (legato && playing_) {
            glideFrom_ = currentNote(); glideTo_ = midi; glidePos_ = (ctl_.glideSec <= 0.0) ? 1.0 : 0.0;
        } else {
            if (fromNote >= 0.0 && ctl_.glideSec > 0.0) { glideFrom_ = fromNote; glideTo_ = midi; glidePos_ = 0.0; }
            else { glideFrom_ = glideTo_ = midi; glidePos_ = 1.0; }
            heldTime_ = 0.0; vibAmt_ = 0.0; vibPhase_ = 0.0;
            if (!playing_) { slipping_ = true; speed_ = 0.0; }
            bite_ = 1.0;
        }
        playing_ = true; damp_ = 0.0;
    }
    void noteOff() noexcept { playing_ = false; }
    bool playing() const noexcept { return playing_; }
    double currentNote() const noexcept { return glideFrom_ + (glideTo_ - glideFrom_) * glidePos_; }
    double currentHz() const noexcept { return noteHz_; }
    bool slipping() const noexcept { return slipping_; }
    double lastBridgeForce() const noexcept { return lastForce_; }   // raw string signal before the body (tests)
    // Output level follower (0..1-ish), to know when a lifted string has died away.
    double level() const noexcept { return level_; }

    // Control rate: once per block of `frames` samples. Everything the per-sample loop needs
    // that does not have to change inside a block is computed here (no exp/pow per sample).
    void block(int frames) noexcept {
        const double dt = 1.0 / sr_;
        const double blockSec = frames * dt;
        const double e = clampd(ctl_.expression, 0.0, 1.0);
        const double p = clampd(ctl_.pressure, 0.0, 1.0);
        speedTarget_ = playing_ ? (0.10 + 0.90 * std::pow(e, 1.4)) : 0.0;
        const double tau = playing_ ? std::max(0.002, ctl_.attackSec) : std::max(0.002, ctl_.releaseSec);
        speedCoef_ = 1.0 - std::exp(-dt / tau);
        forceCoef_ = 1.0 - std::exp(-dt / (tau * 0.7));
        biteDecay_ = std::exp(-dt / 0.06);
        biteAmount_ = 4.0 * clampd(1.0 - (ctl_.attackSec - 0.015) / 0.12, 0.0, 1.0);
        dampStep_ = playing_ ? 0.0 : dt / 0.08;
        // Schelleng: the playable force band scales with bow speed and position (min ~ v/b^2, max ~ v/b);
        // long strings want more force. Evaluated with the speed at the start of the block.
        const double b = ctl_.bowPosition;
        const double noteScale = std::pow(440.0 / std::max(20.0, noteHz_), kNoteAlpha);
        fMinPerSpeed_ = kMin * noteScale / (b * b);
        forceMul_ = std::pow(std::max(1.0, (kMax * noteScale / b) / fMinPerSpeed_), p) * (0.75 + 0.5 * e);
        // The bow sharpens the Helmholtz corner: more pressure and a faster bow = brighter,
        // a light slow bow = darker (flautando). Modelled as the loss filter at the bridge.
        const double bright = std::exp2((clampd(ctl_.brightness, 0.0, 1.0) - 0.5) * 2.0);   // 0.5..2
        const double fc = inst_.bridgeCutoffHz * bright / ((0.55 + 1.3 * (1.0 - p)) * (0.7 + 0.6 * (1.0 - e)));
        a_ = clampd(std::exp(-2.0 * kBowPi * fc / sr_), 0.02, 0.9);
        lpDelay_ = a_ / (1.0 - a_);
        gFinger_ = inst_.fingerLoss; gString_ = inst_.stringLoss;
        // vibrato amount of this block (delay, fade)
        if (ctl_.vibratoCents + ctl_.extraVibratoCents > 0.0 && playing_) {
            const double want = clampd((heldTime_ - ctl_.vibratoDelaySec) / std::max(0.01, ctl_.vibratoFadeSec), 0.0, 1.0);
            vibAmt_ += (want - vibAmt_) * (1.0 - std::exp(-blockSec / 0.04));
        } else vibAmt_ *= std::exp(-blockSec / 0.04);
        vibInc_ = ctl_.vibratoHz * dt;
        vibDepth_ = (clampd(ctl_.vibratoCents, 0.0, 200.0) + clampd(ctl_.extraVibratoCents, 0.0, 200.0)) * vibAmt_;
        // pitch: the glide moves per block, the frequency is interpolated sample by sample inside it
        heldTime_ += blockSec;
        const double hzStart = midiToHz(currentNote() + ctl_.bendSemis);
        if (glidePos_ < 1.0) glidePos_ = std::min(1.0, glidePos_ + blockSec / std::max(blockSec, ctl_.glideSec));
        const double hzEnd = midiToHz(currentNote() + ctl_.bendSemis);
        baseHz_ = hzStart;
        hzStep_ = (hzEnd - hzStart) / std::max(1, frames);
        hairCoef_ = 1.0 - std::exp(-2.0 * kBowPi * kHairRel * hzEnd / sr_);
        levelCoef_ = 1.0 - std::exp(-dt / 0.05);
    }

    float render() noexcept {
        // ---- the bow this sample (speed, force)
        speed_ += (speedTarget_ - speed_) * speedCoef_;
        const double fMin = fMinPerSpeed_ * speed_;
        const double forceTarget = playing_ ? fMin * forceMul_ * (1.0 + biteAmount_ * bite_) : 0.0;
        force_ += (forceTarget - force_) * forceCoef_;
        bite_ *= biteDecay_;
        if (dampStep_ > 0.0) damp_ = std::min(1.0, damp_ + dampStep_);
        // ---- pitch: vibrato on top of the (per-block) glide
        vibPhase_ += vibInc_; if (vibPhase_ >= 1.0) vibPhase_ -= 1.0;
        const double vib = vibDepth_ * std::sin(2.0 * kBowPi * vibPhase_);
        baseHz_ += hzStep_;
        noteHz_ = baseHz_ * std::exp2(vib / 1200.0);
        // ---- string geometry in samples: one-way length S, round trips on both sides of the bow
        const double S = sr_ / (2.0 * noteHz_);
        const double b = ctl_.bowPosition;
        double dn = 2.0 * b * S;
        double db = 2.0 * (1.0 - b) * S - lpDelay_;
        if (dn < 2.5) { db -= (2.5 - dn); dn = 2.5; }
        // ---- waves arriving at the bow from both sides
        const double vn = -gFinger_ * (1.0 - 0.12 * damp_) * nut_.read(dn);
        const double raw = -gString_ * (1.0 - 0.03 * damp_) * bridge_.read(db);
        lp_ += (raw - lp_) * (1.0 - a_);
        const double vb = lp_;
        const double vh = vn + vb;                                  // string velocity at the bow if no bow force
        // ---- the bow: hair roughness as a little noise on the speed; the hair is wide and springy
        noise_ += (white() - noise_) * 0.15;
        hairLp_ += (vh - hairLp_) * hairCoef_;
        const double vBow = speed_ * (1.0 + 0.012 * noise_);
        const double f = friction(vBow - hairLp_, force_ * (1.0 + 0.02 * noise_));
        // ---- scattering at the bow (velocity waves, 2Z = 1)
        nut_.write(static_cast<float>(clampd(vb + f, -64.0, 64.0)));   // safety only: the real waves stay far below
        bridge_.write(static_cast<float>(clampd(vn + f, -64.0, 64.0)));
        // ---- bridge force -> body
        const double bridgeForce = bridge_.read(std::max(2.0, (1.0 - b) * S - lpDelay_ * 0.5));
        lastForce_ = bridgeForce;
        double out = bridgeForce * dry_;
        for (auto& r : body_) out += r.tick(bridgeForce);
        // gentle roll-off above the bridge hill, DC block
        outLp_ += (out - outLp_) * outLpCoef_;
        const double y = outLp_ - dcX_ + 0.995 * dcY_; dcX_ = outLp_; dcY_ = y;
        level_ += (std::abs(y) - level_) * levelCoef_;
        // the bridge force grows as the bow nears the bridge (slip velocity ~ 1/position): keep the
        // instrument's loudness the same from tasto to ponticello, only the colour changes
        return static_cast<float>(y * inst_.outGain * (b / 0.12));
    }

private:
    // calibrated on the regime maps: force band inside the Helmholtz region, bow-hair smoothing
    static constexpr double kMin = 0.03, kMax = 0.22, kNoteAlpha = 0.8, kHairRel = 14.0;

    // Friction force (velocity units, 2Z = 1) for the relative velocity d = vBow - vh that the string
    // would have, and the bow force Fb. Exact Friedlander solution with the hyperbolic friction curve
    //   mu(v) = muD + (muS - muD) * v0 / (v0 + |v|)
    // and the hysteresis rule: once slipping, stay on the slipping branch until it disappears.
    double friction(double d, double Fb) noexcept {
        const double muS = inst_.muStick, muD = inst_.muSlip, v0 = inst_.frictionV0;
        if (Fb <= 1e-9) { slipping_ = true; return 0.0; }
        const double ad = std::abs(d), s = d < 0.0 ? -1.0 : 1.0;
        const double stickMax = Fb * muS;
        if (!slipping_ && ad <= stickMax) return d;                 // sticking: the string follows the bow
        // slipping: x = |relative velocity| solves  x^2 + (v0 + Fb*muD - ad) x + v0 (Fb*muS - ad) = 0
        const double bq = v0 + Fb * muD - ad;
        const double cq = v0 * (stickMax - ad);
        const double disc = bq * bq - 4.0 * cq;
        if (disc >= 0.0) {
            const double x = (-bq + std::sqrt(disc)) * 0.5;         // the outer (stable) slipping solution
            if (x > 0.0) { slipping_ = true; return s * (ad - x); }
        }
        slipping_ = false;                                          // no slipping solution: the string sticks
        return s * std::min(ad, stickMax);
    }
    double white() noexcept { seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5; return static_cast<double>(seed_) * (2.0 / 4294967295.0) - 1.0; }
    void buildBody() noexcept {
        // violin body: air mode A0, corpus modes B1-/B1+, the 1 kHz region, the bridge hill ~2.5 kHz
        static constexpr Mode kViolin[kModes] = {
            {275.0, 11.0, 0.0}, {405.0, 14.0, -6.0}, {462.0, 18.0, 2.0}, {545.0, 16.0, 3.0}, {660.0, 10.0, -3.0},
            {820.0, 9.0, -2.0}, {1010.0, 8.0, -2.0}, {1240.0, 9.0, -1.0}, {1520.0, 9.0, -3.0}, {1900.0, 9.0, -2.0},
            {2350.0, 6.0, 2.0}, {2900.0, 6.0, 1.0}, {3500.0, 8.0, -3.0}, {4400.0, 8.0, -7.0}};
        for (int i = 0; i < kModes; ++i) { Mode s = kViolin[i]; s.freqHz *= inst_.bodyScale; body_[static_cast<std::size_t>(i)].set(sr_, s); }
        dry_ = std::pow(10.0, inst_.bodyDry / 20.0);
        outLpCoef_ = 1.0 - std::exp(-2.0 * kBowPi * std::min(sr_ * 0.4, 7000.0 * inst_.bodyScale * 1.4) / sr_);
    }

    static constexpr int kModes = 14;
    double sr_{48000.0};
    int instrumentIndex_{1};
    Instrument inst_{};
    Controls ctl_{};
    DelayLine nut_, bridge_;
    std::array<Resonator, kModes> body_{};
    double dry_{0.2}, outLpCoef_{0.5};
    // per-block values
    double speedTarget_{0.0}, speedCoef_{0.0}, forceCoef_{0.0}, biteDecay_{1.0}, biteAmount_{0.0}, dampStep_{0.0};
    double fMinPerSpeed_{0.0}, forceMul_{1.0}, a_{0.2}, lpDelay_{0.25}, gFinger_{0.99}, gString_{0.99};
    double vibInc_{0.0}, vibDepth_{0.0}, baseHz_{440.0}, hairCoef_{1.0}, levelCoef_{0.0};
    // state
    double lp_{0.0}, speed_{0.0}, force_{0.0}, noise_{0.0}, damp_{0.0}, bite_{0.0}, hairLp_{0.0};
    double noteHz_{440.0}, hzStep_{0.0}, glideFrom_{69.0}, glideTo_{69.0}, glidePos_{1.0};
    double vibPhase_{0.0}, vibAmt_{0.0}, heldTime_{0.0};
    double dcX_{0.0}, dcY_{0.0}, outLp_{0.0}, lastForce_{0.0}, level_{0.0};
    bool slipping_{true}, playing_{false};
    std::uint32_t seed_{0x2545F491u};
};

} // namespace bow

struct SamplerEngine::Impl {
    struct ChannelState {
        float expression{1.0F};
        float breath{1.0F};
        float pressure{0.0F};
        std::array<float, 128U> cc{};
        bool sustain{false};
        std::int16_t pitchBend{0};
    };

    struct Voice {
        bool active{false};
        bool keyHeld{false};
        bool sustained{false};
        std::uint64_t age{0U};
        std::uint8_t channel{0U};
        std::uint8_t key{0U};
        std::uint8_t velocity{0U};
        int upperIndex{0};
        int soundIndex{0};            // SILVETON_LAYERS_V1: 0 = basic sound, 1..4 = layer
        std::uint16_t routeMask{0U};
        SamplePtr sample;
        std::shared_ptr<const EngineState> ownerState;
        const ProgramLayer* layer{nullptr};
        const SampleZone* zone{nullptr};
        const UpperPart* upper{nullptr};
        double position{0.0};
        bool loopWrapped{false}; // SILVETON_STUDIO_INTERP_V1: passed the loop end at least once
        double ratio{1.0};
        double targetRatio{1.0};
        double glideLogStep{0.0};
        double glideMul{1.0};
        std::uint32_t glideFramesRemaining{0U};
        // SILVETON_SOFT_GLIDE_V1: glide as an offset (semitones) from targetRatio.
        std::uint32_t glideTotalFrames{0U};
        double glideStartSemis{0.0};   // offset when the glide started
        double glideRawSemis{0.0};     // S-curve offset now (before BALANCE rounding)
        double glideSmooth1{0.0};      // BALANCE rounding, stage 1
        double glideSmooth2{0.0};      // actual continuous pitch offset (semitones)
        // SILVETON_SILK_PORTA_V1: remember the audible slope on interrupted glides.
        // Offsets/velocity are in semitones, not Hertz: even musical motion.
        double glideStartVelocity{0.0}; // initial semitone movement over whole glide
        double glideLastStep{0.0};      // last audible semitone change per sample
        bool pitchGliding{false};
        bool glideKronos{false};       // SILVETON_KRONOS_MUSICAL_PORTA_V2: controlled smooth pitch shape
        // SILVETON_TONE_V2
        float velNorm{1.0F};
        float filterEnvValue{0.0F};    // 1 at note on, decays to 0 in FILTER ENV time
        float velCutoffOct{0.0F};
        double endFrame{0.0};          // SAMPLE END (frames), 0 = none
        bool loopOff{false};           // END is before the loop end: no loop
        double vibPhase{0.0};
        double vibAgeSec{0.0};
        double vibSemis{0.0};          // vibrato of this block (semitones)
        // Click-free 4 ms fade used when a voice is taken over (voice limit,
        // MONO/LEGATO note change) instead of a one-sample cut.
        float fadeGain{1.0F};
        float fadeStep{0.0F};
        bool fading{false};
        bool fadeBlend{false};         // SILVETON_XFADE_PORTA_V1: crossfade law between two recordings of one line
        // SILVETON_POLYSIX_V1
        int unisonIndex{0};
        float unisonCents{0.0F};
        float unisonPan{0.0F};
        float unisonGain{1.0F};
        double subPosition{0.0};
        double periodFrames{0.0};      // one period of the recording's own pitch, in its frames (PWM)
        double pwmPhase{0.0};
        float pwmWidthNow{0.0F};
        float dcXL{0.0F}, dcYL{0.0F}, dcXR{0.0F}, dcYR{0.0F};
        // SILVETON_SYNTH_LIGHT_V1: PWM = the read sound minus itself one WIDTH x period later,
        // taken from this short delay line instead of a second interpolated read of the recording.
        static constexpr int kPwmDelay = 2048;      // frames (one period down to 47 Hz at 96 kHz)
        std::array<float, static_cast<std::size_t>(kPwmDelay) * 2U> pwmBuf{};
        int pwmWrite{0};
        int pwmFilled{0};
        float random{0.0F};            // -1..1, fixed for the note
        float modCutOct{0.0F}, modRes{0.0F}, modCut2Oct{0.0F}, modLevelDb{0.0F}, modPan{0.0F};
        float modPitch{0.0F}, modLfoCents{0.0F}, modLfoRateMul{1.0F}, modPwm{0.0F}, modDrive{0.0F}, modAttackMul{1.0F}, modReleaseMul{1.0F};
        std::int8_t filterTypeNow{0};
        LadderFilter ladder{};
        Korg35Filter korg35{};
        StateVariableFilter filter2{};
        float filter2Hz{1000.0F}, filter2Res{0.0F};
        std::uint32_t delayFramesRemaining{0U};
        // Zone/layer gain x velocity. The Upper's level, pan and ON/MUTE/SOLO are
        // the live mix gains below (SILVETON_LIVE_EDIT_V1).
        float baseGain{1.0F};
        float mixL{0.70710678F};
        float mixR{0.70710678F};
        float mixTargetL{0.70710678F};
        float mixTargetR{0.70710678F};
        float mixPanIn{0.0F};
        float mixLevelIn{0.0F};
        bool mixAudibleIn{true};
        // Upper + GLOBAL tuning (semitones) this voice is playing with now.
        double tuneSemis{0.0};
        AmpEnv ampEnv{};
        ToneLowPass toneLp{};
        bool toneOn{false};
        float toneLog{0.0F};
        bool legatoKeep{false};
        bool mono{false};
        Envelope filterEnv{};
        Envelope pitchEnv{};
        LfoState lfo1{};
        LfoState lfo2{};
        StateVariableFilter filter{};
    };

    // SILVETON_BOW_V1: one bowed string per sound slot (basic sound or layer) of every Upper.
    // Monophonic with legato like the real instrument: held keys form a stack, a new key glides
    // (no new attack), letting the top key go glides back to the key still held.
    struct BowVoice {
        bow::BowedString string;
        std::array<std::uint8_t, 16U> keys{};
        std::array<std::uint8_t, 16U> vels{};
        std::uint8_t count{0U};
        bool active{false};
        bool sustained{false};       // all keys up, damper pedal holds the bow on the string
        std::uint8_t channel{0U};
        double lastNote{-1.0};       // the note last played on this string (portamento from it)
        float velNorm{0.8F};
        float mixL{0.0F}, mixR{0.0F};
        std::uint32_t quietFrames{0U};
    };
    std::array<std::array<BowVoice, kSoundsPerUpper>, kUpperCount> bows{};

    double sampleRate{48000.0};
    std::uint32_t maxBlock{512U};
    // 128 playable voices + spare tail slots for 4 ms takeover fades.
    std::array<Voice, static_cast<std::size_t>(kMaxVoices) + SamplerEngine::kTakeoverTailVoices> voices{};
    std::array<ChannelState, 16U> channels{};
    // MONO/LEGATO: keys currently held on each Upper, oldest first.
    struct MonoKeys {
        std::array<std::uint8_t, 16U> key{};
        std::array<std::uint8_t, 16U> vel{};
        std::uint8_t count{0U};
    };
    std::array<MonoKeys, kUpperCount> mono{};
    std::array<bool, kUpperCount> monoWas{};
    std::atomic<float> portaBalance{11.0F};
    // SILVETON_PORTA_NEAR_V1: glide time for neighbouring keys (1..3 semitones), ms.
    std::atomic<float> portaNearMs{90.0F};
    std::uint64_t frameClock{0U};                                   // frames rendered so far
    std::array<int, kUpperCount> polyLastKey{};                     // POLY portamento: last key of each Upper
    std::array<int, kUpperCount> polyAnchorKey{};                   // glide start shared by a chord
    std::array<std::uint64_t, kUpperCount> polyLastOn{};
    // SILVETON_TERCA_V1 (written by the control thread, read by the audio thread)
    std::atomic<int> tercaConfig{-1};    // -1 off, else upperMask | root << 8 | cell << 12
    std::atomic<int> scaleUppers{0};     // Uppers whose Q key is on
    int tercaApplied{-1};                // config the maps below belong to (audio thread)
    std::array<std::array<std::int8_t, 128>, 16> tercaKey{};       // physical key -> harmony (-1)
    std::array<std::array<std::uint8_t, 128>, 16> tercaRefs{};     // harmony refcount
    std::uint64_t voiceAge{0U};
    std::uint32_t noiseState{0x9E3779B9U};   // SILVETON_POLYSIX_V1: per-note random
    float peakL{0.0F};
    float peakR{0.0F};
    float limiterGrDb{0.0F};
    std::array<float, kUpperCount> upperPeakL{};
    std::array<float, kUpperCount> upperPeakR{};
    std::array<std::uint16_t, kUpperCount> upperRouteMask{};
    Biquad lowEq{}, midEq{}, highEq{};
    // SILVETON_EQ5_V1: common EQ of every Upper; SILVETON_TONE_V2: bus of every sound.
    std::array<UpperEqState, kUpperCount> upperEq{};
    std::array<std::array<SoundBusState, kSoundsPerUpper>, kUpperCount> soundBus{};
    // Per-Upper, per-sound mix of one chunk (voices are rendered voice by voice).
    static constexpr std::uint32_t kRenderChunk = 256U;
    std::array<std::array<std::array<float, kRenderChunk>, kSoundsPerUpper>, kUpperCount> chunkL{};
    std::array<std::array<std::array<float, kRenderChunk>, kSoundsPerUpper>, kUpperCount> chunkR{};
    // SILVETON_FX_V3: one effect unit per send slot (A and B for the tails) and
    // one per Upper INSERT. Their buffers are allocated once in prepare().
    std::array<std::array<FxUnit, 2U>, kFxSendSlots> sendFx{};
    std::array<FxUnit, kUpperCount> insertFx{};
    std::array<std::uint8_t, kFxSendSlots> fxInst{};        // engine the slot plays now (0 = A, 1 = B)
    std::array<EffectSlot, kFxSendSlots> fxRunning{};       // settings of the last block
    bool fxRunningValid{false};
    std::uint32_t fxGeneration{0U};
    std::array<EffectSlot, kFxSendSlots> tailCfg{};
    std::array<std::uint8_t, kFxSendSlots> tailInst{};
    std::array<std::uint32_t, kFxSendSlots> tailLeft{};     // frames the tail may still ring (0 = no tail)
    std::array<std::uint32_t, kFxSendSlots> tailQuiet{};    // frames the tail has been silent
    // SILVETON_ANALYZER_V1
    std::array<float, SamplerEngine::kAnalyzerRing> tapRing{};
    std::atomic<std::uint32_t> tapWrite{0U};
    std::atomic<int> tapUpper{-1};
    std::atomic<bool> tapAfter{true};
    // SILVETON_CPU_METER_V1
    std::atomic<float> cpuLoad{0.0F};
    float cpuLoadHold{0.0F};

    // Read-ahead windows for SampleStreamPrefetcher (one seqlock per slot).
    struct WindowSlot {
        std::atomic<std::uint32_t> seq{0U};
        std::atomic<std::uintptr_t> begin{0U};
        std::atomic<std::size_t> bytes{0U};
    };
    std::array<WindowSlot, SamplerEngine::kMaxStreamWindows> windows{};
    std::atomic<double> lookaheadSec{0.75};

    void publishWindow(std::size_t slot, std::uintptr_t begin, std::size_t bytes) noexcept {
        auto& w = windows[slot];
        if (w.begin.load(std::memory_order_relaxed) == begin && w.bytes.load(std::memory_order_relaxed) == bytes) return;
        const std::uint32_t s0 = w.seq.load(std::memory_order_relaxed);
        w.seq.store(s0 + 1U, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        w.begin.store(begin, std::memory_order_relaxed);
        w.bytes.store(bytes, std::memory_order_relaxed);
        w.seq.store(s0 + 2U, std::memory_order_release);
    }

    void clearWindows() noexcept {
        for (std::size_t i = 0; i < windows.size(); ++i) publishWindow(i, 0U, 0U);
    }

    void reset(double sr) noexcept {
        sampleRate = sr;
        voiceAge = 0U;
        peakL = peakR = limiterGrDb = 0.0F;
        for (auto& ch : tercaKey) ch.fill(-1);
        for (auto& ch : tercaRefs) ch.fill(0U);
        tercaApplied = -1;
        polyLastKey.fill(-1); polyAnchorKey.fill(-1); polyLastOn.fill(0U); frameClock = 0U;
        upperPeakL.fill(0.0F);
        upperPeakR.fill(0.0F);
        upperRouteMask.fill(0U);
        for (auto& c : channels) c = ChannelState{};
        for (auto& m : mono) m = MonoKeys{};
        monoWas.fill(false);
        for (auto& v : voices) v = Voice{};
        for (auto& row : bows) for (auto& b : row) { b.string.reset(); b.count = 0U; b.active = false; b.sustained = false; b.lastNote = -1.0; b.mixL = b.mixR = 0.0F; b.quietFrames = 0U; }
        lowEq.reset(); midEq.reset(); highEq.reset();
        for (auto& e : upperEq) e.reset();
        for (auto& row : soundBus) for (auto& b : row) b.reset();
        for (auto& pair : sendFx) for (auto& u : pair) u.reset();
        for (auto& u : insertFx) u.reset();
        fxInst.fill(0U); tailLeft.fill(0U); tailQuiet.fill(0U); fxRunningValid = false; fxGeneration = 0U;
        cpuLoadHold = 0.0F; cpuLoad.store(0.0F, std::memory_order_relaxed);
        clearWindows();
    }

    // Control thread only (allocates): effect memory for this sample rate.
    void prepareFx(double sr) {
        for (auto& pair : sendFx) for (auto& u : pair) u.prepare(sr, 2.6);
        for (auto& u : insertFx) u.prepare(sr, 1.1);
        for (auto& row : bows) for (auto& b : row) b.string.prepare(sr);   // SILVETON_BOW_V1
    }
};

UpperToneValues effectiveUpperTone(const UpperPart& upper) noexcept {
    EnvelopeParams env{};
    if (upper.program != nullptr && !upper.program->layers.empty()) env = upper.program->layers.front().ampEnvelope;
    return effectiveToneFor(basicToneOf(upper), upper, env);
}

UpperToneValues effectiveLayerTone(const UpperLayer& layer) noexcept {
    EnvelopeParams env{};
    if (layer.program != nullptr && !layer.program->layers.empty()) env = layer.program->layers.front().ampEnvelope;
    UpperPart plain{};   // a layer never uses the Easy Edit scales of the Upper
    return effectiveToneFor(layer.tone, plain, env);
}

SoundTone basicSoundTone(const UpperPart& upper) noexcept { return basicToneOf(upper); }

// ---------------------------------------------------------------- SILVETON_POLYSIX_V1 built-in waves
namespace {
constexpr std::array<const char*, kBuiltinWaveCount> kBuiltinWaveNames{{"SAW", "SQUARE", "PULSE 25", "TRIANGLE", "SINE", "SAW WARM"}};

SamplePtr makeWaveTable(int wave, int rootKey) {
    const double f = 440.0 * std::pow(2.0, (rootKey - 69) / 12.0);
    const auto P = static_cast<std::size_t>(std::max(16L, std::lround(96000.0 / f)));   // one period in frames
    const std::size_t periods = 8U;
    auto s = std::make_shared<SampleData>();
    s->name = std::string(kBuiltinWaveNames[static_cast<std::size_t>(wave)]) + " " + std::to_string(rootKey);
    s->sampleRate = static_cast<std::uint32_t>(std::lround(static_cast<double>(P) * f));   // so that one period = exactly P frames
    s->channels = 1U;
    s->rootKey = static_cast<std::uint8_t>(rootKey);
    s->interleaved.assign(P * periods, 0.0F);
    s->totalFrames = P * periods;
    s->loop.enabled = true; s->loop.startFrame = 0U; s->loop.endFrame = P * periods; s->loop.crossfadeFrames = 0U;
    const int nMax = static_cast<int>(P / 2U) - 1;   // the table's own Nyquist
    std::vector<double> one(P, 0.0);
    for (int n = 1; n <= nMax; ++n) {
        double a = 0.0; bool cosTerm = false;
        switch (wave) {
            case 0: a = ((n % 2) ? 1.0 : -1.0) / n; break;                                        // saw
            case 1: a = (n % 2) ? 1.0 / n : 0.0; break;                                            // square
            case 2: a = 2.0 / (n * kPi) * std::sin(n * kPi * 0.25); cosTerm = true; break;         // pulse 25 %
            case 3: a = (n % 2) ? (((n / 2) % 2 == 0) ? 1.0 : -1.0) / (static_cast<double>(n) * n) : 0.0; break;   // triangle
            case 4: a = n == 1 ? 1.0 : 0.0; break;                                                  // sine
            default: a = ((n % 2) ? 1.0 : -1.0) / n / (1.0 + std::pow(n / 12.0, 2.0)); break;       // saw warm
        }
        if (a == 0.0) continue;
        const double sigma = std::sin(kPi * n / (nMax + 1.0)) / (kPi * n / (nMax + 1.0));   // Lanczos: no Gibbs ripple
        a *= sigma;
        for (std::size_t i = 0; i < P; ++i) {
            const double th = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(P);
            one[i] += a * (cosTerm ? std::cos(n * th) : std::sin(n * th));
        }
    }
    double peak = 1.0e-9; double mean = 0.0;
    for (double v : one) mean += v;
    mean /= static_cast<double>(P);
    for (double& v : one) { v -= mean; peak = std::max(peak, std::abs(v)); }
    for (std::size_t k = 0; k < periods; ++k)
        for (std::size_t i = 0; i < P; ++i) s->interleaved[k * P + i] = static_cast<float>(one[i] / peak * 0.5);
    return s;
}
} // namespace

const char* builtinWaveName(int index) noexcept {
    return (index >= 0 && index < kBuiltinWaveCount) ? kBuiltinWaveNames[static_cast<std::size_t>(index)] : "";
}

ProgramPtr builtinWaveProgram(int index) {
    if (index < 0 || index >= kBuiltinWaveCount) return nullptr;
    static std::array<ProgramPtr, kBuiltinWaveCount> cache{};
    static std::mutex cacheMutex;
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto& slot = cache[static_cast<std::size_t>(index)];
    if (slot != nullptr) return slot;
    auto ms = std::make_shared<Multisample>();
    ms->name = kBuiltinWaveNames[static_cast<std::size_t>(index)];
    for (int root = 12; root <= 120; root += 12) {
        SampleZone z{};
        z.sample = makeWaveTable(index, root);
        z.rootKey = static_cast<std::uint8_t>(root);
        z.keyLow = static_cast<std::uint8_t>(root == 12 ? 0 : root - 6);
        z.keyHigh = static_cast<std::uint8_t>(root == 120 ? 127 : root + 5);
        z.velocityLow = 1U; z.velocityHigh = 127U;
        ms->zones.push_back(z);
    }
    auto prog = std::make_shared<Program>();
    prog->name = kBuiltinWaveNames[static_cast<std::size_t>(index)];
    ProgramLayer layer{};
    layer.name = prog->name;
    layer.multisample = ms;
    layer.ampEnvelope = EnvelopeParams{0.003F, 0.05F, 1.0F, 0.08F};
    prog->layers.push_back(layer);
    slot = prog;
    return slot;
}

// ---------------------------------------------------------------- SILVETON_BOW_V1
// The bowed instruments: a Program without recordings, the sound is the string model.
namespace {
constexpr std::array<const char*, kBowInstrumentCount> kBowNames{{"ЛЪК ЦИГУЛКА", "ЛЪК ВИОЛА", "ЛЪК ЧЕЛО", "ЛЪК КОНТРАБАС"}};
}

const char* bowInstrumentName(int index) noexcept {
    return (index >= 0 && index < kBowInstrumentCount) ? kBowNames[static_cast<std::size_t>(index)] : "";
}

bool bowSoundSlot(std::uint16_t slot) noexcept {
    return slot >= kBowFirstSlot && slot < kBowFirstSlot + static_cast<std::uint16_t>(kBowInstrumentCount);
}

ProgramPtr bowInstrumentProgram(int index) {
    if (index < 0 || index >= kBowInstrumentCount) return nullptr;
    static std::array<ProgramPtr, kBowInstrumentCount> cache{};
    static std::mutex cacheMutex;
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto& slot = cache[static_cast<std::size_t>(index)];
    if (slot != nullptr) return slot;
    auto prog = std::make_shared<Program>();
    prog->name = kBowNames[static_cast<std::size_t>(index)];
    prog->bowInstrument = static_cast<std::uint8_t>(index + 1);
    prog->mono = true;
    prog->legato = true;
    slot = prog;
    return slot;
}


SamplerEngine::SamplerEngine()
    : impl_(std::make_unique<Impl>()), state_(std::make_shared<EngineState>()), interpolator_(InterpolationQuality::Studio) { // SILVETON_STUDIO_INTERP_V1
    prepare(48000.0, 512U);
}
SamplerEngine::~SamplerEngine() = default;

void SamplerEngine::prepare(double sampleRate, std::uint32_t maximumBlockFrames) {
    const double sr = std::clamp(sampleRate, 8000.0, 384000.0);
    impl_->maxBlock = std::max<std::uint32_t>(16U, maximumBlockFrames);
    impl_->prepareFx(sr);
    impl_->reset(sr);
}

void SamplerEngine::setPortamentoBalance(float balance) noexcept {
    impl_->portaBalance.store(std::isfinite(balance) ? std::clamp(balance, 3.0F, 40.0F) : 11.0F, std::memory_order_relaxed);
}

namespace {
struct TercaCell { std::array<int, 7> scale; int direction; };
constexpr std::array<TercaCell, 6> kTercaCells{{
    {{{0, 2, 4, 5, 7, 9, 11}}, +1},   // major third up
    {{{0, 2, 4, 5, 7, 9, 11}}, -1},   // major third down
    {{{0, 2, 3, 5, 7, 8, 10}}, +1},   // minor third up
    {{{0, 2, 3, 5, 7, 8, 10}}, -1},   // minor third down
    {{{0, 1, 3, 5, 7, 8, 10}}, +1},   // phrygian third up
    {{{0, 1, 4, 5, 7, 8, 10}}, +1},   // hijaz third up
}};
int tercaDegree(const std::array<int, 7>& scale, int relativePc) noexcept {
    for (int i = 0; i < 7; ++i) if (scale[static_cast<std::size_t>(i)] == relativePc) return i;
    return -1;
}
} // namespace

int SamplerEngine::tercaHarmony(int root, int cell, int key) noexcept {
    if (cell < 0 || cell >= 6 || key < 0 || key > 127) return -1;
    const auto& c = kTercaCells[static_cast<std::size_t>(cell)];
    const int dir = c.direction;
    const int pc = key % 12;
    const int relative = (pc - (root % 12) + 12) % 12;
    const int degree = tercaDegree(c.scale, relative);
    if (degree < 0) {
        // Not in the scale: the minor third that lands in the scale, else the
        // major third that does, else a plain minor third (processor rule).
        int delta = 3 * dir;
        if (tercaDegree(c.scale, ((relative + 3 * dir) % 12 + 12) % 12) >= 0) delta = 3 * dir;
        else if (tercaDegree(c.scale, ((relative + 4 * dir) % 12 + 12) % 12) >= 0) delta = 4 * dir;
        const int r = key + delta;
        return (r < 0 || r > 127) ? -1 : r;
    }
    const int step = degree + 2 * dir;
    const int octave = step >= 7 ? 12 : (step < 0 ? -12 : 0);
    const int target = c.scale[static_cast<std::size_t>(((step % 7) + 7) % 7)] + octave;
    const int r = key + target - c.scale[static_cast<std::size_t>(degree)];
    return (r < 0 || r > 127) ? -1 : r;
}

void SamplerEngine::setTerca(int upperMask, int root, int cell) noexcept {
    const int mask = upperMask & 0x7F;
    const int cfg = (mask == 0 || cell < 0 || cell >= 6) ? -1 : (mask | ((root % 12 + 12) % 12) << 8 | cell << 12);
    impl_->tercaConfig.store(cfg, std::memory_order_release);
}

void SamplerEngine::setScaleUppers(int upperMask) noexcept {
    impl_->scaleUppers.store(upperMask & 0x7F, std::memory_order_release);
}

void SamplerEngine::setPortamentoNear(float ms) noexcept {
    impl_->portaNearMs.store(std::isfinite(ms) ? std::clamp(ms, 0.0F, 400.0F) : 90.0F, std::memory_order_relaxed);
}

float SamplerEngine::portamentoNear() const noexcept {
    return impl_->portaNearMs.load(std::memory_order_relaxed);
}

float SamplerEngine::portamentoBalance() const noexcept {
    return impl_->portaBalance.load(std::memory_order_relaxed);
}

void SamplerEngine::reset() noexcept {
    impl_->reset(impl_->sampleRate);
    events_.clear();
}

void SamplerEngine::setState(std::shared_ptr<const EngineState> state) noexcept {
    if (state == nullptr) return;
    // No file I/O, no locks and no ALSA stop here. Latest complete graph wins.
    std::atomic_store_explicit(&pendingState_, std::move(state), std::memory_order_release);
}

bool SamplerEngine::readStreamWindow(std::size_t slot, StreamWindow& out) const noexcept {
    out = StreamWindow{};
    if (slot >= impl_->windows.size()) return false;
    const auto& w = impl_->windows[slot];
    const std::uint32_t s1 = w.seq.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0U) return false;
    const std::uintptr_t begin = w.begin.load(std::memory_order_relaxed);
    const std::size_t bytes = w.bytes.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (w.seq.load(std::memory_order_relaxed) != s1) return false;
    out.begin = begin;
    out.bytes = bytes;
    return true;
}

void SamplerEngine::setAnalyzerTap(int upper, bool afterEq) noexcept {
    impl_->tapAfter.store(afterEq, std::memory_order_relaxed);
    impl_->tapUpper.store((upper >= 0 && upper < kUpperCount) ? upper : -1, std::memory_order_release);
}

void SamplerEngine::readAnalyzer(float* out, std::size_t frames) const noexcept {
    if (out == nullptr || frames == 0U) return;
    const std::size_t n = std::min(frames, kAnalyzerRing);
    const std::uint32_t w = impl_->tapWrite.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (static_cast<std::size_t>(w) + kAnalyzerRing - n + i) % kAnalyzerRing;
        out[i] = impl_->tapRing[idx];
    }
    for (std::size_t i = n; i < frames; ++i) out[i] = 0.0F;
}

void SamplerEngine::setStreamLookaheadSeconds(double seconds) noexcept {
    impl_->lookaheadSec.store(std::clamp(seconds, 0.05, 4.0), std::memory_order_relaxed);
}

bool SamplerEngine::postNoteOn(std::uint8_t ch, std::uint8_t key, std::uint8_t vel) noexcept {
    return events_.push({EventType::NoteOn, static_cast<std::uint8_t>(ch & 0x0FU), static_cast<std::uint8_t>(key & 0x7FU), static_cast<std::uint8_t>(vel & 0x7FU), 0});
}
bool SamplerEngine::postNoteOff(std::uint8_t ch, std::uint8_t key, std::uint8_t vel) noexcept {
    return events_.push({EventType::NoteOff, static_cast<std::uint8_t>(ch & 0x0FU), static_cast<std::uint8_t>(key & 0x7FU), static_cast<std::uint8_t>(vel & 0x7FU), 0});
}
bool SamplerEngine::postControlChange(std::uint8_t ch, std::uint8_t cc, std::uint8_t value) noexcept {
    return events_.push({EventType::ControlChange, static_cast<std::uint8_t>(ch & 0x0FU), static_cast<std::uint8_t>(cc & 0x7FU), static_cast<std::uint8_t>(value & 0x7FU), 0});
}
bool SamplerEngine::postPitchBend(std::uint8_t ch, std::int16_t value) noexcept {
    RealtimeEvent e{}; e.type = EventType::PitchBend; e.channel = static_cast<std::uint8_t>(ch & 0x0FU); e.pitchBend = static_cast<std::int16_t>(std::clamp<int>(value, -8192, 8191));
    return events_.push(e);
}
bool SamplerEngine::postChannelPressure(std::uint8_t ch, std::uint8_t value) noexcept {
    return events_.push({EventType::ChannelPressure, static_cast<std::uint8_t>(ch & 0x0FU), static_cast<std::uint8_t>(value & 0x7FU), 0U, 0});
}
bool SamplerEngine::postPanic() noexcept { return events_.push({EventType::Panic,0U,0U,0U,0}); }

namespace {

constexpr float kTakeoverFadeSec = 0.004F; // 4 ms: click-free and inaudible as a fade

void softKill(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, float fadeSec = kTakeoverFadeSec) noexcept {
    if (!v.active || v.fading) return;
    if (v.delayFramesRemaining > 0U) { v.active = false; return; } // has not sounded yet
    v.fading = true;
    v.keyHeld = false;
    v.sustained = false;
    v.fadeStep = -1.0F / static_cast<float>(std::max(1.0, static_cast<double>(fadeSec) * impl.sampleRate));
}

SamplerEngine::Impl::Voice* chooseVoice(SamplerEngine::Impl& impl) noexcept {
    constexpr auto kPlayable = static_cast<std::size_t>(kMaxVoices);
    for (std::size_t i = 0; i < kPlayable; ++i) if (!impl.voices[i].active) return &impl.voices[i];
    // All 128 voices sound (SILVETON_VOICE_GUARD_V1): the quietest one stops - a
    // released note first, then the one with the least level, the oldest on a
    // tie. Instead of cutting it in one sample, its sound continues in a spare
    // tail slot and fades out in 4 ms. Copying a voice copies pointers only.
    auto* oldest = &impl.voices[0];
    const auto loudness = [](const SamplerEngine::Impl::Voice& v) noexcept {
        float w = v.ampEnv.value * v.baseGain * std::max(std::abs(v.mixL), std::abs(v.mixR));
        if (v.keyHeld || v.sustained) w *= 8.0F;   // a held note is worth more than a ringing release
        if (v.fading) w *= 0.01F;
        return w;
    };
    for (std::size_t i = 0; i < kPlayable; ++i) {
        const auto& c = impl.voices[i];
        const float lc = loudness(c), lo = loudness(*oldest);
        if (lc < lo || (lc == lo && c.age < oldest->age)) oldest = &impl.voices[i];
    }
    for (std::size_t i = kPlayable; i < impl.voices.size(); ++i) {
        auto& tail = impl.voices[i];
        if (tail.active) continue;
        tail = *oldest;
        softKill(impl, tail);
        break;
    }
    return oldest;
}

// Playback ratio of a key for one layer/zone (also used by LEGATO pitch changes).
// ulayer = the layer of the Upper this sound belongs to (nullptr = basic sound).
std::uint8_t playKeyFor(const UpperPart& upper, const GlobalState& global, std::uint8_t key,
                        const UpperLayer* ulayer = nullptr) noexcept {
    int k = static_cast<int>(key) + static_cast<int>(upper.transposeSemitones) +
            static_cast<int>(upper.octave) * 12 + static_cast<int>(global.transposeSemitones);
    if (ulayer != nullptr) k += static_cast<int>(ulayer->transposeSemitones) + static_cast<int>(ulayer->octave) * 12;
    return static_cast<std::uint8_t>(std::clamp(k, 0, 127));
}

double noteRatio(const SamplerEngine::Impl& impl, const UpperPart& upper, const ProgramLayer& layer,
                 const SampleZone& zone, std::uint8_t key, const GlobalState& global, int ui,
                 const UpperLayer* ulayer = nullptr) noexcept {
    // SILVETON_OCTAVE_KEY_V1: OCTAVE / TRANSPOSE move the KEY (like Korg): the
    // sample recorded for the new key is chosen and played at its own pitch,
    // instead of stretching the sample of the pressed key.
    const std::uint8_t pk = playKeyFor(upper, global, key, ulayer);
    const int keyTracking = zone.nonTranspose ? 0 : (static_cast<int>(pk) - static_cast<int>(zone.rootKey));
    const int semitones = keyTracking +
                          static_cast<int>(zone.transposeSemitones) + static_cast<int>(layer.transposeSemitones);
    const int root = static_cast<int>(global.scaleRoot % 12U);
    // SILVETON_TQ_TRANSPOSE_V1: the scale follows the PLAYED key, so TRANSPOSE /
    // OCTAVE / TUNE never move it (same keys as SCAN saw).
    const int pc = (static_cast<int>(key) - root + 120) % 12;
    // SILVETON_SCALE_V1: only Uppers with their Q key on follow the scale.
    const bool scaleOn = ((impl.scaleUppers.load(std::memory_order_relaxed) >> ui) & 1) != 0;
    const float scaleCents = scaleOn ? std::clamp(global.scaleOffsetsCents[static_cast<std::size_t>(pc)], -100.0F, 100.0F) : 0.0F;
    const float cents = zone.tuneCents + layer.tuneCents + upper.tuneCents + zone.sample->fineTuneCents + scaleCents +
                        (ulayer != nullptr ? ulayer->tuneCents : 0.0F);
    const double tuneHz = std::clamp(static_cast<double>(global.masterTuneHz), 400.0, 480.0);
    const double masterTuneSemis = 12.0 * std::log2(tuneHz / 440.0);
    const double pitch = static_cast<double>(semitones) + static_cast<double>(cents) / 100.0 + masterTuneSemis;
    return (static_cast<double>(zone.sample->sampleRate) / impl.sampleRate) * std::pow(2.0, pitch / 12.0);
}

// SILVETON_LIVE_EDIT_V1 - every setting is heard on notes that already sound.
// While an Upper keeps the same Sound, its sounding notes follow every edit at
// once: level, pan, ON / MUTE / SOLO (smoothed, no clicks), tuning (octave,
// transpose, fine, master tune, global transpose), TONE, LFO, controllers, EQ.
// A note of a Sound that was replaced (or of another COMBI) keeps its own
// settings and ends naturally.
constexpr double kLiveMixSmoothSec = 0.005;

double liveTuneSemis(const UpperPart& upper, const GlobalState& global, std::uint8_t key, bool scaleOn,
                     const UpperLayer* ulayer = nullptr) noexcept {
    // OCTAVE / TRANSPOSE are part of the key (SILVETON_OCTAVE_KEY_V1): they take
    // effect on the next key press; fine tune, MASTER TUNE and SCALE stay live.
    const int root = static_cast<int>(global.scaleRoot % 12U);
    const int pc = (static_cast<int>(key) - root + 120) % 12;   // SILVETON_TQ_TRANSPOSE_V1: played key
    const float scaleCents = scaleOn ? std::clamp(global.scaleOffsetsCents[static_cast<std::size_t>(pc)], -100.0F, 100.0F) : 0.0F;
    const double tuneHz = std::clamp(static_cast<double>(global.masterTuneHz), 400.0, 480.0);
    return static_cast<double>(upper.tuneCents + scaleCents + (ulayer != nullptr ? ulayer->tuneCents : 0.0F)) / 100.0 +
           12.0 * std::log2(tuneHz / 440.0);
}

bool upperAudible(const EngineState& state, const UpperPart& upper) noexcept {
    if (!upper.enabled || upper.mute) return false;
    for (const auto& u : state.combi.uppers) if (u.enabled && u.solo) return upper.solo;
    return true;
}

// Mix gains of a voice: Upper level x equal-power pan x ON/MUTE/SOLO.
// Recomputed only when one of its inputs changes.
void updateMixTarget(SamplerEngine::Impl::Voice& v, const UpperPart& upper, bool audible, float extraPan) noexcept {
    // SILVETON_LAYERS_V1: a layer adds its own level and pan to the Upper's.
    const UpperLayer* ul = layerOf(upper, v.soundIndex);
    const float panIn = v.zone->pan + v.layer->pan + upper.pan + extraPan + (ul != nullptr ? ul->pan : 0.0F);
    const float levelIn = upper.levelDb + (ul != nullptr ? ul->levelDb : 0.0F);
    if (panIn == v.mixPanIn && levelIn == v.mixLevelIn && audible == v.mixAudibleIn) return;
    v.mixPanIn = panIn;
    v.mixLevelIn = levelIn;
    v.mixAudibleIn = audible;
    if (!audible) { v.mixTargetL = v.mixTargetR = 0.0F; return; }
    const float pan = std::clamp(panIn, -1.0F, 1.0F);
    const float angle = (pan + 1.0F) * static_cast<float>(kPi * 0.25);
    const float level = dbToGain(levelIn);
    v.mixTargetL = level * std::cos(angle);
    v.mixTargetR = level * std::sin(angle);
}

// SILVETON_TONE_V2: SAMPLE START in frames of the sample, moved forward to the
// next zero crossing (within 4 ms) so the note never starts with a click.
// Frame at or after `start` where the recording crosses zero (within 4 ms).
double snapToZeroCrossing(const SampleData& s, double start) noexcept {
    const auto pre = s.preloadedFrameCount();
    const auto ch = static_cast<std::size_t>(std::max<std::uint16_t>(1U, s.channels));
    auto i = static_cast<std::uint64_t>(std::max(0.0, start));
    const std::uint64_t limit = std::min<std::uint64_t>(pre, i + static_cast<std::uint64_t>(0.004 * s.sampleRate) + 1U);
    if (i + 1U < limit) {
        float prev = s.interleaved[static_cast<std::size_t>(i) * ch];
        for (std::uint64_t k = i + 1U; k < limit; ++k) {
            const float cur = s.interleaved[static_cast<std::size_t>(k) * ch];
            if ((prev <= 0.0F && cur >= 0.0F) || (prev >= 0.0F && cur <= 0.0F)) { i = k; break; }
            prev = cur;
        }
    }
    return static_cast<double>(i);
}

// SILVETON_XFADE_PORTA_V1: where a recording starts when it takes over a held
// line (legato / portamento): past its attack - at the loop start if it has
// one, else 40 ms in - so the new note is not struck again.
double legatoStartFrame(const SampleData& s) noexcept {
    const double frames = static_cast<double>(s.frameCount());
    if (s.loop.enabled && s.loop.endFrame > s.loop.startFrame && s.loop.startFrame > 0U &&
        static_cast<double>(s.loop.startFrame) < frames - 8.0) {
        return snapToZeroCrossing(s, static_cast<double>(s.loop.startFrame));
    }
    const double skip = 0.040 * static_cast<double>(s.sampleRate);
    if (frames > skip * 3.0) return snapToZeroCrossing(s, skip);
    return 0.0;
}

double startFrameFor(const SampleData& s, float startMs) noexcept {
    if (startMs <= 0.0F) return 0.0;
    const double frames = static_cast<double>(s.frameCount());
    double start = std::clamp(static_cast<double>(startMs), 0.0, static_cast<double>(kToneMaxStartMs)) * 0.001 * static_cast<double>(s.sampleRate);
    if (start >= frames - 8.0) return 0.0;
    const auto pre = s.preloadedFrameCount();
    const auto ch = static_cast<std::size_t>(std::max<std::uint16_t>(1U, s.channels));
    auto i = static_cast<std::uint64_t>(start);
    const std::uint64_t limit = std::min<std::uint64_t>(pre, i + static_cast<std::uint64_t>(0.004 * s.sampleRate) + 1U);
    if (i + 1U < limit) {
        float prev = s.interleaved[static_cast<std::size_t>(i) * ch];
        for (std::uint64_t k = i + 1U; k < limit; ++k) {
            const float cur = s.interleaved[static_cast<std::size_t>(k) * ch];
            if ((prev <= 0.0F && cur >= 0.0F) || (prev >= 0.0F && cur <= 0.0F)) { i = k; break; }
            prev = cur;
        }
    }
    return static_cast<double>(i);
}

void startVoice(SamplerEngine::Impl& impl,
                 SamplerEngine::Impl::Voice& v,
                 const UpperPart& upper,
                 const ProgramLayer& layer,
                 const SampleZone& zone,
                 int upperIndex,
                 std::uint8_t channel,
                 std::uint8_t key,
                 std::uint8_t velocity,
                 const GlobalState& global,
                 const std::shared_ptr<const EngineState>& ownerState,
                 int soundIndex = 0,
                 bool legatoStart = false,
                 int unisonIndex = 0) noexcept {
    v = SamplerEngine::Impl::Voice{};
    v.active = true;
    v.keyHeld = true;
    v.sustained = false;
    v.age = ++impl.voiceAge;
    v.channel = channel;
    v.key = key;
    v.velocity = velocity;
    v.upperIndex = upperIndex;
    v.soundIndex = soundIndex;
    v.routeMask = impl.upperRouteMask[static_cast<std::size_t>(std::clamp(upperIndex, 0, kUpperCount - 1))];
    v.sample = zone.sample;
    v.ownerState = ownerState;
    v.layer = &layer;
    v.zone = &zone;
    v.upper = &upper;
    const UpperLayer* ul = layerOf(upper, soundIndex);
    const SoundTone tone = soundToneOf(upper, soundIndex);
    v.position = zone.reverse && zone.sample != nullptr ? static_cast<double>(zone.sample->frameCount() - 1U) : 0.0;
    v.loopWrapped = false;
    // SILVETON_TONE_V2: SAMPLE START / END
    if (!zone.reverse && zone.sample != nullptr) {
        const auto& s = *zone.sample;
        v.position = startFrameFor(s, tone.x.startMs);
        if (legatoStart && tone.x.startMs <= 0.0F) v.position = legatoStartFrame(s);   // SILVETON_XFADE_PORTA_V1
        if (s.loop.enabled && v.position >= static_cast<double>(s.loop.endFrame) && s.loop.endFrame > s.loop.startFrame) {
            v.position = static_cast<double>(s.loop.startFrame);
        }
        if (tone.x.endMs > 0.0F) {
            const double end = static_cast<double>(tone.x.endMs) * 0.001 * static_cast<double>(s.sampleRate);
            if (end > v.position + 16.0 && end < static_cast<double>(s.frameCount())) {
                v.endFrame = end;
                if (s.loop.enabled && end < static_cast<double>(s.loop.endFrame)) v.loopOff = true;
            }
        }
    }

    v.ratio = noteRatio(impl, upper, layer, zone, key, global, upperIndex, ul);
    v.targetRatio = v.ratio;
    v.pitchGliding = false;
    v.glideFramesRemaining = 0U;
    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = 0.0;
    v.glideLastStep = v.glideStartVelocity = 0.0;

    const float velocityNorm = std::max(0.0F, static_cast<float>(velocity) * kInv127);
    v.velNorm = velocityNorm;
    float velGain = velocityNorm;
    switch (global.velocityCurve) {
        case 1U: velGain = std::sqrt(velocityNorm); break;
        case 2U: velGain = velocityNorm * velocityNorm; break;
        case 3U: velGain = 1.0F; break;
        default: break;
    }
    // SILVETON_TONE_V2: VEL->LEVEL (1 = as before, 0 = every velocity the same level)
    velGain = 1.0F + (velGain - 1.0F) * std::clamp(tone.x.velToLevel, 0.0F, 1.0F);
    v.velCutoffOct = -std::clamp(tone.x.velToCutoff, 0.0F, 8.0F) * (1.0F - velocityNorm);
    v.filterEnvValue = 1.0F;
    // SILVETON_POLYSIX_V1: unison copy, sub position, PWM period, per-note random
    {
        const int n = std::clamp<int>(tone.x.unisonVoices, 1, 4);
        v.unisonIndex = unisonIndex;
        if (n > 1) {
            const float t = (static_cast<float>(unisonIndex) - 0.5F * static_cast<float>(n - 1)) / (0.5F * static_cast<float>(n - 1));   // -1..1
            v.unisonCents = t * std::clamp(tone.x.unisonDetuneCents, 0.0F, 100.0F);
            v.unisonPan = t * std::clamp(tone.x.unisonSpread, 0.0F, 1.0F);
            v.unisonGain = 1.0F / std::sqrt(static_cast<float>(n));
        }
        v.subPosition = v.position;
        v.periodFrames = 0.0;
        if (zone.sample != nullptr) {
            const double rootHz = 440.0 * std::pow(2.0, (static_cast<double>(zone.rootKey) - 69.0) / 12.0) *
                                  std::pow(2.0, -static_cast<double>(zone.tuneCents + zone.sample->fineTuneCents) / 1200.0);
            v.periodFrames = static_cast<double>(zone.sample->sampleRate) / std::max(1.0, rootHz);
        }
        impl.noiseState = impl.noiseState * 1664525U + 1013904223U;
        v.random = static_cast<float>(impl.noiseState >> 8) / 8388608.0F - 1.0F;
        if (tone.x.analogFeel) { v.unisonCents += v.random * 3.0F; }   // a few cents, like a real analog
    }
    float layerVelocityGain = 1.0F;
    if (layer.velocityFadeIn > 0U) {
        const int fadeEnd = static_cast<int>(layer.velocityLow) + static_cast<int>(layer.velocityFadeIn);
        if (static_cast<int>(velocity) < fadeEnd) {
            layerVelocityGain *= std::clamp(
                static_cast<float>(static_cast<int>(velocity) - static_cast<int>(layer.velocityLow)) / static_cast<float>(layer.velocityFadeIn),
                0.0F, 1.0F);
        }
    }
    if (layer.velocityFadeOut > 0U) {
        const int fadeStart = static_cast<int>(layer.velocityHigh) - static_cast<int>(layer.velocityFadeOut);
        if (static_cast<int>(velocity) > fadeStart) {
            layerVelocityGain *= std::clamp(
                static_cast<float>(static_cast<int>(layer.velocityHigh) - static_cast<int>(velocity)) / static_cast<float>(layer.velocityFadeOut),
                0.0F, 1.0F);
        }
    }
    v.baseGain = dbToGain(zone.gainDb + layer.levelDb) * velGain * layerVelocityGain;
    v.mixPanIn = std::numeric_limits<float>::quiet_NaN();
    updateMixTarget(v, upper, true, 0.0F);
    v.mixL = v.mixTargetL;
    v.mixR = v.mixTargetR;
    v.tuneSemis = liveTuneSemis(upper, global, key, ((impl.scaleUppers.load(std::memory_order_relaxed) >> upperIndex) & 1) != 0, ul) + static_cast<double>(v.unisonCents) / 100.0;
    v.ratio *= std::exp2(static_cast<double>(v.unisonCents) / 1200.0);
    v.targetRatio = v.ratio;
    v.filterEnv.params = layer.filterEnvelope;
    v.pitchEnv.params = layer.pitchEnvelope;
    v.ampEnv = AmpEnv{};
    const UpperToneValues tv0 = effectiveToneFor(tone, upper, layer.ampEnvelope);
    v.ampEnv.setBlock(tv0, impl.sampleRate);
    // SILVETON_TONE_V2: the note starts AT its CUTOFF (before, the filter opened
    // from 20 kHz for the first ~30 ms of every note); FILTER ENV / VEL->CUTOFF too.
    {
        const float envOct = std::clamp(tone.x.filterEnvOctaves, -8.0F, 8.0F) + v.velCutoffOct;
        const bool wantOn = tv0.cutoffHz < kToneCutoffOpenHz - 1.0F || envOct < -0.001F;
        if (wantOn) {
            v.toneOn = true;
            v.toneLog = std::log(tv0.cutoffHz);
            v.toneLp.prime = true;
            v.toneLp.set(std::clamp(std::exp(v.toneLog + envOct * 0.69314718F), 20.0F, kToneCutoffOpenHz), tv0.resonance, impl.sampleRate);
        }
    }
    v.ampEnv.trigger(); v.filterEnv.trigger(); v.pitchEnv.trigger();
    v.lfo1.reset(layer.lfo1.keySync); v.lfo2.reset(layer.lfo2.keySync);
    v.delayFramesRemaining = static_cast<std::uint32_t>(std::clamp(
        static_cast<double>(layer.delaySec) * impl.sampleRate, 0.0, static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
    v.filter.reset();
}

float calibratedController(std::uint8_t value, float minValue, float maxValue, float curve) noexcept {
    const float x = static_cast<float>(value) * kInv127;
    const float lo = std::clamp(minValue, 0.0F, 1.0F);
    const float hi = std::clamp(maxValue, lo + 0.0001F, 1.0F);
    const float normalized = std::clamp((x - lo) / (hi - lo), 0.0F, 1.0F);
    return std::pow(normalized, std::clamp(curve, 0.1F, 8.0F));
}

float routeSourceValue(const UpperControllerRoute& route, const SamplerEngine::Impl::ChannelState& ch) noexcept {
    float value = 0.0F;
    bool inherentlyBipolar = false;
    switch (route.source) {
        case ControllerSource::Off: return 0.0F;
        case ControllerSource::Breath: value = ch.breath; break;
        case ControllerSource::Expression: value = ch.expression; break;
        case ControllerSource::Aftertouch: value = ch.pressure; break;
        case ControllerSource::JoystickX:
            value = std::clamp(static_cast<float>(ch.pitchBend) / 8192.0F, -1.0F, 1.0F);
            inherentlyBipolar = true;
            break;
        case ControllerSource::JoystickY:
        case ControllerSource::ControlChange:
            value = ch.cc[static_cast<std::size_t>(route.cc & 0x7FU)];
            break;
    }
    if (route.bipolar && !inherentlyBipolar) value = value * 2.0F - 1.0F;
    return std::clamp(value, -1.0F, 1.0F);
}

float controllerAmount(const UpperPart& upper, const SamplerEngine::Impl::ChannelState& ch,
                       ModDestination destination) noexcept {
    float sum = 0.0F;
    for (const auto& route : upper.controllerRoutes) {
        if (route.source == ControllerSource::Off || route.destination != destination || !std::isfinite(route.amount)) continue;
        sum += routeSourceValue(route, ch) * route.amount;
    }
    return sum;
}

void releaseOne(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, bool sustainDown) noexcept {
    v.keyHeld = false;
    if (v.zone != nullptr && v.zone->oneShot) return;
    if (sustainDown && v.upper != nullptr && v.upper->damperEnabled && v.layer != nullptr && v.layer->damperEnabled) {
        v.sustained = true;
    } else {
        v.ampEnv.release(impl.sampleRate);
        v.filterEnv.release(impl.sampleRate);
        v.pitchEnv.release(impl.sampleRate);
    }
}

// handledUppers: MONO/LEGATO Uppers whose key stack already took care of this key.
void releaseMatching(SamplerEngine::Impl& impl, std::uint8_t channel, std::uint8_t key, std::uint16_t handledUppers) noexcept {
    const bool sustain = impl.channels[channel].sustain;
    for (auto& v : impl.voices) {
        if (!v.active || v.channel != channel) continue;
        if (v.key != key) continue;
        if (v.upperIndex >= 0 && v.upperIndex < kUpperCount &&
            ((handledUppers >> static_cast<unsigned>(v.upperIndex)) & 1U) != 0U) continue;
        releaseOne(impl, v, sustain);
    }
}

void releaseUpperVoices(SamplerEngine::Impl& impl, int ui, std::uint8_t channel) noexcept {
    const bool sustain = impl.channels[channel].sustain;
    for (auto& v : impl.voices) {
        if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld) releaseOne(impl, v, sustain);
    }
}

void releaseSustained(SamplerEngine::Impl& impl, std::uint8_t channel) noexcept {
    for (auto& v : impl.voices) {
        if (v.active && v.channel == channel && v.sustained && !v.keyHeld) {
            v.sustained = false;
            v.ampEnv.release(impl.sampleRate);
            v.filterEnv.release(impl.sampleRate);
            v.pitchEnv.release(impl.sampleRate);
        }
    }
}

void panic(SamplerEngine::Impl& impl) noexcept {
    for (auto& v : impl.voices) {
        v.ampEnv.forceOff(); v.filterEnv.forceOff(); v.pitchEnv.forceOff(); v.active = false;
    }
    for (auto& c : impl.channels) c = SamplerEngine::Impl::ChannelState{};
    for (auto& m : impl.mono) m = SamplerEngine::Impl::MonoKeys{};
}

constexpr float kLegatoCrossfadeSec = 0.030F; // equal-power, only between two different recordings

void monoRemoveAt(SamplerEngine::Impl::MonoKeys& m, std::size_t index) noexcept {
    for (std::size_t i = index + 1U; i < m.count; ++i) { m.key[i - 1U] = m.key[i]; m.vel[i - 1U] = m.vel[i]; }
    if (m.count > 0U) --m.count;
}

void monoPush(SamplerEngine::Impl::MonoKeys& m, std::uint8_t key, std::uint8_t vel) noexcept {
    for (std::size_t i = 0; i < m.count; ++i) if (m.key[i] == key) { monoRemoveAt(m, i); break; }
    if (m.count >= m.key.size()) monoRemoveAt(m, 0U);
    m.key[m.count] = key; m.vel[m.count] = vel; ++m.count;
}

// SILVETON_PORTA_PAGE_V1 - PORTAMENTO exactly like the SILVETON portamento page
// (Silveton-Portamento-Keys): while portamento is ON an Upper plays one line
// (last key wins, letting it go returns to a key that is still held).
//  * While a key is held the line is ONE continuous sound: a new key, or going
//    back to a held key, is not struck again - the pitch glides from the pitch
//    sounding right now (also part-way through a glide) to the new key in
//    exactly TIME (1..100 ms), evenly in semitones, the same time for every interval.
//  * With no key held the next note is struck normally and does not glide.
//  * The note that already sounds, pressed again, changes nothing.
// (PORTA BALANCE now rounds every glide: see SILVETON_SOFT_GLIDE_V1.)
constexpr float kPortamentoMaxSec = 0.100F;

// SILVETON_PORTA_MODES_V1: KRONOS portamento TIME 0..127 -> seconds (0 = off,
// 127 = 5 s; the lower half of the range is fine, like the instrument).
float kronosPortaSec(std::uint8_t time) noexcept {
    const float t = std::clamp(static_cast<float>(time), 0.0F, 127.0F) / 127.0F;
    return 5.0F * std::pow(t, 2.2F);
}

float portamentoSecFor(const UpperPart& upper) noexcept {
    if (upper.portaMode == PortamentoMode::Kronos) return kronosPortaSec(upper.portaTime);
    return std::clamp(upper.portamentoSec, 0.0F, kPortamentoMaxSec);
}

// SILVETON_POLY_PORTA_V1: POLY stays POLY with portamento ON (chords keep sounding,
// every new key glides from the key played before it, like Korg Pa).
UpperVoiceMode effectiveVoiceMode(const UpperPart& upper) noexcept {
    return upper.voiceMode;
}

// SILVETON_PORTA_NEAR_V1: far intervals glide in TIME (as before); neighbouring
// keys take longer and softer: 1 semitone = NEAR, 2 = 2/3, 3 = 1/3 of the way.
float glideSecFor(const SamplerEngine::Impl& impl, const UpperPart& upper, double fromKey, double toKey, int ui = -1) noexcept {
    const float base = portamentoSecFor(upper);
    if (base <= 0.0F || fromKey < 0.0) return base;
    // SILVETON_PORTA_SAME_V1: the glide is the same at any playing speed - exactly what
    // the PORTA page says, slow or fast (the earlier tempo shortening is gone).
    (void)ui;
    const auto capped = [](float sec) noexcept { return sec; };
    const double d = std::abs(toKey - fromKey);
    if (upper.portaMode == PortamentoMode::Kronos) {
        // TIME: one duration for every interval. RATE: duration per octave.
        // A small, bounded near-note floor avoids almost instantaneous jumps
        // when using RATE with one or two semitones. Never extend a very short
        // user TIME setting beyond that setting, and never affect TIME mode.
        if (!upper.portaRateMode) return capped(base);
        const float scaled = base * static_cast<float>(std::max(d, 0.05) / 12.0);
        return capped(std::max(scaled, std::min(base, 0.055F)));
    }
    // SILVETON_PROCESSOR_PORTA_V1: the glide of the SILVETON keyboard processor on the Korg.
    // The processor picks the Korg's rate (CC#5) for every interval so that it lasts TIME;
    // PORTA BALANCE (11 = the player's keyboard) decides how the real time grows with the
    // interval: t(I) = TIME x I^((11 - BALANCE) / 11), bounded. At 11 every interval takes
    // TIME; below 11 a big jump takes longer than a step, above 11 a big jump is quicker.
    const double bal = static_cast<double>(impl.portaBalance.load(std::memory_order_relaxed));
    const double expo = std::clamp((11.0 - bal) / 11.0, -1.0, 1.0);
    const float baseB = std::clamp(static_cast<float>(static_cast<double>(base) * std::pow(std::max(1.0, d), expo)), 0.001F, 10.0F);
    const double w = d <= 1.0 ? 1.0 : (d >= 4.0 ? 0.0 : (4.0 - d) / 3.0);
    const float nearSec = impl.portaNearMs.load(std::memory_order_relaxed) / 1000.0F;
    return capped(nearSec > baseB ? baseB + static_cast<float>(w) * (nearSec - baseB) : baseB);
}

// Mode at this event; a key memory never survives a switch between POLY and MONO.
UpperVoiceMode voiceModeNow(SamplerEngine::Impl& impl, int ui, const UpperPart& upper) noexcept {
    const UpperVoiceMode mode = effectiveVoiceMode(upper);
    const bool mono = mode != UpperVoiceMode::Poly;
    auto& was = impl.monoWas[static_cast<std::size_t>(ui)];
    if (mono != was) {
        impl.mono[static_cast<std::size_t>(ui)] = SamplerEngine::Impl::MonoKeys{};
        was = mono;
    }
    return mode;
}

// SILVETON_SOFT_GLIDE_V1 / SILVETON_SILK_PORTA_V1
// The pitch starts at exactly the audible previous pitch, then follows a C2
// eased semitone-space path and reaches its target ON TIME (no lagging low-pass
// tail). When a key interrupts a moving glide, carry its velocity into the new
// path, bounded to keep the result monotonic / free of pitch overshoot.
void beginGlide(SamplerEngine::Impl::Voice& v, double newTargetRatio, std::uint32_t frames) noexcept {
    if (!(newTargetRatio > 0.0) || !std::isfinite(newTargetRatio)) return;
    const double start = v.ratio > 0.0 && std::isfinite(v.ratio)
        ? 12.0 * std::log2(v.ratio / newTargetRatio) : 0.0;
    const auto total = std::max<std::uint32_t>(1U, frames);
    // A reversal of direction brakes at the actual sounding pitch. Otherwise
    // preserve a limited amount of momentum, avoiding the characteristic corner
    // during very fast legato runs.
    const double carried = v.pitchGliding ? v.glideLastStep * static_cast<double>(total) : 0.0;
    const double desiredDirection = -start;
    // KRONOS changes keep the audible slope but never let a fast interruption
    // overshoot the target. SILVETON retains its existing wider momentum bound.
    const double maxCarry = (v.glideKronos ? 0.45 : 2.0) * std::abs(start);
    const double velocity = (carried * desiredDirection > 0.0)
        ? std::copysign(std::min(std::abs(carried), maxCarry), desiredDirection) : 0.0;
    v.glideStartVelocity = velocity;
    v.glideStartSemis = start;
    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = start;
    v.glideTotalFrames = total;
    v.glideFramesRemaining = total;
    v.targetRatio = newTargetRatio;
    v.pitchGliding = true;
    // Do not recalculate ratio here: preserve exactly the currently audible
    // floating-point value until the next generated sample.
}

void startPortamentoGlide(const SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, double fromKey, float seconds) noexcept {
    if (seconds <= 0.0F || fromKey < 0.0 || v.ratio <= 0.0) return;
    if (v.zone != nullptr && v.zone->nonTranspose) return; // fixed-pitch sample: nothing to glide
    const double semitones = fromKey - static_cast<double>(v.key);
    if (std::abs(semitones) < 1.0e-6) return;
    v.targetRatio = v.ratio;
    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = semitones;   // starts on the old key
    v.glideLastStep = 0.0; // a newly struck voice has no inherited motion
    v.pitchGliding = true;
    v.ratio = v.targetRatio * std::exp2(semitones / 12.0);
    const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(seconds) * impl.sampleRate));
    beginGlide(v, v.targetRatio, frames);
}

// SILVETON_PHASE_ALIGN_V1: one mono frame of a recording (RAM prefix or streamed part).
float monoFrameAt(const SampleData& s, std::uint64_t k) noexcept {
    const auto ch = static_cast<std::size_t>(std::max<std::uint16_t>(1U, s.channels));
    if (k < s.preloadedFrameCount()) {
        const float* p = &s.interleaved[static_cast<std::size_t>(k) * ch];
        return ch >= 2U ? 0.5F * (p[0] + p[1]) : p[0];
    }
    if (s.stream != nullptr && k < s.stream->frameCount()) {
        return ch >= 2U ? 0.5F * (s.stream->readFrame(k, 0U) + s.stream->readFrame(k, 1U)) : s.stream->readFrame(k, 0U);
    }
    return 0.0F;
}

// SILVETON_PHASE_ALIGN_V1: when another recording takes over a held line, its start is
// moved by up to one period so its waveform lines up with the recording still sounding
// (both play the same pitch during the blend). Without this the two can sit in opposite
// phase and eat each other for a moment - a hollow dip in the middle of every change of
// recording. ~150 sample reads and 2300 multiplies, once per takeover.
void alignLegatoPhase(SamplerEngine::Impl::Voice& v, const SamplerEngine::Impl::Voice& old, double sampleRate) noexcept {
    if (v.sample == nullptr || old.sample == nullptr || v.ratio <= 0.0 || old.ratio <= 0.0 || v.periodFrames <= 0.0) return;
    // period of the sounding pitch, in frames of each recording: both read their own
    // recording at `ratio` frames per output frame and the output period is the same
    const double outPeriod = v.periodFrames / v.ratio;                 // output frames per period (new voice at its current pitch)
    if (!(outPeriod > 8.0) || !(outPeriod < 0.1 * sampleRate)) return;  // 10 Hz .. ~12 kHz: outside = not a pitched line
    const double Pn = outPeriod * v.ratio;
    const double Po = outPeriod * old.ratio;
    constexpr int M = 48;
    const double limitNew = static_cast<double>((v.sample->loop.enabled && v.sample->loop.endFrame > 0U) ? std::min<std::uint64_t>(v.sample->loop.endFrame, v.sample->frameCount()) : v.sample->frameCount());
    const double limitOld = static_cast<double>(old.sample->frameCount());
    if (v.position + 2.0 * Pn + 2.0 >= limitNew || old.position + Po + 2.0 >= limitOld || old.position < 0.0) return;
    float o[M]; float n[2 * M];
    for (int k = 0; k < M; ++k) o[k] = monoFrameAt(*old.sample, static_cast<std::uint64_t>(old.position + static_cast<double>(k) * Po / M));
    for (int k = 0; k < 2 * M; ++k) n[k] = monoFrameAt(*v.sample, static_cast<std::uint64_t>(v.position + static_cast<double>(k) * Pn / M));
    double best = -1.0e30; int bestJ = 0;
    for (int j = 0; j < M; ++j) {
        double c = 0.0;
        for (int k = 0; k < M; ++k) c += static_cast<double>(o[k]) * static_cast<double>(n[j + k]);
        if (c > best) { best = c; bestJ = j; }
    }
    v.position += static_cast<double>(bestJ) * Pn / M;
    v.subPosition = v.position;
}

// SILVETON_LAYERS_V1: every recording this key plays on this Upper - the basic
// sound (soundIndex 0) and every layer that is on and covers the key.
template <typename Fn>
void forEachSoundZone(const UpperPart& upper, const GlobalState& global, std::uint8_t key, std::uint8_t velocity, Fn&& fn) noexcept {
    for (int s = 0; s < kSoundsPerUpper; ++s) {
        const UpperLayer* ul = layerOf(upper, s);
        const Program* prog = soundProgramOf(upper, s);
        if (prog == nullptr) continue;
        if (ul != nullptr && !ul->matches(key, velocity)) continue;
        const std::uint8_t pk = playKeyFor(upper, global, key, ul);
        for (const auto& layer : prog->layers) {
            if (!layer.matches(pk, velocity) || layer.multisample == nullptr) continue;
            for (const auto& zone : layer.multisample->zones) {
                if (!zone.matches(pk, velocity) || !zone.sample->valid()) continue;
                // SILVETON_POLYSIX_V1: UNISON = the same recording 1..4 times, detuned and spread
                const SoundTone st = soundToneOf(upper, s);
                const int n = std::clamp<int>(st.x.unisonVoices, 1, 4);
                for (int u = 0; u < n; ++u) fn(s, layer, zone, u);
            }
        }
    }
}

// Same sound keeps playing; only its pitch moves to the new key (true glissando).
void legatoTo(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, const UpperPart& upper,
              const ProgramLayer& layer, const SampleZone& zone, std::uint8_t key, const GlobalState& global,
              float glideSec) noexcept {
    const UpperLayer* ul = layerOf(upper, v.soundIndex);
    const double target = noteRatio(impl, upper, layer, zone, key, global, v.upperIndex, ul);
    v.key = key;
    v.tuneSemis = liveTuneSemis(upper, global, key, ((impl.scaleUppers.load(std::memory_order_relaxed) >> v.upperIndex) & 1) != 0, ul);
    const float portamentoSec = glideSec;
    if (portamentoSec > 0.0F && v.ratio > 0.0 && target > 0.0) {
        const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(portamentoSec) * impl.sampleRate));
        beginGlide(v, target, frames);
    } else {
        v.targetRatio = target;
        v.ratio = target;
        v.glideFramesRemaining = 0U;
        v.glideLastStep = v.glideStartVelocity = 0.0;
        v.pitchGliding = false;
    }
}

void playUpperNote(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, int ui,
                   const UpperPart& upper, std::uint8_t channel, std::uint8_t key, std::uint8_t velocity,
                   double glideFromKey = -1.0, float glideSec = 0.0F) noexcept {
    const auto& global = stateOwner->global;
    const UpperVoiceMode mode = effectiveVoiceMode(upper);
    const bool upperMono = mode != UpperVoiceMode::Poly;
    const bool upperLegato = mode == UpperVoiceMode::Legato;
    bool hadHeldVoice = false;
    float previousAmp = 0.0F;
    // MONO / LEGATO portamento is played by playPortamentoNote(); POLY portamento
    // passes glideFromKey / glideSec (SILVETON_POLY_PORTA_V1).
    if (upperMono) {
        for (auto& existing : impl.voices) {
            if (!existing.active || existing.fading || existing.upperIndex != ui) continue;
            if (existing.keyHeld && existing.delayFramesRemaining == 0U) hadHeldVoice = true;
            previousAmp = std::max(previousAmp, existing.ampEnv.value);
            existing.legatoKeep = false;
        }
        if (upperLegato && hadHeldVoice) {
            // 1) a still-held voice on the same recording just changes pitch
            forEachSoundZone(upper, global, key, velocity, [&](int s, const ProgramLayer& layer, const SampleZone& zone, int un) noexcept {
                for (auto& existing : impl.voices) {
                    if (existing.active && !existing.fading && existing.upperIndex == ui && existing.keyHeld &&
                        existing.delayFramesRemaining == 0U && !existing.legatoKeep && existing.soundIndex == s && existing.unisonIndex == un &&
                        existing.layer == &layer && existing.zone == &zone) {
                        legatoTo(impl, existing, upper, layer, zone, key, global, 0.0F);
                        existing.legatoKeep = true;
                        break;
                    }
                }
            });
        }
        // 2) every other sounding voice of this Upper leaves with a short fade.
        //    Automatic MONO of a POLY Upper (portamento ON) works like the
        //    processor: the new note first, then a normal Note Off for the old one,
        //    so the old note ends with its own release (the damper pedal holds it).
        const bool autoMono = upper.voiceMode == UpperVoiceMode::Poly;
        const bool sustainDown = impl.channels[channel & 0x0FU].sustain;
        for (auto& existing : impl.voices) {
            if (existing.active && !existing.fading && existing.upperIndex == ui && !existing.legatoKeep) {
                if (autoMono) {
                    if (existing.ampEnv.stage != AmpEnv::Stage::Release && !existing.sustained) releaseOne(impl, existing, sustainDown);
                } else {
                    softKill(impl, existing, (upperLegato && hadHeldVoice) ? kLegatoCrossfadeSec : kTakeoverFadeSec);
                }
            }
        }
    }
    const bool kronos = upper.portaMode == PortamentoMode::Kronos;
    forEachSoundZone(upper, global, key, velocity, [&](int s, const ProgramLayer& layer, const SampleZone& zone, int un) noexcept {
        if (upperLegato && hadHeldVoice) {
            for (const auto& existing : impl.voices) {
                if (existing.active && existing.legatoKeep && existing.upperIndex == ui && existing.soundIndex == s && existing.unisonIndex == un &&
                    existing.layer == &layer && existing.zone == &zone) return;
            }
        }
        auto* v = chooseVoice(impl);
        startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner, s, upperLegato && hadHeldVoice, un);
        v->mono = upperMono;
        v->glideKronos = kronos;
        startPortamentoGlide(impl, *v, glideFromKey, glideSec);
        if (upperLegato && hadHeldVoice) {
            v->ampEnv.value = previousAmp;
            v->ampEnv.stage = AmpEnv::Stage::Decay;
            v->fadeGain = 0.0F;
            v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kLegatoCrossfadeSec) * impl.sampleRate));
        }
    });
    if (upperMono) for (auto& existing : impl.voices) if (existing.upperIndex == ui) existing.legatoKeep = false;
}

// SILVETON_PORTA_PAGE_V1: one note of an Upper whose portamento is ON.
// keyWasHeld = another key of this Upper was held before this note;
// previousKey = that key (used only if its recording already ended).
void playPortamentoNote(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, int ui,
                        const UpperPart& upper, std::uint8_t channel, std::uint8_t key, std::uint8_t velocity,
                        bool keyWasHeld, int previousKey) noexcept {
    if (!upper.hasAnySound()) return;
    const auto& global = stateOwner->global;
    float glideSec = portamentoSecFor(upper);
    SamplerEngine::Impl::Voice* sounding = nullptr;   // the voice of the held line
    for (auto& v : impl.voices) {
        if (!v.active || v.fading || v.upperIndex != ui || !v.keyHeld || v.delayFramesRemaining > 0U) continue;
        if (sounding == nullptr || v.age > sounding->age) sounding = &v;
    }
    const bool kronos = upper.portaMode == PortamentoMode::Kronos;
    if (!keyWasHeld || sounding == nullptr) {
        // Nothing held: struck normally, no glide (a held line whose recording
        // already ended starts again and glides from the previous key).
        // KRONOS (not Fingered): glides from the last key even when nothing is held.
        double fromKey = (keyWasHeld && previousKey >= 0) ? static_cast<double>(previousKey) : -1.0;
        if (kronos && !upper.portaFingered && fromKey < 0.0) fromKey = static_cast<double>(impl.polyLastKey[static_cast<std::size_t>(ui)]);
        glideSec = glideSecFor(impl, upper, fromKey, static_cast<double>(key), ui);
        forEachSoundZone(upper, global, key, velocity, [&](int s, const ProgramLayer& layer, const SampleZone& zone, int un) noexcept {
            auto* v = chooseVoice(impl);
            startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner, s, false, un);
            v->mono = true;
            v->glideKronos = kronos;
            startPortamentoGlide(impl, *v, fromKey, glideSec);
        });
        impl.polyLastKey[static_cast<std::size_t>(ui)] = key;
        return;
    }
    // A key is held: one continuous sound glides to the new key in exactly TIME.
    const double fromPitch = static_cast<double>(sounding->key) + 12.0 * std::log2(sounding->ratio / sounding->targetRatio);
    glideSec = glideSecFor(impl, upper, fromPitch, static_cast<double>(key), ui);
    if (!kronos) {
        // SILVETON_PROCESSOR_PORTA_V1: like the Korg under the processor - the rate is the one
        // for the interval between the two KEYS; interrupted mid-glide, the rest of the way
        // is covered at that rate (so it arrives sooner, never slower).
        const double keyInterval = std::abs(static_cast<double>(key) - static_cast<double>(sounding->key));
        const double pitchInterval = std::abs(static_cast<double>(key) - fromPitch);
        if (keyInterval >= 0.5 && pitchInterval > 1.0e-6) {
            const float keyTime = glideSecFor(impl, upper, static_cast<double>(sounding->key), static_cast<double>(key), ui);
            glideSec = std::clamp(static_cast<float>(keyTime * pitchInterval / keyInterval), 0.001F, 10.0F);
        }
    }
    const float previousAmp = sounding->ampEnv.value;
    // SILVETON_XFADE_PORTA_V1 - crossfade portamento: when the new key needs
    // another recording, both recordings play the SAME pitch glide - the old one
    // fading out, the new one (started past its attack) fading in - over the
    // whole glide. No stretched single sample, no re-struck attack, no jump of
    // timbre at the arrival.
    // SILVETON_PORTA_SOFT_FAST_V1: the blend between the two recordings lasts the whole
    // glide (up to 300 ms) at any playing speed, and every tail still fading from an
    // earlier note glides along to the newest key, so nothing lags behind in pitch.
    const float xfadeSec = std::clamp(glideSec, kLegatoCrossfadeSec, 0.300F);
    for (auto& v : impl.voices) if (v.upperIndex == ui) v.legatoKeep = false;
    // First every held voice of the line gets its new glide (same rate, same arrival), so a
    // recording that takes over below copies the glide as it is NOW - not the one it was
    // interrupting (that left the two recordings out of tune for a moment).
    for (auto& v : impl.voices) {
        if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && v.delayFramesRemaining == 0U && v.layer != nullptr && v.zone != nullptr) {
            v.glideKronos = kronos; legatoTo(impl, v, upper, *v.layer, *v.zone, key, global, glideSec);
        }
    }
    forEachSoundZone(upper, global, key, velocity, [&](int s, const ProgramLayer& layer, const SampleZone& zone, int un) noexcept {
        SamplerEngine::Impl::Voice* keep = nullptr;
        for (auto& v : impl.voices) {
            if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && v.delayFramesRemaining == 0U &&
                !v.legatoKeep && v.soundIndex == s && v.unisonIndex == un && v.layer == &layer && v.zone == &zone) { keep = &v; break; }
        }
        if (keep != nullptr) {
            // the same recording keeps sounding and only moves its pitch
            keep->glideKronos = kronos;
            legatoTo(impl, *keep, upper, layer, zone, key, global, glideSec);
            keep->legatoKeep = true;
            return;
        }
        // another recording takes over without a new strike and glides from the pitch that sounded
        auto* v = chooseVoice(impl);
        startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner, s, true, un);
        v->mono = true;
        v->glideKronos = kronos;
        startPortamentoGlide(impl, *v, fromPitch, glideSec);
        if (v != sounding && v->pitchGliding && sounding->pitchGliding) {
            // SILVETON_PHASE_ALIGN_V1: the new recording takes over the exact glide of the old one
            // (same momentum, same curve position) so the two stay in tune - and in phase - all the way
            v->glideStartVelocity = sounding->glideStartVelocity; v->glideStartSemis = sounding->glideStartSemis;
            v->glideRawSemis = sounding->glideRawSemis; v->glideSmooth1 = sounding->glideSmooth1; v->glideSmooth2 = sounding->glideSmooth2;
            v->glideLastStep = sounding->glideLastStep; v->glideTotalFrames = sounding->glideTotalFrames; v->glideFramesRemaining = sounding->glideFramesRemaining;
            v->ratio = v->targetRatio * std::exp2(12.0 * std::log2(sounding->ratio / sounding->targetRatio) / 12.0);
        }
        if (v != sounding) alignLegatoPhase(*v, *sounding, impl.sampleRate);   // SILVETON_PHASE_ALIGN_V1
        v->ampEnv.value = previousAmp;
        v->ampEnv.stage = AmpEnv::Stage::Decay;
        v->fadeGain = 0.0F;
        v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(xfadeSec) * impl.sampleRate));
        v->fadeBlend = true;
        v->legatoKeep = true;
    });
    impl.polyLastKey[static_cast<std::size_t>(ui)] = key;
    for (auto& v : impl.voices) {
        if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && !v.legatoKeep) {
            // the old recording glides along to the new key while it fades out
            if (v.layer != nullptr && v.zone != nullptr) { v.glideKronos = kronos; legatoTo(impl, v, upper, *v.layer, *v.zone, key, global, glideSec); }
            softKill(impl, v, xfadeSec);
            v.fadeBlend = true;
        } else if (v.active && v.fading && v.fadeBlend && v.upperIndex == ui && !v.legatoKeep && v.layer != nullptr && v.zone != nullptr) {
            // SILVETON_PORTA_SOFT_FAST_V1: a tail from an earlier note of this line follows the newest key too
            v.glideKronos = kronos; legatoTo(impl, v, upper, *v.layer, *v.zone, key, global, glideSec);
        }
    }
    for (auto& v : impl.voices) if (v.upperIndex == ui) v.legatoKeep = false;
}

// ---------------------------------------------------------------- SILVETON_BOW_V1 note handling
constexpr double kBowSlurSec = 0.03;        // legato without portamento: a quick finger shift

int bowInstrumentOf(const UpperPart& u, int soundIndex) noexcept {
    const Program* p = soundProgramOf(u, soundIndex);
    return p != nullptr ? static_cast<int>(p->bowInstrument) : 0;
}

bow::Controls bowControlsFor(const ToneExtra& x) noexcept {
    bow::Controls c;
    c.pressure = std::clamp(static_cast<double>(x.bowPressure), 0.0, 1.0);
    c.bowPosition = std::clamp(static_cast<double>(x.bowPosition), 0.05, 0.18);
    c.brightness = std::clamp(static_cast<double>(x.bowBrightness), 0.0, 1.0);
    c.vibratoCents = std::clamp(static_cast<double>(x.bowVibratoCents), 0.0, 100.0);
    c.vibratoHz = std::clamp(static_cast<double>(x.bowVibratoHz), 3.0, 9.0);
    c.vibratoDelaySec = std::clamp(static_cast<double>(x.bowVibratoDelayMs), 0.0, 2000.0) * 0.001;
    c.attackSec = std::clamp(static_cast<double>(x.bowAttackMs), 5.0, 200.0) * 0.001;
    c.releaseSec = std::clamp(static_cast<double>(x.bowReleaseMs), 10.0, 500.0) * 0.001;
    return c;
}

// The note (fractional MIDI) this key plays on the bowed sound: OCTAVE / TRANSPOSE / FINE / MASTER TUNE / SCALE.
double bowNoteFor(const SamplerEngine::Impl& impl, const UpperPart& upper, const GlobalState& global, int ui, std::uint8_t key, const UpperLayer* ul) noexcept {
    const bool scaleOn = ((impl.scaleUppers.load(std::memory_order_relaxed) >> ui) & 1) != 0;
    return static_cast<double>(playKeyFor(upper, global, key, ul)) + liveTuneSemis(upper, global, key, scaleOn, ul);
}

// PORTA page of the Upper decides the glide of a legato note; without portamento a quick slur.
double bowGlideSec(const SamplerEngine::Impl& impl, const UpperPart& upper, double fromNote, double toNote, int ui) noexcept {
    if (portamentoSecFor(upper) <= 0.0F) return kBowSlurSec;
    return std::max(static_cast<double>(glideSecFor(impl, upper, fromNote, toNote, ui)), 0.005);
}

void bowNoteOn(SamplerEngine::Impl& impl, const EngineState& state, int ui, const UpperPart& upper,
               std::uint8_t channel, std::uint8_t key, std::uint8_t vel) noexcept {
    for (int s = 0; s < kSoundsPerUpper; ++s) {
        const int inst = bowInstrumentOf(upper, s);
        if (inst == 0) continue;
        const UpperLayer* ul = layerOf(upper, s);
        if (ul != nullptr && !ul->matches(key, vel)) continue;
        auto& b = impl.bows[static_cast<std::size_t>(ui)][static_cast<std::size_t>(s)];
        if (!b.active) { b.string.reset(); b.count = 0U; b.sustained = false; }
        if (b.string.instrument() != inst) b.string.setInstrument(inst);
        const double note = bowNoteFor(impl, upper, state.global, ui, key, ul);
        const bool legato = b.active && b.string.playing() && (b.count > 0U || b.sustained);
        // held-key stack (no duplicates, newest on top)
        for (std::size_t i = 0; i < b.count; ++i) if (b.keys[i] == key) { for (std::size_t j = i + 1U; j < b.count; ++j) { b.keys[j - 1U] = b.keys[j]; b.vels[j - 1U] = b.vels[j]; } --b.count; break; }
        if (b.count >= b.keys.size()) { for (std::size_t j = 1U; j < b.count; ++j) { b.keys[j - 1U] = b.keys[j]; b.vels[j - 1U] = b.vels[j]; } --b.count; }
        b.keys[b.count] = key; b.vels[b.count] = vel; ++b.count;
        b.channel = channel;
        b.velNorm = static_cast<float>(vel) * kInv127;
        bow::Controls c = bowControlsFor(ul != nullptr ? ul->tone.x : upper.toneExtra);
        double fromNote = -1.0;
        if (legato) {
            c.glideSec = bowGlideSec(impl, upper, b.string.currentNote(), note, ui);
        } else if (portamentoSecFor(upper) > 0.0F && b.lastNote >= 0.0 &&
                   (upper.portaMode == PortamentoMode::Silveton || !upper.portaFingered)) {
            // a new stroke with portamento ON glides from the key played before (as the processor does)
            fromNote = b.lastNote;
            c.glideSec = bowGlideSec(impl, upper, fromNote, note, ui);
        } else {
            c.glideSec = 0.0;
        }
        b.string.setControls(c);
        b.string.noteOn(note, legato, fromNote);
        b.lastNote = note;
        b.active = true;
        b.sustained = false;
        b.quietFrames = 0U;
    }
}

void bowNoteOff(SamplerEngine::Impl& impl, const EngineState& state, int ui, const UpperPart& upper,
                std::uint8_t channel, std::uint8_t key, bool sustainDown) noexcept {
    for (int s = 0; s < kSoundsPerUpper; ++s) {
        auto& b = impl.bows[static_cast<std::size_t>(ui)][static_cast<std::size_t>(s)];
        if (!b.active || b.channel != channel) continue;
        std::size_t index = b.count;
        for (std::size_t i = 0; i < b.count; ++i) if (b.keys[i] == key) { index = i; break; }
        if (index >= b.count) continue;
        const bool wasTop = index + 1U == b.count;
        for (std::size_t j = index + 1U; j < b.count; ++j) { b.keys[j - 1U] = b.keys[j]; b.vels[j - 1U] = b.vels[j]; }
        --b.count;
        if (!wasTop) continue;   // a lower held key was let go: the sound goes on
        if (b.count > 0U) {
            // back to the key that is still held, without a new stroke
            const UpperLayer* ul = layerOf(upper, s);
            const double note = bowNoteFor(impl, upper, state.global, ui, b.keys[b.count - 1U], ul);
            bow::Controls c = b.string.controls();
            c.glideSec = bowGlideSec(impl, upper, b.string.currentNote(), note, ui);
            b.string.setControls(c);
            b.string.noteOn(note, true);
            b.lastNote = note;
        } else if (sustainDown && upper.damperEnabled) {
            b.sustained = true;      // the pedal keeps the bow on the string
        } else {
            b.string.noteOff();
        }
    }
}

// The damper pedal went up: bows held only by the pedal lift.
void bowReleaseSustained(SamplerEngine::Impl& impl, std::uint8_t channel) noexcept {
    for (auto& row : impl.bows) for (auto& b : row) {
        if (b.active && b.sustained && b.channel == channel && b.count == 0U) { b.sustained = false; b.string.noteOff(); }
    }
}

// All notes off / panic: the strings stop at once.
void bowAllOff(SamplerEngine::Impl& impl, int channel) noexcept {
    for (auto& row : impl.bows) for (auto& b : row) {
        if (!b.active || (channel >= 0 && b.channel != static_cast<std::uint8_t>(channel))) continue;
        b.string.reset(); b.count = 0U; b.active = false; b.sustained = false; b.mixL = b.mixR = 0.0F;
    }
}

void handleNoteOff(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner,
                   std::uint8_t channel, std::uint8_t key) noexcept {
    std::uint16_t handled = 0U;
    // SILVETON_TERCA_V1: the TERCA Upper releases the third that this key made.
    const int tmask = impl.tercaApplied >= 0 ? (impl.tercaApplied & 0x7F) : 0;
    int harmony = -1;
    bool harmonyEnds = false;
    if (tmask != 0) {
        auto& slot = impl.tercaKey[channel & 0x0FU][key & 0x7FU];
        harmony = slot;
        slot = -1;
        if (harmony >= 0) {
            auto& refs = impl.tercaRefs[channel & 0x0FU][static_cast<std::size_t>(harmony)];
            if (refs > 0U) --refs;
            harmonyEnds = refs == 0U;
        }
    }
    for (int ui = 0; ui < kUpperCount; ++ui) {
        const auto& upper = stateOwner->combi.uppers[static_cast<std::size_t>(ui)];
        if (upper.midiChannel != (channel & 0x0FU)) continue;
        // SILVETON_BOW_V1: the bowed sounds of this Upper (a T Upper lets its third go when it ends)
        if (((tmask >> ui) & 1) == 0) bowNoteOff(impl, *stateOwner, ui, upper, channel, key, impl.channels[channel & 0x0FU].sustain);
        else if (harmony >= 0 && harmonyEnds) bowNoteOff(impl, *stateOwner, ui, upper, channel, static_cast<std::uint8_t>(harmony), impl.channels[channel & 0x0FU].sustain);
        if (voiceModeNow(impl, ui, upper) == UpperVoiceMode::Poly) continue;
        std::uint8_t keyHere = key;
        if (((tmask >> ui) & 1) != 0) {
            if (harmony < 0 || !harmonyEnds) continue;
            keyHere = static_cast<std::uint8_t>(harmony);
        }
        auto& mk = impl.mono[static_cast<std::size_t>(ui)];
        std::size_t index = mk.count;
        for (std::size_t i = 0; i < mk.count; ++i) if (mk.key[i] == keyHere) { index = i; break; }
        if (index >= mk.count) continue;
        handled = static_cast<std::uint16_t>(handled | static_cast<std::uint16_t>(1U << static_cast<unsigned>(ui)));
        const bool wasTop = index + 1U == mk.count;
        monoRemoveAt(mk, index);
        if (!wasTop) continue; // a lower held key was let go: the sound goes on
        if (mk.count > 0U && upper.hasAnySound()) {
            // back to the key that is still held
            if (portamentoSecFor(upper) > 0.0F) {
                playPortamentoNote(impl, stateOwner, ui, upper, channel, mk.key[mk.count - 1U], mk.vel[mk.count - 1U], true, static_cast<int>(keyHere));
            } else {
                playUpperNote(impl, stateOwner, ui, upper, channel, mk.key[mk.count - 1U], mk.vel[mk.count - 1U]);
            }
        } else {
            releaseUpperVoices(impl, ui, channel);
        }
    }
    if (tmask != 0) {
        const auto tBits = static_cast<std::uint16_t>(tmask);
        if (harmony >= 0 && harmonyEnds) {
            // only the TERCA Uppers not already handled as MONO above
            releaseMatching(impl, channel, static_cast<std::uint8_t>(harmony),
                            static_cast<std::uint16_t>((0x7FU & ~static_cast<unsigned>(tBits)) | handled));
        }
        handled = static_cast<std::uint16_t>(handled | tBits);
    }
    releaseMatching(impl, channel, key, handled);
}

void syncTerca(SamplerEngine::Impl& impl) noexcept {
    const int cfg = impl.tercaConfig.load(std::memory_order_acquire);
    if (cfg == impl.tercaApplied) return;
    // TERCA changed (on/off, other Upper or tonality): the old TERCA Upper lets
    // its thirds go, so no third can hang.
    const int oldMask = impl.tercaApplied >= 0 ? (impl.tercaApplied & 0x7F) : 0;
    const int newMask = cfg >= 0 ? (cfg & 0x7F) : 0;
    // SILVETON_TQ_TRANSPOSE_V1: also the Upper that just became T (its held keys
    // would otherwise never get their note-off).
    const int touched = oldMask | newMask;
    for (int ui = 0; ui < kUpperCount; ++ui) {
        if (((touched >> ui) & 1) == 0) continue;
        for (auto& v : impl.voices) {
            if (v.active && v.upperIndex == ui) releaseOne(impl, v, false);
        }
        impl.mono[static_cast<std::size_t>(ui)] = SamplerEngine::Impl::MonoKeys{};
    }
    for (auto& ch : impl.tercaKey) ch.fill(-1);
    for (auto& ch : impl.tercaRefs) ch.fill(0U);
    impl.tercaApplied = cfg;
}

void processEvent(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, const RealtimeEvent& e) noexcept {
    if (stateOwner == nullptr) return;
    syncTerca(impl);
    const auto& state = *stateOwner;
    const std::uint8_t channel = static_cast<std::uint8_t>(e.channel & 0x0FU);
    if (e.type != EventType::Panic && (state.global.midiInputChannelMask & (1U << channel)) == 0U) return;
    auto& ch = impl.channels[channel];
    switch (e.type) {
        case EventType::NoteOn: {
            if (state.global.filterNotes) break;
            if (e.data2 == 0U) { handleNoteOff(impl, stateOwner, e.channel, e.data1); break; }
            const bool anySolo = std::any_of(state.combi.uppers.begin(), state.combi.uppers.end(), [](const UpperPart& u) { return u.enabled && u.solo; });
            const int tmask = impl.tercaApplied >= 0 ? (impl.tercaApplied & 0x7F) : 0;
            int harmonyKey = -1;   // SILVETON_TERCA_V1: the third, sounded once for all T Uppers
            if (tmask != 0) {
                const int cfg = impl.tercaApplied;
                auto& slot = impl.tercaKey[e.channel & 0x0FU][e.data1 & 0x7FU];
                if (slot < 0) {
                    const int h = SamplerEngine::tercaHarmony((cfg >> 8) & 0x0F, (cfg >> 12) & 0x0F, e.data1);
                    if (h >= 0) {
                        slot = static_cast<std::int8_t>(h);
                        auto& refs = impl.tercaRefs[e.channel & 0x0FU][static_cast<std::size_t>(h)];
                        if (refs < 255U) ++refs;
                        if (refs == 1U) harmonyKey = h;   // otherwise another key already sounds it
                    }
                }
            }
            for (int ui = 0; ui < kUpperCount; ++ui) {
                const auto& upper = state.combi.uppers[static_cast<std::size_t>(ui)];
                if (upper.midiChannel != (e.channel & 0x0FU)) continue;
                if (anySolo && !upper.solo) continue;
                if (!upper.matches(e.data1, e.data2)) continue;
                std::uint8_t key = e.data1;
                if (((tmask >> ui) & 1) != 0) {
                    // SILVETON_TERCA_V1: a T Upper plays the third of the key.
                    if (harmonyKey < 0) continue;
                    key = static_cast<std::uint8_t>(harmonyKey);
                }
                bowNoteOn(impl, state, ui, upper, e.channel, key, e.data2);   // SILVETON_BOW_V1
                if (voiceModeNow(impl, ui, upper) != UpperVoiceMode::Poly) {
                    auto& mk = impl.mono[static_cast<std::size_t>(ui)];
                    const bool keyWasHeld = mk.count > 0U;
                    const int previousKey = keyWasHeld ? static_cast<int>(mk.key[mk.count - 1U]) : -1;
                    monoPush(mk, key, e.data2);
                    if (portamentoSecFor(upper) > 0.0F) {
                        // SILVETON_PORTA_PAGE_V1: the note that already sounds changes nothing
                        if (previousKey != static_cast<int>(key)) {
                            playPortamentoNote(impl, stateOwner, ui, upper, e.channel, key, e.data2, keyWasHeld, previousKey);
                        }
                        continue;
                    }
                }
                if (portamentoSecFor(upper) > 0.0F) {
                    // SILVETON_POLY_PORTA_V1: glide from the key played before, only while
                    // a key of this Upper is held; keys of one chord share the same start.
                    const auto u = static_cast<std::size_t>(ui);
                    bool held = false;
                    for (const auto& v : impl.voices) if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld) { held = true; break; }
                    const bool sameChord = impl.frameClock - impl.polyLastOn[u] < static_cast<std::uint64_t>(0.050 * impl.sampleRate);   // a rolled chord is one chord
                    // KRONOS (not Fingered): always from the last key, held or not.
                    const bool always = upper.portaMode == PortamentoMode::Kronos && !upper.portaFingered;
                    if (!sameChord) impl.polyAnchorKey[u] = (held || always) ? impl.polyLastKey[u] : -1;
                    const double from = impl.polyAnchorKey[u];
                    impl.polyLastKey[u] = key;
                    impl.polyLastOn[u] = impl.frameClock;
                    playUpperNote(impl, stateOwner, ui, upper, e.channel, key, e.data2,
                                  from, glideSecFor(impl, upper, from, static_cast<double>(key), ui));
                    continue;
                }
                impl.polyLastKey[static_cast<std::size_t>(ui)] = key;
                playUpperNote(impl, stateOwner, ui, upper, e.channel, key, e.data2);
            }
            break;
        }
        case EventType::NoteOff:
            if (!state.global.filterNotes) handleNoteOff(impl, stateOwner, e.channel, e.data1);
            break;
        case EventType::ControlChange:
            // Safety CCs remain active even when ordinary CCs are filtered.
            if (e.data1 == 120U || e.data1 == 123U) {
                for (auto& v : impl.voices) if (v.active && v.channel == e.channel) { v.ampEnv.forceOff(); v.active = false; }
                bowAllOff(impl, static_cast<int>(e.channel));   // SILVETON_BOW_V1
                for (int ui = 0; ui < kUpperCount; ++ui) {
                    if (state.combi.uppers[static_cast<std::size_t>(ui)].midiChannel == (e.channel & 0x0FU)) {
                        impl.mono[static_cast<std::size_t>(ui)] = SamplerEngine::Impl::MonoKeys{};
                    }
                }
                break;
            }
            if (state.global.filterControlChange) break;
            ch.cc[static_cast<std::size_t>(e.data1 & 0x7FU)] = static_cast<float>(e.data2) * kInv127;
            if (e.data1 == state.global.breathCc) {
                ch.breath = calibratedController(e.data2, state.global.breathMin, state.global.breathMax, state.global.breathCurve);
            }
            else if (e.data1 == state.global.expressionCc) {
                ch.expression = calibratedController(e.data2, state.global.expressionMin, state.global.expressionMax, state.global.expressionCurve);
            }
            else if (e.data1 == state.global.damperCc) {
                const bool old = ch.sustain;
                const bool pressed = e.data2 >= 64U;
                ch.sustain = state.global.damperPolarityReversed ? !pressed : pressed;
                if (old && !ch.sustain) { releaseSustained(impl, e.channel); bowReleaseSustained(impl, channel); }   // SILVETON_BOW_V1
            }
            break;
        case EventType::PitchBend:
            if (!state.global.filterPitchBend && state.global.pitchBendEnabled) ch.pitchBend = e.pitchBend;
            break;
        case EventType::ChannelPressure:
            ch.pressure = (!state.global.filterAftertouch && state.global.aftertouchEnabled)
                              ? static_cast<float>(e.data1) * kInv127
                              : 0.0F;
            break;
        case EventType::Panic: panic(impl); bowAllOff(impl, -1); break;   // SILVETON_BOW_V1
    }
}

void readWithLoopCrossfade(const SincInterpolator& interp, const SampleData& sample, double position,
                           double ratio, bool looping, float& left, float& right) noexcept {
    if (!looping || !sample.loop.enabled || sample.loop.crossfadeFrames == 0U ||
        sample.loop.endFrame <= sample.loop.startFrame) {
        interp.readStereo(sample, position, ratio, looping, left, right);
        return;
    }
    const double xfade = static_cast<double>(sample.loop.crossfadeFrames);
    const double startFade = static_cast<double>(sample.loop.endFrame) - xfade;
    if (position < startFade || position >= static_cast<double>(sample.loop.endFrame)) {
        interp.readStereo(sample, position, ratio, looping, left, right);
        return;
    }
    const double t = std::clamp((position - startFade) / xfade, 0.0, 1.0);
    const double headPos = static_cast<double>(sample.loop.startFrame) + (position - startFade);
    float tailL = 0.0F, tailR = 0.0F, headL = 0.0F, headR = 0.0F;
    interp.readStereo(sample, position, ratio, false, tailL, tailR);
    interp.readStereo(sample, headPos, ratio, true, headL, headR);
    const float a = static_cast<float>(std::cos(t * kPi * 0.5));
    const float b = static_cast<float>(std::sin(t * kPi * 0.5));
    left = tailL * a + headL * b;
    right = tailR * a + headR * b;
}

// Publishes which part of the bank this voice will read during the next
// lookahead seconds (only the part that is not in the RAM attack prefix).
void publishVoiceWindows(SamplerEngine::Impl& impl, std::size_t vi) noexcept {
    auto& v = impl.voices[vi];
    const std::size_t slotA = vi * 2U;
    const std::size_t slotB = slotA + 1U;
    const RawPcmView raw = (v.active && v.sample != nullptr && v.sample->stream != nullptr && v.zone != nullptr)
        ? v.sample->stream->rawView() : RawPcmView{};
    if (raw.data == nullptr || raw.frames == 0U) {
        impl.publishWindow(slotA, 0U, 0U);
        impl.publishWindow(slotB, 0U, 0U);
        return;
    }
    const auto& s = *v.sample;
    constexpr double kTapMargin = 32.0;
    const double speed = std::max(0.05, std::abs(v.ratio));
    const double look = impl.lookaheadSec.load(std::memory_order_relaxed) * impl.sampleRate * speed + kTapMargin;
    const double frames = static_cast<double>(raw.frames);
    const double preloaded = static_cast<double>(s.preloadedFrameCount());
    const double pos = v.position;
    const std::size_t frameBytes = raw.frameBytes();
    const auto emit = [&](std::size_t slot, double lo, double hi) noexcept {
        lo = std::max(std::max(lo, preloaded), 0.0);
        hi = std::min(hi, frames);
        if (!(hi > lo)) { impl.publishWindow(slot, 0U, 0U); return; }
        const auto first = static_cast<std::uint64_t>(lo);
        const auto last = std::min(static_cast<std::uint64_t>(std::ceil(hi)), raw.frames);
        if (last <= first) { impl.publishWindow(slot, 0U, 0U); return; }
        impl.publishWindow(slot,
                           reinterpret_cast<std::uintptr_t>(raw.data + static_cast<std::size_t>(first) * frameBytes),
                           static_cast<std::size_t>(last - first) * frameBytes);
    };
    if (v.zone->reverse) {
        emit(slotA, pos - look, pos + kTapMargin);
        impl.publishWindow(slotB, 0U, 0U);
        return;
    }
    const double end = pos + look;
    const bool looping = s.loop.enabled && !v.zone->oneShot && s.loop.endFrame > s.loop.startFrame;
    const double loopEnd = static_cast<double>(s.loop.endFrame);
    if (looping && end > loopEnd) {
        const double loopStart = static_cast<double>(s.loop.startFrame);
        emit(slotA, pos - kTapMargin, loopEnd + kTapMargin);
        emit(slotB, loopStart - kTapMargin, std::min(loopEnd, loopStart + (end - loopEnd)) + kTapMargin);
    } else {
        emit(slotA, pos - kTapMargin, end);
        impl.publishWindow(slotB, 0U, 0U);
    }
}

} // namespace

void SamplerEngine::render(float* out, std::uint32_t frames) noexcept {
    if (out == nullptr || frames == 0U) return;
    std::memset(out, 0, static_cast<std::size_t>(frames) * 2U * sizeof(float));

    // Adopt a complete control-thread graph only at a realtime block boundary.
    // Do not reset voices or FX: old voices own their immutable source graph and
    // continue naturally while new notes immediately use the new SOUND/COMBI.
    auto pending = std::atomic_exchange_explicit(
        &pendingState_, std::shared_ptr<const EngineState>{}, std::memory_order_acq_rel);
    if (pending != nullptr) {
        std::atomic_store_explicit(&state_, pending, std::memory_order_release);
        const auto& g = pending->global;
        impl_->lowEq.setShelf(impl_->sampleRate, 120.0F, g.eqLowDb, false);
        impl_->midEq.setPeaking(impl_->sampleRate, 1200.0F, 0.75F, g.eqMidDb);
        impl_->highEq.setShelf(impl_->sampleRate, 7000.0F, g.eqHighDb, true);
        for (int i = 0; i < kUpperCount; ++i) {
            const auto index = static_cast<std::size_t>(i);
            const auto& u = pending->combi.uppers[index];
            std::uint16_t routeMask = 0U;
            for (const auto& route : u.controllerRoutes) {
                if (route.source == ControllerSource::Off || std::abs(route.amount) <= 1.0e-9F) continue;
                routeMask = static_cast<std::uint16_t>(routeMask | static_cast<std::uint16_t>(1U << static_cast<unsigned>(route.destination)));
            }
            impl_->upperRouteMask[index] = routeMask;
        }
    }

    const auto current = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (current == nullptr) return;
    const auto renderStart = std::chrono::steady_clock::now();   // SILVETON_CPU_METER_V1

    RealtimeEvent ev{};
    while (events_.pop(ev)) processEvent(*impl_, current, ev);
    impl_->frameClock += frames;

    // SILVETON_EQ5_V1 / SILVETON_TONE_V2: the common EQ of every Upper and the
    // HP + 3-band EQ of every sound follow the state once per block (smoothed).
    {
        const float eqAlpha = static_cast<float>(1.0 - std::exp(-static_cast<double>(frames) / (0.020 * impl_->sampleRate)));
        for (std::size_t ui = 0; ui < static_cast<std::size_t>(kUpperCount); ++ui) {
            const auto& u = current->combi.uppers[ui];
            impl_->upperEq[ui].update(u.eq, impl_->sampleRate, eqAlpha);
            for (int s = 0; s < kSoundsPerUpper; ++s) {
                const UpperLayer* ul = layerOf(u, s);
                impl_->soundBus[ui][static_cast<std::size_t>(s)].update(ul != nullptr ? ul->tone.x : u.toneExtra, impl_->sampleRate);
            }
        }
    }

    // Live TONE values (ADSR times, SUSTAIN, CUTOFF, RESONANCE, FILTER ENV,
    // VEL->CUTOFF, vibrato) of the current Upper state, once per block per voice.
    {
        const double blockAlpha = 1.0 - std::exp(-static_cast<double>(frames) / (0.012 * impl_->sampleRate));
        const float openLog = std::log(kToneCutoffOpenHz);
        const double blockSec = static_cast<double>(frames) / impl_->sampleRate;
        for (auto& v : impl_->voices) {
            if (!v.active || v.layer == nullptr || v.upper == nullptr) continue;
            const auto uix = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
            // SILVETON_LIVE_EDIT_V1: the note follows the newest settings of its Upper
            // while that Upper still plays the same Sound.
            if (v.ownerState.get() != current.get()) {
                const auto& now = current->combi.uppers[uix];
                if (soundProgramOf(now, v.soundIndex) != nullptr && soundProgramOf(*v.upper, v.soundIndex) == soundProgramOf(now, v.soundIndex)) {
                    v.ownerState = current;
                    v.upper = &now;
                    v.routeMask = impl_->upperRouteMask[uix];
                }
            }
            const UpperLayer* ul = layerOf(*v.upper, v.soundIndex);
            const double tune = liveTuneSemis(*v.upper, current->global, v.key,
                ((impl_->scaleUppers.load(std::memory_order_relaxed) >> v.upperIndex) & 1) != 0, ul) + static_cast<double>(v.unisonCents) / 100.0;
            if (tune != v.tuneSemis) {
                const double m = std::exp2((tune - v.tuneSemis) / 12.0);
                v.ratio *= m;
                v.targetRatio *= m;
                v.tuneSemis = tune;
            }
            const SoundTone st = soundToneOf(*v.upper, v.soundIndex);
            // SILVETON_SYNTH_BYPASS_V1: a unison copy still sounding after SYNTH ИЗКЛ fades out
            if (v.unisonIndex > 0 && st.x.unisonVoices <= 1 && !v.fading) softKill(*impl_, v, 0.004F);
            UpperToneValues tv = effectiveToneFor(st, *v.upper, v.layer->ampEnvelope);
            // FILTER ENV: 1 at the start of the note, down to 0 in FILTER ENV time (exponential).
            {
                const double ms = std::clamp(static_cast<double>(st.x.filterEnvMs), 1.0, 20000.0);
                v.filterEnvValue *= static_cast<float>(std::exp(-blockSec * 1000.0 / ms * 4.6));   // -40 dB at FILTER ENV time
            }
            // SILVETON_POLYSIX_V1: the modulation matrix of this sound, once per block
            {
                const auto& ch = impl_->channels[v.channel & 0x0FU];
                v.modCutOct = v.modRes = v.modCut2Oct = v.modLevelDb = v.modPan = v.modPitch = v.modLfoCents = v.modPwm = v.modDrive = 0.0F;
                v.modLfoRateMul = v.modAttackMul = v.modReleaseMul = 1.0F;
                for (const auto& r : st.mod.route) {
                    if (r.source == ModSource::Off || r.amount == 0.0F) continue;
                    float x = 0.0F; bool bip = false;
                    switch (r.source) {
                        case ModSource::Off: break;
                        case ModSource::Velocity: x = v.velNorm; break;
                        case ModSource::Note: x = (static_cast<float>(v.key) - 60.0F) / 60.0F; bip = true; break;
                        case ModSource::Aftertouch: x = ch.pressure; break;
                        case ModSource::ModWheel: x = ch.cc[1]; break;
                        case ModSource::Breath: x = ch.breath; break;
                        case ModSource::Expression: x = ch.expression; break;
                        case ModSource::Pedal: x = ch.sustain ? 1.0F : 0.0F; break;
                        case ModSource::Bend: x = static_cast<float>(ch.pitchBend) / 8192.0F; bip = true; break;
                        case ModSource::Lfo: x = static_cast<float>(std::sin(v.vibPhase)); bip = true; break;
                        case ModSource::FilterEnv: x = v.filterEnvValue; break;
                        case ModSource::AmpEnv: x = v.ampEnv.value; break;
                        case ModSource::Random: x = v.random; bip = true; break;
                    }
                    if (r.bipolar && !bip) x = x * 2.0F - 1.0F;
                    const float a = std::clamp(r.amount, -1.0F, 1.0F) * x;
                    switch (r.target) {
                        case ModTarget::Cutoff: v.modCutOct += a * 4.0F; break;
                        case ModTarget::Resonance: v.modRes += a; break;
                        case ModTarget::Cutoff2: v.modCut2Oct += a * 4.0F; break;
                        case ModTarget::Level: v.modLevelDb += a * 24.0F; break;
                        case ModTarget::Pan: v.modPan += a; break;
                        case ModTarget::Pitch: v.modPitch += a * 12.0F; break;
                        case ModTarget::LfoDepth: v.modLfoCents += a * 100.0F; break;
                        case ModTarget::LfoRate: v.modLfoRateMul *= std::exp2(a * 2.0F); break;
                        case ModTarget::PwmWidth: v.modPwm += a * 0.45F; break;
                        case ModTarget::Drive: v.modDrive += a; break;
                        case ModTarget::Attack: v.modAttackMul *= std::exp2(a * 3.0F); break;
                        case ModTarget::Release: v.modReleaseMul *= std::exp2(a * 3.0F); break;
                    }
                }
                if (v.modAttackMul != 1.0F) tv.attackMs = std::clamp(tv.attackMs * v.modAttackMul, 0.0F, kToneMaxTimeMs);
                if (v.modReleaseMul != 1.0F) tv.releaseMs = std::clamp(tv.releaseMs * v.modReleaseMul, 0.0F, kToneMaxTimeMs);
                tv.resonance = std::clamp(tv.resonance + v.modRes, 0.0F, 1.0F);
            }
            v.ampEnv.setBlock(tv, impl_->sampleRate);
            float envOct = 0.0F;
            if (st.x.filterEnvOctaves != 0.0F) envOct = std::clamp(st.x.filterEnvOctaves, -8.0F, 8.0F) * v.filterEnvValue;
            const float keyTrackOct = std::clamp(st.x.filterKeyTrack, -1.0F, 1.0F) * (static_cast<float>(v.key) - 60.0F) / 12.0F;
            const float extraOct = envOct + v.velCutoffOct + v.modCutOct + keyTrackOct + (st.x.analogFeel ? v.random * 0.08F : 0.0F);
            // filter type of CUTOFF (clean / POLYSIX ladder / KORG35) - a change resets the new filter's state
            if (v.filterTypeNow != st.x.filterType) { v.filterTypeNow = st.x.filterType; v.ladder.prime = true; v.korg35.prime = true; v.toneLp.prime = true; }
            // second filter
            if (st.x.filter2Type != 0) {
                const float hz2 = std::clamp(st.x.filter2Hz * std::exp2(v.modCut2Oct), 20.0F, kToneCutoffOpenHz);
                v.filter2Hz = hz2; v.filter2Res = std::clamp(st.x.filter2Res, 0.0F, 1.0F);
            }
            // PWM width of this block
            if (st.x.pwmWidth > 0.0F && v.periodFrames > 0.0) {
                v.pwmPhase += 2.0 * kPi * std::clamp(static_cast<double>(st.x.pwmRateHz), 0.02, 20.0) * blockSec;
                if (v.pwmPhase > 2.0 * kPi) v.pwmPhase -= 2.0 * kPi;
                v.pwmWidthNow = std::clamp(st.x.pwmWidth + v.modPwm + st.x.pwmDepth * static_cast<float>(std::sin(v.pwmPhase)), 0.02F, 0.98F);
            } else {
                v.pwmWidthNow = 0.0F;
            }
            const bool wantOn = tv.cutoffHz < kToneCutoffOpenHz - 1.0F || extraOct < -0.001F;
            if (wantOn && !v.toneOn) { v.toneOn = true; v.toneLog = openLog; v.toneLp.prime = true; }
            if (v.toneOn) {
                const float target = tv.cutoffHz < kToneCutoffOpenHz - 1.0F ? std::log(tv.cutoffHz) : openLog;
                v.toneLog += (target - v.toneLog) * static_cast<float>(blockAlpha);
                const float hz = std::clamp(std::exp(v.toneLog + extraOct * 0.69314718F), 20.0F, kToneCutoffOpenHz);
                if (!wantOn && v.toneLog >= openLog - 0.05F) v.toneOn = false;
                else if (v.filterTypeNow == 1) v.ladder.set(hz, tv.resonance, impl_->sampleRate);
                else if (v.filterTypeNow == 2) v.korg35.set(hz, tv.resonance, impl_->sampleRate);
                else v.toneLp.set(hz, tv.resonance, impl_->sampleRate);
            }
            // Vibrato (TONE LFO): depth from the TONE page, scaled by the modulation wheel if chosen.
            v.vibSemis = static_cast<double>(v.modPitch);
            {
                const auto& ch = impl_->channels[v.channel & 0x0FU];
                const float wheel = st.x.lfoModWheel ? ch.cc[1] : 1.0F;
                v.vibAgeSec += blockSec;
                v.vibPhase += 2.0 * kPi * std::clamp(static_cast<double>(st.x.lfoRateHz * v.modLfoRateMul), 0.05, 30.0) * blockSec;
                if (v.vibPhase > 2.0 * kPi) v.vibPhase -= 2.0 * kPi;
                const float depthCents = std::clamp(st.x.lfoDepthCents * wheel + v.modLfoCents, 0.0F, 1200.0F);
                const double delay = std::max(0.0, static_cast<double>(st.x.lfoDelayMs)) * 0.001;
                double fade = 1.0;
                if (v.vibAgeSec < delay) fade = 0.0;
                else if (st.x.lfoFadeMs > 0.0F) fade = std::clamp((v.vibAgeSec - delay) / (static_cast<double>(st.x.lfoFadeMs) * 0.001), 0.0, 1.0);
                if (depthCents > 0.0F && fade > 0.0) {
                    v.vibSemis += std::sin(v.vibPhase) * static_cast<double>(depthCents) * 0.01 * fade;
                }
            }
        }
    }

    float peakL = 0.0F, peakR = 0.0F;
    std::array<float, kUpperCount> upperPeakL{};
    std::array<float, kUpperCount> upperPeakR{};
    const float masterGain = dbToGain(current->global.masterGainDb);
    const float mixSmooth = static_cast<float>(1.0 - std::exp(-1.0 / (kLiveMixSmoothSec * impl_->sampleRate)));
    // SILVETON_SILK_PORTA_V1: BALANCE 3..40 controls the softness / skew of
    // the time-normalized curve instead of delaying its arrival with filters.
    // Loaded once per block, no expensive filter or allocation on audio thread.

    const int tapUpperNow = impl_->tapUpper.load(std::memory_order_acquire);
    const bool tapAfterNow = impl_->tapAfter.load(std::memory_order_relaxed);
    std::uint32_t tapW = impl_->tapWrite.load(std::memory_order_relaxed);

    // SILVETON_FX_TAIL_V1: another BLEND was called up -> an effect that changes keeps
    // ringing out on its own engine; the new effect starts on the other one.
    const auto tailFadeFrames = static_cast<std::uint32_t>(0.25 * impl_->sampleRate);
    const auto tailQuietFrames = static_cast<std::uint32_t>(0.30 * impl_->sampleRate);
    {
        if (impl_->fxRunningValid && current->combiGeneration != impl_->fxGeneration) {
            for (std::size_t slot = 0; slot < static_cast<std::size_t>(kFxSendSlots); ++slot) {
                const EffectSlot& was = impl_->fxRunning[slot];
                const EffectSlot& now = current->combi.fx[slot];
                const bool same = was.type == now.type && was.wet == now.wet && was.timeMs == now.timeMs &&
                                  was.feedback == now.feedback && was.rateHz == now.rateHz &&
                                  was.depth == now.depth && was.tone == now.tone && was.size == now.size &&
                                  was.preDelayMs == now.preDelayMs && was.damping == now.damping && was.mod == now.mod;
                if (same || was.type == EffectType::Off || was.wet <= 0.0F) continue;
                impl_->tailCfg[slot] = was;
                impl_->tailInst[slot] = impl_->fxInst[slot];
                impl_->tailLeft[slot] = static_cast<std::uint32_t>(20.0 * impl_->sampleRate);
                impl_->tailQuiet[slot] = 0U;
                impl_->fxInst[slot] = static_cast<std::uint8_t>(impl_->fxInst[slot] ^ 1U);
            }
        }
        impl_->fxGeneration = current->combiGeneration;
        impl_->fxRunning = current->combi.fx;
        impl_->fxRunningValid = true;
    }

    // SILVETON_RENDER_SPEED_V1: every voice renders its whole chunk at once and
    // everything that cannot change inside a block (controllers, pitch bend,
    // LFO scaling, filter base, pan, level) is computed once per voice per chunk
    // instead of once per sample. Per sample the work and its order are the same,
    // so the sound is the same.
    constexpr std::uint32_t kChunk = Impl::kRenderChunk;
    for (std::uint32_t chunkStart = 0U; chunkStart < frames; chunkStart += kChunk) {
      const std::uint32_t n = std::min(kChunk, frames - chunkStart);
      for (auto& row : impl_->chunkL) for (auto& buf : row) std::fill_n(buf.begin(), n, 0.0F);
      for (auto& row : impl_->chunkR) for (auto& buf : row) std::fill_n(buf.begin(), n, 0.0F);

      for (auto& v : impl_->voices) {
        if (!v.active || v.sample == nullptr || v.layer == nullptr || v.zone == nullptr || v.upper == nullptr) continue;
        const auto& sample = *v.sample;
        const bool looping = sample.loop.enabled && !v.zone->oneShot && !v.zone->reverse && !v.loopOff;
        const auto& ch = impl_->channels[v.channel & 0x0FU];
        const auto ui = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
        const auto si = static_cast<std::size_t>(std::clamp(v.soundIndex, 0, kSoundsPerUpper - 1));
        const std::uint16_t routeMask = v.routeMask;
        const auto routeActive = [routeMask](ModDestination d) noexcept {
            return (routeMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(d))) != 0U;
        };
        auto& outL = impl_->chunkL[ui][si];
        auto& outR = impl_->chunkR[ui][si];

        const bool filterActive = v.layer->filter.mode != FilterMode::Bypass;
        const bool lfoDepthRouted = routeActive(ModDestination::LfoDepth);
        const bool lfo1Needed = lfoDepthRouted || v.layer->lfo1.pitchCents != 0.0F ||
            v.layer->lfo1.filterOctaves != 0.0F || v.layer->lfo1.ampDepth != 0.0F ||
            (filterActive && v.layer->filter.lfoAmount != 0.0F);
        const bool lfo2Needed = lfoDepthRouted || v.layer->lfo2.pitchCents != 0.0F ||
            v.layer->lfo2.filterOctaves != 0.0F || v.layer->lfo2.ampDepth != 0.0F;

        LfoParams lfo1Params = v.layer->lfo1;
        LfoParams lfo2Params = v.layer->lfo2;
        if (lfo1Needed || lfo2Needed) {
            const float lfoRateScale = std::clamp(v.upper->lfoRateScale, 0.0F, 16.0F);
            const float controllerLfoDepth = lfoDepthRouted
                ? controllerAmount(*v.upper, ch, ModDestination::LfoDepth) : 0.0F;
            const float lfoDepthScale = std::clamp(
                v.upper->lfoDepthScale * std::max(0.0F, 1.0F + controllerLfoDepth), 0.0F, 8.0F);
            if (lfo1Needed) {
                lfo1Params.frequencyHz *= lfoRateScale;
                lfo1Params.pitchCents *= lfoDepthScale;
                lfo1Params.filterOctaves *= lfoDepthScale;
                lfo1Params.ampDepth *= lfoDepthScale;
            }
            if (lfo2Needed) {
                lfo2Params.frequencyHz *= lfoRateScale;
                lfo2Params.pitchCents *= lfoDepthScale;
                lfo2Params.filterOctaves *= lfoDepthScale;
                lfo2Params.ampDepth *= lfoDepthScale;
            }
        }

        const bool pitchEnvNeeded = v.layer->pitchEnvelopeCents != 0.0F;
        const bool filterEnvNeeded = filterActive && v.layer->filter.envelopeAmount != 0.0F;
        const bool lfoPitch = (lfo1Needed && lfo1Params.pitchCents != 0.0F) || (lfo2Needed && lfo2Params.pitchCents != 0.0F);
        const bool dynamicPitchActive = ch.pitchBend != 0 || routeActive(ModDestination::PitchSemitones) ||
            pitchEnvNeeded || lfoPitch || v.vibSemis != 0.0;
        const float bendNorm = static_cast<float>(ch.pitchBend) / 8192.0F;
        const float bendSemis = bendNorm * static_cast<float>(v.upper->pitchBendRange);
        const float controllerPitchSemis = routeActive(ModDestination::PitchSemitones)
            ? controllerAmount(*v.upper, ch, ModDestination::PitchSemitones) : 0.0F;
        const bool pitchPerSample = pitchEnvNeeded || lfoPitch;
        // Bend / controller / vibrato: the same factor for the whole block.
        const double staticPitchMul = (dynamicPitchActive && !pitchPerSample)
            ? std::pow(2.0, (static_cast<double>(bendSemis + controllerPitchSemis) + v.vibSemis) / 12.0)
            : 1.0;

        float filterBaseCutoff = 0.0F, filterStaticOct = 0.0F, filterResonance = 0.0F;
        const bool filterOctPerSample = filterEnvNeeded || lfo1Needed || lfo2Needed;
        if (filterActive) {
            const float controllerCutoff = routeActive(ModDestination::FilterCutoffOctaves)
                ? controllerAmount(*v.upper, ch, ModDestination::FilterCutoffOctaves) : 0.0F;
            filterBaseCutoff = v.layer->filter.cutoffHz * std::pow(
                2.0F, std::clamp(v.upper->filterCutoffOctaves + controllerCutoff, -10.0F, 10.0F));
            const float keyDelta = (static_cast<float>(v.key) - 60.0F) / 12.0F;
            const float velocityNorm = static_cast<float>(v.velocity) * kInv127;
            filterStaticOct = keyDelta * v.layer->filter.keyTrack + velocityNorm * v.layer->filter.velocityAmount;
            const float controllerResonance = routeActive(ModDestination::FilterResonance)
                ? controllerAmount(*v.upper, ch, ModDestination::FilterResonance) : 0.0F;
            filterResonance = std::clamp(
                v.layer->filter.resonance + v.upper->filterResonanceOffset + controllerResonance, 0.0F, 1.0F);
        }
        const float staticFilterCutoff = (filterActive && !filterOctPerSample)
            ? (filterStaticOct != 0.0F ? filterBaseCutoff * std::pow(2.0F, filterStaticOct) : filterBaseCutoff)
            : 0.0F;

        const bool ampLfo = (lfo1Needed && lfo1Params.ampDepth != 0.0F) || (lfo2Needed && lfo2Params.ampDepth != 0.0F);
        const bool anyChannelGain = v.upper->expressionEnabled || v.upper->breathEnabled || v.upper->aftertouchEnabled;
        const float controllerLevelDb = routeActive(ModDestination::LevelDb)
            ? controllerAmount(*v.upper, ch, ModDestination::LevelDb) : 0.0F;
        const float levelGain = controllerLevelDb != 0.0F ? dbToGain(std::clamp(controllerLevelDb, -96.0F, 24.0F)) : 1.0F;
        const float controllerPan = routeActive(ModDestination::Pan) ? controllerAmount(*v.upper, ch, ModDestination::Pan) : 0.0F;
        updateMixTarget(v, *v.upper, upperAudible(*v.ownerState, *v.upper), controllerPan + v.unisonPan + v.modPan);
        // SILVETON_POLYSIX_V1 per-chunk settings of this sound
        const SoundTone stC = soundToneOf(*v.upper, v.soundIndex);
        const float subLevel = std::clamp(stC.x.subLevel, 0.0F, 1.0F);
        const double subRatioDiv = stC.x.subOctave >= 2 ? 4.0 : 2.0;
        const bool subOn = subLevel > 0.0F;
        const bool pwmOn = v.pwmWidthNow > 0.0F && v.periodFrames > 0.0;
        const float pwmMix = std::clamp(stC.x.pwmMix, 0.0F, 1.0F);
        const double pwmDelta = pwmOn ? static_cast<double>(v.pwmWidthNow) * v.periodFrames : 0.0;   // in recording frames
        const float driveAmt = std::clamp(stC.x.drive + v.modDrive, 0.0F, 1.0F);
        const float driveK = 1.0F + 9.0F * driveAmt * driveAmt;
        const float driveNorm = 1.0F / std::sqrt(driveK);
        const int f2type = stC.x.filter2Type;
        const bool f2parallel = stC.x.filter2Parallel;
        const float modLevelGain = v.modLevelDb != 0.0F ? dbToGain(std::clamp(v.modLevelDb, -60.0F, 24.0F)) : 1.0F;
        const float dcCoef = 1.0F - static_cast<float>(2.0 * kPi * 20.0 / impl_->sampleRate);
        const float mixTargetL = v.mixTargetL;
        const float mixTargetR = v.mixTargetR;
        const bool mixMoving = mixTargetL != v.mixL || mixTargetR != v.mixR;
        const bool reverse = v.zone->reverse;
        const double loopEnd = static_cast<double>(sample.loop.endFrame);
        const double loopStart = static_cast<double>(sample.loop.startFrame);
        const double loopLen = static_cast<double>(sample.loop.endFrame - sample.loop.startFrame);
        const double sampleEnd = static_cast<double>(sample.frameCount());

        for (std::uint32_t i = 0U; i < n; ++i) {
            if (v.delayFramesRemaining > 0U) {
                --v.delayFramesRemaining;
                continue;
            }
            float lfo1 = 0.0F, lfo2 = 0.0F;
            if (lfo1Needed) lfo1 = v.lfo1.process(lfo1Params, impl_->sampleRate);
            if (lfo2Needed) lfo2 = v.lfo2.process(lfo2Params, impl_->sampleRate);

            const float pitchEnv = pitchEnvNeeded ? v.pitchEnv.process(impl_->sampleRate) : 0.0F;
            const float ampEnv = v.ampEnv.process();
            const float filterEnv = filterEnvNeeded ? v.filterEnv.process(impl_->sampleRate) : 0.0F;
            if (!v.ampEnv.active()) { v.active = false; break; }

            if (v.pitchGliding) {
                double raw = 0.0;
                const double previous = v.glideRawSemis;
                if (v.glideFramesRemaining > 0U) {
                    --v.glideFramesRemaining;
                    const double t = 1.0 - static_cast<double>(v.glideFramesRemaining) / static_cast<double>(v.glideTotalFrames);
                    if (v.glideKronos) {
                        // SILVETON_KRONOS_MUSICAL_PORTA_V2: a firmer, more natural
                        // ease than SILVETON's velvet S-shape, with especially
                        // gentle starts/arrivals on adjacent keys. Use semitones
                        // (not Hz), no low-pass delay and EXACT arrival in TIME.
                        const double near = std::clamp(
                            (5.0 - std::abs(v.glideStartSemis)) / 4.0, 0.0, 1.0);
                        const double ease = 0.66 + 0.22 * near;
                        const double smooth = t * t * (3.0 - 2.0 * t);
                        const double progress = t + ease * (smooth - t);
                        // C1 continuity during overlapping, rapid legato notes.
                        // End-slope is independent of inherited movement.
                        const double inv = 1.0 - t;
                        const double velocityBasis = t * inv * inv;
                        raw = v.glideStartSemis * (1.0 - progress)
                            + v.glideStartVelocity * velocityBasis;
                    } else {
                        // SILVETON_PROCESSOR_PORTA_V1: the glide of the SILVETON keyboard processor on
                        // the Korg - a straight line at constant rate, from the pitch that sounds to the
                        // new key, arriving exactly in time. No S-curve, no carried momentum
                        // (the SILVETON_SILK_PORTA_V1 shape is retired).
                        raw = v.glideStartSemis * (1.0 - t);
                    }
                }
                v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = raw;
                v.glideLastStep = raw - previous;
                if (v.glideFramesRemaining == 0U) {
                    // No low-pass tail: destination is exact at TIME (including
                    // 1 ms changes). No lingering pitch drift after landing.
                    v.pitchGliding = false;
                    v.glideStartVelocity = v.glideLastStep = 0.0;
                    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = 0.0;
                    v.ratio = v.targetRatio;
                } else {
                    v.ratio = v.targetRatio * std::exp2(raw / 12.0);
                }
            }

            double ratio = v.ratio;
            if (dynamicPitchActive) {
                if (pitchPerSample) {
                    const float lfoPitchCents = lfo1 * lfo1Params.pitchCents + lfo2 * lfo2Params.pitchCents;
                    const float envPitchCents = pitchEnv * v.layer->pitchEnvelopeCents;
                    const double dynamicPitch = static_cast<double>(bendSemis + controllerPitchSemis) + v.vibSemis +
                        static_cast<double>(lfoPitchCents + envPitchCents) / 100.0;
                    ratio *= std::pow(2.0, dynamicPitch / 12.0);
                } else {
                    ratio *= staticPitchMul;
                }
            }

            float sL = 0.0F, sR = 0.0F;
            // After the first pass through the loop, the frames just before the loop
            // start are the loop END (that is what was heard), not the attack: read
            // one loop length later so the interpolator takes the loop-end history.
            double readPos = v.position;
            if (v.loopWrapped && looping && loopLen > 0.0 && readPos < loopStart + 64.0) readPos += loopLen;
            readWithLoopCrossfade(interpolator_, sample, readPos, ratio, looping, sL, sR);
            if (pwmOn) {
                // SILVETON_SYNTH_LIGHT_V1 - PWM: the recording minus itself shifted by WIDTH x its own
                // period (saw -> pulse; any harmonic recording -> the comb PWM gives). The shifted copy
                // comes from a delay line fed with the sound just read (4-point interpolation) - the
                // same result as a second read of the recording at a tenth of the cost. 20 Hz DC blocker.
                constexpr int kD = SamplerEngine::Impl::Voice::kPwmDelay;
                const int w = v.pwmWrite;
                v.pwmBuf[static_cast<std::size_t>(w) * 2U] = sL; v.pwmBuf[static_cast<std::size_t>(w) * 2U + 1U] = sR;
                v.pwmWrite = (w + 1) & (kD - 1);
                if (v.pwmFilled < kD) ++v.pwmFilled;
                const double delay = std::clamp(pwmDelta / std::max(1.0e-6, ratio), 1.0, static_cast<double>(kD - 4));   // output frames
                if (static_cast<double>(v.pwmFilled) > delay + 2.0) {
                    const double rp = static_cast<double>(w) - delay;
                    const int i1 = static_cast<int>(std::floor(rp));
                    const float t = static_cast<float>(rp - static_cast<double>(i1));
                    const auto at = [&](int ix, int c) noexcept { return v.pwmBuf[static_cast<std::size_t>(ix & (kD - 1)) * 2U + static_cast<std::size_t>(c)]; };
                    const auto herm = [t](float y0, float y1, float y2, float y3) noexcept {
                        const float c1 = 0.5F * (y2 - y0), c2 = y0 - 2.5F * y1 + 2.0F * y2 - 0.5F * y3, c3 = 0.5F * (y3 - y0) + 1.5F * (y1 - y2);
                        return ((c3 * t + c2) * t + c1) * t + y1;
                    };
                    const float dL = herm(at(i1 - 1, 0), at(i1, 0), at(i1 + 1, 0), at(i1 + 2, 0));
                    const float dR = herm(at(i1 - 1, 1), at(i1, 1), at(i1 + 1, 1), at(i1 + 2, 1));
                    sL -= dL * pwmMix; sR -= dR * pwmMix;
                }
                const float yL = sL - v.dcXL + dcCoef * v.dcYL; v.dcXL = sL; v.dcYL = yL; sL = yL;
                const float yR = sR - v.dcXR + dcCoef * v.dcYR; v.dcXR = sR; v.dcYR = yR; sR = yR;
            }
            if (subOn) {
                // SUB: the same recording an octave (or two) lower, from its own position
                // (SILVETON_SYNTH_LIGHT_V1: read with the light 4-point interpolation - it runs at half
                // or quarter speed, where that is already clean - instead of the Studio kernel)
                float subL = 0.0F, subR = 0.0F;
                double subPos = v.subPosition;
                if (v.loopWrapped && looping && loopLen > 0.0 && subPos < loopStart + 64.0) subPos += loopLen;
                readWithLoopCrossfade(subInterpolator_, sample, subPos, ratio / subRatioDiv, looping, subL, subR);
                sL += subL * subLevel; sR += subR * subLevel;
                v.subPosition += ratio / subRatioDiv;
                if (looping && v.subPosition >= loopEnd && loopLen > 0.0) v.subPosition = loopStart + std::fmod(v.subPosition - loopStart, loopLen);
                else if (!looping && v.subPosition >= sampleEnd) v.subPosition = sampleEnd - 1.0;
            }

            if (filterActive) {
                float cutoff = staticFilterCutoff;
                if (filterOctPerSample) {
                    cutoff = filterBaseCutoff;
                    const float filterOct = filterStaticOct +
                                            filterEnv * v.layer->filter.envelopeAmount +
                                            lfo1 * v.layer->filter.lfoAmount +
                                            lfo1 * lfo1Params.filterOctaves +
                                            lfo2 * lfo2Params.filterOctaves;
                    if (filterOct != 0.0F) cutoff *= std::pow(2.0F, filterOct);
                }
                v.filter.process(sL, sR, v.layer->filter.mode, cutoff, filterResonance, impl_->sampleRate);
            }

            if (driveAmt > 0.0F) { sL = std::tanh(sL * driveK) * driveNorm; sR = std::tanh(sR * driveK) * driveNorm; }
            if (v.toneOn) {
                if (v.filterTypeNow == 1) v.ladder.process(sL, sR);
                else if (v.filterTypeNow == 2) v.korg35.process(sL, sR);
                else v.toneLp.process(sL, sR);
            }
            if (f2type != 0) {
                float bL = sL, bR = sR;
                const FilterMode fm = f2type == 1 ? FilterMode::LowPass : (f2type == 2 ? FilterMode::HighPass : FilterMode::BandPass);
                v.filter2.process(bL, bR, fm, v.filter2Hz, v.filter2Res, impl_->sampleRate);
                if (f2type == 4) { bL = sL - bL; bR = sR - bR; }   // band reject = input minus band-pass
                if (f2parallel) { sL = 0.5F * (sL + bL); sR = 0.5F * (sR + bR); } else { sL = bL; sR = bR; }
            }

            float modGain = ampEnv;
            if (ampLfo) modGain *= std::max(0.0F, 1.0F + lfo1 * lfo1Params.ampDepth + lfo2 * lfo2Params.ampDepth);
            if (anyChannelGain) {
                if (v.upper->expressionEnabled) modGain *= ch.expression;
                if (v.upper->breathEnabled) modGain *= ch.breath;
                if (v.upper->aftertouchEnabled) modGain *= (0.5F + 0.5F * ch.pressure);
            }
            float g = v.baseGain * modGain * v.unisonGain * modLevelGain;
            if (controllerLevelDb != 0.0F) g *= levelGain;
            if (v.fadeStep != 0.0F) {
                v.fadeGain += v.fadeStep;
                if (v.fadeGain <= 0.0F) { v.ampEnv.forceOff(); v.active = false; v.fading = false; break; }
                if (v.fadeGain >= 1.0F) { v.fadeGain = 1.0F; v.fadeStep = 0.0F; }
            }
            if (v.fadeStep != 0.0F || v.fadeGain < 1.0F) {
                // equal-power fade; a crossfade between two recordings of the same line uses a
                // middle law (x^0.75): no +3 dB bump when they are in phase, no -3 dB hole when not
                g *= v.fadeBlend ? std::pow(v.fadeGain, 0.75F) : std::sin(v.fadeGain * 1.57079632679F);
            }
            if (mixMoving) {
                v.mixL += (mixTargetL - v.mixL) * mixSmooth;
                v.mixR += (mixTargetR - v.mixR) * mixSmooth;
            }
            outL[i] += sL * g * v.mixL;
            outR[i] += sR * g * v.mixR;

            if (reverse) v.position -= ratio; else v.position += ratio;
            if (!reverse && looping && v.position >= loopEnd) {
                if (loopLen > 0.0) { v.position = loopStart + std::fmod(v.position - loopStart, loopLen); v.loopWrapped = true; }
            }
            if (reverse && v.position < 0.0) { v.ampEnv.forceOff(); v.active = false; break; }
            else if (!looping && v.position >= sampleEnd) { v.ampEnv.forceOff(); v.active = false; break; }
            else if (v.endFrame > 0.0 && !reverse && v.position >= v.endFrame && !v.fading) {
                // SILVETON_TONE_V2: SAMPLE END - the note stops here with a short click-free fade
                softKill(*impl_, v, 0.003F);
            }
        }
        if (mixMoving) {
            if (std::abs(mixTargetL - v.mixL) < 1.0e-6F) v.mixL = mixTargetL;
            if (std::abs(mixTargetR - v.mixR) < 1.0e-6F) v.mixR = mixTargetR;
        }
      }

      // Which sound buses carry anything this chunk (skip the silent ones).
      std::array<std::array<bool, kSoundsPerUpper>, kUpperCount> busLive{};
      for (const auto& v : impl_->voices) {
          if (v.active && v.upperIndex >= 0 && v.upperIndex < kUpperCount)
              busLive[static_cast<std::size_t>(v.upperIndex)][static_cast<std::size_t>(std::clamp(v.soundIndex, 0, kSoundsPerUpper - 1))] = true;
      }

      // SILVETON_BOW_V1: the bowed strings render into the bus of their sound (basic sound or layer),
      // so level, pan, ON/MUTE/SOLO, the sound's EQ, the Upper EQ, INSERT and sends apply as to a recording.
      for (std::size_t ui = 0; ui < static_cast<std::size_t>(kUpperCount); ++ui) {
        for (std::size_t si = 0; si < static_cast<std::size_t>(kSoundsPerUpper); ++si) {
          auto& b = impl_->bows[ui][si];
          if (!b.active) continue;
          const auto& u = current->combi.uppers[ui];
          const UpperLayer* ul = layerOf(u, static_cast<int>(si));
          const auto& ch = impl_->channels[b.channel & 0x0FU];
          // the slot plays a recording now (or nothing): the bow lifts, the string rings out
          if (bowInstrumentOf(u, static_cast<int>(si)) == 0 && b.string.playing()) { b.string.noteOff(); b.count = 0U; b.sustained = false; }
          bow::Controls c = bowControlsFor(ul != nullptr ? ul->tone.x : u.toneExtra);
          c.glideSec = b.string.controls().glideSec;   // set at the note, not by the page
          // dynamics: the stroke's velocity sets the base, the expression pedal (and breath) shape it live
          float dyn = 0.35F + 0.65F * b.velNorm;
          if (u.expressionEnabled) dyn *= ch.expression;
          if (u.breathEnabled) dyn *= ch.breath;
          c.expression = std::clamp(static_cast<double>(dyn), 0.0, 1.0);
          if (u.aftertouchEnabled) c.pressure = std::min(1.0, c.pressure + 0.4 * static_cast<double>(ch.pressure));
          c.bendSemis = static_cast<double>(ch.pitchBend) / 8192.0 * static_cast<double>(u.pitchBendRange);
          c.extraVibratoCents = static_cast<double>(ch.cc[1]) * 50.0;   // the modulation wheel adds vibrato
          b.string.setControls(c);
          b.string.block(static_cast<int>(n));
          // mix gains: Upper level x equal-power pan x ON/MUTE/SOLO (+ the layer's own level and pan)
          const std::uint16_t routeMask = impl_->upperRouteMask[ui];
          const auto routed = [routeMask](ModDestination d) noexcept { return (routeMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(d))) != 0U; };
          float targetL = 0.0F, targetR = 0.0F;
          if (upperAudible(*current, u)) {
              const float panIn = std::clamp(u.pan + (ul != nullptr ? ul->pan : 0.0F) + (routed(ModDestination::Pan) ? controllerAmount(u, ch, ModDestination::Pan) : 0.0F), -1.0F, 1.0F);
              const float levelDb = u.levelDb + (ul != nullptr ? ul->levelDb : 0.0F) + (routed(ModDestination::LevelDb) ? std::clamp(controllerAmount(u, ch, ModDestination::LevelDb), -96.0F, 24.0F) : 0.0F);
              const float angle = (panIn + 1.0F) * static_cast<float>(kPi * 0.25);
              const float level = dbToGain(levelDb);
              targetL = level * std::cos(angle); targetR = level * std::sin(angle);
          }
          auto& outL = impl_->chunkL[ui][si];
          auto& outR = impl_->chunkR[ui][si];
          for (std::uint32_t i = 0U; i < n; ++i) {
              b.mixL += (targetL - b.mixL) * mixSmooth;
              b.mixR += (targetR - b.mixR) * mixSmooth;
              const float y = b.string.render();
              outL[i] += y * b.mixL;
              outR[i] += y * b.mixR;
          }
          busLive[ui][si] = true;
          // a lifted string that has died away frees the voice
          if (!b.string.playing() && b.string.level() < 1.0e-4) { b.quietFrames += n; if (b.quietFrames > static_cast<std::uint32_t>(0.25 * impl_->sampleRate)) { b.active = false; b.count = 0U; b.sustained = false; b.string.reset(); } }
          else b.quietFrames = 0U;
        }
      }
      // Send amounts of every Upper (once per chunk).
      std::array<std::array<float, kFxSendSlots>, kUpperCount> sendAmt{};
      for (std::size_t index = 0; index < static_cast<std::size_t>(kUpperCount); ++index) {
          const auto& u = current->combi.uppers[index];
          const auto& upperChannel = impl_->channels[static_cast<std::size_t>(u.midiChannel & 0x0FU)];
          const std::uint16_t upperMask = impl_->upperRouteMask[index];
          const std::array<float, kFxSendSlots> base{{u.fxSend1, u.fxSend2, u.fxSend3, u.fxSend4}};
          const std::array<ModDestination, kFxSendSlots> dest{{ModDestination::FxSend1, ModDestination::FxSend2, ModDestination::FxSend3, ModDestination::FxSend4}};
          for (std::size_t s = 0; s < static_cast<std::size_t>(kFxSendSlots); ++s) {
              const bool routed = (upperMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(dest[s]))) != 0U;
              sendAmt[index][s] = std::clamp(base[s] + (routed ? controllerAmount(u, upperChannel, dest[s]) : 0.0F), 0.0F, 1.0F);
          }
      }

      for (std::uint32_t ci = 0U; ci < n; ++ci) {
        const std::uint32_t i = chunkStart + ci;
        float dryL = 0.0F, dryR = 0.0F;
        std::array<float, kFxSendSlots> sendL{}, sendR{};
        for (std::size_t index = 0; index < static_cast<std::size_t>(kUpperCount); ++index) {
            // SILVETON_TONE_V2: every sound (basic + layers) through its HP + 3-band EQ, then summed
            float l = 0.0F, r = 0.0F;
            for (std::size_t s = 0; s < static_cast<std::size_t>(kSoundsPerUpper); ++s) {
                auto& bus = impl_->soundBus[index][s];
                if (!busLive[index][s] && !bus.any()) continue;
                float sl = impl_->chunkL[index][s][ci];
                float sr = impl_->chunkR[index][s][ci];
                if (bus.any()) bus.process(sl, sr);
                l += sl; r += sr;
            }
            // SILVETON_ANALYZER_V1: before the EQ
            const bool tapped = static_cast<int>(index) == tapUpperNow;
            if (tapped && !tapAfterNow) { impl_->tapRing[tapW] = 0.5F * (l + r); tapW = (tapW + 1U) % static_cast<std::uint32_t>(SamplerEngine::kAnalyzerRing); }
            // SILVETON_EQ5_V1: common EQ of the Upper
            impl_->upperEq[index].process(l, r);
            // SILVETON_FX_V3: INSERT of the Upper (wet = mix)
            const EffectSlot& ins = current->combi.insert[index];
            if (ins.type != EffectType::Off) {
                float wl = 0.0F, wr = 0.0F;
                impl_->insertFx[index].process(l, r, ins, wl, wr);
                const float mix = clamp01(ins.wet);
                if (ins.type == EffectType::Compressor || ins.type == EffectType::Drive) { l = wl; r = wr; }   // whole signal
                else { l += (wl - l) * mix; r += (wr - r) * mix; }
            }
            if (tapped && tapAfterNow) { impl_->tapRing[tapW] = 0.5F * (l + r); tapW = (tapW + 1U) % static_cast<std::uint32_t>(SamplerEngine::kAnalyzerRing); }
            upperPeakL[index] = std::max(upperPeakL[index], std::abs(l));
            upperPeakR[index] = std::max(upperPeakR[index], std::abs(r));
            dryL += l;
            dryR += r;
            for (std::size_t s = 0; s < static_cast<std::size_t>(kFxSendSlots); ++s) {
                const float a = sendAmt[index][s];
                if (a > 0.0F) { sendL[s] += l * a; sendR[s] += r * a; }
            }
        }

        const auto runFx = [&](std::size_t slot, std::uint8_t inst, const EffectSlot& fx, float inL, float inR, float& wetL, float& wetR) noexcept {
            impl_->sendFx[slot][inst & 1U].process(inL, inR, fx, wetL, wetR);
        };
        float l = dryL, r = dryR;
        float prevWetL = 0.0F, prevWetR = 0.0F;
        for (std::size_t slot = 0; slot < static_cast<std::size_t>(kFxSendSlots); ++slot) {
            const EffectSlot& fx = current->combi.fx[slot];
            float inL = sendL[slot], inR = sendR[slot];
            if (slot == 1U && current->combi.fxRouting == EffectRouting::Fx1IntoFx2) {
                const float chainGain = clamp01(current->combi.fx[0].wet);
                inL += prevWetL * chainGain; inR += prevWetR * chainGain;
            }
            float wl = 0.0F, wr = 0.0F;
            if (fx.type != EffectType::Off) runFx(slot, impl_->fxInst[slot], fx, inL, inR, wl, wr);
            const float w = clamp01(fx.wet);
            l += wl * w; r += wr * w;
            prevWetL = wl; prevWetR = wr;
        }
        // SILVETON_FX_TAIL_V1: the effect of the previous BLEND rings out by itself.
        for (std::size_t slot = 0; slot < static_cast<std::size_t>(kFxSendSlots); ++slot) {
            if (impl_->tailLeft[slot] == 0U) continue;
            float tl = 0.0F, tr = 0.0F;
            runFx(slot, impl_->tailInst[slot], impl_->tailCfg[slot], 0.0F, 0.0F, tl, tr);
            const float w = clamp01(impl_->tailCfg[slot].wet);
            float fade = 1.0F;
            if (impl_->tailLeft[slot] < tailFadeFrames) fade = static_cast<float>(impl_->tailLeft[slot]) / static_cast<float>(tailFadeFrames);
            l += tl * w * fade;
            r += tr * w * fade;
            if (std::abs(tl) + std::abs(tr) < 2.0e-5F) ++impl_->tailQuiet[slot]; else impl_->tailQuiet[slot] = 0U;
            --impl_->tailLeft[slot];
            if (impl_->tailQuiet[slot] > tailQuietFrames) impl_->tailLeft[slot] = 0U;
        }

        if (current->global.eqLowDb != 0.0F) impl_->lowEq.process(l, r);
        if (current->global.eqMidDb != 0.0F) impl_->midEq.process(l, r);
        if (current->global.eqHighDb != 0.0F) impl_->highEq.process(l, r);
        // SILVETON_MASTER_6DB_V1: no limiter in the sound - MASTER (default -6 dB) sets the level.
        l *= masterGain; r *= masterGain;

        if (!std::isfinite(l)) l = 0.0F;
        if (!std::isfinite(r)) r = 0.0F;
        out[static_cast<std::size_t>(i) * 2U] = l;
        out[static_cast<std::size_t>(i) * 2U + 1U] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
      }
    }

    for (std::size_t vi = 0; vi < impl_->voices.size(); ++vi) publishVoiceWindows(*impl_, vi);

    if (tapUpperNow >= 0) impl_->tapWrite.store(tapW, std::memory_order_release);
    impl_->peakL = peakL;
    impl_->peakR = peakR;
    impl_->limiterGrDb = 0.0F;
    impl_->upperPeakL = upperPeakL;
    impl_->upperPeakR = upperPeakR;

    // SILVETON_CPU_METER_V1: DSP time of this block as a share of its length,
    // held at its peak and let down slowly so the display is readable.
    {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - renderStart).count();
        const double blockSec = static_cast<double>(frames) / impl_->sampleRate;
        const float now = static_cast<float>(std::clamp(elapsed / std::max(1.0e-9, blockSec) * 100.0, 0.0, 999.0));
        impl_->cpuLoadHold = std::max(now, impl_->cpuLoadHold * (1.0F - static_cast<float>(blockSec) * 2.0F));
        impl_->cpuLoad.store(impl_->cpuLoadHold, std::memory_order_relaxed);
    }
}

MeterSnapshot SamplerEngine::meters() const noexcept {
    MeterSnapshot out{};
    out.peakLeft = impl_->peakL;
    out.peakRight = impl_->peakR;
    out.limiterGainReductionDb = impl_->limiterGrDb;
    out.activeVoices = activeVoiceCount();
    out.droppedEvents = events_.droppedCount();
    out.upperPeakLeft = impl_->upperPeakL;
    out.upperPeakRight = impl_->upperPeakR;
    out.cpuLoadPercent = impl_->cpuLoad.load(std::memory_order_relaxed);
    return out;
}

std::uint32_t SamplerEngine::activeVoiceCount() const noexcept {
    // A voice that is fading out after a takeover (MONO/LEGATO, voice limit)
    // is no longer a playing note, so it is not counted.
    std::uint32_t count = 0U;
    for (const auto& v : impl_->voices) if (v.active && !v.fading) ++count;
    return count;
}

} // namespace silveton::sampler
