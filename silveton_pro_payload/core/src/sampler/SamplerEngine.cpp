#include "silveton/sampler/SamplerEngine.h"
#include "silveton/sampler/SampleStream.h"
// SILVETON_CRACKLE_FIX_V1
// SILVETON_PRO_SOUND_V1

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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

// Values of one Upper's TONE page for one PROGRAM layer. Unset (negative)
// Upper values fall back to the layer envelope times the Easy Edit scales.
UpperToneValues effectiveToneFor(const UpperPart& u, const EnvelopeParams& env) noexcept {
    UpperToneValues t;
    const auto ms = [](float sec, float scale) noexcept {
        const float v = sec * std::clamp(scale, 0.0F, 16.0F) * 1000.0F;
        return std::isfinite(v) ? std::clamp(v, 0.0F, kMaxFallbackMs) : 0.0F;
    };
    t.attackMs = u.toneAttackMs >= 0.0F ? std::min(u.toneAttackMs, kToneMaxTimeMs) : ms(env.attackSec, u.ampAttackScale);
    t.decayMs = u.toneDecayMs >= 0.0F ? std::min(u.toneDecayMs, kToneMaxTimeMs) : ms(env.decaySec, u.ampDecayScale);
    t.releaseMs = u.toneReleaseMs >= 0.0F ? std::min(u.toneReleaseMs, kToneMaxTimeMs) : ms(env.releaseSec, u.ampReleaseScale);
    t.sustain = u.toneSustain >= 0.0F ? std::min(u.toneSustain, 1.0F)
                                      : std::clamp(env.sustain * std::clamp(u.ampSustainScale, 0.0F, 2.0F), 0.0F, 1.0F);
    t.cutoffHz = u.toneCutoffHz >= 0.0F ? std::clamp(u.toneCutoffHz, 20.0F, kToneCutoffOpenHz) : kToneCutoffOpenHz;
    t.resonance = u.toneResonance >= 0.0F ? std::min(u.toneResonance, 1.0F) : 0.0F;
    return t;
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

// SILVETON_FX_V2: DELAY with TIME changes that glide instead of clicking and a
// TONE low-pass in the repeats (1 = bright, 0 = dark).
struct DelayFx {
    static constexpr std::size_t kMaxFrames = 192000U * 3U;
    std::array<float, kMaxFrames> left{};
    std::array<float, kMaxFrames> right{};
    std::size_t write{0U};
    double sampleRate{48000.0};
    double currentDelay{-1.0};
    double timeSmooth{0.0};
    float toneL{0.0F}, toneR{0.0F};

    void reset(double sr) noexcept {
        sampleRate = sr;
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        write = 0U;
        currentDelay = -1.0;
        timeSmooth = 1.0 - std::exp(-1.0 / (0.060 * std::max(1.0, sr)));
        toneL = toneR = 0.0F;
    }

    float tap(const std::array<float, kMaxFrames>& b, double delayFrames) const noexcept {
        double rp = static_cast<double>(write) - delayFrames;
        while (rp < 0.0) rp += static_cast<double>(kMaxFrames);
        const auto i0 = static_cast<std::size_t>(rp) % kMaxFrames;
        const auto i1 = (i0 + 1U) % kMaxFrames;
        const float frac = static_cast<float>(rp - std::floor(rp));
        return b[i0] + (b[i1] - b[i0]) * frac;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const double ms = std::clamp(static_cast<double>(p.timeMs), 1.0, 2500.0);
        const double target = std::clamp(sampleRate * ms / 1000.0, 1.0, static_cast<double>(kMaxFrames - 2U));
        if (currentDelay < 0.0) currentDelay = target;
        else currentDelay += (target - currentDelay) * timeSmooth;
        const float dl = tap(left, currentDelay);
        const float dr = tap(right, currentDelay);
        const float t = std::clamp(p.tone, 0.0F, 1.0F);
        const float a = 0.08F + 0.92F * t * t;
        toneL += (dl - toneL) * a;
        toneR += (dr - toneR) * a;
        const float fb = std::clamp(p.feedback, 0.0F, 0.95F);
        left[write] = inL + toneL * fb;
        right[write] = inR + toneR * fb;
        write = (write + 1U) % kMaxFrames;
        wetL = toneL;
        wetR = toneR;
    }
};

struct ChorusFx {
    static constexpr std::size_t kMaxFrames = 192000U / 5U;
    std::array<float, kMaxFrames> left{};
    std::array<float, kMaxFrames> right{};
    std::size_t write{0U};
    double phase{0.0};
    double sampleRate{48000.0};

    void reset(double sr) noexcept {
        sampleRate = sr;
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        write = 0U;
        phase = 0.0;
    }

    float interp(const std::array<float, kMaxFrames>& b, double delayFrames) const noexcept {
        double rp = static_cast<double>(write) - delayFrames;
        while (rp < 0.0) rp += static_cast<double>(kMaxFrames);
        const auto i0 = static_cast<std::size_t>(rp) % kMaxFrames;
        const auto i1 = (i0 + 1U) % kMaxFrames;
        const float frac = static_cast<float>(rp - std::floor(rp));
        return b[i0] + (b[i1] - b[i0]) * frac;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        left[write] = inL;
        right[write] = inR;
        const double base = sampleRate * 0.012;
        const double depth = sampleRate * 0.008 * static_cast<double>(std::clamp(p.depth, 0.0F, 1.0F));
        const double lfoL = 0.5 + 0.5 * std::sin(2.0 * kPi * phase);
        const double lfoR = 0.5 + 0.5 * std::sin(2.0 * kPi * (phase + 0.25));
        wetL = interp(left, base + depth * lfoL);
        wetR = interp(right, base + depth * lfoR);
        write = (write + 1U) % kMaxFrames;
        phase += static_cast<double>(std::clamp(p.rateHz, 0.05F, 8.0F)) / sampleRate;
        phase -= std::floor(phase);
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
    FxLine<2048U> pre{};
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
        lPre = sc(0.010 * kBaseRate, 2048U);
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
        const float tone = std::clamp(p.tone, 0.0F, 1.0F);
        const float damping = 0.0005F + (1.0F - tone) * 0.70F;

        float x = delay(pre, lPre, (inL + inR) * 0.5F);
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

// SILVETON_SMOOTH_LIMITER_V1: zero-latency limiter (no added delay).
// When a sample would pass the ceiling the gain drops at once to exactly what
// that sample needs, then comes back up slowly (80 ms). Unlike the old limiter
// it does not jump back to full gain on the next sample, so the waveform is
// not cut flat (no clipping distortion), and nothing is delayed.
struct SmoothLimiter {
    float gain{1.0F};
    float releaseCoef{0.0F};

    void reset(double sr) noexcept {
        gain = 1.0F;
        releaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (0.080 * std::max(1.0, sr))));
    }

    // Returns the gain that was applied (for the meter).
    float process(float& l, float& r, float ceiling) noexcept {
        const float peak = std::max(std::abs(l), std::abs(r));
        const float need = (peak > ceiling && peak > 0.0F) ? ceiling / peak : 1.0F;
        gain += (1.0F - gain) * releaseCoef;
        if (need < gain) gain = need;
        l *= gain; r *= gain;
        return gain;
    }
};

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
        double glideSmooth2{0.0};      // BALANCE rounding, stage 2 = what is heard
        bool pitchGliding{false};
        // Click-free 4 ms fade used when a voice is taken over (voice limit,
        // MONO/LEGATO note change) instead of a one-sample cut.
        float fadeGain{1.0F};
        float fadeStep{0.0F};
        bool fading{false};
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
    float peakL{0.0F};
    float peakR{0.0F};
    float limiterGrDb{0.0F};
    SmoothLimiter limiter{};
    std::array<float, kUpperCount> upperPeakL{};
    std::array<float, kUpperCount> upperPeakR{};
    std::array<std::uint16_t, kUpperCount> upperRouteMask{};
    Biquad lowEq{}, midEq{}, highEq{};
    std::array<Biquad, kUpperCount> upperLowEq{};
    std::array<Biquad, kUpperCount> upperMidEq{};
    std::array<Biquad, kUpperCount> upperHighEq{};
    // Per-Upper mix of one chunk (voices are rendered voice by voice).
    static constexpr std::uint32_t kRenderChunk = 256U;
    std::array<std::array<float, kRenderChunk>, kUpperCount> chunkL{};
    std::array<std::array<float, kRenderChunk>, kUpperCount> chunkR{};
    // One effect engine per FX slot, so FX1 and FX2 of the same type do not share state.
    std::array<DelayFx, 2U> delay{};
    std::array<ChorusFx, 2U> chorus{};
    std::array<ReverbFx, 2U> reverb{};
    // SILVETON_FX_TAIL_V1: a second effect engine per slot. When another BLEND is
    // called up, the old effect keeps ringing out (reverb tail, delay repeats) on
    // one engine while the new effect starts on the other - no cut, no restart.
    std::array<DelayFx, 2U> delayB{};
    std::array<ChorusFx, 2U> chorusB{};
    std::array<ReverbFx, 2U> reverbB{};
    std::array<std::uint8_t, 2U> fxInst{};        // engine the slot plays now (0 = A, 1 = B)
    std::array<EffectSlot, 2U> fxRunning{};       // settings of the last block
    bool fxRunningValid{false};
    std::uint32_t fxGeneration{0U};
    std::array<EffectSlot, 2U> tailCfg{};
    std::array<std::uint8_t, 2U> tailInst{};
    std::array<std::uint32_t, 2U> tailLeft{};     // frames the tail may still ring (0 = no tail)
    std::array<std::uint32_t, 2U> tailQuiet{};    // frames the tail has been silent

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
        limiter.reset(sr);
        upperPeakL.fill(0.0F);
        upperPeakR.fill(0.0F);
        upperRouteMask.fill(0U);
        for (auto& c : channels) c = ChannelState{};
        for (auto& m : mono) m = MonoKeys{};
        monoWas.fill(false);
        for (auto& v : voices) v = Voice{};
        lowEq.reset(); midEq.reset(); highEq.reset();
        for (auto& q : upperLowEq) q.reset();
        for (auto& q : upperMidEq) q.reset();
        for (auto& q : upperHighEq) q.reset();
        for (std::size_t k = 0; k < 2U; ++k) { delay[k].reset(sr); chorus[k].reset(sr); reverb[k].reset(sr); }
        for (std::size_t k = 0; k < 2U; ++k) { delayB[k].reset(sr); chorusB[k].reset(sr); reverbB[k].reset(sr); }
        fxInst.fill(0U); tailLeft.fill(0U); tailQuiet.fill(0U); fxRunningValid = false; fxGeneration = 0U;
        clearWindows();
    }
};

