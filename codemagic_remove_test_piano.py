from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
audit = root / 'ci/audit_gui_contract.py'
tests = root / 'core/tests/SamplerTests.cpp'
for p in (cpp, view, audit, tests):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# The real SILVETON bank/runtime is now the only playable sound source.
# Keep the old diagnostic class compiled for compatibility with the earlier patch
# chain, but disable it permanently so it can never sound or layer over bank audio.
s = cpp.read_text(encoding='utf-8')
old = '''        const bool hasPlayableProgram = std::any_of(
            state_->combi.uppers.begin(), state_->combi.uppers.end(),
            [](const UpperPart& u) { return u.enabled && u.program != nullptr; });
        // Fallback is only for an empty fresh install. It must never layer over a real bank SOUND.
        testInstrument_.setEnabled(!hasPlayableProgram);
'''
new = '''        // Real bank SOUND is authoritative. The old diagnostic TEST EP is retired.
        testInstrument_.setEnabled(false);
'''
if old in s:
    s = s.replace(old, new, 1)
elif 'testInstrument_.setEnabled(false);' not in s:
    raise SystemExit('final TEST EP disable anchor not found')
cpp.write_text(s, encoding='utf-8')

# Remove all user-visible TEST EP / PIANO fallback controls and the synthetic
# Stage Piano set-list entry. Real bank names/slots remain untouched.
v = view.read_text(encoding='utf-8')

piano_header = '''        val test = RectF(254f*sx,12f*sy,374f*sx,64f*sy)
        val testReady = sampler.testInstrumentEnabled()
        silvKey(c,test,testReady,false,enabled=testReady)
        silvLed(c,test.left+18f*sx,test.centerY(),testReady,false,4.5f*ss)
        text(c,"PIANO",test.centerX()+7f*sx,test.centerY()+6f*sy,14f*ss,if(testReady)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=testReady)
        if(testReady) hit(test,{_,_-> if(sampler.testEpNoteOn(60,110)) postDelayed({sampler.testEpNoteOff(60)},650L)})

'''
v = v.replace(piano_header, '', 1)
v = re.sub(r'(?m)^\s*text\(c,\s*"TEST EP"[^\n]*\)\s*\n?', '', v)
v = v.replace(
    '        val currentName = activeItem?.name ?: if (sampler.testInstrumentEnabled()) "STAGE PIANO" else "Силветон"\n',
    '        val currentName = activeItem?.name ?: "Силветон"\n',
    1,
)
v = v.replace(
    '        val currentName = activeItem?.name ?: if (sampler.testInstrumentEnabled()) "TEST EP" else "Силветон"\n',
    '        val currentName = activeItem?.name ?: "Силветон"\n',
    1,
)
v = v.replace('        val builtInPiano = entries.isEmpty() && sampler.testInstrumentEnabled()\n\n', '', 1)

built_in_slot = '''            if (builtInPiano && idx == 0) {
                silvKey(c, rr, true, false, enabled=true)
                silvLed(c, rr.left+18f*sx, rr.centerY(), true, false, 5.5f*ss)
                text(c, "001", rr.left+42f*sx, rr.centerY()-8f*sy, 17f*ss, C_ACCENT, bold=true)
                text(c, "STAGE PIANO", rr.left+98f*sx, rr.centerY()-5f*sy, 22f*ss, C_TEXT, bold=true)
                text(c, "BUILT-IN  •  RAW MIDI", rr.left+98f*sx, rr.centerY()+19f*sy, 12f*ss, C_TEXT_MUTED)
                continue
            }
'''
v = v.replace(built_in_slot, '', 1)

for forbidden in ['"STAGE PIANO"', '"BUILT-IN  •  RAW MIDI"', 'text(c,"PIANO"', 'text(c, "TEST EP"']:
    if forbidden in v:
        raise SystemExit(f'obsolete test-piano UI remains: {forbidden}')
view.write_text(v, encoding='utf-8')

# The GUI audit must validate the production sampler UI, not the retired hardware
# validation piano. Keep all real navigation/status/controller checks.
a = audit.read_text(encoding='utf-8')
a = a.replace('    "PIANO control": \'"PIANO"\',\n', '')
a = a.replace('    "STAGE PIANO set-list slot": \'"STAGE PIANO"\',\n', '')

piano_audit_block = '''if "sampler.testEpNoteOn" not in view or "sampler.testEpNoteOff" not in view:
    raise SystemExit("MISSING GUI contract: PIANO control is not wired to native note on/off")
print("PASS - GUI PIANO control is wired to native note on/off")

'''
a = a.replace(piano_audit_block, '', 1)
a = a.replace('    "PIANO note on": "fun testEpNoteOn(",\n', '')
a = a.replace('    "PIANO note off": "fun testEpNoteOff(",\n', '')

