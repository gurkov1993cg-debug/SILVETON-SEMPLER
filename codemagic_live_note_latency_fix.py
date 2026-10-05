#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
sv_runtime = root / 'core/src/sampler/SvBankRuntime.cpp'
for p in (bridge, root_mode, sv_runtime):
    if not p.is_file():
        raise SystemExit(f'missing live-latency source: {p}')

b = bridge.read_text(encoding='utf-8')

# Direct raw-MIDI must wake immediately enough for live playing. The previous
# 20 ms poll timeout alone could dominate total instrument latency.
b = b.replace('configureRealtimeThread("SilvMidiRT", 0, 5, 70);',
              'configureRealtimeThread("SilvMidiRT", 6, 6, 75);')
b = b.replace('const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(count), 20);',
              'const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(count), 1);')
b = b.replace('std::this_thread::sleep_for(std::chrono::milliseconds(20));',
              'std::this_thread::sleep_for(std::chrono::milliseconds(5));')

if 'configureRealtimeThread("SilvMidiRT", 6, 6, 75);' not in b:
    raise SystemExit('failed to dedicate CPU6/FIFO75 to Direct MIDI')
if '::poll(pollFds.data(), static_cast<nfds_t>(count), 1)' not in b:
    raise SystemExit('failed to reduce Direct MIDI poll timeout to 1 ms')
if '::poll(pollFds.data(), static_cast<nfds_t>(count), 20)' in b:
    raise SystemExit('stale 20 ms MIDI poll timeout remains')

# Do not weaken the proven Direct-ALSA output contract while fixing MIDI latency.
for token in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
    'kPreferredMinimumPeriodFrames = 96U;',
    'configureRealtimeThread("SilvAudioRT", 7, 7, 80);',
    'SNDRV_PCM_IOCTL_PREPARE',
    'SNDRV_PCM_IOCTL_WRITEI_FRAMES',
]:
    if token not in b:
        raise SystemExit(f'Direct ALSA protection/latency contract lost: {token}')
bridge.write_text(b, encoding='utf-8')

# Root promotion must match the native topology: audio CPU7, MIDI CPU6.
r = root_mode.read_text(encoding='utf-8')
r = r.replace('promote SilvMidiRT 3f 70', 'promote SilvMidiRT 40 75')
if 'promote SilvMidiRT 40 75' not in r:
    raise SystemExit('root MIDI CPU6/FIFO75 promotion missing')
if 'promote SilvAudioRT 80 80' not in r:
    raise SystemExit('audio CPU7/FIFO80 promotion was disturbed')
for forbidden in ['setenforce 0', 'thermal disable', 'Speaker Protect 0', 'SmartPA 0', 'chmod 777']:
    if forbidden in r:
        raise SystemExit(f'unsafe root change detected: {forbidden}')
root_mode.write_text(r, encoding='utf-8')

# Manager bank records do not carry an amp-envelope attack field. Leaving the
# ProgramLayer default adds an artificial 5 ms fade to every bank note. For a
# sampler, the recorded sample itself defines the attack, so bank-created layers
# must start immediately. Delay stays zero unless explicitly edited later.
s = sv_runtime.read_text(encoding='utf-8')
anchor = 'layer.tuneCents = r.tuneCents; layer.levelDb = r.levelDb; layer.pan = r.pan;'
replacement = anchor + '\n        layer.ampEnvelope.attackSec = 0.0F;\n        layer.delaySec = 0.0F;'
if replacement not in s:
    if anchor not in s:
        raise SystemExit('SV bank layer construction anchor changed')
    s = s.replace(anchor, replacement, 1)
if 'layer.ampEnvelope.attackSec = 0.0F;' not in s:
    raise SystemExit('bank-note zero artificial attack missing')
sv_runtime.write_text(s, encoding='utf-8')

print('PASS: live note path tightened (1 ms raw-MIDI poll + CPU6/FIFO75 + zero artificial bank attack); ALSA 48k/96x3 and safety protections preserved')