UpperToneValues effectiveUpperTone(const UpperPart& upper) noexcept {
    EnvelopeParams env{};
    if (upper.program != nullptr && !upper.program->layers.empty()) env = upper.program->layers.front().ampEnvelope;
    return effectiveToneFor(upper, env);
}

SamplerEngine::SamplerEngine()
    : impl_(std::make_unique<Impl>()), state_(std::make_shared<EngineState>()), interpolator_(InterpolationQuality::Studio) { // SILVETON_STUDIO_INTERP_V1
    prepare(48000.0, 512U);
}
SamplerEngine::~SamplerEngine() = default;

void SamplerEngine::prepare(double sampleRate, std::uint32_t maximumBlockFrames) {
    const double sr = std::clamp(sampleRate, 8000.0, 384000.0);
    impl_->maxBlock = std::max<std::uint32_t>(16U, maximumBlockFrames);
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
    // All 128 voices sound: take the oldest one, exactly as before, but instead
    // of cutting it in one sample, its sound continues in a spare tail slot and
    // fades out in 4 ms. Copying a voice copies pointers only (no allocation).
    auto* oldest = &impl.voices[0];
    for (std::size_t i = 0; i < kPlayable; ++i) if (impl.voices[i].age < oldest->age) oldest = &impl.voices[i];
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
std::uint8_t playKeyFor(const UpperPart& upper, const GlobalState& global, std::uint8_t key) noexcept {
    const int k = static_cast<int>(key) + static_cast<int>(upper.transposeSemitones) +
                  static_cast<int>(upper.octave) * 12 + static_cast<int>(global.transposeSemitones);
    return static_cast<std::uint8_t>(std::clamp(k, 0, 127));
}

double noteRatio(const SamplerEngine::Impl& impl, const UpperPart& upper, const ProgramLayer& layer,
                 const SampleZone& zone, std::uint8_t key, const GlobalState& global, int ui) noexcept {
    // SILVETON_OCTAVE_KEY_V1: OCTAVE / TRANSPOSE move the KEY (like Korg): the
    // sample recorded for the new key is chosen and played at its own pitch,
    // instead of stretching the sample of the pressed key.
    const std::uint8_t pk = playKeyFor(upper, global, key);
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
    const float cents = zone.tuneCents + layer.tuneCents + upper.tuneCents + zone.sample->fineTuneCents + scaleCents;
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

double liveTuneSemis(const UpperPart& upper, const GlobalState& global, std::uint8_t key, bool scaleOn) noexcept {
    // OCTAVE / TRANSPOSE are part of the key (SILVETON_OCTAVE_KEY_V1): they take
    // effect on the next key press; fine tune, MASTER TUNE and SCALE stay live.
    const int root = static_cast<int>(global.scaleRoot % 12U);
    const int pc = (static_cast<int>(key) - root + 120) % 12;   // SILVETON_TQ_TRANSPOSE_V1: played key
    const float scaleCents = scaleOn ? std::clamp(global.scaleOffsetsCents[static_cast<std::size_t>(pc)], -100.0F, 100.0F) : 0.0F;
    const double tuneHz = std::clamp(static_cast<double>(global.masterTuneHz), 400.0, 480.0);
    return static_cast<double>(upper.tuneCents + scaleCents) / 100.0 +
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
    const float panIn = v.zone->pan + v.layer->pan + upper.pan + extraPan;
    if (panIn == v.mixPanIn && upper.levelDb == v.mixLevelIn && audible == v.mixAudibleIn) return;
    v.mixPanIn = panIn;
    v.mixLevelIn = upper.levelDb;
    v.mixAudibleIn = audible;
    if (!audible) { v.mixTargetL = v.mixTargetR = 0.0F; return; }
    const float pan = std::clamp(panIn, -1.0F, 1.0F);
    const float angle = (pan + 1.0F) * static_cast<float>(kPi * 0.25);
    const float level = dbToGain(upper.levelDb);
    v.mixTargetL = level * std::cos(angle);
    v.mixTargetR = level * std::sin(angle);
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
                 const std::shared_ptr<const EngineState>& ownerState) noexcept {
    v = SamplerEngine::Impl::Voice{};
    v.active = true;
    v.keyHeld = true;
    v.sustained = false;
    v.age = ++impl.voiceAge;
    v.channel = channel;
    v.key = key;
    v.velocity = velocity;
    v.upperIndex = upperIndex;
    v.routeMask = impl.upperRouteMask[static_cast<std::size_t>(std::clamp(upperIndex, 0, kUpperCount - 1))];
    v.sample = zone.sample;
    v.ownerState = ownerState;
    v.layer = &layer;
    v.zone = &zone;
    v.upper = &upper;
    v.position = zone.reverse && zone.sample != nullptr ? static_cast<double>(zone.sample->frameCount() - 1U) : 0.0;
    v.loopWrapped = false;

    v.ratio = noteRatio(impl, upper, layer, zone, key, global, upperIndex);
    v.targetRatio = v.ratio;
    v.pitchGliding = false;
    v.glideFramesRemaining = 0U;
    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = 0.0;

    const float velocityNorm = std::max(0.0F, static_cast<float>(velocity) * kInv127);
    float velGain = velocityNorm;
    switch (global.velocityCurve) {
        case 1U: velGain = std::sqrt(velocityNorm); break;
        case 2U: velGain = velocityNorm * velocityNorm; break;
        case 3U: velGain = 1.0F; break;
        default: break;
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
    v.tuneSemis = liveTuneSemis(upper, global, key, ((impl.scaleUppers.load(std::memory_order_relaxed) >> upperIndex) & 1) != 0);
    v.filterEnv.params = layer.filterEnvelope;
    v.pitchEnv.params = layer.pitchEnvelope;
    v.ampEnv = AmpEnv{};
    v.ampEnv.setBlock(effectiveToneFor(upper, layer.ampEnvelope), impl.sampleRate);
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

float portamentoSecFor(const UpperPart& upper) noexcept {
    return std::clamp(upper.portamentoSec, 0.0F, kPortamentoMaxSec);
}

// SILVETON_POLY_PORTA_V1: POLY stays POLY with portamento ON (chords keep sounding,
// every new key glides from the key played before it, like Korg Pa).
UpperVoiceMode effectiveVoiceMode(const UpperPart& upper) noexcept {
    return upper.voiceMode;
}

// SILVETON_PORTA_NEAR_V1: far intervals glide in TIME (as before); neighbouring
// keys take longer and softer: 1 semitone = NEAR, 2 = 2/3, 3 = 1/3 of the way.
float glideSecFor(const SamplerEngine::Impl& impl, const UpperPart& upper, double fromKey, double toKey) noexcept {
    const float base = portamentoSecFor(upper);
    if (base <= 0.0F || fromKey < 0.0) return base;
    const double d = std::abs(toKey - fromKey);
    const double w = d <= 1.0 ? 1.0 : (d >= 4.0 ? 0.0 : (4.0 - d) / 3.0);
    const float nearSec = impl.portaNearMs.load(std::memory_order_relaxed) / 1000.0F;
    return nearSec > base ? base + static_cast<float>(w) * (nearSec - base) : base;
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

// SILVETON_SOFT_GLIDE_V1: every glide moves on a soft S-curve (slow start, slow
// end) in exactly TIME, and PORTA BALANCE (3..40 ms) rounds the pitch further, so
// neighbouring notes slide smoothly and a new glide that starts mid-glide carries
// on without a corner. The pitch is a continuous offset from the new key.
void beginGlide(SamplerEngine::Impl::Voice& v, double newTargetRatio, std::uint32_t frames) noexcept {
    const double oldTarget = v.targetRatio > 0.0 ? v.targetRatio : newTargetRatio;
    if (!v.pitchGliding) {
        const double now = (v.ratio > 0.0 && oldTarget > 0.0) ? 12.0 * std::log2(v.ratio / oldTarget) : 0.0;
        v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = now;
    }
    const double shift = 12.0 * std::log2(oldTarget / newTargetRatio);
    v.glideRawSemis += shift;
    v.glideSmooth1 += shift;
    v.glideSmooth2 += shift;
    v.glideStartSemis = v.glideRawSemis;
    v.glideTotalFrames = std::max<std::uint32_t>(1U, frames);
    v.glideFramesRemaining = v.glideTotalFrames;
    v.targetRatio = newTargetRatio;
    v.pitchGliding = true;
    v.ratio = v.targetRatio * std::exp2(v.glideSmooth2 / 12.0);
}

void startPortamentoGlide(const SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, double fromKey, float seconds) noexcept {
    if (seconds <= 0.0F || fromKey < 0.0 || v.ratio <= 0.0) return;
    if (v.zone != nullptr && v.zone->nonTranspose) return; // fixed-pitch sample: nothing to glide
    const double semitones = fromKey - static_cast<double>(v.key);
    if (std::abs(semitones) < 1.0e-6) return;
    v.targetRatio = v.ratio;
    v.glideRawSemis = v.glideSmooth1 = v.glideSmooth2 = semitones;   // starts on the old key
    v.pitchGliding = true;
    v.ratio = v.targetRatio * std::exp2(semitones / 12.0);
    const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(seconds) * impl.sampleRate));
    beginGlide(v, v.targetRatio, frames);
}

// Same sound keeps playing; only its pitch moves to the new key (true glissando).
void legatoTo(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, const UpperPart& upper,
              const ProgramLayer& layer, const SampleZone& zone, std::uint8_t key, const GlobalState& global,
              float glideSec) noexcept {
    const double target = noteRatio(impl, upper, layer, zone, key, global, v.upperIndex);
    v.key = key;
    v.tuneSemis = liveTuneSemis(upper, global, key, ((impl.scaleUppers.load(std::memory_order_relaxed) >> v.upperIndex) & 1) != 0);
    const float portamentoSec = glideSec;
    if (portamentoSec > 0.0F && v.ratio > 0.0 && target > 0.0) {
        const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(portamentoSec) * impl.sampleRate));
        beginGlide(v, target, frames);
    } else {
        v.targetRatio = target;
        v.ratio = target;
        v.glideFramesRemaining = 0U;
        v.pitchGliding = false;
    }
}

void playUpperNote(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, int ui,
                   const UpperPart& upper, std::uint8_t channel, std::uint8_t key, std::uint8_t velocity,
                   double glideFromKey = -1.0, float glideSec = 0.0F) noexcept {
    const auto& global = stateOwner->global;
    const auto& program = *upper.program;
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
            for (const auto& layer : program.layers) {
                if (!layer.matches(playKeyFor(upper, global, key), velocity)) continue;
                for (const auto& zone : layer.multisample->zones) {
                    if (!zone.matches(playKeyFor(upper, global, key), velocity) || !zone.sample->valid()) continue;
                    for (auto& existing : impl.voices) {
                        if (existing.active && !existing.fading && existing.upperIndex == ui && existing.keyHeld &&
                            existing.delayFramesRemaining == 0U && !existing.legatoKeep &&
                            existing.layer == &layer && existing.zone == &zone) {
                            legatoTo(impl, existing, upper, layer, zone, key, global, 0.0F);
                            existing.legatoKeep = true;
                            break;
                        }
                    }
                }
            }
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
    for (const auto& layer : program.layers) {
        if (!layer.matches(playKeyFor(upper, global, key), velocity)) continue;
        for (const auto& zone : layer.multisample->zones) {
            if (!zone.matches(playKeyFor(upper, global, key), velocity) || !zone.sample->valid()) continue;
            if (upperLegato && hadHeldVoice) {
                bool kept = false;
                for (const auto& existing : impl.voices) {
                    if (existing.active && existing.legatoKeep && existing.upperIndex == ui &&
                        existing.layer == &layer && existing.zone == &zone) { kept = true; break; }
                }
                if (kept) continue;
            }
            auto* v = chooseVoice(impl);
            startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner);
            v->mono = upperMono;
            startPortamentoGlide(impl, *v, glideFromKey, glideSec);
            if (upperLegato && hadHeldVoice) {
                v->ampEnv.value = previousAmp;
                v->ampEnv.stage = AmpEnv::Stage::Decay;
                v->fadeGain = 0.0F;
                v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kLegatoCrossfadeSec) * impl.sampleRate));
            }
        }
    }
    if (upperMono) for (auto& existing : impl.voices) if (existing.upperIndex == ui) existing.legatoKeep = false;
}