if '"PIANO control"' in a or '"STAGE PIANO set-list slot"' in a or 'PIANO control is not wired' in a:
    raise SystemExit('obsolete PIANO requirement remains in GUI audit')
audit.write_text(a, encoding='utf-8')

# Final regression normalization for the complete seamless state-replacement
# contract. Existing voices continue from their immutable owner state.
t = tests.read_text(encoding='utf-8')
start = t.find('void testSafeStateReplacement()')
if start < 0:
    raise SystemExit('testSafeStateReplacement not found')
end = t.find('\nvoid test', start + 1)
if end < 0:
    end = t.find('\nint main(', start + 1)
if end < 0:
    raise SystemExit('cannot bound testSafeStateReplacement')
block = t[start:end]

voice_old = [
    'assert(engine.activeVoiceCount() == 0U);',
    'assert(engine.activeVoiceCount()==0U);',
]
voice_new = 'assert(engine.activeVoiceCount() > 0U); // seamless replacement preserves sounding voice ownership'
if voice_new not in block:
    hits = [x for x in voice_old if x in block]
    if len(hits) == 1 and block.count(hits[0]) == 1:
        block = block.replace(hits[0], voice_new, 1)
    elif not any('activeVoiceCount() > 0U' in line for line in block.splitlines()):
        raise SystemExit('cannot normalize active voice assertion in testSafeStateReplacement')

rms_old = [
    'assert(rms(out) == 0.0F);',
    'assert(rms(out)==0.0F);',
]
rms_new = 'assert(rms(out) > 0.0F); // preserved old voice must continue rendering after state replacement'
if rms_new not in block:
    hits = [x for x in rms_old if x in block]
    if len(hits) != 1 or block.count(hits[0]) != 1:
        raise SystemExit('cannot uniquely locate stale silence assertion in testSafeStateReplacement')
    block = block.replace(hits[0], rms_new, 1)

for stale in voice_old + rms_old:
    if stale in block:
        raise SystemExit(f'stale destructive seamless-state expectation remains: {stale}')
if 'activeVoiceCount() > 0U' not in block or 'rms(out) > 0.0F' not in block:
    raise SystemExit('incomplete seamless state-replacement regression contract')

t = t[:start] + block + t[end:]
tests.write_text(t, encoding='utf-8')

final_cpp = cpp.read_text(encoding='utf-8')
if 'testInstrument_.setEnabled(false);' not in final_cpp:
    raise SystemExit('TEST EP is not permanently disabled')
if 'testInstrument_.setEnabled(!hasPlayableProgram);' in final_cpp or 'testInstrument_.setEnabled(true);' in final_cpp:
    raise SystemExit('obsolete TEST EP enable path remains')

# Final UI workflow is deliberately applied last so no earlier visual-skin patch
# can hide these user-facing controls again.
workflow = Path(__file__).with_name('codemagic_visible_combi_list_workflow.py')
if not workflow.is_file():
    raise SystemExit(f'missing visible COMBI/LIST workflow patch: {workflow}')
exec(compile(workflow.read_text(encoding='utf-8'), str(workflow), 'exec'), {
    '__name__': '__main__',
    '__file__': str(workflow),
})

# Android 15 scoped-storage fix is applied after all runtime/UI rewrites so no
# earlier patch can remove the bank permission, root app-op, or SOUND diagnostics.
bank_access = Path(__file__).with_name('codemagic_bank_storage_access_fix.py')
if not bank_access.is_file():
    raise SystemExit(f'missing bank storage access patch: {bank_access}')
exec(compile(bank_access.read_text(encoding='utf-8'), str(bank_access), 'exec'), {
    '__name__': '__main__',
    '__file__': str(bank_access),
})

final_view = view.read_text(encoding='utf-8')
for token in ['"WRITE COMBI"', '"ADD TO LIST"', '"EDIT COMBI"', '"BANK HAS NO SOUNDS"']:
    if token not in final_view:
        raise SystemExit(f'visible COMBI/LIST/bank UI token missing after final patch: {token}')

root_mode = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt'
manifest = root / 'android/app/src/main/AndroidManifest.xml'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
for p in (root_mode, manifest, controller):
    if not p.is_file():
        raise SystemExit(f'missing final bank access source: {p}')
if 'MANAGE_EXTERNAL_STORAGE allow' not in root_mode.read_text(encoding='utf-8'):
    raise SystemExit('root all-files app-op missing after final patch')
if 'android.permission.MANAGE_EXTERNAL_STORAGE' not in manifest.read_text(encoding='utf-8'):
    raise SystemExit('MANAGE_EXTERNAL_STORAGE manifest permission missing after final patch')
if 'SilvetonRootRuntime.refreshBankStorageAccess()' not in controller.read_text(encoding='utf-8'):
    raise SystemExit('bank storage refresh is not wired into startup')

print('PASS: production bank-only sound + seamless regression + visible COMBI/LIST + Android 15 bank access applied')
