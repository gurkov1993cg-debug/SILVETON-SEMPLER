#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
test_candidates = [
    root / 'core/tests/CoreTests.cpp',
    root / 'core/tests/SamplerCoreTests.cpp',
]
tests = next((p for p in test_candidates if p.is_file()), None)

if not bridge.is_file():
    raise SystemExit(f'missing sampler bridge: {bridge}')
if tests is None:
    raise SystemExit('missing sampler core test source')

# This patch validates the real bank playback graph only. It deliberately does
# not touch Direct ALSA, root, SELinux, mixer, thermal or speaker protection.
s = bridge.read_text(encoding='utf-8')

old = '''        auto state = std::make_shared<EngineState>();
        if (state_ != nullptr) state->global = state_->global;
        state->combi.name = loaded.data.programs.front().program->name;
        state->combi.uppers[0].name = loaded.data.programs.front().program->name;
        state->combi.uppers[0].enabled = true;
        state->combi.uppers[0].program = loaded.data.programs.front().program;
        state->combi.uppers[0].soundSlotId = loaded.data.programs.front().address.slot;
        state->combi.uppers[0].midiChannel = 0U;
        if (!installState(std::move(state))) return false;
        setTextError({});
        return true;
'''

new = '''        // Do not accept a bank merely because SOUND names are visible. Select
        // a SOUND that resolves all the way to a valid SAMPLE.
        const ProgramSlot* selectedSound = nullptr;
        std::uint8_t selectedKey = 60U;
        std::uint8_t selectedVelocity = 100U;
        for (const auto& slot : loaded.data.programs) {
            if (slot.program == nullptr) continue;
            for (const auto& layer : slot.program->layers) {
                if (!layer.enabled || layer.multisample == nullptr) continue;
                for (const auto& zone : layer.multisample->zones) {
                    if (zone.sample == nullptr || !zone.sample->valid()) continue;
                    const int keyLow = std::max<int>(layer.keyLow, zone.keyLow);
                    const int keyHigh = std::min<int>(layer.keyHigh, zone.keyHigh);
                    const int velLow = std::max<int>(layer.velocityLow, zone.velocityLow);
                    const int velHigh = std::min<int>(layer.velocityHigh, zone.velocityHigh);
                    if (keyLow > keyHigh || velLow > velHigh || velHigh <= 0) continue;
                    selectedSound = &slot;
                    selectedKey = static_cast<std::uint8_t>(std::clamp<int>(zone.rootKey, keyLow, keyHigh));
                    selectedVelocity = static_cast<std::uint8_t>(std::clamp<int>(100, std::max(1, velLow), velHigh));
                    break;
                }
                if (selectedSound != nullptr) break;
            }
            if (selectedSound != nullptr) break;
        }
        if (selectedSound == nullptr || selectedSound->program == nullptr) {
            setTextError("SILVETON bank has SOUND names but no playable SAMPLE path");
            return false;
        }

        // Offline proof with the exact mmap-backed objects. No hardware audio
        // route is opened, stopped or changed by this probe.
        auto probeState = std::make_shared<EngineState>();
        probeState->combi.name = selectedSound->program->name;
        auto& probeUpper = probeState->combi.uppers[0];
        probeUpper.name = selectedSound->program->name;
        probeUpper.enabled = true;
        probeUpper.mute = false;
        probeUpper.solo = false;
        probeUpper.program = selectedSound->program;
        probeUpper.soundSlotId = selectedSound->address.slot;
        probeUpper.midiChannel = 0U;

        SamplerEngine probeEngine;
        probeEngine.prepare(48000.0, 256U);
        probeEngine.setInterpolationQuality(InterpolationQuality::LiveCubic4);
        probeEngine.setState(probeState);
        if (!probeEngine.postNoteOn(0U, selectedKey, selectedVelocity)) {
            setTextError("SILVETON bank playback probe could not queue MIDI note");
            return false;
        }
        std::vector<float> probeAudio(256U * 2U, 0.0F);
        float probePeak = 0.0F;
        for (int blockIndex = 0; blockIndex < 64 && probePeak <= 1.0e-6F; ++blockIndex) {
            probeEngine.render(probeAudio.data(), 256U);
            for (const float value : probeAudio) {
                const float magnitude = value < 0.0F ? -value : value;
                if (magnitude > probePeak) probePeak = magnitude;
            }
        }
        if (!(probePeak > 1.0e-6F)) {
            setTextError("SILVETON bank loaded but SAMPLE playback path is silent");
            return false;
        }

        auto state = std::make_shared<EngineState>();
        if (state_ != nullptr) state->global = state_->global;
        state->combi.name = selectedSound->program->name;
        auto& firstUpper = state->combi.uppers[0];
        firstUpper.name = selectedSound->program->name;
        firstUpper.enabled = true;
        firstUpper.mute = false;
        firstUpper.solo = false;
        firstUpper.program = selectedSound->program;
        firstUpper.soundSlotId = selectedSound->address.slot;
        firstUpper.midiChannel = 0U;
        if (!installState(std::move(state))) return false;
        setTextError({});
        return true;
'''

