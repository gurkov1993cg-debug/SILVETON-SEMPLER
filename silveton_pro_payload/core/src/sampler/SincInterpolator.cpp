#include "silveton/sampler/SincInterpolator.h"
#include "silveton/sampler/SampleStream.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace silveton::sampler {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr int kMaxTaps = 32;
constexpr int kPhases = 1024;          // polyphase rows for the normal (cutoff 1) case
constexpr int kSincResolution = 512;   // table points per unit of the sinc argument
constexpr int kSincMaxArgument = 16;   // |x| <= half of 32 taps
constexpr int kWindowResolution = 4096;

double exactSinc(double x) noexcept {
    if (std::abs(x) < 1.0e-12) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

double exactBlackman(double normalized) noexcept {
    const double x = std::clamp(normalized, -1.0, 1.0);
    return 0.42 + 0.5 * std::cos(kPi * x) + 0.08 * std::cos(2.0 * kPi * x);
}

struct KernelTables {
    std::vector<float> poly8;
    std::vector<float> poly16;
    std::vector<float> poly32;
    std::vector<float> sinc;
    std::vector<float> window;

    KernelTables() {
        buildPoly(poly8, 8);
        buildPoly(poly16, 16);
        buildPoly(poly32, 32);
        sinc.resize(static_cast<std::size_t>(kSincMaxArgument * kSincResolution + 2));
        for (std::size_t i = 0; i < sinc.size(); ++i) {
            sinc[i] = static_cast<float>(exactSinc(static_cast<double>(i) / kSincResolution));
        }
        window.resize(static_cast<std::size_t>(kWindowResolution + 2));
        for (std::size_t i = 0; i < window.size(); ++i) {
            window[i] = static_cast<float>(exactBlackman(static_cast<double>(i) / kWindowResolution));
        }
    }

    static void buildPoly(std::vector<float>& table, int taps) {
        const int half = taps / 2;
        table.assign(static_cast<std::size_t>((kPhases + 1) * taps), 0.0F);
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            std::array<double, kMaxTaps> w{};
            double sum = 0.0;
            for (int k = 0; k < taps; ++k) {
                const double distance = static_cast<double>(k - half + 1) - frac;
                w[static_cast<std::size_t>(k)] = exactSinc(distance) * exactBlackman(distance / half);
                sum += w[static_cast<std::size_t>(k)];
            }
            for (int k = 0; k < taps; ++k) {
                table[static_cast<std::size_t>(p * taps + k)] =
                    static_cast<float>(std::abs(sum) < 1.0e-12 ? 0.0 : w[static_cast<std::size_t>(k)] / sum);
            }
        }
    }

    [[nodiscard]] const std::vector<float>& poly(int taps) const noexcept {
        return taps <= 8 ? poly8 : (taps <= 16 ? poly16 : poly32);
    }

    [[nodiscard]] float sincAt(float x) const noexcept {
        const float u = std::min(std::abs(x) * static_cast<float>(kSincResolution),
                                 static_cast<float>(kSincMaxArgument * kSincResolution));
        const auto i = static_cast<std::size_t>(u);
        const float t = u - static_cast<float>(i);
        return sinc[i] + (sinc[i + 1U] - sinc[i]) * t;
    }

    [[nodiscard]] float windowAt(float normalized) const noexcept {
        const float u = std::min(std::abs(normalized), 1.0F) * static_cast<float>(kWindowResolution);
        const auto i = static_cast<std::size_t>(u);
        const float t = u - static_cast<float>(i);
        return window[i] + (window[i + 1U] - window[i]) * t;
    }
};

