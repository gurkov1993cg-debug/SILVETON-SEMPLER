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

    void process(float& l, float& r, FilterMode mode, float cutoffHz, float resonance, double sr) noexcept {
        l = processOne(l, ic1eqL, ic2eqL, mode, cutoffHz, resonance, sr);
        r = processOne(r, ic1eqR, ic2eqR, mode, cutoffHz, resonance, sr);
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

struct DelayFx {
    static constexpr std::size_t kMaxFrames = 192000U * 3U;
    std::array<float, kMaxFrames> left{};
    std::array<float, kMaxFrames> right{};
    std::size_t write{0U};
    double sampleRate{48000.0};

    void reset(double sr) noexcept {
        sampleRate = sr;
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        write = 0U;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const double ms = std::clamp(static_cast<double>(p.timeMs), 1.0, 2500.0);
        const std::size_t delayFrames = std::min<std::size_t>(kMaxFrames - 1U,
            static_cast<std::size_t>(std::max(1.0, std::round(sampleRate * ms / 1000.0))));
        const std::size_t read = (write + kMaxFrames - delayFrames) % kMaxFrames;
        const float dl = left[read];
        const float dr = right[read];
        const float fb = std::clamp(p.feedback, 0.0F, 0.95F);
        left[write] = inL + dl * fb;
        right[write] = inR + dr * fb;
        write = (write + 1U) % kMaxFrames;
        wetL = dl;
        wetR = dr;
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

struct ReverbFx {
    // Compact Schroeder network. Fixed arrays: no allocations in audio callback.
    std::array<float, 4096U> c1L{}, c1R{};
    std::array<float, 4608U> c2L{}, c2R{};
    std::array<float, 5120U> c3L{}, c3R{};
    std::array<float, 5632U> c4L{}, c4R{};
    std::array<std::size_t, 4U> pos{{0U,0U,0U,0U}};
    float dampL1{0.0F}, dampR1{0.0F}, dampL2{0.0F}, dampR2{0.0F};

    void reset(double) noexcept {
        c1L.fill(0.0F); c1R.fill(0.0F); c2L.fill(0.0F); c2R.fill(0.0F);
        c3L.fill(0.0F); c3R.fill(0.0F); c4L.fill(0.0F); c4R.fill(0.0F);
        pos = {{0U,0U,0U,0U}};
        dampL1 = dampR1 = dampL2 = dampR2 = 0.0F;
    }

    template<std::size_t N>
    static float comb(std::array<float,N>& b, std::size_t& p, float x, float feedback, float tone, float& damp) noexcept {
        const float y = b[p];
        damp += (y - damp) * std::clamp(tone, 0.05F, 0.95F);
        b[p] = x + damp * feedback;
        p = (p + 1U) % N;
        return y;
    }

    void process(float inL, float inR, const EffectSlot& p, float& wetL, float& wetR) noexcept {
        const float fb = std::clamp(0.55F + p.feedback * 0.35F, 0.4F, 0.92F);
        const float tone = 0.02F + std::clamp(p.tone, 0.0F, 1.0F) * 0.25F;
        const float mono = (inL + inR) * 0.35F;
        const float aL = comb(c1L, pos[0], mono, fb, tone, dampL1);
        const float aR = comb(c1R, pos[0], mono, fb * 0.997F, tone, dampR1);
        const float bL = comb(c2L, pos[1], mono, fb * 0.989F, tone, dampL2);
        const float bR = comb(c2R, pos[1], mono, fb * 0.983F, tone, dampR2);
        // The remaining two taps are written/read manually so their positions are independent.
        float cL = c3L[pos[2]], cR = c3R[pos[2]];
        c3L[pos[2]] = mono + cL * (fb * 0.971F);
        c3R[pos[2]] = mono + cR * (fb * 0.967F);
        pos[2] = (pos[2] + 1U) % c3L.size();
        float dL = c4L[pos[3]], dR = c4R[pos[3]];
        c4L[pos[3]] = mono + dL * (fb * 0.959F);
        c4R[pos[3]] = mono + dR * (fb * 0.953F);
        pos[3] = (pos[3] + 1U) % c4L.size();
        wetL = 0.25F * (aL + bL + cL + dL);
        wetR = 0.25F * (aR + bR + cR + dR);
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
        double ratio{1.0};
        double targetRatio{1.0};
        double glideLogStep{0.0};
        double glideMul{1.0};
        std::uint32_t glideFramesRemaining{0U};
        // Click-free 4 ms fade used when a voice is taken over (voice limit,
        // MONO/LEGATO note change) instead of a one-sample cut.
        float fadeGain{1.0F};
        float fadeStep{0.0F};
        bool fading{false};
        std::uint32_t delayFramesRemaining{0U};
        float baseGain{1.0F};
        float basePan{0.0F};
        float leftGain{0.70710678F};
        float rightGain{0.70710678F};
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
    // PORTAMENTO like the SILVETON keyboard processor: the key each Upper last
    // started (-1 = none, the next note does not glide) and the Sound it belongs to.
    std::array<int, kUpperCount> portaLastKey{{-1, -1, -1, -1, -1, -1, -1}};
    std::array<const Program*, kUpperCount> portaProgram{};
    std::uint64_t voiceAge{0U};
    float peakL{0.0F};
    float peakR{0.0F};
    float limiterGrDb{0.0F};
    std::array<float, kUpperCount> upperPeakL{};
    std::array<float, kUpperCount> upperPeakR{};
    std::array<std::uint16_t, kUpperCount> upperRouteMask{};
    Biquad lowEq{}, midEq{}, highEq{};
    std::array<Biquad, kUpperCount> upperLowEq{};
    std::array<Biquad, kUpperCount> upperMidEq{};
    std::array<Biquad, kUpperCount> upperHighEq{};
    DelayFx delay{};
    ChorusFx chorus{};
    ReverbFx reverb{};

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
        upperPeakL.fill(0.0F);
        upperPeakR.fill(0.0F);
        upperRouteMask.fill(0U);
        for (auto& c : channels) c = ChannelState{};
        for (auto& m : mono) m = MonoKeys{};
        portaLastKey.fill(-1);
        portaProgram.fill(nullptr);
        for (auto& v : voices) v = Voice{};
        lowEq.reset(); midEq.reset(); highEq.reset();
        for (auto& q : upperLowEq) q.reset();
        for (auto& q : upperMidEq) q.reset();
        for (auto& q : upperHighEq) q.reset();
        delay.reset(sr); chorus.reset(sr); reverb.reset(sr);
        clearWindows();
    }
};

UpperToneValues effectiveUpperTone(const UpperPart& upper) noexcept {
    EnvelopeParams env{};
    if (upper.program != nullptr && !upper.program->layers.empty()) env = upper.program->layers.front().ampEnvelope;
    return effectiveToneFor(upper, env);
}

SamplerEngine::SamplerEngine()
    : impl_(std::make_unique<Impl>()), state_(std::make_shared<EngineState>()), interpolator_(InterpolationQuality::LiveCubic4) {
    prepare(48000.0, 512U);
}
SamplerEngine::~SamplerEngine() = default;

void SamplerEngine::prepare(double sampleRate, std::uint32_t maximumBlockFrames) {
    const double sr = std::clamp(sampleRate, 8000.0, 384000.0);
    impl_->maxBlock = std::max<std::uint32_t>(16U, maximumBlockFrames);
    impl_->reset(sr);
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
double noteRatio(const SamplerEngine::Impl& impl, const UpperPart& upper, const ProgramLayer& layer,
                 const SampleZone& zone, std::uint8_t key, const GlobalState& global) noexcept {
    const int keyTracking = zone.nonTranspose ? 0 : (static_cast<int>(key) - static_cast<int>(zone.rootKey));
    const int semitones = keyTracking +
                          static_cast<int>(zone.transposeSemitones) + static_cast<int>(layer.transposeSemitones) +
                          static_cast<int>(upper.transposeSemitones) + static_cast<int>(upper.octave) * 12 +
                          static_cast<int>(global.transposeSemitones);
    const int root = static_cast<int>(global.scaleRoot % 12U);
    const int pc = (static_cast<int>(key) - root + 120) % 12;
    const float scaleCents = std::clamp(global.scaleOffsetsCents[static_cast<std::size_t>(pc)], -100.0F, 100.0F);
    const float cents = zone.tuneCents + layer.tuneCents + upper.tuneCents + zone.sample->fineTuneCents + scaleCents;
    const double tuneHz = std::clamp(static_cast<double>(global.masterTuneHz), 400.0, 480.0);
    const double masterTuneSemis = 12.0 * std::log2(tuneHz / 440.0);
    const double pitch = static_cast<double>(semitones) + static_cast<double>(cents) / 100.0 + masterTuneSemis;
    return (static_cast<double>(zone.sample->sampleRate) / impl.sampleRate) * std::pow(2.0, pitch / 12.0);
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

    v.ratio = noteRatio(impl, upper, layer, zone, key, global);
    v.targetRatio = v.ratio;

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
    v.baseGain = dbToGain(zone.gainDb + layer.levelDb + upper.levelDb) * velGain * layerVelocityGain;
    const float pan = std::clamp(zone.pan + layer.pan + upper.pan, -1.0F, 1.0F);
    v.basePan = pan;
    const float angle = (pan + 1.0F) * static_cast<float>(kPi * 0.25);
    v.leftGain = std::cos(angle);
    v.rightGain = std::sin(angle);
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
    impl.portaLastKey.fill(-1);
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

// SILVETON_PORTA_PROCESSOR_V1 / SILVETON_PORTA_FINGERED_V1 - PORTAMENTO like
// KORG KRONOS (Fingered, Constant Time): it glides ONLY when the new key is
// pressed while another key of that Upper is still held (legato playing); a
// detached note starts clean on its own pitch. Every interval takes exactly the
// same time (TIME 1..100 ms), the pitch moves evenly in semitones. MONO/LEGATO
// glide from the pitch that is sounding, POLY from the last key played. It
// starts fresh when the Upper gets another Sound or portamento was OFF.
constexpr float kPortamentoMaxSec = 0.100F;

float portamentoSecFor(const UpperPart& upper) noexcept {
    return std::clamp(upper.portamentoSec, 0.0F, kPortamentoMaxSec);
}

// Pitch (in keys) the Upper glides from: the pitch a MONO/LEGATO Upper is
// sounding right now (also part-way through a glide), otherwise the last key.
double portamentoFromKey(const SamplerEngine::Impl& impl, int ui, bool mono) noexcept {
    if (mono) {
        const SamplerEngine::Impl::Voice* newest = nullptr;
        for (const auto& v : impl.voices) {
            if (!v.active || v.fading || v.upperIndex != ui || v.ratio <= 0.0 || v.targetRatio <= 0.0) continue;
            if (newest == nullptr || v.age > newest->age) newest = &v;
        }
        if (newest != nullptr) {
            return static_cast<double>(newest->key) + 12.0 * std::log2(newest->ratio / newest->targetRatio);
        }
    }
    return static_cast<double>(impl.portaLastKey[static_cast<std::size_t>(ui)]);
}

void startPortamentoGlide(const SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, double fromKey, float seconds) noexcept {
    if (seconds <= 0.0F || fromKey < 0.0 || v.ratio <= 0.0) return;
    if (v.zone != nullptr && v.zone->nonTranspose) return; // fixed-pitch sample: nothing to glide
    const double semitones = fromKey - static_cast<double>(v.key);
    if (std::abs(semitones) < 1.0e-6) return;
    v.targetRatio = v.ratio;
    v.ratio = v.targetRatio * std::exp2(semitones / 12.0);
    const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(seconds) * impl.sampleRate));
    v.glideFramesRemaining = frames;
    v.glideLogStep = std::log(v.targetRatio / v.ratio) / static_cast<double>(frames);
    v.glideMul = std::exp(v.glideLogStep);
}

// Same sound keeps playing; only its pitch moves to the new key (true glissando).
void legatoTo(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v, const UpperPart& upper,
              const ProgramLayer& layer, const SampleZone& zone, std::uint8_t key, const GlobalState& global) noexcept {
    const double target = noteRatio(impl, upper, layer, zone, key, global);
    v.key = key;
    v.targetRatio = target;
    const float portamentoSec = portamentoSecFor(upper);
    if (portamentoSec > 0.0F && v.ratio > 0.0 && target > 0.0) {
        const auto frames = static_cast<std::uint32_t>(std::max(1.0, static_cast<double>(portamentoSec) * impl.sampleRate));
        v.glideFramesRemaining = frames;
        v.glideLogStep = std::log(target / v.ratio) / static_cast<double>(frames);
        v.glideMul = std::exp(v.glideLogStep);
    } else {
        v.ratio = target;
        v.glideFramesRemaining = 0U;
    }
}

void playUpperNote(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, int ui,
                   const UpperPart& upper, std::uint8_t channel, std::uint8_t key, std::uint8_t velocity,
                   bool legatoPlayed) noexcept {
    const auto& global = stateOwner->global;
    const auto& program = *upper.program;
    const bool upperMono = upper.voiceMode != UpperVoiceMode::Poly;
    const bool upperLegato = upper.voiceMode == UpperVoiceMode::Legato;
    bool hadHeldVoice = false;
    float previousAmp = 0.0F;
    const float portaSec = portamentoSecFor(upper);
    auto& portaLast = impl.portaLastKey[static_cast<std::size_t>(ui)];
    auto& portaProgram = impl.portaProgram[static_cast<std::size_t>(ui)];
    if (portaSec <= 0.0F || portaProgram != upper.program.get()) portaLast = -1; // OFF or another Sound: start fresh
    portaProgram = upper.program.get();
    // Fingered: only legato playing glides.
    const double glideFromKey = (portaSec > 0.0F && legatoPlayed) ? portamentoFromKey(impl, ui, upperMono) : -1.0;
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
                if (!layer.matches(key, velocity)) continue;
                for (const auto& zone : layer.multisample->zones) {
                    if (!zone.matches(key, velocity) || !zone.sample->valid()) continue;
                    for (auto& existing : impl.voices) {
                        if (existing.active && !existing.fading && existing.upperIndex == ui && existing.keyHeld &&
                            existing.delayFramesRemaining == 0U && !existing.legatoKeep &&
                            existing.layer == &layer && existing.zone == &zone) {
                            legatoTo(impl, existing, upper, layer, zone, key, global);
                            existing.legatoKeep = true;
                            break;
                        }
                    }
                }
            }
        }
        // 2) every other sounding voice of this Upper leaves with a short fade
        for (auto& existing : impl.voices) {
            if (existing.active && !existing.fading && existing.upperIndex == ui && !existing.legatoKeep) {
                softKill(impl, existing, (upperLegato && hadHeldVoice) ? kLegatoCrossfadeSec : kTakeoverFadeSec);
            }
        }
    }
    for (const auto& layer : program.layers) {
        if (!layer.matches(key, velocity)) continue;
        for (const auto& zone : layer.multisample->zones) {
            if (!zone.matches(key, velocity) || !zone.sample->valid()) continue;
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
            startPortamentoGlide(impl, *v, glideFromKey, portaSec);
            if (upperLegato && hadHeldVoice) {
                v->ampEnv.value = previousAmp;
                v->ampEnv.stage = AmpEnv::Stage::Decay;
                v->fadeGain = 0.0F;
                v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kLegatoCrossfadeSec) * impl.sampleRate));
            }
        }
    }
    if (upperMono) for (auto& existing : impl.voices) if (existing.upperIndex == ui) existing.legatoKeep = false;
    if (portaSec > 0.0F) portaLast = static_cast<int>(key);
}

