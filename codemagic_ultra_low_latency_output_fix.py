#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
audit = root / 'ci/audit_direct_alsa.py'
for p in (bridge, view, audit):
    if not p.is_file():
        raise SystemExit(f'missing ultra-low-latency source: {p}')

b = bridge.read_text(encoding='utf-8')

# Live target: 96 kHz / 96 frames / 2 periods.
# 96 frames at 96 kHz = 1 ms; two periods = 2 ms queue capacity.
# This is the only output configuration compatible with the product requirement
# of roughly 3-4 ms key-to-sound latency once MIDI/event/DAC time is included.
b = b.replace('constexpr unsigned kRate = 48000U;', 'constexpr unsigned kRate = 96000U;')
b = b.replace('constexpr unsigned kPeriods = 3U;', 'constexpr unsigned kPeriods = 2U;')
b = b.replace('48 kHz / 96 x 3', '96 kHz / 96 x 2')
b = b.replace('48 kHz / 96x3', '96 kHz / 96x2')
b = b.replace('96 frames x 3 at 48 kHz', '96 frames x 2 at 96 kHz')

# Keep one-period startup threshold. With a 192-frame buffer this starts after
# 96 frames (1 ms), instead of pre-filling a long queue.
if 'config.bufferFrames - config.periodFrames' not in b:
    raise SystemExit('low-latency start threshold contract missing')

# Never weaken recovery or the physically proven device route.
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

for forbidden in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
]:
    if forbidden in b:
        raise SystemExit(f'stale high-latency final contract remains: {forbidden}')

if 'constexpr unsigned kRate = 96000U;' not in b or 'constexpr unsigned kPeriods = 2U;' not in b:
    raise SystemExit('96k/96x2 final live contract not installed')
bridge.write_text(b, encoding='utf-8')

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

print('PASS: final LIVE output = 96 kHz / 96x2 (2 ms ALSA queue capacity); XRUN recovery, route and safety protections preserved')