// ---- SILVETON_STUDIO_INTERP_V1 ---------------------------------------------
// Ratio-adaptive Kaiser-windowed sinc for the 96 kHz engine.
//
// For a playback ratio r (source frames per output frame) the source runs at
// r*96 kHz in output time. Only two things are audible:
//   * the wanted signal from 0 to 20 kHz  -> passband, kept flat;
//   * any image or alias that ends up between 0 and 20 kHz after the 96 kHz
//     output folds it                     -> stopband, rejected by kStudioRejectDb.
// Images/aliases that land between 20 and 48 kHz are inaudible; the kernel still
// pushes them down as far as its length allows.
// One kernel per 1/12 octave of r, designed for the worst ratio inside it, so a
// kernel is never used outside the range it was designed for.
#ifndef SILVETON_STUDIO_REJECT_DB
#define SILVETON_STUDIO_REJECT_DB 110.0
#endif
#ifndef SILVETON_STUDIO_MIN_TAPS
#define SILVETON_STUDIO_MIN_TAPS 24
#endif
#ifndef SILVETON_STUDIO_PITCHED_DOWN_TAPS
#define SILVETON_STUDIO_PITCHED_DOWN_TAPS 64
#endif
constexpr double kStudioRejectDb = SILVETON_STUDIO_REJECT_DB;
constexpr int kStudioMinTaps = SILVETON_STUDIO_MIN_TAPS;
constexpr int kStudioPitchedDownTaps = SILVETON_STUDIO_PITCHED_DOWN_TAPS;
constexpr int kStudioMaxTaps = 64;
constexpr int kStudioPhases = 512;                 // + linear blend between phases
constexpr int kStudioPerOctave = 12;
constexpr int kStudioLog2Low = -6;                 // r = 1/64
constexpr int kStudioLog2High = 5;                 // r = 32
constexpr int kStudioBuckets = (kStudioLog2High - kStudioLog2Low) * kStudioPerOctave;
constexpr double kStudioOutputRate = 96000.0;
constexpr double kStudioAudibleHz = 20000.0;
constexpr double kStudioContentTop = 0.455;        // 20 kHz of a 44.1 kHz recording, cycles/sample

double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 200; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1.0e-17) break;
    }
    return sum;
}

struct StudioKernel {
    int taps{0};
    std::size_t offset{0};   // into StudioTables::data, (kStudioPhases + 1) rows of `taps`
};

struct StudioTables {
    std::vector<float> data;
    std::array<StudioKernel, kStudioBuckets> bucket{};
    std::array<float, kStudioPerOctave> step{};   // 2^(j/12), j = 0..11

    static int tapsFor(double transition) noexcept {
        // Kaiser estimate: N = (A - 7.95) / (14.36 * transition[cycles/sample]).
        const double n = (kStudioRejectDb - 7.95) / (14.36 * transition);
        int t = static_cast<int>(std::ceil(n / 4.0)) * 4;
        return std::clamp(t, kStudioMinTaps, kStudioMaxTaps);
    }
    static double transitionFor(int taps) noexcept {
        return (kStudioRejectDb - 7.95) / (14.36 * static_cast<double>(taps));
    }

    StudioTables() {
        for (int j = 0; j < kStudioPerOctave; ++j) {
            step[static_cast<std::size_t>(j)] = static_cast<float>(std::exp2(static_cast<double>(j) / kStudioPerOctave));
        }
        const double beta = 0.1102 * (kStudioRejectDb - 8.7);
        const double i0Beta = besselI0(beta);
        std::size_t pitchedDownOffset = 0U;
        bool havePitchedDown = false;
        for (int b = 0; b < kStudioBuckets; ++b) {
            const double r0 = std::exp2(kStudioLog2Low + static_cast<double>(b) / kStudioPerOctave);
            const double r1 = std::exp2(kStudioLog2Low + static_cast<double>(b + 1) / kStudioPerOctave);
            // Pitched down so far that images of the top of the recording reach
            // the audible band: classic half-band kernel centred on Nyquist.
            const bool pitchedDown = r0 * kStudioOutputRate * (1.0 - kStudioContentTop) <= kStudioAudibleHz;
            int taps = 0;
            double fc = 0.5;
            if (pitchedDown) {
                // Stopband starts where the first image of the top of the recording
                // lands, so no image of recorded content reaches the audible band;
                // the passband ends one transition width lower.
                taps = kStudioPitchedDownTaps;
                if (havePitchedDown) {
                    bucket[static_cast<std::size_t>(b)] = StudioKernel{taps, pitchedDownOffset};
                    continue;
                }
                const double sEdge = 1.0 - kStudioContentTop;
                fc = sEdge - 0.5 * transitionFor(taps);
            } else {
                const double pass = std::min(kStudioContentTop, kStudioAudibleHz / (r0 * kStudioOutputRate));
                const double stop = (kStudioOutputRate - kStudioAudibleHz) / (r1 * kStudioOutputRate);
                taps = tapsFor(stop - pass);
                double tr = transitionFor(taps);
                double p = pass;
                double sEdge = std::min(stop, pass + tr);
                if (sEdge - p < tr) p = std::max(0.25 * stop, sEdge - tr);   // beyond ~+3.5 octaves: duller, never aliasing
                fc = 0.5 * (p + sEdge);
            }
            const std::size_t offset = data.size();
            data.resize(offset + static_cast<std::size_t>((kStudioPhases + 1) * taps), 0.0F);
            const int half = taps / 2;
            std::array<double, kStudioMaxTaps> w{};
            for (int ph = 0; ph <= kStudioPhases; ++ph) {
                const double frac = static_cast<double>(ph) / kStudioPhases;
                double sum = 0.0;
                for (int k = 0; k < taps; ++k) {
                    const double t = static_cast<double>(k - half + 1) - frac;
                    const double x = t / static_cast<double>(half);
                    const double win = (std::abs(x) >= 1.0) ? 0.0 : besselI0(beta * std::sqrt(1.0 - x * x)) / i0Beta;
                    const double v = 2.0 * fc * exactSinc(2.0 * fc * t) * win;
                    w[static_cast<std::size_t>(k)] = v;
                    sum += v;
                }
                const double inv = std::abs(sum) < 1.0e-12 ? 0.0 : 1.0 / sum;
                float* row = data.data() + offset + static_cast<std::size_t>(ph * taps);
                for (int k = 0; k < taps; ++k) row[k] = static_cast<float>(w[static_cast<std::size_t>(k)] * inv);
            }
            bucket[static_cast<std::size_t>(b)] = StudioKernel{taps, offset};
            if (pitchedDown) { havePitchedDown = true; pitchedDownOffset = offset; }
        }
    }

