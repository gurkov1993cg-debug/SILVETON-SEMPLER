#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
audit = root / 'ci/audit_direct_alsa.py'
sinc = root / 'core/src/sampler/SincInterpolator.cpp'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
tests = root / 'core/tests/SamplerTests.cpp'
for p in (bridge, view, audit, sinc, root_mode, tests):
    if not p.is_file():
        raise SystemExit(f'missing sound-priority source: {p}')

b = bridge.read_text(encoding='utf-8')

# SOUND IS PRIORITY #1.
# Final live target: 96 kHz / 96 frames / 2 periods.
# 96 frames at 96 kHz = 1 ms; two periods = 2 ms queue capacity.
# This leaves the remaining budget for raw MIDI, event dispatch, DSP and DAC.
b = b.replace('constexpr unsigned kRate = 48000U;', 'constexpr unsigned kRate = 96000U;')
b = b.replace('constexpr unsigned kPeriods = 3U;', 'constexpr unsigned kPeriods = 2U;')
b = b.replace('48 kHz / 96 x 3', '96 kHz / 96 x 2')
b = b.replace('48 kHz / 96x3', '96 kHz / 96x2')
b = b.replace('96 frames x 3 at 48 kHz', '96 frames x 2 at 96 kHz')

# One-period startup threshold: start after 96 frames (1 ms), not after a long
# prefill. Keep the proven XRUN recovery and physical SM-X130 route untouched.
if 'config.bufferFrames - config.periodFrames' not in b:
    raise SystemExit('low-latency start threshold contract missing')
for token in [
    'kPreferredMinimumPeriodFrames = 96U;',
    'SNDRV_PCM_IOCTL_PREPARE',
    'SNDRV_PCM_IOCTL_WRITEI_FRAMES',
    'xrunCount_.fetch_add',
    'sw.xfer_align = 1U;',
    'kPcmDevice = "/dev/snd/pcmC0D0p"',
    'Headset Volume',
    'configureRealtimeThread("SilvAudioRT", 7, 7, 80);',
]:
    if token not in b:
        raise SystemExit(f'ALSA safety/recovery contract lost: {token}')
if 'constexpr unsigned kRate = 96000U;' not in b or 'constexpr unsigned kPeriods = 2U;' not in b:
    raise SystemExit('96k/96x2 final live contract not installed')
if 'constexpr unsigned kRate = 48000U;' in b or 'constexpr unsigned kPeriods = 3U;' in b:
    raise SystemExit('stale high-latency output contract remains')
bridge.write_text(b, encoding='utf-8')

# Root scheduling contract: audio owns CPU7 at FIFO85, raw MIDI owns CPU6 at
# FIFO75, and all ordinary app/UI/file work is pushed to CPU0-5. The native
# thread setup remains a safe fallback; root promotion is the final live policy.
r = root_mode.read_text(encoding='utf-8')
for token in [
    'taskset -p 3f',
    'promote SilvAudioRT 80 85',
    'promote SilvMidiRT 40 75',
]:
    if token not in r:
        raise SystemExit(f'sound-priority scheduling contract missing: {token}')
for forbidden in ['setenforce 0', 'thermal disable', 'Speaker Protect 0', 'SmartPA 0', 'chmod 777']:
    if forbidden in r:
        raise SystemExit(f'unsafe sound-priority change detected: {forbidden}')

