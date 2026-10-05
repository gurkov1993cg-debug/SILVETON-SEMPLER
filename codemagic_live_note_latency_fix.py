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

# Direct raw-MIDI must wake quickly enough for live playing. The previous 20 ms
# poll timeout alone could dominate the total key-to-sound latency. Keep the
# already-proven CPU/priority topology unchanged in this targeted fix.
b = b.replace('const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(count), 20);',
              'const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(count), 1);')
b = b.replace('std::this_thread::sleep_for(std::chrono::milliseconds(20));',
              'std::this_thread::sleep_for(std::chrono::milliseconds(5));')

if '::poll(pollFds.data(), static_cast<nfds_t>(count), 1)' not in b:
    raise SystemExit('failed to reduce Direct MIDI poll timeout to 1 ms')
if '::poll(pollFds.data(), static_cast<nfds_t>(count), 20)' in b:
    raise SystemExit('stale 20 ms MIDI poll timeout remains')

# Preserve the proven Direct-ALSA/RT contract while fixing note latency.
for token in [
    'constexpr unsigned kRate = 48000U;',
    'constexpr unsigned kPeriods = 3U;',
    'kPreferredMinimumPeriodFrames = 96U;',
    'configureRealtimeThread("SilvAudioRT", 7, 7, 80);',
    'configureRealtimeThread("SilvMidiRT", 0, 5, 70);',
    'SNDRV_PCM_IOCTL_PREPARE',
    'SNDRV_PCM_IOCTL_WRITEI_FRAMES',
]:
    if token not in b:
        raise SystemExit(f'Direct ALSA/MIDI protection contract lost: {token}')
bridge.write_text(b, encoding='utf-8')

# Root/safety policy must remain untouched.
r = root_mode.read_text(encoding='utf-8')
if 'promote SilvAudioRT 80 80' not in r or 'promote SilvMidiRT 3f 70' not in r:
    raise SystemExit('realtime root promotion topology changed unexpectedly')
for forbidden in ['setenforce 0', 'thermal disable', 'Speaker Protect 0', 'SmartPA 0', 'chmod 777']:
    if forbidden in r:
        raise SystemExit(f'unsafe root change detected: {forbidden}')

# Manager bank records do not carry an amp-envelope attack field. Leaving the
# ProgramLayer default adds an artificial 5 ms fade to every bank note. The
# recorded sample itself defines the attack, so bank-created layers start at once.
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

print('PASS: live note latency tightened (1 ms raw-MIDI poll + zero artificial bank attack); ALSA 48k/96x3, RT topology and safety protections preserved')
