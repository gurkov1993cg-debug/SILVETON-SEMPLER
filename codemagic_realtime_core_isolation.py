from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'

for p in (cpp, root_mode):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# Dedicated SM-X130 realtime topology:
#   CPU7 = Direct ALSA audio render/write + audio IRQ only
#   CPU6 = Direct raw-MIDI only
#   CPU0-5 = UI, Android, disk/bank work and every other SILVETON thread
# Keep the two big cores out of ordinary app work so scheduling jitter cannot
# consume a 96-frame deadline.
s = cpp.read_text(encoding='utf-8')
s = s.replace(
    'configureRealtimeThread("SilvAudioRT", 7, 7, 80);',
    'configureRealtimeThread("SilvAudioRT", 7, 7, 85);'
)
s = s.replace(
    'configureRealtimeThread("SilvMidiRT", 0, 5, 70);',
    'configureRealtimeThread("SilvMidiRT", 6, 6, 75);'
)
if 'configureRealtimeThread("SilvAudioRT", 7, 7, 85);' not in s:
    raise SystemExit('audio realtime CPU7 anchor missing')
if 'configureRealtimeThread("SilvMidiRT", 6, 6, 75);' not in s:
    raise SystemExit('MIDI realtime CPU6 anchor missing')
cpp.write_text(s, encoding='utf-8')

k = root_mode.read_text(encoding='utf-8')

# Root-side promotion is authoritative if the unprivileged native process cannot
# set SCHED_FIFO itself. First move every non-RT SILVETON thread to CPUs 0-5,
# then pin/promote the two realtime workers to their dedicated big cores.
old = '''            promote SilvAudioRT 80 80
            promote SilvMidiRT 3f 70
            exit 0
'''
new = '''            for t in /proc/${pid}/task/[0-9]*; do
                [ -r "${'$'}t/comm" ] || continue
                NAME="${'$'}(cat "${'$'}t/comm" 2>/dev/null)"
                case "${'$'}NAME" in
                    SilvAudioRT|SilvMidiRT) continue ;;
                esac
                TID="${'$'}{t##*/}"
                command -v taskset >/dev/null 2>&1 && taskset -p 3f "${'$'}TID" >/dev/null 2>&1 || true
            done
            promote SilvAudioRT 80 85
            promote SilvMidiRT 40 75
            exit 0
'''
if old in k:
    k = k.replace(old, new, 1)
elif 'promote SilvAudioRT 80 85' in k and 'promote SilvMidiRT 40 75' in k:
    pass
else:
    raise SystemExit('root realtime promotion block changed')

# The existing live-mode gate already keeps background/system/restricted work on
# CPUs 0-5, locks CPU6/7 governors to performance, disables deep idle on 6/7 and
# keeps the MediaTek AFE IRQ on CPU7. Preserve those settings and make them hard
# contracts for this patch.
required_root = [
    'write_node /dev/cpuset/background/cpus 0-5',
    'write_node /dev/cpuset/system-background/cpus 0-5',
    'write_node /dev/cpuset/restricted/cpus 0-5',
    'write_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor performance',
    'write_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor performance',
    'echo 1 > "${\'$\'}st/disable"',
    'echo 80 > "/proc/irq/${\'$\'}AFE/smp_affinity"',
    'promote SilvAudioRT 80 85',
    'promote SilvMidiRT 40 75',
    'taskset -p 3f',
]
for token in required_root:
    if token not in k:
        raise SystemExit(f'realtime core-isolation contract missing: {token}')

root_mode.write_text(k, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
if 'configureRealtimeThread("SilvMidiRT", 0, 5, 70);' in final_cpp:
    raise SystemExit('stale MIDI housekeeping-core topology remains')
if 'configureRealtimeThread("SilvAudioRT", 7, 7, 80);' in final_cpp:
    raise SystemExit('stale audio RT priority remains')

print('PASS: dedicated realtime core isolation installed (CPU7 audio, CPU6 MIDI, CPU0-5 everything else)')
