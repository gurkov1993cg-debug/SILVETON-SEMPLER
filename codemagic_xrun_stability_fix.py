from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'

for p in (cpp, view):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# Hardware validation showed that 48 kHz / 96x2 is playable but continuously
# underruns on the real SM-X130. Keep the 96-frame period (2 ms) for response,
# but add one reserve period. 96x3 = 288 frames = 6 ms total queue capacity.
s = cpp.read_text(encoding='utf-8')

if 'constexpr unsigned kPeriods = 2U;' in s:
    s = s.replace('constexpr unsigned kPeriods = 2U;', 'constexpr unsigned kPeriods = 3U;', 1)
elif 'constexpr unsigned kPeriods = 3U;' not in s:
    raise SystemExit('Direct ALSA period-count constant not found')

# Start only after two complete periods are queued. This leaves one full 96-frame
# reserve period after start and after every PREPARE/XRUN recovery instead of
# restarting with effectively no scheduling margin.
old_start = '''        sw.start_threshold = static_cast<snd_pcm_uframes_t>(
            std::max(config.periodFrames, config.bufferFrames / 2U));
'''
new_start = '''        sw.start_threshold = static_cast<snd_pcm_uframes_t>(
            config.bufferFrames - config.periodFrames);
'''
if old_start in s:
    s = s.replace(old_start, new_start, 1)
elif 'config.bufferFrames - config.periodFrames' not in s:
    raise SystemExit('Direct ALSA start-threshold block not found')

s = s.replace('48 kHz / 96 x 2', '48 kHz / 96 x 3')
s = s.replace('48 kHz / 96x2', '48 kHz / 96x3')
s = s.replace('96 frames x 2 at 48 kHz', '96 frames x 3 at 48 kHz')

cpp.write_text(s, encoding='utf-8')

# Keep the hardware status truthful: 96-frame periods, 3 periods, 288-frame buffer.
v = view.read_text(encoding='utf-8')
v = v.replace('a.bufferSizeFrames == 192', 'a.bufferSizeFrames == 288')
v = v.replace('"48k 96x2"', '"48k 96x3"')
view.write_text(v, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
for token in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
    'kPreferredMinimumPeriodFrames = 96U;',
    'config.bufferFrames - config.periodFrames',
    'sw.avail_min = static_cast<snd_pcm_uframes_t>(config.periodFrames);',
    'sw.stop_threshold = static_cast<snd_pcm_uframes_t>(config.bufferFrames);',
    'xrunCount_.fetch_add(1, std::memory_order_relaxed);',
    'SNDRV_PCM_IOCTL_PREPARE',
]:
    if token not in final_cpp:
        raise SystemExit(f'XRUN stability contract missing: {token}')

if 'constexpr unsigned kPeriods = 2U;' in final_cpp:
    raise SystemExit('stale 2-period Direct ALSA queue remains')

final_view = view.read_text(encoding='utf-8')
for token in ['a.bufferSizeFrames == 288', '"48k 96x3"']:
    if token not in final_view:
        raise SystemExit(f'XRUN UI contract missing: {token}')

print('PASS: SM-X130 XRUN stability fix applied (48 kHz / 96x3, 288-frame queue, 2-period start prime)')