    // Bucket of a ratio without log(): exponent from the float bits, the 1/12
    // octave step by comparison with 2^(j/12). Exact, so r is never put into a
    // kernel designed for smaller ratios.
    [[nodiscard]] const StudioKernel& select(double ratio) const noexcept {
        float r = static_cast<float>(std::abs(ratio));
        if (!(r > 1.0e-30F) || !(r < 1.0e30F)) r = 1.0F;
        std::uint32_t bits = 0U;
        std::memcpy(&bits, &r, sizeof(bits));
        const int e = static_cast<int>((bits >> 23U) & 0xFFU) - 127;   // r = m * 2^e
        const std::uint32_t mbits = (bits & 0x007FFFFFU) | 0x3F800000U;
        float m = 1.0F;                                                 // 1 <= m < 2
        std::memcpy(&m, &mbits, sizeof(m));
        int j = 0;
        for (int k = 1; k < kStudioPerOctave; ++k) j += (m >= step[static_cast<std::size_t>(k)]) ? 1 : 0;
        int b = (e - kStudioLog2Low) * kStudioPerOctave + j;
        b = std::clamp(b, 0, kStudioBuckets - 1);
        return bucket[static_cast<std::size_t>(b)];
    }
};

// Four floats at a time (NEON on the tablet, SSE on a PC). Kernels are a
// multiple of 4 points long. Two accumulator sets keep the FPU busy.
typedef float StudioVec __attribute__((vector_size(16)));
inline StudioVec studioLoad(const float* p) noexcept { StudioVec v; std::memcpy(&v, p, sizeof(v)); return v; }
inline float studioSum(StudioVec v) noexcept { return (v[0] + v[1]) + (v[2] + v[3]); }

const StudioTables& studioTables() {
    static const StudioTables instance;
    return instance;
}

const KernelTables& tables() {
    static const KernelTables instance;
    return instance;
}

// Sinc kernel weights for one output sample (sum == 1).
void sincWeights(const KernelTables& t, int taps, double frac, double ratio, float* w) noexcept {
    const double absRatio = std::max(1.0e-9, std::abs(ratio));
    if (absRatio <= 1.0) {
        const double scaled = frac * kPhases;
        int p = static_cast<int>(scaled);
        p = std::clamp(p, 0, kPhases - 1);
        const float mix = static_cast<float>(scaled - static_cast<double>(p));
        const float* a = t.poly(taps).data() + static_cast<std::size_t>(p * taps);
        const float* b = a + taps;
        for (int k = 0; k < taps; ++k) w[k] = a[k] + (b[k] - a[k]) * mix;
        return;
    }
    // Reading faster than one source frame per output frame: narrow the
    // low-pass so upward transpositions do not alias.
    const float cutoff = static_cast<float>(1.0 / absRatio);
    const int half = taps / 2;
    const float invHalf = 1.0F / static_cast<float>(half);
    const float fracF = static_cast<float>(frac);
    float sum = 0.0F;
    for (int k = 0; k < taps; ++k) {
        const float distance = static_cast<float>(k - half + 1) - fracF;
        const float v = t.sincAt(distance * cutoff) * t.windowAt(distance * invHalf);
        w[k] = v;
        sum += v;
    }
    if (std::abs(sum) < 1.0e-12F) {
        for (int k = 0; k < taps; ++k) w[k] = 0.0F;
        return;
    }
    const float inv = 1.0F / sum;
    for (int k = 0; k < taps; ++k) w[k] *= inv;
}