// SILVETON_PORTA_PAGE_V1: one note of an Upper whose portamento is ON.
// keyWasHeld = another key of this Upper was held before this note;
// previousKey = that key (used only if its recording already ended).
void playPortamentoNote(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, int ui,
                        const UpperPart& upper, std::uint8_t channel, std::uint8_t key, std::uint8_t velocity,
                        bool keyWasHeld, int previousKey) noexcept {
    if (upper.program == nullptr) return;
    const auto& global = stateOwner->global;
    const auto& program = *upper.program;
    float glideSec = portamentoSecFor(upper);
    SamplerEngine::Impl::Voice* sounding = nullptr;   // the voice of the held line
    for (auto& v : impl.voices) {
        if (!v.active || v.fading || v.upperIndex != ui || !v.keyHeld || v.delayFramesRemaining > 0U) continue;
        if (sounding == nullptr || v.age > sounding->age) sounding = &v;
    }
    if (!keyWasHeld || sounding == nullptr) {
        // Nothing held: struck normally, no glide (a held line whose recording
        // already ended starts again and glides from the previous key).
        const double fromKey = (keyWasHeld && previousKey >= 0) ? static_cast<double>(previousKey) : -1.0;
        glideSec = glideSecFor(impl, upper, fromKey, static_cast<double>(key));
        for (const auto& layer : program.layers) {
            if (!layer.matches(playKeyFor(upper, global, key), velocity)) continue;
            for (const auto& zone : layer.multisample->zones) {
                if (!zone.matches(playKeyFor(upper, global, key), velocity) || !zone.sample->valid()) continue;
                auto* v = chooseVoice(impl);
                startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner);
                v->mono = true;
                startPortamentoGlide(impl, *v, fromKey, glideSec);
            }
        }
        return;
    }
    // A key is held: one continuous sound glides to the new key in exactly TIME.
    const double fromPitch = static_cast<double>(sounding->key) + 12.0 * std::log2(sounding->ratio / sounding->targetRatio);
    glideSec = glideSecFor(impl, upper, fromPitch, static_cast<double>(key));
    const float previousAmp = sounding->ampEnv.value;
    for (auto& v : impl.voices) if (v.upperIndex == ui) v.legatoKeep = false;
    for (const auto& layer : program.layers) {
        if (!layer.matches(playKeyFor(upper, global, key), velocity)) continue;
        for (const auto& zone : layer.multisample->zones) {
            if (!zone.matches(playKeyFor(upper, global, key), velocity) || !zone.sample->valid()) continue;
            SamplerEngine::Impl::Voice* keep = nullptr;
            for (auto& v : impl.voices) {
                if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && v.delayFramesRemaining == 0U &&
                    !v.legatoKeep && v.layer == &layer && v.zone == &zone) { keep = &v; break; }
            }
            if (keep != nullptr) {
                // the same recording keeps sounding and only moves its pitch
                legatoTo(impl, *keep, upper, layer, zone, key, global, glideSec);
                keep->legatoKeep = true;
                continue;
            }
            // another recording takes over without a new strike and glides from the pitch that sounded
            auto* v = chooseVoice(impl);
            startVoice(impl, *v, upper, layer, zone, ui, channel, key, velocity, global, stateOwner);
            v->mono = true;
            startPortamentoGlide(impl, *v, fromPitch, glideSec);
            v->ampEnv.value = previousAmp;
            v->ampEnv.stage = AmpEnv::Stage::Decay;
            v->fadeGain = 0.0F;
            v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kLegatoCrossfadeSec) * impl.sampleRate));
            v->legatoKeep = true;
        }
    }
    for (auto& v : impl.voices) {
        if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && !v.legatoKeep) softKill(impl, v, kLegatoCrossfadeSec);
    }
    for (auto& v : impl.voices) if (v.upperIndex == ui) v.legatoKeep = false;
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
        if (upper.midiChannel != (channel & 0x0FU) || voiceModeNow(impl, ui, upper) == UpperVoiceMode::Poly) continue;
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
        if (mk.count > 0U && upper.program != nullptr) {
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
                    const bool sameChord = impl.frameClock - impl.polyLastOn[u] < static_cast<std::uint64_t>(0.030 * impl.sampleRate);
                    if (!sameChord) impl.polyAnchorKey[u] = held ? impl.polyLastKey[u] : -1;
                    const double from = impl.polyAnchorKey[u];
                    impl.polyLastKey[u] = key;
                    impl.polyLastOn[u] = impl.frameClock;
                    playUpperNote(impl, stateOwner, ui, upper, e.channel, key, e.data2,
                                  from, glideSecFor(impl, upper, from, static_cast<double>(key)));
                    continue;
                }
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
                if (old && !ch.sustain) releaseSustained(impl, e.channel);
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
        case EventType::Panic: panic(impl); break;
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
            impl_->upperLowEq[index].setShelf(impl_->sampleRate, 120.0F, u.eqLowDb, false);
            impl_->upperMidEq[index].setPeaking(impl_->sampleRate, 1200.0F, 0.75F, u.eqMidDb);
            impl_->upperHighEq[index].setShelf(impl_->sampleRate, 7000.0F, u.eqHighDb, true);
        }
    }

    const auto current = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (current == nullptr) return;

    RealtimeEvent ev{};
    while (events_.pop(ev)) processEvent(*impl_, current, ev);
    impl_->frameClock += frames;

    // Live TONE values (ADSR times, SUSTAIN, CUTOFF, RESONANCE) of the current Upper
    // state, evaluated once per block for every sounding voice.
    {
        const double blockAlpha = 1.0 - std::exp(-static_cast<double>(frames) / (0.012 * impl_->sampleRate));
        const float openLog = std::log(kToneCutoffOpenHz);
        for (auto& v : impl_->voices) {
            if (!v.active || v.layer == nullptr || v.upper == nullptr) continue;
            const auto uix = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
            // SILVETON_LIVE_EDIT_V1: the note follows the newest settings of its Upper
            // while that Upper still plays the same Sound.
            if (v.ownerState.get() != current.get()) {
                const auto& now = current->combi.uppers[uix];
                if (now.program != nullptr && v.upper->program.get() == now.program.get()) {
                    v.ownerState = current;
                    v.upper = &now;
                    v.routeMask = impl_->upperRouteMask[uix];
                }
            }
            const double tune = liveTuneSemis(*v.upper, current->global, v.key,
                ((impl_->scaleUppers.load(std::memory_order_relaxed) >> v.upperIndex) & 1) != 0);
            if (tune != v.tuneSemis) {
                const double m = std::exp2((tune - v.tuneSemis) / 12.0);
                v.ratio *= m;
                v.targetRatio *= m;
                v.tuneSemis = tune;
            }
            const UpperToneValues tv = effectiveToneFor(*v.upper, v.layer->ampEnvelope);
            v.ampEnv.setBlock(tv, impl_->sampleRate);
            const bool wantOn = tv.cutoffHz < kToneCutoffOpenHz - 1.0F;
            if (wantOn && !v.toneOn) { v.toneOn = true; v.toneLog = openLog; v.toneLp.prime = true; }
            if (v.toneOn) {
                const float target = wantOn ? std::log(tv.cutoffHz) : openLog;
                v.toneLog += (target - v.toneLog) * static_cast<float>(blockAlpha);
                if (!wantOn && v.toneLog >= openLog - 0.05F) v.toneOn = false;
                else v.toneLp.set(std::exp(v.toneLog), tv.resonance, impl_->sampleRate);
            }
        }
    }

    float peakL = 0.0F, peakR = 0.0F;
    float maxGr = 0.0F;
    std::array<float, kUpperCount> upperPeakL{};
    std::array<float, kUpperCount> upperPeakR{};
    const float masterGain = dbToGain(current->global.masterGainDb);
    const float mixSmooth = static_cast<float>(1.0 - std::exp(-1.0 / (kLiveMixSmoothSec * impl_->sampleRate)));
    const float limiterCeiling = dbToGain(current->global.limiterCeilingDb);
    // PORTA BALANCE (ms) -> two rounding stages, each half the balance time.
    const double glideRound = 1.0 - std::exp(-1.0 / (std::max(0.0005, 0.0005 * static_cast<double>(
        impl_->portaBalance.load(std::memory_order_relaxed))) * impl_->sampleRate));

    // SILVETON_FX_TAIL_V1: another BLEND was called up -> an effect that changes keeps
    // ringing out on its own engine; the new effect starts on the other one.
    const auto tailFadeFrames = static_cast<std::uint32_t>(0.25 * impl_->sampleRate);
    const auto tailQuietFrames = static_cast<std::uint32_t>(0.30 * impl_->sampleRate);
    {
        const EffectSlot* nowFx[2] = {&current->combi.fx1, &current->combi.fx2};
        if (impl_->fxRunningValid && current->combiGeneration != impl_->fxGeneration) {
            for (std::size_t slot = 0; slot < 2U; ++slot) {
                const EffectSlot& was = impl_->fxRunning[slot];
                const EffectSlot& now = *nowFx[slot];
                const bool same = was.type == now.type && was.wet == now.wet && was.timeMs == now.timeMs &&
                                  was.feedback == now.feedback && was.rateHz == now.rateHz &&
                                  was.depth == now.depth && was.tone == now.tone;
                if (same || was.type == EffectType::Off || was.wet <= 0.0F) continue;
                impl_->tailCfg[slot] = was;
                impl_->tailInst[slot] = impl_->fxInst[slot];
                impl_->tailLeft[slot] = static_cast<std::uint32_t>(20.0 * impl_->sampleRate);
                impl_->tailQuiet[slot] = 0U;
                impl_->fxInst[slot] = static_cast<std::uint8_t>(impl_->fxInst[slot] ^ 1U);
            }
        }
        impl_->fxGeneration = current->combiGeneration;
        impl_->fxRunning[0] = current->combi.fx1;
        impl_->fxRunning[1] = current->combi.fx2;
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
      for (auto& buf : impl_->chunkL) std::fill_n(buf.begin(), n, 0.0F);
      for (auto& buf : impl_->chunkR) std::fill_n(buf.begin(), n, 0.0F);

      for (auto& v : impl_->voices) {
        if (!v.active || v.sample == nullptr || v.layer == nullptr || v.zone == nullptr || v.upper == nullptr) continue;
        const auto& sample = *v.sample;
        const bool looping = sample.loop.enabled && !v.zone->oneShot && !v.zone->reverse;
        const auto& ch = impl_->channels[v.channel & 0x0FU];
        const auto ui = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
        const std::uint16_t routeMask = v.routeMask;
        const auto routeActive = [routeMask](ModDestination d) noexcept {
            return (routeMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(d))) != 0U;
        };
        auto& outL = impl_->chunkL[ui];
        auto& outR = impl_->chunkR[ui];

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
            pitchEnvNeeded || lfoPitch;
        const float bendNorm = static_cast<float>(ch.pitchBend) / 8192.0F;
        const float bendSemis = bendNorm * static_cast<float>(v.upper->pitchBendRange);
        const float controllerPitchSemis = routeActive(ModDestination::PitchSemitones)
            ? controllerAmount(*v.upper, ch, ModDestination::PitchSemitones) : 0.0F;
        const bool pitchPerSample = pitchEnvNeeded || lfoPitch;
        // Bend / controller only: the same factor for the whole block.
        const double staticPitchMul = (dynamicPitchActive && !pitchPerSample)
            ? std::pow(2.0, (static_cast<double>(bendSemis + controllerPitchSemis) + 0.0) / 12.0)
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
        updateMixTarget(v, *v.upper, upperAudible(*v.ownerState, *v.upper), controllerPan);
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
                if (v.glideFramesRemaining > 0U) {
                    --v.glideFramesRemaining;
                    const double t = 1.0 - static_cast<double>(v.glideFramesRemaining) / static_cast<double>(v.glideTotalFrames);
                    raw = v.glideStartSemis * (1.0 - t * t * (3.0 - 2.0 * t));
                }
                v.glideRawSemis = raw;
                v.glideSmooth1 += (raw - v.glideSmooth1) * glideRound;
                v.glideSmooth2 += (v.glideSmooth1 - v.glideSmooth2) * glideRound;
                if (v.glideFramesRemaining == 0U && std::abs(v.glideSmooth2) < 1.0e-6 && std::abs(v.glideSmooth1) < 1.0e-6) {
                    v.pitchGliding = false;
                    v.glideSmooth1 = v.glideSmooth2 = 0.0;
                    v.ratio = v.targetRatio;
                } else {
                    v.ratio = v.targetRatio * std::exp2(v.glideSmooth2 / 12.0);
                }
            }

            double ratio = v.ratio;
            if (dynamicPitchActive) {
                if (pitchPerSample) {
                    const float lfoPitchCents = lfo1 * lfo1Params.pitchCents + lfo2 * lfo2Params.pitchCents;
                    const float envPitchCents = pitchEnv * v.layer->pitchEnvelopeCents;
                    const double dynamicPitch = static_cast<double>(bendSemis + controllerPitchSemis) +
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

            if (v.toneOn) v.toneLp.process(sL, sR);

            float modGain = ampEnv;
            if (ampLfo) modGain *= std::max(0.0F, 1.0F + lfo1 * lfo1Params.ampDepth + lfo2 * lfo2Params.ampDepth);
            if (anyChannelGain) {
                if (v.upper->expressionEnabled) modGain *= ch.expression;
                if (v.upper->breathEnabled) modGain *= ch.breath;
                if (v.upper->aftertouchEnabled) modGain *= (0.5F + 0.5F * ch.pressure);
            }
            float g = v.baseGain * modGain;
            if (controllerLevelDb != 0.0F) g *= levelGain;
            if (v.fadeStep != 0.0F) {
                v.fadeGain += v.fadeStep;
                if (v.fadeGain <= 0.0F) { v.ampEnv.forceOff(); v.active = false; v.fading = false; break; }
                if (v.fadeGain >= 1.0F) { v.fadeGain = 1.0F; v.fadeStep = 0.0F; }
            }
            if (v.fadeStep != 0.0F || v.fadeGain < 1.0F) g *= std::sin(v.fadeGain * 1.57079632679F); // equal-power fade
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
        }
        if (mixMoving) {
            if (std::abs(mixTargetL - v.mixL) < 1.0e-6F) v.mixL = mixTargetL;
            if (std::abs(mixTargetR - v.mixR) < 1.0e-6F) v.mixR = mixTargetR;
        }
      }

      for (std::uint32_t ci = 0U; ci < n; ++ci) {
        const std::uint32_t i = chunkStart + ci;
        std::array<float, kUpperCount> upperL{};
        std::array<float, kUpperCount> upperR{};
        for (std::size_t u = 0; u < static_cast<std::size_t>(kUpperCount); ++u) {
            upperL[u] = impl_->chunkL[u][ci];
            upperR[u] = impl_->chunkR[u][ci];
        }

        float dryL = 0.0F, dryR = 0.0F;
        float send1L = 0.0F, send1R = 0.0F, send2L = 0.0F, send2R = 0.0F;
        for (int ui = 0; ui < kUpperCount; ++ui) {
            const auto index = static_cast<std::size_t>(ui);
            float l = upperL[index];
            float r = upperR[index];
            const auto& u = current->combi.uppers[index];
            if (u.eqLowDb != 0.0F) impl_->upperLowEq[index].process(l, r);
            if (u.eqMidDb != 0.0F) impl_->upperMidEq[index].process(l, r);
            if (u.eqHighDb != 0.0F) impl_->upperHighEq[index].process(l, r);
            upperPeakL[index] = std::max(upperPeakL[index], std::abs(l));
            upperPeakR[index] = std::max(upperPeakR[index], std::abs(r));
            dryL += l;
            dryR += r;
            const auto& upperChannel = impl_->channels[static_cast<std::size_t>(u.midiChannel & 0x0FU)];
            const std::uint16_t upperMask = impl_->upperRouteMask[index];
            const bool routeSend1 = (upperMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(ModDestination::FxSend1))) != 0U;
            const bool routeSend2 = (upperMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(ModDestination::FxSend2))) != 0U;
            const float s1 = std::clamp(u.fxSend1 + (routeSend1 ? controllerAmount(u, upperChannel, ModDestination::FxSend1) : 0.0F), 0.0F, 1.0F);
            const float s2 = std::clamp(u.fxSend2 + (routeSend2 ? controllerAmount(u, upperChannel, ModDestination::FxSend2) : 0.0F), 0.0F, 1.0F);
            send1L += l * s1; send1R += r * s1;
            send2L += l * s2; send2R += r * s2;
        }

        float fx1L = 0.0F, fx1R = 0.0F, fx2L = 0.0F, fx2R = 0.0F;
        const auto runFx = [&](std::size_t slot, std::uint8_t inst, const EffectSlot& fx, float inL, float inR, float& wetL, float& wetR) noexcept {
            switch (fx.type) {
                case EffectType::Off: wetL = wetR = 0.0F; break;
                case EffectType::Delay: (inst == 0U ? impl_->delay[slot] : impl_->delayB[slot]).process(inL, inR, fx, wetL, wetR); break;
                case EffectType::Chorus: (inst == 0U ? impl_->chorus[slot] : impl_->chorusB[slot]).process(inL, inR, fx, wetL, wetR); break;
                case EffectType::Reverb: (inst == 0U ? impl_->reverb[slot] : impl_->reverbB[slot]).process(inL, inR, fx, wetL, wetR); break;
            }
        };
        const auto processFx = [&](std::size_t slot, const EffectSlot& fx, float inL, float inR, float& wetL, float& wetR) noexcept {
            runFx(slot, impl_->fxInst[slot], fx, inL, inR, wetL, wetR);
        };
        processFx(0U, current->combi.fx1, send1L, send1R, fx1L, fx1R);
        if (current->combi.fxRouting == EffectRouting::Fx1IntoFx2) {
            const float chainGain = clamp01(current->combi.fx1.wet);
            processFx(1U, current->combi.fx2, send2L + fx1L * chainGain, send2R + fx1R * chainGain, fx2L, fx2R);
        } else {
            processFx(1U, current->combi.fx2, send2L, send2R, fx2L, fx2R);
        }
        float l = dryL + fx1L * clamp01(current->combi.fx1.wet) + fx2L * clamp01(current->combi.fx2.wet);
        float r = dryR + fx1R * clamp01(current->combi.fx1.wet) + fx2R * clamp01(current->combi.fx2.wet);
        // SILVETON_FX_TAIL_V1: the effect of the previous BLEND rings out by itself.
        for (std::size_t slot = 0; slot < 2U; ++slot) {
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
        l *= masterGain; r *= masterGain;

        if (current->global.limiterEnabled) {
            const float gain = impl_->limiter.process(l, r, limiterCeiling);
            if (gain < 1.0F) maxGr = std::max(maxGr, -gainToDb(gain));
        }
        if (!std::isfinite(l)) l = 0.0F;
        if (!std::isfinite(r)) r = 0.0F;
        out[static_cast<std::size_t>(i) * 2U] = l;
        out[static_cast<std::size_t>(i) * 2U + 1U] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
      }
    }

    for (std::size_t vi = 0; vi < impl_->voices.size(); ++vi) publishVoiceWindows(*impl_, vi);

    impl_->peakL = peakL;
    impl_->peakR = peakR;
    impl_->limiterGrDb = maxGr;
    impl_->upperPeakL = upperPeakL;
    impl_->upperPeakR = upperPeakR;
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
