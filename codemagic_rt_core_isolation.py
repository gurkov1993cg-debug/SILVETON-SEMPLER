from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
rt = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
for p in (cpp, rt):
    if not p.is_file():
        raise SystemExit(f'missing realtime source: {p}')

s = cpp.read_text(encoding='utf-8')
s = s.replace('configureRealtimeThread("SilvAudioRT", 7, 7, 80);',
              'configureRealtimeThread("SilvAudioRT", 7, 7, 85);')
s = s.replace('configureRealtimeThread("SilvMidiRT", 0, 5, 70);',
              'configureRealtimeThread("SilvMidiRT", 6, 6, 75);')
cpp.write_text(s, encoding='utf-8')

k = rt.read_text(encoding='utf-8')
k = k.replace('promote SilvAudioRT 80 80', 'promote SilvAudioRT 80 85')
k = k.replace('promote SilvMidiRT 3f 70', 'promote SilvMidiRT 40 75')
rt.write_text(k, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
final_rt = rt.read_text(encoding='utf-8')
for token in [
    'configureRealtimeThread("SilvAudioRT", 7, 7, 85);',
    'configureRealtimeThread("SilvMidiRT", 6, 6, 75);',
]:
    if token not in final_cpp:
        raise SystemExit(f'RT core isolation C++ contract missing: {token}')
for token in ['promote SilvAudioRT 80 85', 'promote SilvMidiRT 40 75']:
    if token not in final_rt:
        raise SystemExit(f'RT core isolation root contract missing: {token}')
if 'configureRealtimeThread("SilvMidiRT", 0, 5, 70);' in final_cpp:
    raise SystemExit('stale shared MIDI CPU topology remains')

print('PASS: CPU7 dedicated audio RT + CPU6 dedicated raw MIDI RT topology installed')