inline float decodeRaw(std::uint8_t format, const std::uint8_t* p) noexcept {
    switch (format) {
        case RawPcmView::kPcm16: {
            std::int16_t v = 0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) * (1.0F / 32768.0F);
        }
        case RawPcmView::kPcm24: {
            std::int32_t v = static_cast<std::int32_t>(p[0]) |
                             (static_cast<std::int32_t>(p[1]) << 8) |
                             (static_cast<std::int32_t>(p[2]) << 16);
            if ((v & 0x00800000) != 0) v |= static_cast<std::int32_t>(0xFF000000U);
            return static_cast<float>(v) * (1.0F / 8388608.0F);
        }
        case RawPcmView::kFloat32: {
            float f = 0.0F;
            std::memcpy(&f, p, sizeof(f));
            return std::clamp(f, -1.0F, 1.0F);
        }
        default:
            return 0.0F;
    }
}

// One frame of one channel. Looping: frames at/after loop END wrap into the
// loop; frames before loop START are played as recorded (the attack).
inline float frameAt(const SampleData& s, long long frame, std::uint16_t ch, bool loopActive,
                     long long frames, std::uint64_t preloaded, const RawPcmView& raw) noexcept {
    long long resolved = frame;
    if (loopActive) {
        const auto loopStart = static_cast<long long>(s.loop.startFrame);
        const auto loopEnd = static_cast<long long>(s.loop.endFrame);
        const long long loopLength = loopEnd - loopStart;
        if (resolved >= loopEnd && loopLength > 0) resolved = loopStart + ((resolved - loopStart) % loopLength);
    }
    if (resolved < 0 || resolved >= frames) return 0.0F;
    const auto f = static_cast<std::uint64_t>(resolved);
    if (f < preloaded) return s.interleaved[static_cast<std::size_t>(f) * s.channels + ch];
    if (raw.data != nullptr && f < raw.frames) {
        const std::size_t bps = raw.bytesPerSample();
        const std::uint16_t rc = std::min<std::uint16_t>(ch, static_cast<std::uint16_t>(raw.channels - 1U));
        return decodeRaw(raw.format, raw.data + (static_cast<std::size_t>(f) * raw.channels + rc) * bps);
    }
    return s.stream != nullptr ? s.stream->readFrame(f, ch) : 0.0F;
}

// Frames first..first+taps-1 of both channels.
void gather(const SampleData& s, long long first, int taps, bool looping, float* l, float* r) noexcept {
    const auto frames = static_cast<long long>(s.frameCount());
    const std::uint16_t chR = s.channels > 1U ? 1U : 0U;
    const bool loopActive = looping && s.loop.enabled && s.loop.endFrame > s.loop.startFrame;
    const long long last = first + taps - 1;
    const std::uint64_t preloaded = s.preloadedFrameCount();
    const bool noWrap = !loopActive || last < static_cast<long long>(s.loop.endFrame);

    if (first >= 0 && last < frames && noWrap && static_cast<std::uint64_t>(last) < preloaded) {
        // All taps in the RAM attack prefix.
        const std::size_t stride = s.channels;
        const float* p = s.interleaved.data() + static_cast<std::size_t>(first) * stride;
        for (int k = 0; k < taps; ++k, p += stride) { l[k] = p[0]; r[k] = p[chR]; }
        return;
    }
    const RawPcmView raw = s.stream != nullptr ? s.stream->rawView() : RawPcmView{};
    if (first >= 0 && last < frames && noWrap) {
        if (static_cast<std::uint64_t>(first) >= preloaded && raw.data != nullptr &&
            static_cast<std::uint64_t>(last) < raw.frames) {
            const std::size_t bps = raw.bytesPerSample();
            const std::size_t frameBytes = bps * raw.channels;
            const std::size_t offR = raw.channels > 1U ? static_cast<std::size_t>(chR) * bps : 0U;
            const std::uint8_t* p = raw.data + static_cast<std::size_t>(first) * frameBytes;
            for (int k = 0; k < taps; ++k, p += frameBytes) {
                l[k] = decodeRaw(raw.format, p);
                r[k] = decodeRaw(raw.format, p + offR);
            }
            return;
        }
    }
    for (int k = 0; k < taps; ++k) {
        const long long f = first + k;
        l[k] = frameAt(s, f, 0U, loopActive, frames, preloaded, raw);
        r[k] = chR == 0U ? l[k] : frameAt(s, f, chR, loopActive, frames, preloaded, raw);
    }
}

