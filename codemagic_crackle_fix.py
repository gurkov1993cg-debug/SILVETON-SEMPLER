#!/usr/bin/env python3
"""SILVETON SAMPLER - crackle fix.

Runs LAST in codemagic.yaml, after codemagic_xrun_stability_fix.py, on the
fully generated tree. It does not change the 96 kHz / 96 x 2 audio contract,
the GUI, MIDI, COMBI/LIST/BLENDS, root/SELinux or any other function.

Fixes (each was reproduced on the generated source before this patch):

1. Looped samples lost their attack. The interpolator replaced every frame
   BEFORE the loop start with the frame AT the loop start, so a looped sample
   played a constant (DC) value from note-on until the loop start and then
   jumped into the real audio: a pop at every note and no attack.
   Now only frames after the loop END wrap; the attack plays as recorded.

2. The audio thread read the not-yet-cached part of SILVETON_BANK.sv itself.
   Only the attack is in RAM; when a voice reached the uncached part, the
   1 ms audio thread waited for the flash storage -> XRUN -> crackle.
   New SampleStreamPrefetcher thread (non-realtime, CPU 0-5) loads the next
   0.75 s of every playing voice in advance. The bank mapping uses normal
   kernel read-ahead instead of MADV_SEQUENTIAL (which lets Android drop
   loop pages first).

3. Notes were cut in one sample (click) when MONO/LEGATO changed note and when
   the voice limit was reached. They now fade out in 4 ms; a few voices are
   kept free so a new note never has to cut a sounding one.

4. Less work per sample: both channels are interpolated in one pass with a
   direct pointer into the bank; portamento uses a precomputed multiplier;
   denormal floats are flushed to zero on the audio thread.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
MARK = 'SILVETON_CRACKLE_FIX_V1'


def path(rel):
    p = root / rel
    if not p.is_file():
        raise SystemExit(f'crackle fix: missing file {rel}')
    return p


def once(rel, old, new):
    p = path(rel)
    s = p.read_text(encoding='utf-8')
    n = s.count(old)
    if n != 1:
        head = old.strip().splitlines()[0][:90]
        raise SystemExit(f'crackle fix: anchor found {n}x (expected 1) in {rel}: {head}')
    p.write_text(s.replace(old, new, 1), encoding='utf-8')


def put(rel, content):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding='utf-8')


ENGINE = 'core/src/sampler/SamplerEngine.cpp'
ENGINE_H = 'core/include/silveton/sampler/SamplerEngine.h'
STREAM_H = 'core/include/silveton/sampler/SampleStream.h'
STREAM_CPP = 'core/src/sampler/SampleStream.cpp'
BANK_CPP = 'core/src/sampler/SvBankRuntime.cpp'
BRIDGE = 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
CORE_CMAKE = 'core/CMakeLists.txt'
TESTS = 'core/tests/SamplerTests.cpp'

if MARK in path(ENGINE).read_text(encoding='utf-8'):
    raise SystemExit('crackle fix: already applied to this tree')

# ---------------------------------------------------------------- new files
put('core/include/silveton/sampler/SincInterpolator.h', r'''#pragma once

#include "silveton/sampler/SamplerTypes.h"
#include <cstddef>

namespace silveton::sampler {

enum class InterpolationQuality : int { LiveCubic4 = 4, Fast8 = 8, High16 = 16, Ultra32 = 32 };

// LiveCubic4 is the live default (4-point cubic). Fast8/High16/Ultra32 use a
// Blackman-windowed sinc kernel evaluated from tables built once on the
// control thread (no sin/cos in the audio thread).
//
// Loop rule: when a sample loops, only frames at or after the loop END wrap
// back into the loop. Frames before the loop START are the attack and are
// played as recorded.
class SincInterpolator {
public:
    explicit SincInterpolator(InterpolationQuality quality = InterpolationQuality::LiveCubic4) noexcept;

    void setQuality(InterpolationQuality quality) noexcept { quality_ = quality; }
    [[nodiscard]] InterpolationQuality quality() const noexcept { return quality_; }

    [[nodiscard]] float read(const SampleData& sample,
                             double position,
                             std::uint16_t channel,
                             double playbackRatio,
                             bool looping) const noexcept;

    // Left and right in one pass (mono samples return the same value twice).
    void readStereo(const SampleData& sample,
                    double position,
                    double playbackRatio,
                    bool looping,
                    float& left,
                    float& right) const noexcept;

private:
    InterpolationQuality quality_;
};

} // namespace silveton::sampler
''')
put('core/src/sampler/SincInterpolator.cpp', r'''#include "silveton/sampler/SincInterpolator.h"
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
''')
put('core/include/silveton/sampler/SampleStreamPrefetcher.h', r'''#pragma once

#include "silveton/sampler/SamplerEngine.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace silveton::sampler {

// Brings the part of the sample bank that the playing voices will need next
// into memory on an ordinary (non-realtime) thread.
//
// Only the attack of every sample is kept in RAM. The rest of the sample is
// read from the memory-mapped SILVETON_BANK.sv. Without this thread the audio
// thread itself had to wait for the flash storage whenever a voice reached a
// part of the bank that was not cached yet - that wait is longer than the
// 1 ms audio period and is heard as a crackle.
//
// The thread never writes, never takes a lock the audio thread uses and cannot
// crash on a bank that was just unloaded (madvise only reports an error then).
class SampleStreamPrefetcher {
public:
    explicit SampleStreamPrefetcher(const SamplerEngine& engine) noexcept : engine_(engine) {}
    ~SampleStreamPrefetcher() { stop(); }

    SampleStreamPrefetcher(const SampleStreamPrefetcher&) = delete;
    SampleStreamPrefetcher& operator=(const SampleStreamPrefetcher&) = delete;

    // Optional CPU range for the read-ahead thread (Linux/Android only), set
    // before start(). The sampler keeps it on CPU 0-5, away from the audio core.
    void setCpuRange(int firstCpu, int lastCpu) noexcept { firstCpu_ = firstCpu; lastCpu_ = lastCpu; }

    bool start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return run_.load(std::memory_order_acquire); }

    // Diagnostics: number of memory ranges requested since start.
    [[nodiscard]] std::uint64_t requests() const noexcept { return requests_.load(std::memory_order_relaxed); }

    // One pass over all voices (also used by tests). Thread-safe w.r.t. audio.
    void runOnce() noexcept;

private:
    void loop() noexcept;

    struct SlotCache {
        std::uintptr_t begin{0U};
        std::uintptr_t end{0U};
        std::int64_t stampMs{0};
    };

    const SamplerEngine& engine_;
    std::thread thread_{};
    std::atomic<bool> run_{false};
    std::atomic<std::uint64_t> requests_{0U};
    std::atomic<int> advice_{0};
    bool populateWorked_{false};
    int firstCpu_{-1};
    int lastCpu_{-1};
    bool lastPassHadWork_{false};
    SlotCache cache_[SamplerEngine::kMaxStreamWindows]{};
};

} // namespace silveton::sampler
''')
put('core/src/sampler/SampleStreamPrefetcher.cpp', r'''#include "silveton/sampler/SampleStreamPrefetcher.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
#endif

namespace silveton::sampler {
namespace {

constexpr std::int64_t kRefreshMs = 100;   // re-assert residency of an unchanged window
constexpr auto kPassInterval = std::chrono::milliseconds(2);   // while voices play
constexpr auto kIdleInterval = std::chrono::milliseconds(10);  // nothing to read ahead

std::uintptr_t pageSize() noexcept {
    static const std::uintptr_t size = [] {
        const long v = ::sysconf(_SC_PAGESIZE);
        return static_cast<std::uintptr_t>(v > 0 ? v : 4096L);
    }();
    return size;
}

std::int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

bool SampleStreamPrefetcher::start() noexcept {
    if (run_.load(std::memory_order_acquire)) return true;
    for (auto& c : cache_) c = SlotCache{};
    run_.store(true, std::memory_order_release);
    try {
        thread_ = std::thread(&SampleStreamPrefetcher::loop, this);
    } catch (...) {
        run_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void SampleStreamPrefetcher::stop() noexcept {
    run_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void SampleStreamPrefetcher::runOnce() noexcept {
    const std::uintptr_t page = pageSize();
    const std::int64_t now = nowMs();
    bool work = false;
    for (std::size_t slot = 0; slot < SamplerEngine::kMaxStreamWindows; ++slot) {
        SamplerEngine::StreamWindow w{};
        if (!engine_.readStreamWindow(slot, w) || w.begin == 0U || w.bytes == 0U) {
            cache_[slot] = SlotCache{};
            continue;
        }
        work = true;
        const std::uintptr_t b = w.begin & ~(page - 1U);
        const std::uintptr_t e = (w.begin + w.bytes + page - 1U) & ~(page - 1U);
        SlotCache& c = cache_[slot];
        const bool fresh = c.end != 0U && (now - c.stampMs) < kRefreshMs;
        if (fresh && b >= c.begin && e <= c.end) continue;   // already resident
        std::uintptr_t from = b;
        if (fresh && b >= c.begin && b < c.end) from = c.end; // only the new part
        if (e > from) {
            void* addr = reinterpret_cast<void*>(from);
            const std::size_t len = static_cast<std::size_t>(e - from);
            int rc = -1;
#if defined(__linux__) || defined(__ANDROID__)
            if (advice_.load(std::memory_order_relaxed) == 0) {
                // Synchronously fault the pages in here, so the audio thread
                // finds them mapped. Returns ENOMEM (no crash) for unmapped memory.
                rc = ::madvise(addr, len, MADV_POPULATE_READ);
                if (rc == 0) {
                    populateWorked_ = true;
                } else if (errno == EINVAL && !populateWorked_) {
                    advice_.store(1, std::memory_order_relaxed); // kernel < 5.14: never supported
                }
            }
#endif
            if (rc != 0) {
                // Asynchronous read-ahead into the page cache (older kernels / macOS tests).
                (void)::madvise(addr, len, MADV_WILLNEED);
            }
            requests_.fetch_add(1U, std::memory_order_relaxed);
        }
        c.begin = b;
        c.end = e;
        if (from == b) c.stampMs = now;
    }
    lastPassHadWork_ = work;
}

void SampleStreamPrefetcher::loop() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    (void)::pthread_setname_np(::pthread_self(), "SilvPrefetch");
    if (firstCpu_ >= 0 && lastCpu_ >= firstCpu_) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu = firstCpu_; cpu <= lastCpu_; ++cpu) CPU_SET(cpu, &set);
        (void)::sched_setaffinity(0, sizeof(set), &set);
    }
#endif
    while (run_.load(std::memory_order_acquire)) {
        runOnce();
        std::this_thread::sleep_for(lastPassHadWork_ ? kPassInterval : kIdleInterval);
    }
}

} // namespace silveton::sampler
''')

once(CORE_CMAKE,
     '    src/sampler/SampleStream.cpp\n',
     '    src/sampler/SampleStream.cpp\n    src/sampler/SampleStreamPrefetcher.cpp\n')

# ------------------------------------------------- direct view of bank memory
once(STREAM_H,
     '''class SampleStream {
public:
    virtual ~SampleStream() = default;
    [[nodiscard]] virtual std::uint64_t frameCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sampleRate() const noexcept = 0;
    [[nodiscard]] virtual float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept = 0;
};''',
     '''// Direct read-only view of interleaved little-endian sample memory (normally
// the memory-mapped bank). data == nullptr means "not available".
struct RawPcmView {
    static constexpr std::uint8_t kNone = 0U;
    static constexpr std::uint8_t kPcm16 = 1U;
    static constexpr std::uint8_t kPcm24 = 2U;
    static constexpr std::uint8_t kFloat32 = 3U;

    const std::uint8_t* data{nullptr}; // first byte of frame 0
    std::uint64_t frames{0};
    std::uint16_t channels{0};
    std::uint8_t format{kNone};

    [[nodiscard]] std::size_t bytesPerSample() const noexcept {
        return format == kPcm16 ? 2U : (format == kPcm24 ? 3U : (format == kFloat32 ? 4U : 0U));
    }
    [[nodiscard]] std::size_t frameBytes() const noexcept { return bytesPerSample() * channels; }
};

class SampleStream {
public:
    virtual ~SampleStream() = default;
    [[nodiscard]] virtual std::uint64_t frameCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint16_t channels() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t sampleRate() const noexcept = 0;
    [[nodiscard]] virtual float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept = 0;
    [[nodiscard]] virtual RawPcmView rawView() const noexcept { return {}; }
};''')
once(STREAM_H,
     '''    [[nodiscard]] float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept override;

private:
    MappedPcmStream() = default;''',
     '''    [[nodiscard]] float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept override;
    [[nodiscard]] RawPcmView rawView() const noexcept override;

private:
    MappedPcmStream() = default;''')
once(STREAM_H, '#include <cstdint>\n#include <memory>', '#include <cstddef>\n#include <cstdint>\n#include <memory>')

once(STREAM_CPP,
     '''#ifdef MADV_SEQUENTIAL
    (void)madvise(map, static_cast<std::size_t>(st.st_size), MADV_SEQUENTIAL);
#endif
''',
     '''    // Normal read-ahead: MADV_SEQUENTIAL lets the kernel drop pages right after
    // use, which hurts loops that return to the same pages again and again.
#ifdef MADV_NORMAL
    (void)madvise(map, static_cast<std::size_t>(st.st_size), MADV_NORMAL);
#endif
''')
once(STREAM_CPP,
     'float MappedPcmStream::readFrame(',
     '''RawPcmView MappedPcmStream::rawView() const noexcept {
    RawPcmView v{};
    if (mapping_ == nullptr || spec_.bigEndian) return v;
    std::uint8_t format = RawPcmView::kNone;
    if (spec_.formatTag == 1U && spec_.bitDepth == 16U) format = RawPcmView::kPcm16;
    else if (spec_.formatTag == 1U && spec_.bitDepth == 24U) format = RawPcmView::kPcm24;
    else if (spec_.formatTag == 3U && spec_.bitDepth == 32U) format = RawPcmView::kFloat32;
    if (format == RawPcmView::kNone) return v;
    v.format = format;
    v.channels = spec_.channels;
    const std::uint64_t needed = spec_.dataOffset + spec_.frameCount * v.frameBytes();
    if (needed > mappingSize_) return RawPcmView{};
    v.data = static_cast<const std::uint8_t*>(mapping_) + spec_.dataOffset;
    v.frames = spec_.frameCount;
    return v;
}

float MappedPcmStream::readFrame(''')

once(BANK_CPP,
     '''#ifdef MADV_SEQUENTIAL
        (void)madvise(mapping, static_cast<std::size_t>(size), MADV_SEQUENTIAL);
#endif
''',
     '''        // Normal read-ahead (the sequential hint marks bank pages as "drop first",
        // so Android evicted loop pages and the audio thread had to re-read them
        // from flash). Pages ahead of each playing voice are brought in by
        // SampleStreamPrefetcher on a non-realtime thread.
#ifdef MADV_NORMAL
        (void)madvise(mapping, static_cast<std::size_t>(size), MADV_NORMAL);
#endif
''')
once(BANK_CPP,
     '''    [[nodiscard]] float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept override {
        if (bank_ == nullptr || frame >= frames_ || channels_ == 0U) return 0.0F;''',
     '''    [[nodiscard]] RawPcmView rawView() const noexcept override {
        RawPcmView v{};
        if (bank_ == nullptr || channels_ == 0U) return v;
        if (encoding_ == 1U && bitsPerSample_ == 16U) v.format = RawPcmView::kPcm16;
        else if (encoding_ == 2U && bitsPerSample_ == 24U) v.format = RawPcmView::kPcm24;
        else if (encoding_ == 3U && bitsPerSample_ == 32U) v.format = RawPcmView::kFloat32;
        else return v;
        v.channels = channels_;
        const std::uint64_t bytes = frames_ * v.frameBytes();
        if (dataOffset_ > bank_->size() || bytes > bank_->size() - dataOffset_) return RawPcmView{};
        v.data = bank_->data() + static_cast<std::size_t>(dataOffset_);
        v.frames = frames_;
        return v;
    }

    [[nodiscard]] float readFrame(std::uint64_t frame, std::uint16_t channel) const noexcept override {
        if (bank_ == nullptr || frame >= frames_ || channels_ == 0U) return 0.0F;''')

# ------------------------------------------------------------ engine header
once(ENGINE_H,
     '''class SamplerEngine {
public:
    SamplerEngine();''',
     '''class SamplerEngine {
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

    SamplerEngine();''')
once(ENGINE_H,
     '''    [[nodiscard]] std::uint64_t droppedEventCount() const noexcept { return events_.droppedCount(); }
''',
     '''    [[nodiscard]] std::uint64_t droppedEventCount() const noexcept { return events_.droppedCount(); }

    // Any thread, no locks. false = slot changed while reading (try next pass).
    bool readStreamWindow(std::size_t slot, StreamWindow& out) const noexcept;
    // How far ahead of each voice the bank is brought into memory (seconds).
    void setStreamLookaheadSeconds(double seconds) noexcept;
''')

# ------------------------------------------------------------ engine source
once(ENGINE,
     '#include "silveton/sampler/SamplerEngine.h"\n',
     '#include "silveton/sampler/SamplerEngine.h"\n#include "silveton/sampler/SampleStream.h"\n// ' + MARK + '\n')

once(ENGINE,
     '''        double glideLogStep{0.0};
        std::uint32_t glideFramesRemaining{0U};''',
     '''        double glideLogStep{0.0};
        double glideMul{1.0};
        std::uint32_t glideFramesRemaining{0U};
        // Click-free 4 ms fade used when a voice is taken over (voice limit,
        // MONO/LEGATO note change) instead of a one-sample cut.
        float fadeGain{1.0F};
        float fadeStep{0.0F};
        bool fading{false};''')

once(ENGINE,
     '    std::array<Voice, kMaxVoices> voices{};\n',
     '    // 128 playable voices + spare tail slots for 4 ms takeover fades.\n'
     '    std::array<Voice, static_cast<std::size_t>(kMaxVoices) + SamplerEngine::kTakeoverTailVoices> voices{};\n')

once(ENGINE,
     '''    DelayFx delay{};
    ChorusFx chorus{};
    ReverbFx reverb{};

    void reset(double sr) noexcept {''',
     '''    DelayFx delay{};
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

    void reset(double sr) noexcept {''')

once(ENGINE,
     '''        delay.reset(sr); chorus.reset(sr); reverb.reset(sr);
    }
};''',
     '''        delay.reset(sr); chorus.reset(sr); reverb.reset(sr);
        clearWindows();
    }
};''')

once(ENGINE,
     '''    std::atomic_store_explicit(&pendingState_, std::move(state), std::memory_order_release);
}''',
     '''    std::atomic_store_explicit(&pendingState_, std::move(state), std::memory_order_release);
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
}''')

once(ENGINE,
     '''SamplerEngine::Impl::Voice* chooseVoice(SamplerEngine::Impl& impl) noexcept {
    for (auto& v : impl.voices) if (!v.active) return &v;
    auto* oldest = &impl.voices.front();
    for (auto& v : impl.voices) if (v.age < oldest->age) oldest = &v;
    return oldest;
}''',
     '''constexpr float kTakeoverFadeSec = 0.004F; // 4 ms: click-free and inaudible as a fade

void softKill(SamplerEngine::Impl& impl, SamplerEngine::Impl::Voice& v) noexcept {
    if (!v.active || v.fading) return;
    if (v.delayFramesRemaining > 0U) { v.active = false; return; } // has not sounded yet
    v.fading = true;
    v.keyHeld = false;
    v.sustained = false;
    v.fadeStep = -1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kTakeoverFadeSec) * impl.sampleRate));
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
}''')

once(ENGINE,
     '''                    for (auto& existing : impl.voices) {
                        if (existing.active && existing.upperIndex == ui) {
                            hadMonoVoice = true;
                            previousRatio = existing.ratio;
                            previousAmp = std::max(previousAmp, existing.ampEnv.value);
                            existing.active = false;
                        }
                    }''',
     '''                    for (auto& existing : impl.voices) {
                        if (existing.active && !existing.fading && existing.upperIndex == ui) {
                            hadMonoVoice = true;
                            previousRatio = existing.ratio;
                            previousAmp = std::max(previousAmp, existing.ampEnv.value);
                            softKill(impl, existing); // was a one-sample cut (click)
                        }
                    }''')

once(ENGINE,
     '''                            v->glideLogStep = std::log(v->targetRatio / v->ratio) / static_cast<double>(glideFrames);
                        }
                        if (upperLegato && hadMonoVoice) {
                            v->ampEnv.value = previousAmp;
                            v->ampEnv.stage = Envelope::Stage::Decay;
                        }''',
     '''                            v->glideLogStep = std::log(v->targetRatio / v->ratio) / static_cast<double>(glideFrames);
                            v->glideMul = std::exp(v->glideLogStep);
                        }
                        if (upperLegato && hadMonoVoice) {
                            v->ampEnv.value = previousAmp;
                            v->ampEnv.stage = Envelope::Stage::Decay;
                            // Cross-fade against the previous note, which fades out in 4 ms.
                            v->fadeGain = 0.0F;
                            v->fadeStep = 1.0F / static_cast<float>(std::max(1.0, static_cast<double>(kTakeoverFadeSec) * impl.sampleRate));
                        }''')

once(ENGINE,
     '''float sampleWithLoopCrossfade(const SincInterpolator& interp, const SampleData& sample, double position,
                              std::uint16_t channel, double ratio, bool looping) noexcept {
    if (!looping || !sample.loop.enabled || sample.loop.crossfadeFrames == 0U ||
        sample.loop.endFrame <= sample.loop.startFrame) {
        return interp.read(sample, position, channel, ratio, looping);
    }
    const double xfade = static_cast<double>(sample.loop.crossfadeFrames);
    const double startFade = static_cast<double>(sample.loop.endFrame) - xfade;
    if (position < startFade || position >= static_cast<double>(sample.loop.endFrame)) {
        return interp.read(sample, position, channel, ratio, looping);
    }
    const double t = std::clamp((position - startFade) / xfade, 0.0, 1.0);
    const double headPos = static_cast<double>(sample.loop.startFrame) + (position - startFade);
    const float tail = interp.read(sample, position, channel, ratio, false);
    const float head = interp.read(sample, headPos, channel, ratio, true);
    const float a = static_cast<float>(std::cos(t * kPi * 0.5));
    const float b = static_cast<float>(std::sin(t * kPi * 0.5));
    return tail * a + head * b;
}''',
     '''void readWithLoopCrossfade(const SincInterpolator& interp, const SampleData& sample, double position,
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
}''')

once(ENGINE,
     '                v.ratio *= std::exp(v.glideLogStep);\n',
     '                v.ratio *= v.glideMul;\n')

once(ENGINE,
     '''            float sL = sampleWithLoopCrossfade(interpolator_, sample, v.position, 0U, ratio, looping);
            float sR = sample.channels > 1U
                ? sampleWithLoopCrossfade(interpolator_, sample, v.position, 1U, ratio, looping)
                : sL;''',
     '''            float sL = 0.0F, sR = 0.0F;
            readWithLoopCrossfade(interpolator_, sample, v.position, ratio, looping, sL, sR);''')

once(ENGINE,
     '            const float oL = sL * g * panLeft;\n',
     '''            if (v.fadeStep != 0.0F) {
                v.fadeGain += v.fadeStep;
                if (v.fadeGain <= 0.0F) { v.ampEnv.forceOff(); v.active = false; v.fading = false; continue; }
                if (v.fadeGain >= 1.0F) { v.fadeGain = 1.0F; v.fadeStep = 0.0F; }
            }
            g *= v.fadeGain;
            const float oL = sL * g * panLeft;
''')

once(ENGINE,
     '    impl_->peakL = peakL;\n',
     '''    for (std::size_t vi = 0; vi < impl_->voices.size(); ++vi) publishVoiceWindows(*impl_, vi);

    impl_->peakL = peakL;
''')

once(ENGINE,
     '''    std::uint32_t count = 0U;
    for (const auto& v : impl_->voices) if (v.active) ++count;
    return count;''',
     '''    // A voice that is fading out after a takeover (MONO/LEGATO, voice limit)
    // is no longer a playing note, so it is not counted.
    std::uint32_t count = 0U;
    for (const auto& v : impl_->voices) if (v.active && !v.fading) ++count;
    return count;''')

# ------------------------------------------------------------------ bridge
once(BRIDGE,
     '#include "silveton/sampler/SvBankRuntime.h"\n',
     '#include "silveton/sampler/SvBankRuntime.h"\n#include "silveton/sampler/SampleStreamPrefetcher.h"\n')
once(BRIDGE,
     '''        const bool midiOk = startDirectMidiLocked();
        const bool audioOk = openLocked();''',
     '''        const bool midiOk = startDirectMidiLocked();
        // Bank read-ahead runs on an ordinary thread (CPU 0-5), never on the audio core.
        prefetcher_.setCpuRange(0, 5);
        if (!prefetcher_.start()) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "Bank read-ahead thread could not start");
        }
        const bool audioOk = openLocked();''')
once(BRIDGE,
     '''        stopDirectMidiLocked();
        closePcmLocked();''',
     '''        stopDirectMidiLocked();
        prefetcher_.stop();
        closePcmLocked();''')
once(BRIDGE,
     '        configureRealtimeThread("SilvAudioRT", 7, 7, 80);\n',
     '''        configureRealtimeThread("SilvAudioRT", 7, 7, 80);
#if defined(__aarch64__)
        // Flush denormal floats to zero on the audio thread (reverb/delay tails
        // decay into denormals; they cost extra cycles and are inaudible).
        {
            std::uint64_t fpcr = 0U;
            __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
            fpcr |= (std::uint64_t{1} << 24);
            __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
        }
#endif
''')
once(BRIDGE,
     '''    SamplerEngine engine_;
    BuiltinTestInstrument testInstrument_{};''',
     '''    SamplerEngine engine_;
    SampleStreamPrefetcher prefetcher_{engine_};
    BuiltinTestInstrument testInstrument_{};''')

# ------------------------------------------------------------------- tests
once(TESTS,
     '#include "silveton/sampler/SampleStream.h"\n',
     '#include "silveton/sampler/SampleStream.h"\n#include "silveton/sampler/SampleStreamPrefetcher.h"\n#include <chrono>\n')
once(TESTS, '''
}

int main() {''', r'''
// ---- crackle fix regression tests -------------------------------------------

// A looped sample must play its attack (the frames before the loop start).
// Before the fix the interpolator returned the loop-start frame for all of them:
// silence / a DC step and a pop at note-on.
void testLoopedSampleKeepsAttack() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=1U; s->rootKey=69U;
    s->interleaved.resize(48000U);
    for(std::size_t i=0;i<s->interleaved.size();++i) {
        const double t=static_cast<double>(i)/48000.0;
        s->interleaved[i]=static_cast<float>((t<0.25?0.8:0.4)*std::sin(2.0*3.14159265358979323846*440.0*t));
    }
    s->totalFrames=48000U; s->loop.enabled=true; s->loop.startFrame=24011U; s->loop.endFrame=48000U; // loop start value != 0
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    assert(e.postNoteOn(0U,69U,127U));
    std::vector<float> out(96U*2U); double sum=0.0, sq=0.0, maxJump=0.0; float prev=0.0F; std::size_t n=0U;
    for(int b=0;b<200;++b) { // ~0.2 s, all before the loop start (0.5 s)
        e.render(out.data(),96U);
        for(std::size_t i=0;i<96U;++i) {
            const float x=out[i*2U]; sum+=x; sq+=static_cast<double>(x)*x;
            maxJump=std::max(maxJump,static_cast<double>(std::abs(x-prev))); prev=x; ++n;
        }
    }
    const double mean=sum/static_cast<double>(n); const double rmsv=std::sqrt(sq/static_cast<double>(n));
    assert(rmsv>0.2);              // attack is audible
    assert(std::abs(mean)<0.01);   // no DC plateau
    assert(maxJump<0.1);           // no step at note-on (440 Hz slope at 96 kHz is ~0.02)
}

float refFrame(const SampleData& s, long long f, std::uint16_t ch, bool looping) {
    long long r=f;
    if(looping && s.loop.enabled && s.loop.endFrame>s.loop.startFrame) {
        const auto ls=static_cast<long long>(s.loop.startFrame), le=static_cast<long long>(s.loop.endFrame);
        if(r>=le) r=ls+((r-ls)%(le-ls));
    }
    if(r<0 || r>=static_cast<long long>(s.frameCount())) return 0.0F;
    const auto u=static_cast<std::uint64_t>(r);
    const std::uint16_t c=std::min<std::uint16_t>(ch,static_cast<std::uint16_t>(s.channels-1U));
    if(u<s.preloadedFrameCount()) return s.interleaved[static_cast<std::size_t>(u)*s.channels+c];
    return s.stream->readFrame(u,c);
}

// The direct-pointer stereo path must give exactly the same samples as a plain
// frame-by-frame cubic, in RAM, in the mapped file, across the RAM/file border
// and across the loop end.
void testStereoReadMatchesChannelRead() {
    const std::string path="/tmp/silveton_crackle_stereo.wav";
    const std::uint32_t frames=20000U;
    {
        std::ofstream f(path,std::ios::binary|std::ios::trunc);
        f.write("RIFF",4); putLe32(f,36U+frames*4U); f.write("WAVEfmt ",8); putLe32(f,16U); putLe16(f,1U); putLe16(f,2U);
        putLe32(f,48000U); putLe32(f,48000U*4U); putLe16(f,4U); putLe16(f,16U); f.write("data",4); putLe32(f,frames*4U);
        for(std::uint32_t i=0;i<frames;++i) {
            putLe16(f,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(9000.0*std::sin(0.031*i)))));
            putLe16(f,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(7000.0*std::cos(0.017*i)))));
        }
    }
    MappedPcmSpec spec{}; spec.dataOffset=44U; spec.frameCount=frames; spec.sampleRate=48000U; spec.channels=2U; spec.bitDepth=16U; spec.formatTag=1U;
    std::string err; auto stream=MappedPcmStream::open(path,spec,err); assert(stream!=nullptr);
    assert(stream->rawView().data!=nullptr);
    SampleData s; s.sampleRate=48000U; s.channels=2U; s.totalFrames=frames; s.stream=stream;
    s.interleaved.resize(1000U*2U);
    for(std::uint32_t i=0;i<1000U;++i) { s.interleaved[i*2U]=stream->readFrame(i,0U); s.interleaved[i*2U+1U]=stream->readFrame(i,1U); }
    s.loop.enabled=true; s.loop.startFrame=5000U; s.loop.endFrame=15000U;
    SincInterpolator interp(InterpolationQuality::LiveCubic4);
    for(double pos=0.0; pos<16000.0; pos+=0.731) {
        for(int lp=0; lp<2; ++lp) {
            const bool looping=lp==1;
            if(looping && pos>=15000.0) continue;
            float l=0.0F, r=0.0F; interp.readStereo(s,pos,0.731,looping,l,r);
            const auto i1=static_cast<long long>(std::floor(pos)); const auto t=static_cast<float>(pos-std::floor(pos));
            for(std::uint16_t ch=0; ch<2U; ++ch) {
                const float y0=refFrame(s,i1-1,ch,looping), y1=refFrame(s,i1,ch,looping), y2=refFrame(s,i1+1,ch,looping), y3=refFrame(s,i1+2,ch,looping);
                const float a0=-0.5F*y0+1.5F*y1-1.5F*y2+0.5F*y3, a1=y0-2.5F*y1+2.0F*y2-0.5F*y3, a2=-0.5F*y0+0.5F*y2;
                const float ref=((a0*t+a1)*t+a2)*t+y1;
                const float got=ch==0U?l:r;
                assert(std::abs(got-ref)<1.0e-6F);
                assert(std::abs(interp.read(s,pos,ch,0.731,looping)-got)<1.0e-9F);
            }
        }
    }
    std::filesystem::remove(path);
}

// MONO note change must not cut the previous note in one sample.
void testMonoTakeoverIsClickFree() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=96000U; s->channels=1U; s->rootKey=60U;
    s->interleaved.resize(96000U);
    for(std::size_t i=0;i<s->interleaved.size();++i) s->interleaved[i]=static_cast<float>(0.8*std::sin(2.0*3.14159265358979323846*220.0*static_cast<double>(i)/96000.0));
    s->totalFrames=96000U; s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=96000U;
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    auto mono=std::make_shared<EngineState>(*st); mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(mono);
    std::vector<float> out(96U*2U); double maxJump=0.0; float prev=0.0F;
    const std::uint8_t key=60U; // same pitch every time: only the takeover itself could make a step
    for(int step=0; step<40; ++step) {
        assert(e.postNoteOn(0U,key,127U));
        for(int b=0;b<3+(step%5);++b) {
            e.render(out.data(),96U);
            for(std::size_t i=0;i<96U;++i) { maxJump=std::max(maxJump,static_cast<double>(std::abs(out[i*2U]-prev))); prev=out[i*2U]; }
        }
    }
    // 220 Hz at 0.8*0.707 gain moves at most ~0.01 per sample; a cut would jump up to ~0.56.
    assert(maxJump<0.08);
    assert(e.activeVoiceCount()<=2U);
}

// Playing voices publish the not-yet-cached part of the mapped sample and the
// read-ahead thread requests it; nothing is published for RAM-only samples.
void testBankReadAheadWindows() {
    const std::string path="/tmp/silveton_crackle_readahead.wav";
    const std::uint32_t frames=240000U;
    {
        std::ofstream f(path,std::ios::binary|std::ios::trunc);
        f.write("RIFF",4); putLe32(f,36U+frames*4U); f.write("WAVEfmt ",8); putLe32(f,16U); putLe16(f,1U); putLe16(f,2U);
        putLe32(f,48000U); putLe32(f,48000U*4U); putLe16(f,4U); putLe16(f,16U); f.write("data",4); putLe32(f,frames*4U);
        std::vector<char> zeros(static_cast<std::size_t>(frames)*4U,0); f.write(zeros.data(),static_cast<std::streamsize>(zeros.size()));
    }
    MappedPcmSpec spec{}; spec.dataOffset=44U; spec.frameCount=frames; spec.sampleRate=48000U; spec.channels=2U; spec.bitDepth=16U; spec.formatTag=1U;
    std::string err; auto stream=MappedPcmStream::open(path,spec,err); assert(stream!=nullptr);
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=2U; s->totalFrames=frames; s->stream=stream; s->rootKey=60U;
    s->interleaved.assign(4096U*2U,0.0F);
    auto st=makeState(s);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    SampleStreamPrefetcher prefetcher(e);
    assert(e.postNoteOn(0U,60U,100U));
    std::vector<float> out(96U*2U);
    for(int b=0;b<4;++b) e.render(out.data(),96U);
    const auto raw=stream->rawView();
    const auto lo=reinterpret_cast<std::uintptr_t>(raw.data);
    const auto hi=lo+static_cast<std::uintptr_t>(raw.frames*raw.frameBytes());
    bool found=false;
    for(std::size_t slot=0; slot<SamplerEngine::kMaxStreamWindows; ++slot) {
        SamplerEngine::StreamWindow w{};
        if(!e.readStreamWindow(slot,w) || w.bytes==0U) continue;
        found=true;
        assert(w.begin>=lo+4096U*4U);          // starts after the RAM attack prefix
        assert(w.begin+w.bytes<=hi);           // never outside the sample
        assert(w.bytes>=static_cast<std::size_t>(0.5*48000.0*4.0)); // about 0.75 s ahead at root pitch
    }
    assert(found);
    prefetcher.runOnce();
    assert(prefetcher.requests()>0U);
    assert(prefetcher.start()); std::this_thread::sleep_for(std::chrono::milliseconds(10)); prefetcher.stop();
    assert(e.postPanic()); e.render(out.data(),96U); e.render(out.data(),96U);
    for(std::size_t slot=0; slot<SamplerEngine::kMaxStreamWindows; ++slot) {
        SamplerEngine::StreamWindow w{}; assert(e.readStreamWindow(slot,w)); assert(w.bytes==0U);
    }
    std::filesystem::remove(path);
}

// With all 128 voices sounding, a new note takes the oldest voice. That voice
// must fade out (spare tail slot) instead of being cut in one sample, and
// polyphony must stay 128.
void testVoiceLimitTakeoverIsClickFree() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=1U; s->rootKey=60U;
    s->interleaved.assign(48000U,0.5F); s->totalFrames=48000U; // constant level: any cut is a visible step
    s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=48000U;
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    prog->layers[0].ampEnvelope={0.01F,0.01F,1.0F,0.02F};
    auto quiet=std::make_shared<EngineState>(*st); quiet->combi.uppers[0].levelDb=-40.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(quiet);
    for(int k=0;k<128;++k) assert(e.postNoteOn(0U,static_cast<std::uint8_t>(k),127U));
    std::vector<float> out(96U*2U);
    for(int b=0;b<30;++b) e.render(out.data(),96U);
    assert(e.activeVoiceCount()==128U);
    float prev=out[95U*2U]; double maxJump=0.0;
    assert(e.postNoteOn(0U,60U,127U)); // 129th note
    for(int b=0;b<12;++b) {
        e.render(out.data(),96U);
        for(std::size_t i=0;i<96U;++i) { maxJump=std::max(maxJump,static_cast<double>(std::abs(out[i*2U]-prev))); prev=out[i*2U]; }
    }
    // One voice is ~0.0035 here: a one-sample cut would jump by that much.
    assert(maxJump<0.001);
    assert(e.activeVoiceCount()==128U);
}
}

int main() {''')
once(TESTS,
     '    testSampleRecorderRf64();\n    std::puts("SILVETON sampler tests passed");',
     '''    testSampleRecorderRf64();
    testLoopedSampleKeepsAttack();
    testStereoReadMatchesChannelRead();
    testMonoTakeoverIsClickFree();
    testBankReadAheadWindows();
    testVoiceLimitTakeoverIsClickFree();
    std::puts("SILVETON sampler tests passed");''')

print('PASS: crackle fix applied (loop attack restored + bank read-ahead thread + 4 ms voice takeover fades + stereo direct read + FTZ); 96k/96x2 contract unchanged')
