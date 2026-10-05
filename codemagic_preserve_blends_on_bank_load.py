from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
if not bridge.is_file():
    raise SystemExit(f'missing sampler bridge: {bridge}')

s = bridge.read_text(encoding='utf-8')

# SILVETON_BANK.sv owns SOUND/SAMPLE/MULTISAMPLE resources. User COMBI (BLENDS)
# and LIST objects are tablet performance objects and must NOT be deleted when the
# sound bank is reloaded. The old load path called library_.replace(loaded.data),
# which cleared combis_ and lists_ every time.
old = '''        const auto loaded = SvBankRuntime::load(path, 4096U, false);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        library_.replace(loaded.data);
        if (loaded.data.programs.empty() || loaded.data.programs.front().program == nullptr) {
            setTextError("SILVETON bank contains no playable SOUND");
            return false;
        }
'''
new = '''        const auto loaded = SvBankRuntime::load(path, 65536U, false);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        if (loaded.data.programs.empty() || loaded.data.programs.front().program == nullptr) {
            setTextError("SILVETON bank contains no playable SOUND");
            return false;
        }

        // Preserve user performance objects across a SOUND-bank replacement.
        // COMBI Upper parts persist fixed soundSlotId references, so after the
        // new SOUND resources are installed we rebind every Upper to the new
        // ProgramPtr from the same 1..500 SOUND position.
        const auto preserved = library_.snapshot();
        auto merged = loaded.data;
        merged.global = preserved.global;
        merged.combis = preserved.combis;
        merged.lists = preserved.lists;
        library_.replace(merged);

        for (const auto& saved : preserved.combis) {
            Combi rebound = saved.combi;
            for (auto& upper : rebound.uppers) {
                if (upper.soundSlotId == kNoSoundSlot) continue;
                if (upper.soundSlotId > kFixedSoundSlotCount) {
                    upper.program.reset();
                    upper.enabled = false;
                    continue;
                }
                upper.program = library_.program(SlotAddress{0U, 0U, upper.soundSlotId});
                if (upper.program != nullptr) {
                    upper.name = upper.program->name;
                } else {
                    upper.enabled = false;
                }
            }
            if (!library_.writeCombi(saved.address, rebound)) {
                setTextError("Failed to restore COMBI after SILVETON bank load");
                return false;
            }
        }
'''

if old in s:
    s = s.replace(old, new, 1)
elif 'const auto preserved = library_.snapshot();' in s and 'merged.combis = preserved.combis;' in s:
    # Keep older generated trees idempotent, then normalize their preload below.
    s = s.replace('SvBankRuntime::load(path, 4096U, false)', 'SvBankRuntime::load(path, 65536U, false)')
else:
    raise SystemExit('loadSvBank replacement anchor changed')

for token in [
    'const auto preserved = library_.snapshot();',
    'merged.combis = preserved.combis;',
    'merged.lists = preserved.lists;',
    'upper.soundSlotId',
    'library_.program(SlotAddress{0U, 0U, upper.soundSlotId})',
    'library_.writeCombi(saved.address, rebound)',
    'SvBankRuntime::load(path, 65536U, false)',
]:
    if token not in s:
        raise SystemExit(f'BLENDS preservation contract missing: {token}')

start = s.find('    bool loadSvBank(const std::string& path) {')
end = s.find('\n    bool loadSet(', start)
if start < 0 or end < 0:
    raise SystemExit('loadSvBank boundaries missing')
block = s[start:end]
if 'library_.replace(loaded.data);' in block:
    raise SystemExit('destructive bank replacement still erases BLENDS')

bridge.write_text(s, encoding='utf-8')

persist = Path(__file__).with_name('codemagic_blends_persistence_fix.py')
if not persist.is_file():
    raise SystemExit(f'missing BLENDS persistence patch: {persist}')
exec(compile(persist.read_text(encoding='utf-8'), str(persist), 'exec'), {
    '__name__': '__main__', '__file__': str(persist),
})

playback = Path(__file__).with_name('codemagic_bank_playback_fix.py')
if not playback.is_file():
    raise SystemExit(f'missing bank playback patch: {playback}')
exec(compile(playback.read_text(encoding='utf-8'), str(playback), 'exec'), {
    '__name__': '__main__', '__file__': str(playback),
})

streaming = Path(__file__).with_name('codemagic_bank_realtime_streaming_fix.py')
if not streaming.is_file():
    raise SystemExit(f'missing realtime bank streaming fix: {streaming}')
exec(compile(streaming.read_text(encoding='utf-8'), str(streaming), 'exec'), {
    '__name__': '__main__', '__file__': str(streaming),
})

# Final tablet interaction layer. It wraps the CURRENT touch engine instead of
# assuming GestureDetector exists, so the practical drag/immediate-touch layer
# remains the one that owns normal touch handling.
pro_ui = Path(__file__).with_name('codemagic_list_context_professional_params_v2.py')
if not pro_ui.is_file():
    raise SystemExit(f'missing current-touch professional LIST/UI patch: {pro_ui}')
exec(compile(pro_ui.read_text(encoding='utf-8'), str(pro_ui), 'exec'), {
    '__name__': '__main__', '__file__': str(pro_ui),
})

compile_fix = Path(__file__).with_name('codemagic_list_ui_compile_fix.py')
if not compile_fix.is_file():
    raise SystemExit(f'missing professional LIST/UI compile guard: {compile_fix}')
exec(compile(compile_fix.read_text(encoding='utf-8'), str(compile_fix), 'exec'), {
    '__name__': '__main__', '__file__': str(compile_fix),
})

print('PASS: bank playback + realtime streaming + persistent BLENDS + current-touch long-press LIST management + professional parameter pages')