// Studio: frames first..first+taps-1 as interleaved floats (stride = channels).
// Returns a pointer straight into the RAM attack prefix when possible, otherwise
// decodes the mapped PCM (or the general looped/edge case) into `tmp`.
const float* studioFrames(const SampleData& s, long long first, int taps, bool looping, float* tmp) noexcept {
    const auto frames = static_cast<long long>(s.frameCount());
    const std::size_t stride = s.channels > 1U ? 2U : 1U;
    const bool loopActive = looping && s.loop.enabled && s.loop.endFrame > s.loop.startFrame;
    const long long last = first + taps - 1;
    const std::uint64_t preloaded = s.preloadedFrameCount();
    const bool noWrap = !loopActive || last < static_cast<long long>(s.loop.endFrame);
    const std::size_t count = static_cast<std::size_t>(taps) * stride;
    if (first >= 0 && last < frames && noWrap) {
        if (static_cast<std::uint64_t>(last) < preloaded) {
            return s.interleaved.data() + static_cast<std::size_t>(first) * stride;
        }
        const RawPcmView raw = s.stream != nullptr ? s.stream->rawView() : RawPcmView{};
        if (static_cast<std::uint64_t>(first) >= preloaded && raw.data != nullptr &&
            static_cast<std::uint64_t>(last) < raw.frames && raw.channels == stride) {
            const std::size_t at = static_cast<std::size_t>(first) * stride;
            if (raw.format == RawPcmView::kPcm16) {
                const std::uint8_t* p = raw.data + at * 2U;
                for (std::size_t i = 0; i < count; ++i) {
                    std::int16_t v = 0;
                    std::memcpy(&v, p + i * 2U, sizeof(v));
                    tmp[i] = static_cast<float>(v) * (1.0F / 32768.0F);
                }
                return tmp;
            }
            if (raw.format == RawPcmView::kPcm24 || raw.format == RawPcmView::kFloat32) {
                const std::size_t bps = raw.bytesPerSample();
                const std::uint8_t* p = raw.data + at * bps;
                for (std::size_t i = 0; i < count; ++i) tmp[i] = decodeRaw(raw.format, p + i * bps);
                return tmp;
            }
        }
    }
    alignas(16) std::array<float, 64> l;
    alignas(16) std::array<float, 64> r;
    gather(s, first, taps, looping, l.data(), r.data());
    if (stride == 1U) {
        for (int k = 0; k < taps; ++k) tmp[k] = l[static_cast<std::size_t>(k)];
    } else {
        for (int k = 0; k < taps; ++k) {
            tmp[static_cast<std::size_t>(k) * 2U] = l[static_cast<std::size_t>(k)];
            tmp[static_cast<std::size_t>(k) * 2U + 1U] = r[static_cast<std::size_t>(k)];
        }
    }
    return tmp;
}

inline float catmullRom(const float* y, float t) noexcept {
    const float a0 = -0.5F*y[0] + 1.5F*y[1] - 1.5F*y[2] + 0.5F*y[3];
    const float a1 = y[0] - 2.5F*y[1] + 2.0F*y[2] - 0.5F*y[3];
    const float a2 = -0.5F*y[0] + 0.5F*y[2];
    return ((a0*t + a1)*t + a2)*t + y[1];
}

} // namespace

SincInterpolator::SincInterpolator(InterpolationQuality quality) noexcept : quality_(quality) {
    // Tables are built here (control thread), never inside the audio thread.
    (void)tables();
    (void)studioTables();
}

