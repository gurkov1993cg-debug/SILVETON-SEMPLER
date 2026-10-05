from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
for p in (cpp, view):
    if not p.is_file():
        raise SystemExit(f'missing required source: {p}')

# The real SILVETON bank/runtime is now the only playable sound source.
# Keep the old diagnostic class compiled for ABI/source compatibility with the
# earlier patch chain, but disable it permanently so it can never layer over,
# replace, or sound instead of a real bank SOUND.
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

# Earlier hardware-diagnostic patch may have inserted a small TEST EP label.
v = re.sub(r'(?m)^\s*text\(c,\s*"TEST EP"[^\n]*\)\s*\n?', '', v)

# No synthetic fallback title when the real list has not populated yet.
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

final_cpp = cpp.read_text(encoding='utf-8')
if 'testInstrument_.setEnabled(false);' not in final_cpp:
    raise SystemExit('TEST EP is not permanently disabled')
if 'testInstrument_.setEnabled(!hasPlayableProgram);' in final_cpp or 'testInstrument_.setEnabled(true);' in final_cpp:
    raise SystemExit('obsolete TEST EP enable path remains')

print('PASS: obsolete TEST EP / Stage Piano retired; real SILVETON bank SOUND is the only playable source')
