from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
runtime = root / 'android/app/src/main/java/com/silveton/sampler/SamplerRuntime.kt'
for p in (cpp, view, runtime):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

s = cpp.read_text(encoding='utf-8')

# Diagnostic/live-test contract: keep the built-in TEST EP permanently enabled
# for this APK, so an empty/default PROGRAM object can never silence it.
old = '''        bool hasRealSound = false;\n        if (state_ != nullptr) {\n            for (const auto& upper : state_->combi.uppers) {\n                if (upper.enabled && upper.program != nullptr) { hasRealSound = true; break; }\n            }\n        }\n        testInstrument_.setEnabled(!hasRealSound);\n        engine_.setState(state_);\n'''
new = '''        // Keep TEST EP enabled for hardware validation. External MIDI note-on/off\n        // always reaches this native instrument, independently of bank content.\n        testInstrument_.setEnabled(true);\n        engine_.setState(state_);\n'''
if old in s:
    s = s.replace(old, new, 1)
elif 'testInstrument_.setEnabled(true);' not in s:
    raise SystemExit('TEST EP state anchor changed')

# Lower diagnostic synth gain enough to avoid clipping with large chords.
s = s.replace('mono += osc * v.env * v.velocityGain * 0.065F;',
              'mono += osc * v.env * v.velocityGain * 0.035F;')

# Contract: raw MIDI must drive the fallback instrument directly.
required_cpp = [
    'testInstrument_.setEnabled(true);',
    'testInstrument_.noteOn(data1, data2);',
    'testInstrument_.noteOff(data1);',
    'testInstrument_.render(renderBuffer_.data(), frames, sampleRate());',
    '0.035F',
]
for needle in required_cpp:
    if needle not in s:
        raise SystemExit(f'keyboard test contract missing in C++: {needle}')
cpp.write_text(s, encoding='utf-8')

# Do not let an AUDIO-node permission failure prevent the native open() call:
# native open() starts raw MIDI first, then attempts PCM. This is essential for
# diagnosis because MIDI RX must remain visible even when ALSA output is broken.
r = runtime.read_text(encoding='utf-8')
perm_block = '''        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()\n        if (!audioPermissions.ok) {\n            statusChanged(audioPermissions.message)\n            return false\n        }\n'''
perm_continue = '''        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()\n        if (!audioPermissions.ok) statusChanged(audioPermissions.message)\n        val midiPermissions = SilvetonRootRuntime.refreshMidiPermissions()\n        if (!midiPermissions.ok) statusChanged(midiPermissions.message)\n'''
count = r.count(perm_block)
if count > 0:
    # Expected in start() and resumeAudio(); recovery has a different block.
    r = r.replace(perm_block, perm_continue)
elif 'val midiPermissions = SilvetonRootRuntime.refreshMidiPermissions()' not in r:
    raise SystemExit('runtime permission/start anchor changed')

# We deliberately continue into nativeAudioOpen(): C++ open() launches Direct
# MIDI before attempting the PCM device, so a dead PCM no longer hides MIDI.
for needle in [
    'val midiPermissions = SilvetonRootRuntime.refreshMidiPermissions()',
    'if (!engine.nativeAudioOpen())',
]:
    if needle not in r:
        raise SystemExit(f'keyboard/MIDI runtime contract missing: {needle}')
runtime.write_text(r, encoding='utf-8')

# Make it explicit on-screen that the keyboard is playing the built-in patch.
v = view.read_text(encoding='utf-8')
# The diagnostics patch already has TEST EP wording on current builds; if the
# visual skin has hidden it, force a clear label near the current sound title.
if 'TEST EP' not in v:
    anchor = 'text(c, activeItem?.name ?: "Силветон", left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n'
    if anchor in v:
        v = v.replace(anchor, anchor + '        text(c, "TEST EP", left+95f*sx, nowBottom+8f*sy, 12f*ss, C_ACCENT, bold=true)\n', 1)
view.write_text(v, encoding='utf-8')

# Replace the old formatting-sensitive GUI audit after all GUI patches have run.
audit_fix = Path(__file__).with_name('codemagic_gui_contract_fix.py')
if not audit_fix.is_file():
    raise SystemExit(f'missing GUI audit fix: {audit_fix}')
exec(compile(audit_fix.read_text(encoding='utf-8'), str(audit_fix), 'exec'), {
    '__name__': '__main__',
    '__file__': str(audit_fix),
})

print('PASS: external keyboard TEST EP forced ON; MIDI remains testable even with broken PCM; GUI audit updated')