void SincInterpolator::readStereo(const SampleData& sample,
                                  double position,
                                  double playbackRatio,
                                  bool looping,
                                  float& left,
                                  float& right) const noexcept {
    left = 0.0F;
    right = 0.0F;
    if (!sample.valid() || !std::isfinite(position)) return;

    const double base = std::floor(position);
    const double frac = position - base;
    const auto i1 = static_cast<long long>(base);

    if (quality_ == InterpolationQuality::LiveCubic4) {
        std::array<float, 4> l{};
        std::array<float, 4> r{};
        gather(sample, i1 - 1LL, 4, looping, l.data(), r.data());
        if (std::abs(playbackRatio - 0.5) < 1.0e-12) {
            if (std::abs(frac) < 1.0e-12) { left = l[1]; right = r[1]; return; }
            if (std::abs(frac - 0.5) < 1.0e-12) {
                left = (-l[0] + 9.0F*l[1] + 9.0F*l[2] - l[3]) * 0.0625F;
                right = (-r[0] + 9.0F*r[1] + 9.0F*r[2] - r[3]) * 0.0625F;
                return;
            }
        }
        if (std::abs(playbackRatio - 1.0) < 1.0e-12) {
            if (std::abs(frac) < 1.0e-12) { left = l[1]; right = r[1]; return; }
            if (std::abs(frac - 1.0) < 1.0e-12) { left = l[2]; right = r[2]; return; }
        }
        const auto t = static_cast<float>(frac);
        left = catmullRom(l.data(), t);
        right = catmullRom(r.data(), t);
        return;
    }

    if (quality_ == InterpolationQuality::Studio) {
        const StudioKernel& kern = studioTables().select(playbackRatio);
        const int taps = kern.taps;
        const double scaled = frac * kStudioPhases;
        int p = static_cast<int>(scaled);
        p = std::clamp(p, 0, kStudioPhases - 1);
        const float mix = static_cast<float>(scaled - static_cast<double>(p));
        const float* a = studioTables().data.data() + kern.offset + static_cast<std::size_t>(p * taps);
        const float* b = a + taps;
        alignas(16) std::array<float, kStudioMaxTaps * 2> tmp;
        const float* x = studioFrames(sample, i1 - taps / 2 + 1, taps, looping, tmp.data());
        const StudioVec mixV = {mix, mix, mix, mix};
        StudioVec acc0 = {0.0F, 0.0F, 0.0F, 0.0F}, acc1 = acc0, acc2 = acc0, acc3 = acc0;
        float accL = 0.0F;
        float accR = 0.0F;
        if (sample.channels > 1U) {
            // x = L R L R ...: each weight is used for an L and an R value.
            for (int k = 0; k < taps; k += 4) {
                const StudioVec wa = studioLoad(a + k), wb = studioLoad(b + k);
                const StudioVec w = wa + (wb - wa) * mixV;
                const StudioVec wLo = __builtin_shufflevector(w, w, 0, 0, 1, 1);
                const StudioVec wHi = __builtin_shufflevector(w, w, 2, 2, 3, 3);
                const float* xk = x + static_cast<std::ptrdiff_t>(k) * 2;
                if ((k & 4) == 0) { acc0 += studioLoad(xk) * wLo; acc1 += studioLoad(xk + 4) * wHi; }
                else              { acc2 += studioLoad(xk) * wLo; acc3 += studioLoad(xk + 4) * wHi; }
            }
            const StudioVec sum = (acc0 + acc1) + (acc2 + acc3);
            accL = sum[0] + sum[2];
            accR = sum[1] + sum[3];
        } else {
            for (int k = 0; k < taps; k += 4) {
                const StudioVec wa = studioLoad(a + k), wb = studioLoad(b + k);
                const StudioVec w = wa + (wb - wa) * mixV;
                if ((k & 4) == 0) acc0 += studioLoad(x + k) * w; else acc1 += studioLoad(x + k) * w;
            }
            accL = studioSum(acc0 + acc1);
            accR = accL;
        }
        left = accL;
        right = accR;
        return;
    }

    const int taps = static_cast<int>(quality_);
    const int half = taps / 2;
    std::array<float, kMaxTaps> w{};
    std::array<float, kMaxTaps> l{};
    std::array<float, kMaxTaps> r{};
    sincWeights(tables(), taps, frac, playbackRatio, w.data());
    gather(sample, i1 - half + 1, taps, looping, l.data(), r.data());
    float accL = 0.0F;
    float accR = 0.0F;
    for (int k = 0; k < taps; ++k) {
        accL += l[static_cast<std::size_t>(k)] * w[static_cast<std::size_t>(k)];
        accR += r[static_cast<std::size_t>(k)] * w[static_cast<std::size_t>(k)];
    }
    left = accL;
    right = accR;
}

float SincInterpolator::read(const SampleData& sample,
                             double position,
                             std::uint16_t channel,
                             double playbackRatio,
                             bool looping) const noexcept {
    float l = 0.0F;
    float r = 0.0F;
    readStereo(sample, position, playbackRatio, looping, l, r);
    return channel == 0U ? l : r;
}

} // namespace silveton::sampler
