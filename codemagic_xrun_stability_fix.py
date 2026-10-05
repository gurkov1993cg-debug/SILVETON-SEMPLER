from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'

for p in (cpp, view, root_mode):
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

v = view.read_text(encoding='utf-8')
v = v.replace('a.bufferSizeFrames == 192', 'a.bufferSizeFrames == 288')
v = v.replace('"48k 96x2"', '"48k 96x3"')
view.write_text(v, encoding='utf-8')

# Dedicated realtime topology on the rooted SM-X130:
# CPU7 = audio RT thread + MediaTek AFE audio IRQ only.
# CPU6 = raw MIDI RT thread only.
# CPU0-5 = UI, disk/bank work and all other SILVETON/Android work.
k = root_mode.read_text(encoding='utf-8')
old_promote = '''            promote SilvAudioRT 80 80
            promote SilvMidiRT 3f 70
            exit 0
'''
new_promote = '''            for t in /proc/${pid}/task/[0-9]*; do
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
if old_promote in k:
    k = k.replace(old_promote, new_promote, 1)
elif 'promote SilvAudioRT 80 85' in k and 'promote SilvMidiRT 40 75' in k:
    pass
else:
    raise SystemExit('root realtime promotion block changed')

root_required = [
    'write_node /dev/cpuset/background/cpus 0-5',
    'write_node /dev/cpuset/system-background/cpus 0-5',
    'write_node /dev/cpuset/restricted/cpus 0-5',
    'write_node /sys/devices/system/cpu/cpufreq/policy6/scaling_governor performance',
    'write_node /sys/devices/system/cpu/cpufreq/policy7/scaling_governor performance',
    'echo 80 > "/proc/irq/${\'$\'}AFE/smp_affinity"',
    'taskset -p 3f',
    'promote SilvAudioRT 80 85',
    'promote SilvMidiRT 40 75',
]
for token in root_required:
    if token not in k:
        raise SystemExit(f'realtime core isolation contract missing: {token}')
root_mode.write_text(k, encoding='utf-8')

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

ui_perf = Path(__file__).with_name('codemagic_ui_interaction_performance_fix.py')
if not ui_perf.is_file():
    raise SystemExit(f'missing UI interaction/performance patch: {ui_perf}')
exec(compile(ui_perf.read_text(encoding='utf-8'), str(ui_perf), 'exec'), {
    '__name__': '__main__',
    '__file__': str(ui_perf),
})

# Keep the compatibility namespace so its temporary SamplerTests alias can be
# restored immediately after bank-v2, before later checks access SamplerTests.cpp.
compat = Path(__file__).with_name('codemagic_bank_runtime_compat.py')
if not compat.is_file():
    raise SystemExit(f'missing bank runtime compatibility normalizer: {compat}')
compat_ns = {
    '__name__': '__main__',
    '__file__': str(compat),
}
exec(compile(compat.read_text(encoding='utf-8'), str(compat), 'exec'), compat_ns)

bank_v2 = Path(__file__).with_name('codemagic_bank_runtime_v2.py')
if not bank_v2.is_file():
    raise SystemExit(f'missing final bank runtime v2 patch: {bank_v2}')
exec(compile(bank_v2.read_text(encoding='utf-8'), str(bank_v2), 'exec'), {
    '__name__': '__main__',
    '__file__': str(bank_v2),
})

restore_tests = compat_ns.get('restore_test_files')
if not callable(restore_tests):
    raise SystemExit('bank compatibility restore hook missing')
restore_tests()

# Align legacy state-replacement regressions with the completed seamless runtime.
# setState() adopts the new graph at the block boundary while already sounding
# voices keep ownership of their immutable old graph until normal note-off/end.
sampler_tests = root / 'core/tests/SamplerTests.cpp'
if not sampler_tests.is_file():
    raise SystemExit(f'missing sampler regression tests after bank-v2 restore: {sampler_tests}')
tests_text = sampler_tests.read_text(encoding='utf-8')

def function_block(text: str, function_name: str):
    start = text.find(f'void {function_name}()')
    if start < 0:
        raise SystemExit(f'{function_name} not found')
    end = text.find('\nvoid test', start + 1)
    if end < 0:
        end = text.find('\nint main(', start + 1)
    if end < 0:
        raise SystemExit(f'cannot bound {function_name}')
    return start, end, text[start:end]

# Older global-controls test used the pre-seamless destructive setState contract.
start, end, block = function_block(tests_text, 'testGlobalAdvancedControls')
legacy_global = 'assert(e.activeVoiceCount()==0U);'
seamless_global_a = 'assert(e.activeVoiceCount()>0U); // seamless state swap must not kill an already sounding voice'
seamless_global_b = 'assert(e.activeVoiceCount()>0U); // seamless state swap must preserve the held voice'
if legacy_global in block:
    if block.count(legacy_global) != 1:
        raise SystemExit('ambiguous legacy activeVoiceCount assertion in testGlobalAdvancedControls')
    block = block.replace(legacy_global, seamless_global_a, 1)
    tests_text = tests_text[:start] + block + tests_text[end:]
elif seamless_global_a not in block and seamless_global_b not in block:
    raise SystemExit('expected seamless-state regression assertion not found in testGlobalAdvancedControls')

# Safe replacement must prove the opposite of the old behavior: replacing the
# state cannot invalidate or abruptly kill a voice that was already sounding.
start, end, block = function_block(tests_text, 'testSafeStateReplacement')
legacy_safe_variants = [
    'assert(engine.activeVoiceCount() == 0U);',
    'assert(engine.activeVoiceCount()==0U);',
]
seamless_safe = 'assert(engine.activeVoiceCount() > 0U); // replaced state must preserve the already sounding voice safely'
if seamless_safe not in block:
    matches = [token for token in legacy_safe_variants if token in block]
    if len(matches) != 1 or block.count(matches[0]) != 1:
        raise SystemExit('cannot uniquely locate legacy voice-reset assertion in testSafeStateReplacement')
    block = block.replace(matches[0], seamless_safe, 1)
    tests_text = tests_text[:start] + block + tests_text[end:]

# Refuse to ship if either state-replacement test still encodes the destructive
# zero-voice behavior. Panic/note-off tests elsewhere are intentionally untouched.
for function_name, forbidden in [
    ('testGlobalAdvancedControls', ['assert(e.activeVoiceCount()==0U);']),
    ('testSafeStateReplacement', legacy_safe_variants),
]:
    _, _, block = function_block(tests_text, function_name)
    for token in forbidden:
        if token in block:
            raise SystemExit(f'stale destructive state-replacement assertion remains in {function_name}')

sampler_tests.write_text(tests_text, encoding='utf-8')

# Final product cleanup: no synthetic TEST EP / Stage Piano is allowed now that
# the real SILVETON bank SOUND path is complete.
remove_test_piano = Path(__file__).with_name('codemagic_remove_test_piano.py')
if not remove_test_piano.is_file():
    raise SystemExit(f'missing final test-piano removal patch: {remove_test_piano}')
exec(compile(remove_test_piano.read_text(encoding='utf-8'), str(remove_test_piano), 'exec'), {
    '__name__': '__main__',
    '__file__': str(remove_test_piano),
})

print('PASS: SM-X130 XRUN stability + CPU isolation + slider UI + bank runtime v2 + seamless state regressions + real-bank-only sound applied')
