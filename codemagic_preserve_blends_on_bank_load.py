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
new = '''        const auto loaded = SvBankRuntime::load(path, 4096U, false);
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
    pass
else:
    raise SystemExit('loadSvBank replacement anchor changed')

for token in [
    'const auto preserved = library_.snapshot();',
    'merged.combis = preserved.combis;',
    'merged.lists = preserved.lists;',
    'upper.soundSlotId',
    'library_.program(SlotAddress{0U, 0U, upper.soundSlotId})',
    'library_.writeCombi(saved.address, rebound)',
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

# Also persist BLENDS/LIST across complete app restarts. This patch stores only
# COMBI/LIST data and fixed SOUND-slot references; it does not duplicate bank audio.
persist = Path(__file__).with_name('codemagic_blends_persistence_fix.py')
if not persist.is_file():
    raise SystemExit(f'missing BLENDS persistence patch: {persist}')
exec(compile(persist.read_text(encoding='utf-8'), str(persist), 'exec'), {
    '__name__': '__main__',
    '__file__': str(persist),
})

# A bank is not accepted merely because names/catalog entries are visible.
# Prove the complete mmap-backed SOUND -> SAMPLE -> MIDI -> DSP path before the
# final audio/root guard runs. This patch never modifies audio/root protections.
playback = Path(__file__).with_name('codemagic_bank_playback_fix.py')
if not playback.is_file():
    raise SystemExit(f'missing bank playback patch: {playback}')
exec(compile(playback.read_text(encoding='utf-8'), str(playback), 'exec'), {
    '__name__': '__main__',
    '__file__': str(playback),
})

print('PASS: bank load preserves BLENDS/COMBI + LIST, persists user state, and proves real bank playback')