# 48 kHz SAMPLE -> 96 kHz engine is the main bank case. At root pitch the ratio
# is exactly 0.5. Preserve the exact LiveCubic4 result while reducing work:
# integer output phases read one source frame; half phases use the algebraically
# identical Catmull-Rom weights [-1, 9, 9, -1] / 16.
q = sinc.read_text(encoding='utf-8')
marker = '    if (quality_ == InterpolationQuality::LiveCubic4) {\n'
fast48 = '''        if (std::abs(playbackRatio - 0.5) < 1.0e-12) {
            const double base = std::floor(position);
            const double frac = position - base;
            const long long i1 = static_cast<long long>(base);
            if (std::abs(frac) < 1.0e-12) {
                return readFrame(sample, i1, channel, looping);
            }
            if (std::abs(frac - 0.5) < 1.0e-12) {
                const float y0 = readFrame(sample, i1 - 1LL, channel, looping);
                const float y1 = readFrame(sample, i1, channel, looping);
                const float y2 = readFrame(sample, i1 + 1LL, channel, looping);
                const float y3 = readFrame(sample, i1 + 2LL, channel, looping);
                return (-y0 + 9.0F*y1 + 9.0F*y2 - y3) * 0.0625F;
            }
        }
'''
if 'std::abs(playbackRatio - 0.5) < 1.0e-12' not in q:
    pos = q.find(marker)
    if pos < 0:
        raise SystemExit('LiveCubic4 insertion point missing')
    pos += len(marker)
    q = q[:pos] + fast48 + q[pos:]
if '(-y0 + 9.0F*y1 + 9.0F*y2 - y3) * 0.0625F' not in q:
    raise SystemExit('exact 48k->96k cubic fast path missing')
sinc.write_text(q, encoding='utf-8')

# Regression: the ordinary 48 kHz test SAMPLE must produce audio with a 96 kHz
# engine too. This proves that changing the hardware/engine rate does not make
# existing 48 kHz banks unplayable.
t = tests.read_text(encoding='utf-8')
f0 = t.find('void testEnginePlaybackAndControls()')
if f0 < 0:
    raise SystemExit('testEnginePlaybackAndControls missing')
f1 = t.find('\nvoid test', f0 + 1)
if f1 < 0:
    raise SystemExit('cannot bound testEnginePlaybackAndControls')
block = t[f0:f1]
regression = '''    SamplerEngine e96; e96.prepare(96000.0,96U); e96.setState(st); e96.setInterpolationQuality(InterpolationQuality::LiveCubic4);
    assert(e96.postNoteOn(0U,60U,120U));
    std::vector<float> out96(192U);
    e96.render(out96.data(),96U);
    assert(rms(out96)>0.001F); // 48 kHz SAMPLE must play through 96 kHz engine
'''
if '48 kHz SAMPLE must play through 96 kHz engine' not in block:
    anchor = '    const auto m=e.meters(); assert(m.peakLeft>0.0F && m.peakRight>0.0F);\n'
    if anchor not in block:
        raise SystemExit('48k->96k playback regression anchor changed')
    block = block.replace(anchor, anchor + regression, 1)
    t = t[:f0] + block + t[f1:]
tests.write_text(t, encoding='utf-8')

v = view.read_text(encoding='utf-8')
v = v.replace('a.bufferSizeFrames == 288', 'a.bufferSizeFrames == 192')
v = v.replace('"48k 96x3"', '"96k 96x2"')
v = v.replace('"48k 96x2"', '"96k 96x2"')
view.write_text(v, encoding='utf-8')

# Align the final CI audit with the actual final hardware contract. Earlier
# preparation patches may temporarily use 48k/96x3, but this patch runs last.
a = audit.read_text(encoding='utf-8')
a = a.replace('48000U', '96000U')
a = a.replace('48000', '96000')
a = a.replace('48 kHz', '96 kHz')
a = a.replace('96x3', '96x2')
a = a.replace('96 x 3', '96 x 2')
audit.write_text(a, encoding='utf-8')

final_v = view.read_text(encoding='utf-8')
for token in ['a.bufferSizeFrames == 192', '"96k 96x2"']:
    if token not in final_v:
        raise SystemExit(f'low-latency UI/status contract missing: {token}')

print('PASS: SOUND-FIRST final contract = 96k/96x2 + Audio CPU7/FIFO85 + MIDI CPU6/FIFO75 + exact 48k->96k cubic fast path; safety protections preserved')