void handleNoteOff(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner,
                   std::uint8_t channel, std::uint8_t key) noexcept {
    std::uint16_t handled = 0U;
    for (int ui = 0; ui < kUpperCount; ++ui) {
        const auto& upper = stateOwner->combi.uppers[static_cast<std::size_t>(ui)];
        if (upper.midiChannel != (channel & 0x0FU) || upper.voiceMode == UpperVoiceMode::Poly) continue;
        auto& mk = impl.mono[static_cast<std::size_t>(ui)];
        std::size_t index = mk.count;
        for (std::size_t i = 0; i < mk.count; ++i) if (mk.key[i] == key) { index = i; break; }
        if (index >= mk.count) continue;
        handled = static_cast<std::uint16_t>(handled | static_cast<std::uint16_t>(1U << static_cast<unsigned>(ui)));
        const bool wasTop = index + 1U == mk.count;
        monoRemoveAt(mk, index);
        if (!wasTop) continue; // a lower held key was let go: the sound goes on
        if (mk.count > 0U && upper.program != nullptr) {
            // back to the key that is still held
            playUpperNote(impl, stateOwner, ui, upper, channel, mk.key[mk.count - 1U], mk.vel[mk.count - 1U], true);
        } else {
            releaseUpperVoices(impl, ui, channel);
        }
    }
    releaseMatching(impl, channel, key, handled);
}

