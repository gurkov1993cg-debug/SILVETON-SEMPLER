#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
header = root / 'core/include/silveton/sampler/SvBankRuntime.h'
runtime = root / 'core/src/sampler/SvBankRuntime.cpp'
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
sinc = root / 'core/src/sampler/SincInterpolator.cpp'
for p in (header, runtime, bridge, sinc):
    if not p.is_file():
        raise SystemExit(f'missing realtime bank source: {p}')

# 1) A live instrument needs a useful RAM attack window. 65536 frames is about
# 1.36 s at 48 kHz, but SvBankRuntime.cpp below applies a fair 256 MiB total cap
# across all SAMPLEs so large banks cannot consume unbounded RAM.
h = header.read_text(encoding='utf-8')
h = h.replace('std::uint64_t attackPreloadFrames = 4096U',
              'std::uint64_t attackPreloadFrames = 65536U')
header.write_text(h, encoding='utf-8')

s = runtime.read_text(encoding='utf-8')

# 2) MADV_RANDOM disables the kernel read-ahead that sequential PCM playback
# needs. Give both fd and mapping sequential hints. Small/medium banks are also
# warmed into page cache while the background bank loader is running; huge banks
# stay demand-paged.
old_advice = '''#ifdef MADV_RANDOM
        (void)madvise(mapping, static_cast<std::size_t>(size), MADV_RANDOM);
#endif
'''
new_advice = '''#if defined(__linux__) || defined(__ANDROID__)
        (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#ifdef MADV_SEQUENTIAL
        (void)madvise(mapping, static_cast<std::size_t>(size), MADV_SEQUENTIAL);
#endif
#ifdef MADV_WILLNEED
        constexpr std::uint64_t kWholeBankWarmLimit = 256ULL * 1024ULL * 1024ULL;
        if (size <= kWholeBankWarmLimit) {
            (void)madvise(mapping, static_cast<std::size_t>(size), MADV_WILLNEED);
        }
#endif
'''
if old_advice in s:
    s = s.replace(old_advice, new_advice, 1)
elif 'POSIX_FADV_SEQUENTIAL' not in s or 'MADV_SEQUENTIAL' not in s:
    raise SystemExit('mmap advice anchor changed')

# 3) Bound the expanded attack cache. Every SAMPLE receives a fair share of a
# 256 MiB float cache. On the current small bank this materializes the complete
# SAMPLEs; on multi-gigabyte banks the tails remain mmap-backed.
old_preload = '''        const std::uint64_t preloadFrames = std::min<std::uint64_t>(attackPreloadFrames, r.totalFrames);
'''
new_preload = '''        constexpr std::uint64_t kAttackCacheBudgetBytes = 256ULL * 1024ULL * 1024ULL;
        const std::uint64_t fairSampleCount = std::max<std::uint64_t>(1U, header.sampleCount);
        const std::uint64_t cacheBytesPerFrame = std::max<std::uint64_t>(1U, r.channels) * sizeof(float);
        const std::uint64_t fairPreloadFrames = std::max<std::uint64_t>(
            1024U, (kAttackCacheBudgetBytes / fairSampleCount) / cacheBytesPerFrame);
        const std::uint64_t preloadFrames = std::min<std::uint64_t>(
            r.totalFrames, std::min<std::uint64_t>(attackPreloadFrames, fairPreloadFrames));
'''
if old_preload in s:
    s = s.replace(old_preload, new_preload, 1)
elif 'kAttackCacheBudgetBytes' not in s:
    raise SystemExit('SV attack preload anchor changed')

runtime.write_text(s, encoding='utf-8')

# 4) The Android bank loader had been explicitly overriding the runtime default
# back down to 4096 frames. Remove that low-latency regression.
b = bridge.read_text(encoding='utf-8')
if 'SvBankRuntime::load(path, 4096U, false)' in b:
    b = b.replace('SvBankRuntime::load(path, 4096U, false)',
                  'SvBankRuntime::load(path, 65536U, false)', 1)
elif 'SvBankRuntime::load(path, 65536U, false)' not in b:
    raise SystemExit('Android bank preload call anchor changed')
bridge.write_text(b, encoding='utf-8')

# 5) Zero-cost quality-preserving fast path for the common case: a 48 kHz SAMPLE
# played at its root key has ratio exactly 1 and integer positions. Cubic4 then
# returns exactly the source frame, so four reads can safely become one.
q = sinc.read_text(encoding='utf-8')
needle = '''    if (quality_ == InterpolationQuality::LiveCubic4) {
        const long long i1 = static_cast<long long>(std::floor(position));
'''
replacement = '''    if (quality_ == InterpolationQuality::LiveCubic4) {
        if (std::abs(playbackRatio - 1.0) < 1.0e-12) {
            const double nearest = std::floor(position + 0.5);
            if (std::abs(position - nearest) < 1.0e-12) {
                return readFrame(sample, static_cast<long long>(nearest), channel, looping);
            }
        }
        const long long i1 = static_cast<long long>(std::floor(position));
'''
if needle in q:
    q = q.replace(needle, replacement, 1)
elif 'std::abs(playbackRatio - 1.0) < 1.0e-12' not in q:
    raise SystemExit('LiveCubic4 fast-path anchor changed')
sinc.write_text(q, encoding='utf-8')

# Refuse to ship if the old anti-streaming hint or tiny Android preload survives.
final_runtime = runtime.read_text(encoding='utf-8')
final_bridge = bridge.read_text(encoding='utf-8')
final_sinc = sinc.read_text(encoding='utf-8')
for token in [
    'POSIX_FADV_SEQUENTIAL',
    'MADV_SEQUENTIAL',
    'kWholeBankWarmLimit',
    'kAttackCacheBudgetBytes',
    'fairPreloadFrames',
]:
    if token not in final_runtime:
        raise SystemExit(f'realtime bank streaming contract missing: {token}')
if 'MADV_RANDOM' in final_runtime:
    raise SystemExit('MADV_RANDOM still disables PCM read-ahead')
if 'SvBankRuntime::load(path, 4096U, false)' in final_bridge:
    raise SystemExit('tiny 4096-frame Android preload still active')
if 'SvBankRuntime::load(path, 65536U, false)' not in final_bridge:
    raise SystemExit('65536-frame Android attack cache missing')
if 'std::abs(playbackRatio - 1.0) < 1.0e-12' not in final_sinc:
    raise SystemExit('exact-rate playback fast path missing')

print('PASS: realtime bank playback uses sequential mmap read-ahead + bounded 65536-frame attack cache + exact-rate fast path')