if old in s:
    s = s.replace(old, new, 1)
elif 'SILVETON bank loaded but SAMPLE playback path is silent' in s and 'const ProgramSlot* selectedSound = nullptr;' in s:
    pass
else:
    load0 = s.find('    bool loadSvBank(const std::string& path) {')
    load1 = s.find('\n    bool loadSet(', load0)
    if load0 < 0 or load1 < 0:
        raise SystemExit('loadSvBank boundaries changed')
    block = s[load0:load1]
    begin = block.find('        auto state = std::make_shared<EngineState>();')
    finish_token = '        setTextError({});\n        return true;\n'
    finish = block.find(finish_token, begin)
    if begin < 0 or finish < 0:
        raise SystemExit('final bank state-install block changed')
    finish += len(finish_token)
    block = block[:begin] + new + block[finish:]
    s = s[:load0] + block + s[load1:]

bridge.write_text(s, encoding='utf-8')

# Regression: locate testSvBankRuntime structurally. The preceding stable-ID
# patch rewrites metadata assertions, so they must never be used as an anchor.
t = tests.read_text(encoding='utf-8')
if 'End-to-end bank playback contract' not in t:
    fn0 = t.find('void testSvBankRuntime()')
    fn1 = t.find('void testSampleRecorderRf64()', fn0)
    if fn0 < 0 or fn1 < 0:
        raise SystemExit('testSvBankRuntime function boundaries changed')
    block = t[fn0:fn1]
    insert_at = block.find('    auto corrupted=file;')
    if insert_at < 0:
        insert_at = block.find('    auto corrupted = file;')
    if insert_at < 0:
        raise SystemExit('testSvBankRuntime corruption-check anchor changed')

    probe_test = '''    // End-to-end bank playback contract: successful bank load must create
    // a real voice and non-zero stereo audio from the SAMPLE stream.
    auto playbackState=std::make_shared<EngineState>();
    playbackState->combi.name=program->name;
    playbackState->combi.uppers[0].name=program->name;
    playbackState->combi.uppers[0].enabled=true;
    playbackState->combi.uppers[0].program=program;
    playbackState->combi.uppers[0].soundSlotId=loaded.data.programs[0].address.slot;
    playbackState->combi.uppers[0].midiChannel=0U;
    SamplerEngine bankEngine; bankEngine.prepare(48000.0,16U); bankEngine.setState(playbackState);
    assert(bankEngine.postNoteOn(0U,60U,127U));
    std::vector<float> bankAudio(8U,0.0F); bankEngine.render(bankAudio.data(),4U);
    float bankPeak=0.0F; for(const float v:bankAudio){const float a=v<0.0F?-v:v;if(a>bankPeak)bankPeak=a;}
    assert(bankEngine.activeVoiceCount()>0U);
    assert(bankPeak>1.0e-6F);

'''
    block = block[:insert_at] + probe_test + block[insert_at:]
    t = t[:fn0] + block + t[fn1:]

tests.write_text(t, encoding='utf-8')

final_bridge = bridge.read_text(encoding='utf-8')
for token in [
    'const ProgramSlot* selectedSound = nullptr;',
    'probeEngine.setState(probeState);',
    'probeEngine.postNoteOn(0U, selectedKey, selectedVelocity)',
    'SILVETON bank loaded but SAMPLE playback path is silent',
    'firstUpper.soundSlotId = selectedSound->address.slot;',
]:
    if token not in final_bridge:
        raise SystemExit(f'bank playback contract missing: {token}')

final_tests = tests.read_text(encoding='utf-8')
for token in [
    'End-to-end bank playback contract',
    'assert(bankEngine.activeVoiceCount()>0U);',
    'assert(bankPeak>1.0e-6F);',
]:
    if token not in final_tests:
        raise SystemExit(f'bank playback regression missing: {token}')

print('PASS: real bank SOUND -> MULTISAMPLE -> ZONE -> SAMPLE -> MIDI -> DSP playback contract enforced')