void processEvent(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, const RealtimeEvent& e) noexcept {
    if (stateOwner == nullptr) return;
    const auto& state = *stateOwner;
    const std::uint8_t channel = static_cast<std::uint8_t>(e.channel & 0x0FU);
    if (e.type != EventType::Panic && (state.global.midiInputChannelMask & (1U << channel)) == 0U) return;
    auto& ch = impl.channels[channel];
    switch (e.type) {
        case EventType::NoteOn: {
            if (state.global.filterNotes) break;
            if (e.data2 == 0U) { handleNoteOff(impl, stateOwner, e.channel, e.data1); break; }
            const bool anySolo = std::any_of(state.combi.uppers.begin(), state.combi.uppers.end(), [](const UpperPart& u) { return u.enabled && u.solo; });
            for (int ui = 0; ui < kUpperCount; ++ui) {
                const auto& upper = state.combi.uppers[static_cast<std::size_t>(ui)];
                if (upper.midiChannel != (e.channel & 0x0FU)) continue;
                if (anySolo && !upper.solo) continue;
                if (!upper.matches(e.data1, e.data2)) continue;
                bool legatoPlayed = false; // another key of this Upper is still held
                if (upper.voiceMode != UpperVoiceMode::Poly) {
                    auto& mk = impl.mono[static_cast<std::size_t>(ui)];
                    monoPush(mk, e.data1, e.data2);
                    legatoPlayed = mk.count >= 2U;
                } else {
                    for (const auto& v : impl.voices) {
                        if (v.active && !v.fading && v.upperIndex == ui && v.keyHeld && v.key != e.data1) { legatoPlayed = true; break; }
                    }
                }
                playUpperNote(impl, stateOwner, ui, upper, e.channel, e.data1, e.data2, legatoPlayed);
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
                    if (state.combi.uppers[static_cast<std::size_t>(ui)].midiChannel == (e.channel & 0x0FU))
                        impl.mono[static_cast<std::size_t>(ui)] = SamplerEngine::Impl::MonoKeys{};
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

    // Live TONE values (ADSR times, SUSTAIN, CUTOFF, RESONANCE) of the current Upper
    // state, evaluated once per block for every sounding voice.
    {
        const double blockAlpha = 1.0 - std::exp(-static_cast<double>(frames) / (0.012 * impl_->sampleRate));
        const float openLog = std::log(kToneCutoffOpenHz);
        for (auto& v : impl_->voices) {
            if (!v.active || v.layer == nullptr || v.upper == nullptr) continue;
            const auto uix = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
            const UpperToneValues tv = effectiveToneFor(current->combi.uppers[uix], v.layer->ampEnvelope);
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
    const float limiterCeiling = dbToGain(current->global.limiterCeilingDb);

    for (std::uint32_t i = 0U; i < frames; ++i) {
        std::array<float, kUpperCount> upperL{};
        std::array<float, kUpperCount> upperR{};

        for (auto& v : impl_->voices) {
            if (!v.active || v.sample == nullptr || v.layer == nullptr || v.zone == nullptr || v.upper == nullptr) continue;
            if (v.delayFramesRemaining > 0U) {
                --v.delayFramesRemaining;
                continue;
            }
            const auto& sample = *v.sample;
            const bool looping = sample.loop.enabled && !v.zone->oneShot && !v.zone->reverse;

            const auto& ch = impl_->channels[v.channel & 0x0FU];
            const auto ui = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
            const std::uint16_t routeMask = v.routeMask;
            const auto routeActive = [routeMask](ModDestination d) noexcept {
                return (routeMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(d))) != 0U;
            };

            const bool filterActive = v.layer->filter.mode != FilterMode::Bypass;
            const bool lfoDepthRouted = routeActive(ModDestination::LfoDepth);
            const bool lfo1Needed = lfoDepthRouted || v.layer->lfo1.pitchCents != 0.0F ||
                v.layer->lfo1.filterOctaves != 0.0F || v.layer->lfo1.ampDepth != 0.0F ||
                (filterActive && v.layer->filter.lfoAmount != 0.0F);
            const bool lfo2Needed = lfoDepthRouted || v.layer->lfo2.pitchCents != 0.0F ||
                v.layer->lfo2.filterOctaves != 0.0F || v.layer->lfo2.ampDepth != 0.0F;

            LfoParams lfo1Params = v.layer->lfo1;
            LfoParams lfo2Params = v.layer->lfo2;
            float lfo1 = 0.0F, lfo2 = 0.0F;
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
                    lfo1 = v.lfo1.process(lfo1Params, impl_->sampleRate);
                }
                if (lfo2Needed) {
                    lfo2Params.frequencyHz *= lfoRateScale;
                    lfo2Params.pitchCents *= lfoDepthScale;
                    lfo2Params.filterOctaves *= lfoDepthScale;
                    lfo2Params.ampDepth *= lfoDepthScale;
                    lfo2 = v.lfo2.process(lfo2Params, impl_->sampleRate);
                }
            }

            const bool pitchEnvNeeded = v.layer->pitchEnvelopeCents != 0.0F;
            const bool filterEnvNeeded = filterActive && v.layer->filter.envelopeAmount != 0.0F;
            const float pitchEnv = pitchEnvNeeded ? v.pitchEnv.process(impl_->sampleRate) : 0.0F;
            const float ampEnv = v.ampEnv.process();
            const float filterEnv = filterEnvNeeded ? v.filterEnv.process(impl_->sampleRate) : 0.0F;
            if (!v.ampEnv.active()) { v.active = false; continue; }

            if (v.glideFramesRemaining > 0U) {
                v.ratio *= v.glideMul;
                --v.glideFramesRemaining;
                if (v.glideFramesRemaining == 0U) v.ratio = v.targetRatio;
            }

            double ratio = v.ratio;
            const bool dynamicPitchActive = ch.pitchBend != 0 || routeActive(ModDestination::PitchSemitones) ||
                pitchEnvNeeded || (lfo1Needed && lfo1Params.pitchCents != 0.0F) ||
                (lfo2Needed && lfo2Params.pitchCents != 0.0F);
            if (dynamicPitchActive) {
                const float bendNorm = static_cast<float>(ch.pitchBend) / 8192.0F;
                const float bendSemis = bendNorm * static_cast<float>(v.upper->pitchBendRange);
                const float controllerPitchSemis = routeActive(ModDestination::PitchSemitones)
                    ? controllerAmount(*v.upper, ch, ModDestination::PitchSemitones) : 0.0F;
                const float lfoPitchCents = lfo1 * lfo1Params.pitchCents + lfo2 * lfo2Params.pitchCents;
                const float envPitchCents = pitchEnv * v.layer->pitchEnvelopeCents;
                const double dynamicPitch = static_cast<double>(bendSemis + controllerPitchSemis) +
                    static_cast<double>(lfoPitchCents + envPitchCents) / 100.0;
                ratio *= std::pow(2.0, dynamicPitch / 12.0);
            }

            float sL = 0.0F, sR = 0.0F;
            readWithLoopCrossfade(interpolator_, sample, v.position, ratio, looping, sL, sR);

            if (filterActive) {
                const float controllerCutoff = routeActive(ModDestination::FilterCutoffOctaves)
                    ? controllerAmount(*v.upper, ch, ModDestination::FilterCutoffOctaves) : 0.0F;
                float cutoff = v.layer->filter.cutoffHz * std::pow(
                    2.0F, std::clamp(v.upper->filterCutoffOctaves + controllerCutoff, -10.0F, 10.0F));
                const float keyDelta = (static_cast<float>(v.key) - 60.0F) / 12.0F;
                const float velocityNorm = static_cast<float>(v.velocity) * kInv127;
                const float filterOct = keyDelta * v.layer->filter.keyTrack +
                                        velocityNorm * v.layer->filter.velocityAmount +
                                        filterEnv * v.layer->filter.envelopeAmount +
                                        lfo1 * v.layer->filter.lfoAmount +
                                        lfo1 * lfo1Params.filterOctaves +
                                        lfo2 * lfo2Params.filterOctaves;
                if (filterOct != 0.0F) cutoff *= std::pow(2.0F, filterOct);
                const float controllerResonance = routeActive(ModDestination::FilterResonance)
                    ? controllerAmount(*v.upper, ch, ModDestination::FilterResonance) : 0.0F;
                const float resonance = std::clamp(
                    v.layer->filter.resonance + v.upper->filterResonanceOffset + controllerResonance, 0.0F, 1.0F);
                v.filter.process(sL, sR, v.layer->filter.mode, cutoff, resonance, impl_->sampleRate);
            }

            if (v.toneOn) v.toneLp.process(sL, sR);

            float modGain = ampEnv;
            if ((lfo1Needed && lfo1Params.ampDepth != 0.0F) || (lfo2Needed && lfo2Params.ampDepth != 0.0F)) {
                modGain *= std::max(0.0F, 1.0F + lfo1 * lfo1Params.ampDepth + lfo2 * lfo2Params.ampDepth);
            }
            if (v.upper->expressionEnabled) modGain *= ch.expression;
            if (v.upper->breathEnabled) modGain *= ch.breath;
            if (v.upper->aftertouchEnabled) modGain *= (0.5F + 0.5F * ch.pressure);

            const float controllerLevelDb = routeActive(ModDestination::LevelDb)
                ? controllerAmount(*v.upper, ch, ModDestination::LevelDb) : 0.0F;
            float panLeft = v.leftGain;
            float panRight = v.rightGain;
            if (routeActive(ModDestination::Pan)) {
                const float controllerPan = controllerAmount(*v.upper, ch, ModDestination::Pan);
                const float dynamicPan = std::clamp(v.basePan + controllerPan, -1.0F, 1.0F);
                const float panAngle = (dynamicPan + 1.0F) * static_cast<float>(kPi * 0.25);
                panLeft = std::cos(panAngle);
                panRight = std::sin(panAngle);
            }
            float g = v.baseGain * modGain;
            if (controllerLevelDb != 0.0F) g *= dbToGain(std::clamp(controllerLevelDb, -96.0F, 24.0F));
            if (v.fadeStep != 0.0F) {
                v.fadeGain += v.fadeStep;
                if (v.fadeGain <= 0.0F) { v.ampEnv.forceOff(); v.active = false; v.fading = false; continue; }
                if (v.fadeGain >= 1.0F) { v.fadeGain = 1.0F; v.fadeStep = 0.0F; }
            }
            if (v.fadeStep != 0.0F || v.fadeGain < 1.0F) g *= std::sin(v.fadeGain * 1.57079632679F); // equal-power fade
            const float oL = sL * g * panLeft;
            const float oR = sR * g * panRight;
            upperL[ui] += oL;
            upperR[ui] += oR;

            if (v.zone->reverse) v.position -= ratio; else v.position += ratio;
            if (!v.zone->reverse && looping && v.position >= static_cast<double>(sample.loop.endFrame)) {
                const double len = static_cast<double>(sample.loop.endFrame - sample.loop.startFrame);
                if (len > 0.0) v.position = static_cast<double>(sample.loop.startFrame) + std::fmod(v.position - static_cast<double>(sample.loop.startFrame), len);
            }
            if (v.zone->reverse && v.position < 0.0) { v.ampEnv.forceOff(); v.active = false; }
            else if (!looping && v.position >= static_cast<double>(sample.frameCount())) { v.ampEnv.forceOff(); v.active = false; }
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
        const auto processFx = [&](const EffectSlot& fx, float inL, float inR, float& wetL, float& wetR) noexcept {
            switch (fx.type) {
                case EffectType::Off: wetL = wetR = 0.0F; break;
                case EffectType::Delay: impl_->delay.process(inL, inR, fx, wetL, wetR); break;
                case EffectType::Chorus: impl_->chorus.process(inL, inR, fx, wetL, wetR); break;
                case EffectType::Reverb: impl_->reverb.process(inL, inR, fx, wetL, wetR); break;
            }
        };
        processFx(current->combi.fx1, send1L, send1R, fx1L, fx1R);
        if (current->combi.fxRouting == EffectRouting::Fx1IntoFx2) {
            const float chainGain = clamp01(current->combi.fx1.wet);
            processFx(current->combi.fx2, send2L + fx1L * chainGain, send2R + fx1R * chainGain, fx2L, fx2R);
        } else {
            processFx(current->combi.fx2, send2L, send2R, fx2L, fx2R);
        }
        float l = dryL + fx1L * clamp01(current->combi.fx1.wet) + fx2L * clamp01(current->combi.fx2.wet);
        float r = dryR + fx1R * clamp01(current->combi.fx1.wet) + fx2R * clamp01(current->combi.fx2.wet);

        if (current->global.eqLowDb != 0.0F) impl_->lowEq.process(l, r);
        if (current->global.eqMidDb != 0.0F) impl_->midEq.process(l, r);
        if (current->global.eqHighDb != 0.0F) impl_->highEq.process(l, r);
        l *= masterGain; r *= masterGain;

        if (current->global.limiterEnabled) {
            const float absPeak = std::max(std::abs(l), std::abs(r));
            if (absPeak > limiterCeiling && absPeak > 0.0F) {
                const float gain = limiterCeiling / absPeak;
                maxGr = std::max(maxGr, -gainToDb(gain));
                l *= gain; r *= gain;
            }
        }
        if (!std::isfinite(l)) l = 0.0F;
        if (!std::isfinite(r)) r = 0.0F;
        out[static_cast<std::size_t>(i) * 2U] = l;
        out[static_cast<std::size_t>(i) * 2U + 1U] = r;
        peakL = std::max(peakL, std::abs(l));
        peakR = std::max(peakR, std::abs(r));
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
