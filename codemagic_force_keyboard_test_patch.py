from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
for p in (cpp, view):
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

# Make it explicit on-screen that the keyboard is playing the built-in patch.
v = view.read_text(encoding='utf-8')
# The diagnostics patch already has TEST EP wording on current builds; if the
# visual skin has hidden it, force a clear label near the current sound title.
if 'TEST EP' not in v:
    anchor = 'text(c, activeItem?.name ?: "Силветон", left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)\n'
    if anchor in v:
        v = v.replace(anchor, anchor + '        text(c, "TEST EP", left+95f*sx, nowBottom+8f*sy, 12f*ss, C_ACCENT, bold=true)\n', 1)
view.write_text(v, encoding='utf-8')

print('PASS: external keyboard TEST EP forced ON (raw MIDI -> native synth -> Direct ALSA)')
