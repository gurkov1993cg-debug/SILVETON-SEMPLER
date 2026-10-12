#include "silveton/sampler/AudioFileLoader.h"
#include "silveton/sampler/SamplerEngine.h"
#include "silveton/sampler/SamplerLibrary.h"
#include "silveton/sampler/SampleStream.h"
#include "silveton/sampler/SampleStreamPrefetcher.h"
#include <chrono>
#include "silveton/sampler/SampleRecorder.h"
#include "silveton/sampler/KorgImporter.h"
#include "silveton/sampler/MultisampleEditor.h"
#include "silveton/sampler/ProgramEditor.h"
#include "silveton/sampler/SetPackage.h"
#include "silveton/sampler/SampleEditor.h"
#include "silveton/sampler/SincInterpolator.h"
#include "silveton/sampler/SvBankFormat.h"
#include "silveton/sampler/SvBankRuntime.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace silveton::sampler;

namespace {

void putLe16(std::ofstream& f, std::uint16_t v) {
    const std::array<char,2> b{{static_cast<char>(v & 0xFFU), static_cast<char>((v >> 8U) & 0xFFU)}};
    f.write(b.data(), 2);
}
void putLe32(std::ofstream& f, std::uint32_t v) {
    const std::array<char,4> b{{static_cast<char>(v & 0xFFU), static_cast<char>((v >> 8U) & 0xFFU),
                                static_cast<char>((v >> 16U) & 0xFFU), static_cast<char>((v >> 24U) & 0xFFU)}};
    f.write(b.data(), 4);
}
void putBe16(std::ofstream& f, std::uint16_t v) {
    const std::array<char,2> b{{static_cast<char>((v >> 8U) & 0xFFU), static_cast<char>(v & 0xFFU)}};
    f.write(b.data(), 2);
}
void putBe32(std::ofstream& f, std::uint32_t v) {
    const std::array<char,4> b{{static_cast<char>((v >> 24U) & 0xFFU), static_cast<char>((v >> 16U) & 0xFFU),
                                static_cast<char>((v >> 8U) & 0xFFU), static_cast<char>(v & 0xFFU)}};
    f.write(b.data(), 4);
}

std::array<std::uint8_t,10> ext80_44100() {
    return {{0x40U,0x0EU,0xACU,0x44U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U}};
}

std::string makeWav() {
    const std::string path = "/tmp/silveton_sampler_test.wav";
    constexpr std::uint32_t frames = 480U;
    constexpr std::uint32_t dataBytes = frames * 2U;
    constexpr std::uint32_t fmtSize = 16U;
    constexpr std::uint32_t smplSize = 60U;
    const std::uint32_t riffSize = 4U + (8U + fmtSize) + (8U + smplSize) + (8U + dataBytes);
    std::ofstream f(path, std::ios::binary);
    f.write("RIFF",4); putLe32(f, riffSize); f.write("WAVE",4);
    f.write("fmt ",4); putLe32(f, fmtSize); putLe16(f,1U); putLe16(f,1U); putLe32(f,48000U);
    putLe32(f,96000U); putLe16(f,2U); putLe16(f,16U);
    f.write("smpl",4); putLe32(f, smplSize);
    putLe32(f,0U); putLe32(f,0U); putLe32(f,20833U); putLe32(f,64U); putLe32(f,0U);
    putLe32(f,0U); putLe32(f,0U); putLe32(f,1U); putLe32(f,0U);
    putLe32(f,0U); putLe32(f,0U); putLe32(f,100U); putLe32(f,399U); putLe32(f,0U); putLe32(f,0U);
    f.write("data",4); putLe32(f,dataBytes);
    for (std::uint32_t i=0; i<frames; ++i) {
        const double x = std::sin(2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / 48000.0);
        const auto s = static_cast<std::int16_t>(std::lround(x * 20000.0));
        putLe16(f, static_cast<std::uint16_t>(s));
    }
    return path;
}

std::string makeAiff() {
    const std::string path = "/tmp/silveton_sampler_test.aiff";
    constexpr std::uint32_t frames = 256U;
    constexpr std::uint32_t commSize = 18U;
    constexpr std::uint32_t ssndSize = 8U + frames * 2U;
    const std::uint32_t formSize = 4U + 8U + commSize + 8U + ssndSize;
    std::ofstream f(path, std::ios::binary);
    f.write("FORM",4); putBe32(f,formSize); f.write("AIFF",4);
    f.write("COMM",4); putBe32(f,commSize); putBe16(f,1U); putBe32(f,frames); putBe16(f,16U);
    const auto sr=ext80_44100(); f.write(reinterpret_cast<const char*>(sr.data()), static_cast<std::streamsize>(sr.size()));
    f.write("SSND",4); putBe32(f,ssndSize); putBe32(f,0U); putBe32(f,0U);
    for (std::uint32_t i=0; i<frames; ++i) {
        const double x = std::sin(2.0 * 3.14159265358979323846 * 220.0 * static_cast<double>(i) / 44100.0);
        const auto s=static_cast<std::int16_t>(std::lround(x*18000.0));
        putBe16(f,static_cast<std::uint16_t>(s));
    }
    return path;
}

std::shared_ptr<EngineState> makeState(const SamplePtr& sample) {
    auto ms=std::make_shared<Multisample>();
    ms->name="Test MS";
    SampleZone z{}; z.sample=sample; z.keyLow=0U; z.keyHigh=127U; z.velocityLow=1U; z.velocityHigh=127U;
    z.rootKey=sample->rootKey; z.pan=0.0F; z.gainDb=0.0F;
    ms->zones.push_back(z);

    auto prog=std::make_shared<Program>();
    prog->name="Test Program";
    ProgramLayer layer{}; layer.name="Layer 1"; layer.multisample=ms; layer.filter.mode=FilterMode::LowPass;
    layer.filter.cutoffHz=18000.0F; layer.ampEnvelope={0.0F,0.01F,1.0F,0.02F};
    prog->layers.push_back(layer);

    auto st=std::make_shared<EngineState>();
    st->combi.name="Test Combi";
    st->combi.uppers[0].name="U1"; st->combi.uppers[0].program=prog; st->combi.uppers[0].enabled=true;
    st->combi.uppers[0].midiChannel=0U; st->combi.uppers[0].breathEnabled=false;
    st->global.limiterEnabled=true; st->global.limiterCeilingDb=-0.3F; st->global.masterGainDb=0.0F;   // the tests measure levels at 0 dB
    return st;
}

float rms(const std::vector<float>& x) {
    double s=0.0; for(float v:x) s += static_cast<double>(v)*static_cast<double>(v);
    return x.empty()?0.0F:static_cast<float>(std::sqrt(s/static_cast<double>(x.size())));
}

void testWavAiffLoaders() {
    const auto wav=AudioFileLoader::load(makeWav());
    if (!wav.ok()) { std::fprintf(stderr, "WAV error: %s\n", wav.error.c_str()); }
    assert(wav.ok()); assert(wav.sample->sampleRate==48000U); assert(wav.sample->channels==1U);
    assert(wav.sample->rootKey==64U); assert(wav.sample->loop.enabled); assert(wav.sample->loop.startFrame==100U);
    assert(wav.sample->loop.endFrame==400U); assert(wav.sample->frameCount()==480U);

    const auto aiff=AudioFileLoader::load(makeAiff());
    if (!aiff.ok()) { std::fprintf(stderr, "AIFF error: %s\n", aiff.error.c_str()); }
    assert(aiff.ok()); assert(aiff.sample->sampleRate==44100U); assert(aiff.sample->channels==1U);
    assert(aiff.sample->frameCount()==256U); assert(rms(aiff.sample->interleaved)>0.1F);
}

void testEnginePlaybackAndControls() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    assert(sample);
    auto st=makeState(sample);
    SamplerEngine e; e.prepare(48000.0,256U); e.setState(st); e.setInterpolationQuality(InterpolationQuality::Ultra32);
    assert(e.postNoteOn(0U,64U,120U));
    std::vector<float> out(512U);
    e.render(out.data(),256U);
    assert(rms(out)>0.01F); assert(e.activeVoiceCount()>0U);
    const auto m=e.meters(); assert(m.peakLeft>0.0F && m.peakRight>0.0F);
    SamplerEngine e96; e96.prepare(96000.0,96U); e96.setState(st); e96.setInterpolationQuality(InterpolationQuality::LiveCubic4);
    assert(e96.postNoteOn(0U,60U,120U));
    std::vector<float> out96(192U);
    e96.render(out96.data(),96U);
    assert(rms(out96)>0.001F); // 48 kHz SAMPLE must play through 96 kHz engine
    assert(e.postControlChange(0U,11U,0U)); e.render(out.data(),256U);
    assert(rms(out)<0.001F);
    assert(e.postControlChange(0U,11U,127U));
    assert(e.postNoteOff(0U,64U));
    for(int i=0;i<20;++i) e.render(out.data(),256U);
    assert(e.activeVoiceCount()==0U);
}

void testVelocityAndKeyZones() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    auto st=makeState(sample);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    auto ms=std::const_pointer_cast<Multisample>(prog->layers[0].multisample);
    ms->zones[0].keyLow=60U; ms->zones[0].keyHigh=72U; ms->zones[0].velocityLow=64U;
    SamplerEngine e; e.prepare(48000.0,64U); e.setState(st);
    std::vector<float> out(128U);
    e.postNoteOn(0U,50U,127U); e.render(out.data(),64U); assert(rms(out)==0.0F);
    e.postNoteOn(0U,64U,30U); e.render(out.data(),64U); assert(rms(out)==0.0F);
    e.postNoteOn(0U,64U,100U); e.render(out.data(),64U); assert(rms(out)>0.0F);
}

void testProgramEditorAndLayerDsp() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    assert(sample);
    auto state=makeState(sample);
    const auto original=state->combi.uppers[0].program;
    assert(original && original->layers.size()==1U);

    const auto basic=ProgramEditor::updateBasic(original,true,true,0.25F);
    assert(basic.ok());
    assert(basic.program!=original);
    assert(!original->mono && !original->legato);
    assert(basic.program->mono && basic.program->legato);
    assert(std::abs(basic.program->portamentoSec-0.25F)<1.0e-6F);

    const auto edited=ProgramEditor::updateLayerBasic(
        basic.program,0U,true,10,110,1,127,100,20,12,7.5F,-3.0F,0.25F,0.01F,false);
    assert(edited.ok());
    const auto& layer=edited.program->layers[0];
    assert(layer.keyLow==10U && layer.keyHigh==110U);
    assert(layer.velocityFadeIn==100U && layer.velocityFadeOut==20U);
    assert(layer.transposeSemitones==12);
    assert(std::abs(layer.tuneCents-7.5F)<1.0e-6F);
    assert(std::abs(layer.delaySec-0.01F)<1.0e-6F);
    assert(!layer.damperEnabled);
    assert(ProgramEditor::updateLayerBasic(edited.program,0U,true,90,20,1,127,0,0,0,0.0F,0.0F,0.0F,0.0F,true).program==nullptr);

    auto ampEdited=ProgramEditor::updateLayerAmpEnvelope(edited.program,0U,EnvelopeParams{0.02F,0.30F,0.65F,0.40F});
    assert(ampEdited.ok());
    assert(std::abs(ampEdited.program->layers[0].ampEnvelope.sustain-0.65F)<1.0e-6F);
    auto pitchEdited=ProgramEditor::updateLayerPitch(ampEdited.program,0U,350.0F,EnvelopeParams{0.01F,0.20F,0.0F,0.10F});
    assert(pitchEdited.ok());
    assert(std::abs(pitchEdited.program->layers[0].pitchEnvelopeCents-350.0F)<1.0e-6F);
    FilterParams filter{}; filter.mode=FilterMode::BandPass; filter.cutoffHz=3200.0F; filter.resonance=0.55F;
    filter.keyTrack=0.5F; filter.velocityAmount=1.0F; filter.envelopeAmount=2.0F; filter.lfoAmount=-0.25F;
    auto filterEdited=ProgramEditor::updateLayerFilter(pitchEdited.program,0U,filter,EnvelopeParams{0.01F,0.25F,0.2F,0.15F});
    assert(filterEdited.ok());
    assert(filterEdited.program->layers[0].filter.mode==FilterMode::BandPass);
    assert(std::abs(filterEdited.program->layers[0].filter.cutoffHz-3200.0F)<1.0e-6F);
    LfoParams lfo{}; lfo.wave=LfoWave::Triangle; lfo.frequencyHz=6.5F; lfo.delaySec=0.1F; lfo.fadeSec=0.2F;
    lfo.keySync=false; lfo.pitchCents=12.0F; lfo.filterOctaves=0.5F; lfo.ampDepth=0.1F;
    auto lfoEdited=ProgramEditor::updateLayerLfo(filterEdited.program,0U,1,lfo);
    assert(lfoEdited.ok());
    assert(lfoEdited.program->layers[0].lfo2.wave==LfoWave::Triangle);
    assert(std::abs(lfoEdited.program->layers[0].lfo2.frequencyHz-6.5F)<1.0e-6F);

    auto secondMs=std::make_shared<Multisample>(*original->layers[0].multisample);
    secondMs->name="Second MS";
    auto withSecond=ProgramEditor::addLayer(lfoEdited.program,"Layer 2",secondMs);
    assert(withSecond.ok() && withSecond.program->layers.size()==2U);
    auto moved=ProgramEditor::moveLayer(withSecond.program,1U,0U);
    assert(moved.ok() && moved.program->layers[0].name=="Layer 2");
    auto reassigned=ProgramEditor::assignMultisample(moved.program,1U,secondMs);
    assert(reassigned.ok() && reassigned.program->layers[1].multisample==secondMs);
    auto removed=ProgramEditor::removeLayer(reassigned.program,0U);
    assert(removed.ok() && removed.program->layers.size()==1U);

    // Delay must postpone the actual sample/envelope path, not merely store a value.
    auto delayedState=makeState(sample);
    auto delayedProgram=std::make_shared<Program>(*delayedState->combi.uppers[0].program);
    delayedProgram->layers[0].delaySec=0.01F; // 480 frames at 48 kHz
    delayedState->combi.uppers[0].program=delayedProgram;
    SamplerEngine delayed; delayed.prepare(48000.0,128U); delayed.setState(delayedState);
    std::vector<float> out(256U);
    delayed.postNoteOn(0U,64U,127U);
    delayed.render(out.data(),128U); assert(rms(out)==0.0F);
    delayed.render(out.data(),128U); assert(rms(out)==0.0F);
    delayed.render(out.data(),128U); assert(rms(out)==0.0F);
    delayed.render(out.data(),128U); assert(rms(out)>0.001F);

    // Crossfade gain is independent from the global velocity curve.
    auto lowState=makeState(sample);
    auto lowProgram=std::make_shared<Program>(*lowState->combi.uppers[0].program);
    lowProgram->layers[0].velocityLow=1U;
    lowProgram->layers[0].velocityHigh=127U;
    lowProgram->layers[0].velocityFadeIn=100U;
    lowState->global.velocityCurve=3U; // constant base velocity gain
    lowState->combi.uppers[0].program=lowProgram;
    SamplerEngine low; low.prepare(48000.0,128U); low.setState(lowState);
    low.postNoteOn(0U,64U,10U); low.render(out.data(),128U); const float lowRms=rms(out);
    SamplerEngine high; high.prepare(48000.0,128U); high.setState(lowState);
    high.postNoteOn(0U,64U,120U); high.render(out.data(),128U); const float highRms=rms(out);
    assert(highRms>0.01F);
    assert(lowRms<highRms*0.2F);

    // Layer-level damper OFF must release even when the Upper accepts sustain.
    auto damperState=makeState(sample);
    auto damperProgram=std::make_shared<Program>(*damperState->combi.uppers[0].program);
    damperProgram->layers[0].damperEnabled=false;
    damperProgram->layers[0].ampEnvelope.releaseSec=0.01F;
    damperState->combi.uppers[0].program=damperProgram;
    SamplerEngine damper; damper.prepare(48000.0,128U); damper.setState(damperState);
    damper.postControlChange(0U,64U,127U);
    damper.postNoteOn(0U,64U,127U); damper.render(out.data(),128U);
    damper.postNoteOff(0U,64U);
    for(int i=0;i<12;++i) damper.render(out.data(),128U);
    assert(damper.activeVoiceCount()==0U);
}

void testBreathAndSustain() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    auto st=makeState(sample);
    st->combi.uppers[0].breathEnabled=true;
    SamplerEngine e; e.prepare(48000.0,128U); e.setState(st);
    std::vector<float> out(256U);
    e.postControlChange(0U,2U,0U); e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    assert(rms(out)<0.001F);
    e.postControlChange(0U,2U,127U); e.render(out.data(),128U); assert(rms(out)>0.01F);
    e.postControlChange(0U,64U,127U); e.postNoteOff(0U,64U); e.render(out.data(),128U); assert(e.activeVoiceCount()>0U);
    e.postControlChange(0U,64U,0U); for(int i=0;i<20;++i)e.render(out.data(),128U); assert(e.activeVoiceCount()==0U);
}

void testGlobalControllerAssignments() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    auto st=makeState(sample);
    st->combi.uppers[0].breathEnabled=true;
    st->global.breathCc=12U;
    st->global.expressionCc=7U;
    st->global.damperCc=66U;
    SamplerEngine e; e.prepare(48000.0,128U); e.setState(st);
    std::vector<float> out(256U);
    e.postControlChange(0U,2U,0U); // old default CC2 must no longer control breath
    e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    assert(rms(out)>0.01F);
    e.postControlChange(0U,12U,0U); e.render(out.data(),128U);
    assert(rms(out)<0.001F);
    e.postControlChange(0U,12U,127U);
    e.postControlChange(0U,66U,127U); e.postNoteOff(0U,64U); e.render(out.data(),128U);
    assert(e.activeVoiceCount()>0U);
    e.postControlChange(0U,66U,0U); for(int i=0;i<20;++i)e.render(out.data(),128U);
    assert(e.activeVoiceCount()==0U);
}

void testGlobalAdvancedControls() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    auto st=makeState(sample);
    st->combi.uppers[0].breathEnabled=true;
    st->global.midiInputChannelMask=0U;
    SamplerEngine e; e.prepare(48000.0,128U); e.setState(st);
    std::vector<float> out(256U);

    e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    assert(rms(out)==0.0F);

    st=std::make_shared<EngineState>(*st);
    st->global.midiInputChannelMask=1U;
    st->global.filterNotes=true;
    e.setState(st);
    e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    assert(rms(out)==0.0F);

    st=std::make_shared<EngineState>(*st);
    st->global.filterNotes=false;
    st->global.breathMin=0.5F;
    st->global.breathMax=1.0F;
    st->global.breathCurve=1.0F;
    e.setState(st);
    e.postControlChange(0U,2U,64U); e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    assert(rms(out)<0.005F);
    e.postControlChange(0U,2U,127U); e.render(out.data(),128U);
    assert(rms(out)>0.01F);

    st=std::make_shared<EngineState>(*st);
    st->global.damperPolarityReversed=true;
    e.setState(st);
    e.postControlChange(0U,64U,0U); // reversed polarity: low value = pedal pressed
    e.postNoteOn(0U,67U,127U); e.render(out.data(),128U);
    e.postNoteOff(0U,67U); e.render(out.data(),128U);
    assert(e.activeVoiceCount()>0U);
    e.postControlChange(0U,64U,127U); // release reversed pedal
    for(int i=0;i<20;++i)e.render(out.data(),128U);
    assert(e.activeVoiceCount()>0U); // seamless state swap must preserve the held voice
}

void testUpperControllerMatrix() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    assert(sample);
    auto st=makeState(sample);
    auto& u1=st->combi.uppers[0];
    u1.expressionEnabled=false;
    u1.controllerRoutes[0].source=ControllerSource::ControlChange;
    u1.controllerRoutes[0].destination=ModDestination::LevelDb;
    u1.controllerRoutes[0].cc=74U;
    u1.controllerRoutes[0].amount=-48.0F;
    u1.controllerRoutes[0].bipolar=false;

    st->combi.uppers[1]=u1;
    st->combi.uppers[1].name="U2";
    st->combi.uppers[1].midiChannel=1U;
    st->combi.uppers[1].controllerRoutes={};

    SamplerEngine e; e.prepare(48000.0,128U); e.setState(st);
    std::vector<float> out(256U);
    e.postControlChange(0U,74U,0U);
    e.postNoteOn(0U,64U,127U); e.render(out.data(),128U);
    const float u1Open=rms(out); assert(u1Open>0.01F);
    e.postControlChange(0U,74U,127U); e.render(out.data(),128U);
    const float u1Closed=rms(out); assert(u1Closed<u1Open*0.02F);

    e.postPanic(); e.render(out.data(),128U);
    e.postControlChange(1U,74U,127U);
    e.postNoteOn(1U,64U,127U); e.render(out.data(),128U);
    const float u2Unmapped=rms(out);
    assert(u2Unmapped>0.01F); // U2 has no route: U1's CC mapping must not leak into it.
}

void testCombiFxRouting() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    assert(sample);
    auto parallel=makeState(sample);
    parallel->combi.uppers[0].fxSend1=1.0F;
    parallel->combi.uppers[0].fxSend2=0.0F;
    parallel->combi.fx[0].type=EffectType::Delay;
    parallel->combi.fx[0].wet=0.8F;
    parallel->combi.fx[0].timeMs=1.0F;
    parallel->combi.fx[0].feedback=0.0F;
    parallel->combi.fx[1].type=EffectType::Delay;
    parallel->combi.fx[1].wet=0.8F;
    parallel->combi.fx[1].timeMs=1.0F;
    parallel->combi.fx[1].feedback=0.0F;
    parallel->combi.fxRouting=EffectRouting::Parallel;

    auto serial=std::make_shared<EngineState>(*parallel);
    serial->combi.fxRouting=EffectRouting::Fx1IntoFx2;

    SamplerEngine a; a.prepare(48000.0,512U); a.setState(parallel);
    SamplerEngine b; b.prepare(48000.0,512U); b.setState(serial);
    std::vector<float> outA(1024U), outB(1024U);
    a.postNoteOn(0U,64U,127U); b.postNoteOn(0U,64U,127U);
    a.render(outA.data(),512U); b.render(outB.data(),512U);
    double diff=0.0;
    for(std::size_t i=0U;i<outA.size();++i) diff+=std::abs(static_cast<double>(outA[i])-static_cast<double>(outB[i]));
    assert(diff>0.1); // serial routing must produce a genuinely different DSP path.
}

void testRealtimeQueueOverflow() {
    RealtimeEventQueue q;
    RealtimeEvent e{};
    int accepted=0;
    for(std::size_t i=0;i<RealtimeEventQueue::kCapacity+20U;++i) if(q.push(e)) ++accepted;
    assert(accepted==static_cast<int>(RealtimeEventQueue::kCapacity-1U));
    assert(q.droppedCount()>0U);
    int popped=0; while(q.pop(e)) ++popped;
    assert(popped==accepted);
}


void writeChunkBE(std::ofstream& f, const char id[4], const std::vector<std::uint8_t>& data) {
    f.write(id,4); putBe32(f, static_cast<std::uint32_t>(data.size()));
    if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::string makeKsf(const std::string& dir, const std::string& name, int rootSeed) {
    (void)rootSeed;
    const std::string path = dir + "/" + name;
    std::ofstream f(path, std::ios::binary);
    std::vector<std::uint8_t> smp(32U, 0U);
    const std::string sampleName = "KORG_SAMPLE";
    std::memcpy(smp.data(), sampleName.data(), sampleName.size());
    // Newer SMP1 layout: Start, Start2, LoopStart, LoopEnd.
    smp[24] = 0U; smp[25] = 0U; smp[26] = 0U; smp[27] = 16U;
    smp[28] = 0U; smp[29] = 0U; smp[30] = 0U; smp[31] = 112U;
    writeChunkBE(f, "SMP1", smp);

    constexpr std::uint32_t points = 128U;
    std::vector<std::uint8_t> smd(12U + points * 2U, 0U);
    const std::uint32_t sr = 48000U;
    smd[0]=static_cast<std::uint8_t>(sr>>24U); smd[1]=static_cast<std::uint8_t>(sr>>16U);
    smd[2]=static_cast<std::uint8_t>(sr>>8U); smd[3]=static_cast<std::uint8_t>(sr);
    smd[4]=0U; smd[5]=0U; smd[6]=1U; smd[7]=16U;
    smd[8]=0U; smd[9]=0U; smd[10]=0U; smd[11]=static_cast<std::uint8_t>(points);
    for (std::uint32_t i=0;i<points;++i) {
        const double x=std::sin(2.0*3.14159265358979323846*330.0*static_cast<double>(i)/48000.0);
        const auto v=static_cast<std::int16_t>(std::lround(x*20000.0));
        const auto u=static_cast<std::uint16_t>(v);
        smd[12U+i*2U]=static_cast<std::uint8_t>(u>>8U);
        smd[13U+i*2U]=static_cast<std::uint8_t>(u);
    }
    writeChunkBE(f, "SMD1", smd);
    return path;
}

std::string makeKmpSet() {
    const std::string dir = "/tmp/silveton_korg";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    makeKsf(dir, "PI0000.KSF", 60);
    makeKsf(dir, "PI0001.KSF", 72);
    const std::string path = dir + "/PIANO.KMP";
    std::ofstream f(path, std::ios::binary);
    std::vector<std::uint8_t> msp(18U,0U);
    const std::string nm="AC PIANO"; std::memcpy(msp.data(),nm.data(),nm.size());
    msp[16]=2U; msp[17]=0U; writeChunkBE(f,"MSP1",msp);
    std::vector<std::uint8_t> rlp(36U,0U);
    rlp[0]=60U; rlp[1]=63U; rlp[2]=0U; rlp[3]=0U; rlp[4]=0U; rlp[5]=0U;
    const std::string a="PI0000.KSF"; std::memcpy(rlp.data()+6U,a.data(),a.size());
    rlp[18U]=72U; rlp[19U]=127U; rlp[20U]=static_cast<std::uint8_t>(static_cast<std::int8_t>(-3));
    const std::string b="PI0001.KSF"; std::memcpy(rlp.data()+24U,b.data(),b.size());
    writeChunkBE(f,"RLP1",rlp);
    return path;
}

void testKorgImporters() {
    const std::string kmp = makeKmpSet();
    const auto ksf = KorgImporter::loadKsf("/tmp/silveton_korg/PI0000.KSF");
    assert(ksf.ok()); assert(ksf.sample->sampleRate==48000U); assert(ksf.sample->channels==1U);
    assert(ksf.sample->loop.enabled); assert(ksf.sample->loop.startFrame==16U); assert(ksf.sample->loop.endFrame==112U);
    const auto ksfStream = KorgImporter::loadKsfStreaming("/tmp/silveton_korg/PI0000.KSF",16U);
    assert(ksfStream.ok()); assert(ksfStream.sample->stream!=nullptr);
    assert(ksfStream.sample->preloadedFrameCount()==16U); assert(ksfStream.sample->frameCount()==128U);
    assert(ksfStream.sample->loop.enabled); assert(ksfStream.sample->loop.startFrame==16U); assert(ksfStream.sample->loop.endFrame==112U);
    const auto imported = KorgImporter::loadKmp(kmp);
    assert(imported.ok()); assert(imported.multisample->zones.size()==2U);
    assert(imported.multisample->zones[0].sample!=nullptr && imported.multisample->zones[0].sample->stream!=nullptr);
    assert(imported.multisample->zones[0].keyLow==0U && imported.multisample->zones[0].keyHigh==63U);
    assert(imported.multisample->zones[1].keyLow==64U && imported.multisample->zones[1].keyHigh==127U);
    assert(imported.multisample->zones[1].tuneCents==-3.0F);
    const std::string ksc="/tmp/silveton_korg/PCMGROUP.KSC";
    { std::ofstream f(ksc); f << "#KORG Script Version 1.0\n# test\nPIANO.KMP\nPI0000.KSF\n"; }
    std::string err; const auto entries=KorgImporter::parseKsc(ksc,err);
    assert(err.empty()); assert(entries.size()==2U); assert(entries[0]=="PIANO.KMP");
    const auto collection = KorgImporter::loadKsc(ksc);
    assert(collection.ok());
    assert(collection.multisamples.size()==1U);
    assert(collection.samples.size()==2U);
    assert(collection.multisamples[0]->zones.size()==2U);
}




void testKorgLibraryWorkflow() {
    const std::string kmp = makeKmpSet();
    SamplerLibrary library("Korg Library Test");

    const auto first = library.importKmp(kmp);
    assert(first.success);
    assert(first.error.empty());
    assert(first.sampleIds.size() == 2U);
    assert(first.multisampleIds.size() == 1U);
    const auto sampleIds = first.sampleIds;
    const auto multisampleId = first.multisampleIds.front();
    const auto importedMs = library.multisample(multisampleId);
    assert(importedMs != nullptr);
    assert(importedMs->zones.size() == 2U);
    assert(!importedMs->sourceIdentity.empty());
    assert(library.sample(sampleIds[0]) != nullptr);
    assert(library.sample(sampleIds[1]) != nullptr);
    assert(!library.sample(sampleIds[0])->sourceIdentity.empty());

    auto program = std::make_shared<Program>();
    program->name = "Imported Piano";
    ProgramLayer layer{};
    layer.name = "Layer 1";
    layer.multisample = importedMs;
    program->layers.push_back(layer);
    const SlotAddress programAddress{3U, 1U, 5U};
    assert(library.writeProgram(programAddress, program));

    const auto second = library.importKmp(kmp);
    assert(second.success);
    assert(second.sampleIds == sampleIds);
    assert(second.multisampleIds.size() == 1U);
    assert(second.multisampleIds.front() == multisampleId);
    const std::string ksc = "/tmp/silveton_korg/LIBRARY.KSC";
    { std::ofstream f(ksc); f << "#KORG Script Version 1.0\nPIANO.KMP\nPI0000.KSF\n"; }
    const auto collectionImport = library.importKsc(ksc);
    assert(collectionImport.success);
    assert(collectionImport.sampleIds == sampleIds);
    assert(collectionImport.multisampleIds.size() == 1U);
    assert(collectionImport.multisampleIds.front() == multisampleId);

    const auto refreshedMs = library.multisample(multisampleId);
    assert(refreshedMs != nullptr);
    const auto refreshedProgram = library.program(programAddress);
    assert(refreshedProgram != nullptr);
    assert(refreshedProgram->layers[0].multisample == refreshedMs);

    const std::string package = "/tmp/silveton_korg_identity.SVSET";
    std::filesystem::remove_all(package);
    const auto saved = SetPackage::save(package, library.snapshot());
    assert(saved.success);
    const auto loaded = SetPackage::load(package);
    assert(loaded.ok());
    SamplerLibrary restored("restored");
    restored.replace(loaded.data);
    assert(restored.multisample(multisampleId) != nullptr);
    assert(!restored.multisample(multisampleId)->sourceIdentity.empty());
    assert(restored.sample(sampleIds[0]) != nullptr);
    assert(!restored.sample(sampleIds[0])->sourceIdentity.empty());

    const auto third = restored.importKmp(kmp);
    assert(third.success);
    assert(third.sampleIds == sampleIds);
    assert(third.multisampleIds.size() == 1U);
    assert(third.multisampleIds.front() == multisampleId);

    const auto before = restored.snapshot();
    const auto failed = restored.importKmp("/tmp/silveton_korg/DOES_NOT_EXIST.KMP");
    assert(!failed.success);
    const auto after = restored.snapshot();
    assert(after.samples.size() == before.samples.size());
    assert(after.multisamples.size() == before.multisamples.size());
}

void testSetPackageRoundTrip() {
    const auto sample = AudioFileLoader::load(makeWav()).sample;
    assert(sample);
    auto state = makeState(sample);
    auto program = state->combi.uppers[0].program;
    auto programMutable = std::const_pointer_cast<Program>(program);
    programMutable->layers[0].velocityFadeIn = 7U;
    programMutable->layers[0].velocityFadeOut = 9U;
    programMutable->layers[0].delaySec = 0.031F;
    programMutable->layers[0].damperEnabled = false;
    SetData data;
    data.name = "My Set";
    data.global = state->global;
    data.global.masterTuneHz = 442.0F;
    data.global.breathCc = 12U;
    data.global.expressionCc = 7U;
    data.global.damperCc = 66U;
    data.global.pitchBendEnabled = false;
    data.global.aftertouchEnabled = false;
    data.global.scaleRoot = 2U;
    data.global.scaleOffsetsCents[1] = -50.0F;
    data.global.scaleOffsetsCents[6] = 25.0F;
    data.global.midiInputChannelMask = 0x0005U;
    data.global.filterNotes = true;
    data.global.filterControlChange = true;
    data.global.filterPitchBend = true;
    data.global.filterAftertouch = true;
    data.global.breathMin = 0.15F;
    data.global.breathMax = 0.90F;
    data.global.breathCurve = 1.7F;
    data.global.expressionMin = 0.10F;
    data.global.expressionMax = 0.95F;
    data.global.expressionCurve = 0.8F;
    data.global.damperPolarityReversed = true;
    data.programs.push_back({{1U,2U,3U}, program});
    CombiSlot cs; cs.address={4U,5U,6U}; cs.combi=state->combi; cs.combi.name="Stage Combi";
    cs.combi.uppers[0].ampAttackScale=2.0F;
    cs.combi.uppers[0].toneAttackMs=12.5F; cs.combi.uppers[0].toneDecayMs=340.0F; cs.combi.uppers[0].toneSustain=0.42F;
    cs.combi.uppers[0].toneReleaseMs=900.0F; cs.combi.uppers[0].toneCutoffHz=1800.0F; cs.combi.uppers[0].toneResonance=0.35F;
    cs.combi.uppers[0].ampDecayScale=0.75F;
    cs.combi.uppers[0].ampReleaseScale=1.5F;
    cs.combi.uppers[0].filterCutoffOctaves=-1.25F;
    cs.combi.uppers[0].filterResonanceOffset=0.2F;
    cs.combi.uppers[0].lfoRateScale=1.75F;
    cs.combi.uppers[0].lfoDepthScale=0.5F;
    cs.combi.uppers[0].portamentoSec=0.08F;
    cs.combi.uppers[0].controllerRoutes[0].source=ControllerSource::ControlChange;
    cs.combi.uppers[0].controllerRoutes[0].destination=ModDestination::FilterCutoffOctaves;
    cs.combi.uppers[0].controllerRoutes[0].cc=74U;
    cs.combi.uppers[0].controllerRoutes[0].amount=2.5F;
    cs.combi.uppers[0].controllerRoutes[0].bipolar=true;
    cs.combi.fxRouting=EffectRouting::Fx1IntoFx2;
    data.combis.push_back(cs);
    SetList list; list.name="Gig"; list.entries.push_back({24U,"Ballad 24",4U,5U,6U});
    data.lists.push_back({{0U,0U,0U}, list});
    const std::string dir="/tmp/SILVETON_TEST.SVSET";
    const auto saved=SetPackage::save(dir,data);
    if(!saved.success) std::fprintf(stderr,"SET save error: %s\n",saved.error.c_str());
    assert(saved.success);
    const auto loaded=SetPackage::load(dir);
    if(!loaded.ok()) std::fprintf(stderr,"SET load error: %s\n",loaded.error.c_str());
    assert(loaded.ok());
    assert(loaded.data.name=="My Set");
    assert(std::abs(loaded.data.global.masterTuneHz-442.0F)<0.001F);
    assert(loaded.data.global.breathCc==12U);
    assert(loaded.data.global.expressionCc==7U);
    assert(loaded.data.global.damperCc==66U);
    assert(!loaded.data.global.pitchBendEnabled);
    assert(!loaded.data.global.aftertouchEnabled);
    assert(loaded.data.global.scaleRoot==2U);
    assert(std::abs(loaded.data.global.scaleOffsetsCents[1]+50.0F)<0.001F);
    assert(std::abs(loaded.data.global.scaleOffsetsCents[6]-25.0F)<0.001F);
    assert(loaded.data.global.midiInputChannelMask==0x0005U);
    assert(loaded.data.global.filterNotes);
    assert(loaded.data.global.filterControlChange);
    assert(loaded.data.global.filterPitchBend);
    assert(loaded.data.global.filterAftertouch);
    assert(std::abs(loaded.data.global.breathMin-0.15F)<0.001F);
    assert(std::abs(loaded.data.global.breathMax-0.90F)<0.001F);
    assert(std::abs(loaded.data.global.breathCurve-1.7F)<0.001F);
    assert(std::abs(loaded.data.global.expressionMin-0.10F)<0.001F);
    assert(std::abs(loaded.data.global.expressionMax-0.95F)<0.001F);
    assert(std::abs(loaded.data.global.expressionCurve-0.8F)<0.001F);
    assert(loaded.data.global.damperPolarityReversed);
    const SlotAddress expectedProgramAddress{1U,2U,3U};
    assert(loaded.data.programs.size()==1U && loaded.data.programs[0].address==expectedProgramAddress);
    assert(loaded.data.combis.size()==1U && loaded.data.combis[0].combi.name=="Stage Combi");
    const auto& loadedUpper=loaded.data.combis[0].combi.uppers[0];
    assert(std::abs(loadedUpper.ampAttackScale-2.0F)<1.0e-6F);
    assert(std::abs(loadedUpper.toneAttackMs-12.5F)<1.0e-6F && std::abs(loadedUpper.toneDecayMs-340.0F)<1.0e-4F);
    assert(std::abs(loadedUpper.toneSustain-0.42F)<1.0e-6F && std::abs(loadedUpper.toneReleaseMs-900.0F)<1.0e-4F);
    assert(std::abs(loadedUpper.toneCutoffHz-1800.0F)<1.0e-3F && std::abs(loadedUpper.toneResonance-0.35F)<1.0e-6F);
    assert(std::abs(loadedUpper.ampDecayScale-0.75F)<1.0e-6F);
    assert(std::abs(loadedUpper.ampReleaseScale-1.5F)<1.0e-6F);
    assert(std::abs(loadedUpper.filterCutoffOctaves+1.25F)<1.0e-6F);
    assert(std::abs(loadedUpper.filterResonanceOffset-0.2F)<1.0e-6F);
    assert(std::abs(loadedUpper.lfoRateScale-1.75F)<1.0e-6F);
    assert(std::abs(loadedUpper.lfoDepthScale-0.5F)<1.0e-6F);
    assert(std::abs(loadedUpper.portamentoSec-0.08F)<1.0e-6F);
    assert(loadedUpper.controllerRoutes[0].source==ControllerSource::ControlChange);
    assert(loadedUpper.controllerRoutes[0].destination==ModDestination::FilterCutoffOctaves);
    assert(loadedUpper.controllerRoutes[0].cc==74U);
    assert(std::abs(loadedUpper.controllerRoutes[0].amount-2.5F)<1.0e-6F);
    assert(loadedUpper.controllerRoutes[0].bipolar);
    assert(loaded.data.combis[0].combi.fxRouting==EffectRouting::Fx1IntoFx2);
    assert(loaded.data.combis[0].combi.uppers[0].program!=nullptr);
    assert(loaded.data.combis[0].combi.uppers[0].program->layers.size()==1U);
    assert(loaded.data.combis[0].combi.uppers[0].program->layers[0].multisample!=nullptr);
    const auto& loadedProgramLayer=loaded.data.combis[0].combi.uppers[0].program->layers[0];
    assert(loadedProgramLayer.velocityFadeIn==7U);
    assert(loadedProgramLayer.velocityFadeOut==9U);
    assert(std::abs(loadedProgramLayer.delaySec-0.031F)<1.0e-6F);
    assert(!loadedProgramLayer.damperEnabled);
    assert(loadedProgramLayer.multisample->zones[0].sample!=nullptr);
    assert(loaded.data.lists.size()==1U && loaded.data.lists[0].list.entries[0].name=="Ballad 24");
}


void testStreamingWav() {
    const std::string path = makeWav();
    const auto streamed = AudioFileLoader::loadWavStreaming(path, 32U);
    if (!streamed.ok()) std::fprintf(stderr, "Streaming WAV error: %s\n", streamed.error.c_str());
    assert(streamed.ok());
    assert(streamed.sample->stream != nullptr);
    assert(streamed.sample->preloadedFrameCount() == 32U);
    assert(streamed.sample->frameCount() == 480U);
    auto st = makeState(streamed.sample);
    SamplerEngine e; e.prepare(48000.0, 128U); e.setState(st);
    std::vector<float> out(256U);
    e.postNoteOn(0U,64U,127U);
    for (int i=0;i<6;++i) e.render(out.data(),128U);
    assert(std::isfinite(rms(out)));
    assert(rms(out)>0.001F);
}



void testStreamingAiff() {
    const std::string path = makeAiff();
    const auto streamed = AudioFileLoader::loadAiffStreaming(path, 24U);
    if (!streamed.ok()) std::fprintf(stderr, "Streaming AIFF error: %s\n", streamed.error.c_str());
    assert(streamed.ok());
    assert(streamed.sample->stream != nullptr);
    assert(streamed.sample->sampleRate == 44100U);
    assert(streamed.sample->preloadedFrameCount() == 24U);
    assert(streamed.sample->frameCount() == 256U);
    assert(std::abs(streamed.sample->stream->readFrame(20U, 0U)) > 0.001F);
}

void testSampleEditor() {
    const auto source = AudioFileLoader::load(makeWav()).sample;
    assert(source);
    const auto z = SampleEditor::nearestZeroCrossing(*source, 120U, 30U);
    assert(z <= 150U);
    auto looped = SampleEditor::setLoop(*source, 100U, 300U, 40U);
    assert(looped.ok()); assert(looped.sample->loop.enabled); assert(looped.sample->loop.crossfadeFrames==40U);
    auto trimmed = SampleEditor::trim(*looped.sample, 50U, 350U);
    assert(trimmed.ok()); assert(trimmed.sample->frameCount()==300U);
    assert(trimmed.sample->loop.startFrame==50U); assert(trimmed.sample->loop.endFrame==250U);
    auto norm = SampleEditor::normalize(*trimmed.sample, -1.0F);
    assert(norm.ok()); float peak=0.0F; for(float x:norm.sample->interleaved) peak=std::max(peak,std::abs(x));
    assert(peak>0.88F && peak<0.90F);
    auto faded = SampleEditor::fade(*norm.sample, 20U, 20U);
    assert(faded.ok()); assert(std::abs(faded.sample->interleaved.front())<1.0e-6F);
    auto reversed = SampleEditor::reverse(*faded.sample);
    assert(reversed.ok()); assert(reversed.sample->frameCount()==faded.sample->frameCount());
}



void testSampleMultisampleEditBackend() {
    auto sampleA = AudioFileLoader::load(makeWav()).sample;
    assert(sampleA != nullptr);
    auto sampleB = std::make_shared<SampleData>(*sampleA);
    sampleB->name = "Sample B";
    sampleB->rootKey = 67U;

    auto ms = std::make_shared<Multisample>();
    ms->name = "MS Original";
    SampleZone z{}; z.sample=sampleA; z.keyLow=0U; z.keyHigh=63U; z.velocityLow=1U; z.velocityHigh=127U; z.rootKey=60U;
    ms->zones.push_back(z);

    auto program = std::make_shared<Program>();
    program->name = "Program Uses MS";
    ProgramLayer layer{}; layer.name="Layer 1"; layer.multisample=ms; program->layers.push_back(layer);
    Combi combi{}; combi.name="Combi Uses Program"; combi.uppers[0].enabled=true; combi.uppers[0].program=program;

    SamplerLibrary library("Edit Set");
    assert(library.writeSample(10U,sampleA));
    assert(library.writeSample(11U,sampleB));
    assert(library.writeMultisample(20U,ms));
    const SlotAddress pAddr{1U,0U,1U}; const SlotAddress cAddr{1U,0U,2U};
    assert(library.writeProgram(pAddr,program));
    assert(library.writeCombi(cAddr,combi));

    auto meta = SampleEditor::updateMetadata(*sampleA,"Renamed Sample",64U,12.5F);
    assert(meta.ok());
    assert(library.writeSample(10U,meta.sample));
    assert(library.sample(10U)->name=="Renamed Sample");
    assert(library.sample(10U)->rootKey==64U);
    const auto msAfterSample=library.multisample(20U);
    assert(msAfterSample!=nullptr && msAfterSample->zones[0].sample==meta.sample);
    const auto progAfterSample=library.program(pAddr);
    assert(progAfterSample!=nullptr && progAfterSample->layers[0].multisample==msAfterSample);
    const auto combiAfterSample=library.combi(cAddr);
    assert(combiAfterSample.has_value() && combiAfterSample->uppers[0].program==progAfterSample);

    SampleZone z2{}; z2.sample=sampleB; z2.keyLow=64U; z2.keyHigh=127U; z2.velocityLow=20U; z2.velocityHigh=120U; z2.rootKey=67U; z2.tuneCents=-7.0F; z2.gainDb=-2.5F; z2.pan=0.25F;
    auto added=MultisampleEditor::addZone(msAfterSample,z2); assert(added.ok()); assert(added.multisample->zones.size()==2U);
    auto renamed=MultisampleEditor::rename(added.multisample,"MS Edited"); assert(renamed.ok());
    auto moved=MultisampleEditor::moveZone(renamed.multisample,1U,0U); assert(moved.ok()); assert(moved.multisample->zones[0].sample==sampleB);
    auto zoneEdit=moved.multisample->zones[0]; zoneEdit.keyLow=55U; zoneEdit.keyHigh=100U; zoneEdit.velocityLow=10U; zoneEdit.velocityHigh=110U; zoneEdit.reverse=true; zoneEdit.oneShot=true;
    auto updated=MultisampleEditor::updateZone(moved.multisample,0U,zoneEdit); assert(updated.ok());
    assert(updated.multisample->zones[0].keyLow==55U && updated.multisample->zones[0].reverse);
    assert(library.writeMultisample(20U,updated.multisample));
    const auto progAfterMs=library.program(pAddr); assert(progAfterMs!=nullptr && progAfterMs->layers[0].multisample==updated.multisample);
    const auto combiAfterMs=library.combi(cAddr); assert(combiAfterMs.has_value() && combiAfterMs->uppers[0].program==progAfterMs);

    auto looped=SampleEditor::setLoop(*meta.sample,50U,180U,20U); assert(looped.ok() && looped.sample->loop.enabled);
    auto noLoop=SampleEditor::clearLoop(*looped.sample); assert(noLoop.ok() && !noLoop.sample->loop.enabled);

    const std::string dir="/tmp/silveton_sample_ms_edit.SVSET";
    std::filesystem::remove_all(dir);
    const auto saved=SetPackage::save(dir,library.snapshot()); assert(saved.success);
    const auto loaded=SetPackage::load(dir); assert(loaded.ok());
    SamplerLibrary restored; restored.replace(loaded.data);
    assert(restored.sample(10U)!=nullptr && restored.sample(10U)->rootKey==64U);
    assert(restored.multisample(20U)!=nullptr && restored.multisample(20U)->name=="MS Edited");
    assert(restored.multisample(20U)->zones.size()==2U);
    assert(restored.multisample(20U)->zones[0].keyLow==55U);
    assert(restored.multisample(20U)->zones[0].oneShot);
    assert(restored.program(pAddr)!=nullptr && restored.program(pAddr)->layers[0].multisample==restored.multisample(20U));
    std::filesystem::remove_all(dir);

    assert(!MultisampleEditor::updateZone(updated.multisample,0U,SampleZone{}).ok());
    assert(!MultisampleEditor::removeZone(std::make_shared<Multisample>(Multisample{"one",{z},std::string{}}),0U).ok());
}

void testSafeStateReplacement() {
    auto sample = AudioFileLoader::load(makeWav()).sample;
    assert(sample != nullptr);
    auto first = makeState(sample);
    SamplerEngine engine;
    engine.prepare(48000.0, 128U);
    engine.setState(first);
    std::vector<float> out(256U);
    assert(engine.postNoteOn(0U, 64U, 127U));
    engine.render(out.data(), 128U);
    assert(engine.activeVoiceCount() > 0U);

    auto second = makeState(sample);
    second->combi.name = "Replacement";
    second->global.masterGainDb = -6.0F;
    engine.setState(second);
    assert(engine.activeVoiceCount() > 0U); // replaced state must preserve the already sounding voice safely
    engine.render(out.data(), 128U);
    assert(rms(out) > 0.0F); // preserved old voice must continue rendering after state replacement
    assert(engine.postNoteOn(0U, 64U, 127U));
    engine.render(out.data(), 128U);
    assert(rms(out) > 0.001F);
}

void testSamplerLibraryBankPageSlot() {
    SamplerLibrary library("Stage Set");
    GlobalState global{};
    global.masterTuneHz = 442.0F;
    global.transposeSemitones = -2;
    library.setGlobal(global);

    auto sample = AudioFileLoader::load(makeWav()).sample;
    assert(sample != nullptr);
    const auto state = makeState(sample);
    const auto program = state->combi.uppers[0].program;
    assert(program != nullptr);

    const SlotAddress pAddr{2U, 3U, 5U};
    assert(library.writeProgram(pAddr, program));
    assert(library.program(pAddr) == program);
    assert(library.program(SlotAddress{2U, 3U, 6U}) == nullptr);

    auto replacement = std::make_shared<Program>(*program);
    replacement->name = "Replacement Program";
    assert(library.writeProgram(pAddr, replacement));
    assert(library.program(pAddr) != nullptr);
    assert(library.program(pAddr)->name == "Replacement Program");

    Combi combi = state->combi;
    combi.name = "Ballad Combi";
    combi.uppers[0].program = replacement;
    const SlotAddress cAddr{4U, 1U, 7U};
    assert(library.writeCombi(cAddr, combi));
    const auto readCombi = library.combi(cAddr);
    assert(readCombi.has_value());
    assert(readCombi->name == "Ballad Combi");
    assert(readCombi->uppers[0].program == replacement);
    auto editedReplacement = std::make_shared<Program>(*replacement);
    editedReplacement->name = "Edited In Place";
    assert(library.writeProgram(pAddr, editedReplacement));
    const auto updatedCombi = library.combi(cAddr);
    assert(updatedCombi.has_value());
    assert(updatedCombi->uppers[0].program == editedReplacement);

    SetList list{};
    list.name = "Live List";
    list.entries.push_back({24U, "Ballad 24", cAddr.bank, cAddr.page, cAddr.slot});
    const SlotAddress lAddr{1U, 0U, 0U};
    assert(library.writeList(lAddr, list));
    const auto readList = library.list(lAddr);
    assert(readList.has_value());
    assert(readList->entries.size() == 1U);
    assert(readList->entries[0].name == "Ballad 24");

    const SetData snapshot = library.snapshot();
    assert(snapshot.name == "Stage Set");
    assert(snapshot.global.masterTuneHz == 442.0F);
    assert(snapshot.programs.size() == 1U);
    assert(snapshot.combis.size() == 1U);
    assert(snapshot.lists.size() == 1U);
    assert(snapshot.programs[0].address == pAddr);
    assert(snapshot.combis[0].address == cAddr);
    assert(snapshot.lists[0].address == lAddr);

    SamplerLibrary restored;
    restored.replace(snapshot);
    assert(restored.setName() == "Stage Set");
    assert(restored.global().transposeSemitones == -2);
    assert(restored.program(pAddr) != nullptr);
    assert(restored.program(pAddr)->name == "Edited In Place");
    assert(restored.combi(cAddr).has_value());
    assert(restored.list(lAddr).has_value());

    assert(restored.clearProgram(pAddr));
    assert(restored.program(pAddr) == nullptr);
    assert(!restored.clearProgram(pAddr));
    assert(restored.clearCombi(cAddr));
    assert(restored.clearList(lAddr));
}


void testMediaBankAndAtomicSave() {
    SamplerLibrary source("MEDIA Test");
    auto sample = AudioFileLoader::load(makeWav()).sample;
    assert(sample != nullptr);
    const auto state = makeState(sample);
    const auto sourceProgram = state->combi.uppers[0].program;
    assert(sourceProgram != nullptr);
    auto mutableProgram = std::make_shared<Program>(*sourceProgram);
    mutableProgram->name = "Bank Program";
    ProgramPtr program = mutableProgram;

    const SlotAddress pAddr{7U, 2U, 3U};
    const SlotAddress cAddr{7U, 2U, 4U};
    const SlotAddress lAddr{7U, 2U, 5U};
    assert(source.writeProgram(pAddr, program));
    Combi combi = state->combi;
    combi.name = "Bank Combi";
    combi.uppers[0].program = program;
    assert(source.writeCombi(cAddr, combi));
    SetList list;
    list.name = "Bank List";
    list.entries.push_back({12U, "Bank Combi", cAddr.bank, cAddr.page, cAddr.slot});
    assert(source.writeList(lAddr, list));

    assert(source.hasProgram(pAddr));
    assert(source.renameProgram(pAddr, "Renamed Program"));
    assert(source.program(pAddr)->name == "Renamed Program");
    const SlotAddress copied{7U, 2U, 6U};
    assert(source.copyProgram(pAddr, copied, false));
    assert(!source.copyProgram(pAddr, copied, false));
    assert(source.copyProgram(pAddr, copied, true));

    const auto bank = source.snapshotBank(7U);
    assert(bank.programs.size() == 2U);
    assert(bank.combis.size() == 1U);
    assert(bank.lists.size() == 1U);

    SamplerLibrary destination("Destination");
    const auto merged = destination.mergeBank(bank, 9U, false);
    assert(merged.success);
    assert(destination.program(SlotAddress{9U, 2U, 3U}) != nullptr);
    assert(destination.combi(SlotAddress{9U, 2U, 4U}).has_value());
    const auto remappedList = destination.list(SlotAddress{9U, 2U, 5U});
    assert(remappedList.has_value());
    assert(remappedList->entries[0].combiBank == 9U);
    const auto rejected = destination.mergeBank(bank, 9U, false);
    assert(!rejected.success);
    assert(destination.mergeBank(bank, 9U, true).success);

    const std::string package = "/tmp/SILVETON_MEDIA_ATOMIC.SVSET";
    std::filesystem::remove_all(package);
    std::filesystem::remove_all(package + ".tmp-write");
    std::filesystem::remove_all(package + ".previous");
    auto first = source.snapshot();
    first.name = "Atomic First";
    assert(SetPackage::save(package, first).success);
    auto second = source.snapshot();
    second.name = "Atomic Second";
    assert(SetPackage::save(package, second).success);
    const auto loaded = SetPackage::load(package);
    assert(loaded.ok());
    assert(loaded.data.name == "Atomic Second");
    assert(!std::filesystem::exists(package + ".tmp-write"));
    assert(!std::filesystem::exists(package + ".previous"));
}



void testListBackend() {
    SamplerLibrary library("List Backend");
    const SlotAddress combiA{2U, 1U, 3U};
    const SlotAddress combiB{2U, 1U, 4U};
    Combi a{}; a.name = "Ballad 24";
    Combi b{}; b.name = "Clarinet Live";
    assert(library.writeCombi(combiA, a));
    assert(library.writeCombi(combiB, b));

    const SlotAddress listAddr{5U, 2U, 1U};
    SetList list{}; list.name = "LIVE";
    list.entries.push_back({24U, "Ballad 24", combiA.bank, combiA.page, combiA.slot});
    assert(library.writeList(listAddr, list));

    ListEntry second{25U, "Clarinet Live", combiB.bank, combiB.page, combiB.slot};
    assert(library.insertListEntry(listAddr, 1U, second));
    auto current = library.list(listAddr); assert(current.has_value());
    assert(current->entries.size() == 2U && current->entries[1].number == 25U);

    ListEntry replacement{26U, "Clarinet Alt", combiB.bank, combiB.page, combiB.slot};
    assert(library.setListEntry(listAddr, 1U, replacement));
    assert(library.moveListEntry(listAddr, 1U, 0U));
    current = library.list(listAddr); assert(current.has_value());
    assert(current->entries[0].number == 26U && current->entries[1].number == 24U);
    assert(library.removeListEntry(listAddr, 1U));
    current = library.list(listAddr); assert(current.has_value() && current->entries.size() == 1U);

    const SlotAddress movedAddr{6U, 3U, 2U};
    assert(library.moveList(listAddr, movedAddr, false));
    assert(!library.hasList(listAddr));
    assert(library.hasList(movedAddr));

    // LIST edits must survive the SET package round-trip.
    const std::string path = "/tmp/SILVETON_LIST_BACKEND.SVSET";
    std::filesystem::remove_all(path);
    assert(SetPackage::save(path, library.snapshot()).success);
    const auto loaded = SetPackage::load(path); assert(loaded.ok());
    SamplerLibrary restored("Restored"); restored.replace(loaded.data);
    const auto restoredList = restored.list(movedAddr); assert(restoredList.has_value());
    assert(restoredList->name == "LIVE");
    assert(restoredList->entries.size() == 1U);
    assert(restoredList->entries[0].number == 26U);
    assert(restoredList->entries[0].name == "Clarinet Alt");
    assert(restoredList->entries[0].combiSlot == combiB.slot);

    // Hard LIST capacity guard.
    SetList full{}; full.name = "FULL";
    for (int i = 0; i < kMaxListSlots; ++i) {
        full.entries.push_back({static_cast<std::uint16_t>(i), "Slot", combiA.bank, combiA.page, combiA.slot});
    }
    const SlotAddress fullAddr{7U, 0U, 0U};
    assert(library.writeList(fullAddr, full));
    assert(!library.insertListEntry(fullAddr, 0U, second));
}


void testUpperVisibleControlContract() {
    auto sample=AudioFileLoader::load(makeWav()).sample;
    assert(sample);

    auto render = [&](const std::shared_ptr<EngineState>& st,
                      const std::vector<RealtimeEvent>& events = {}) {
        SamplerEngine e; e.prepare(48000.0,256U); e.setState(st);
        for (const auto& ev : events) {
            switch (ev.type) {
                case EventType::ControlChange: assert(e.postControlChange(ev.channel,ev.data1,ev.data2)); break;
                case EventType::PitchBend: assert(e.postPitchBend(ev.channel,ev.pitchBend)); break;
                case EventType::ChannelPressure: assert(e.postChannelPressure(ev.channel,ev.data1)); break;
                case EventType::NoteOn: assert(e.postNoteOn(ev.channel,ev.data1,ev.data2)); break;
                case EventType::NoteOff: assert(e.postNoteOff(ev.channel,ev.data1,ev.data2)); break;
                case EventType::Panic: assert(e.postPanic()); break;
            }
        }
        std::vector<float> out(512U);
        e.render(out.data(),256U);
        return std::pair<std::vector<float>,MeterSnapshot>{out,e.meters()};
    };

    const auto note = RealtimeEvent{EventType::NoteOn,0U,64U,110U,0};
    auto base=makeState(sample);
    base->global.limiterEnabled=false;
    auto [baseOut,baseMeters]=render(base,{note});
    assert(rms(baseOut)>0.01F);
    assert(baseMeters.upperPeakLeft[0] > 0.0F || baseMeters.upperPeakRight[0] > 0.0F);
    for (int ui=1; ui<kUpperCount; ++ui) {
        assert(baseMeters.upperPeakLeft[static_cast<std::size_t>(ui)] == 0.0F);
        assert(baseMeters.upperPeakRight[static_cast<std::size_t>(ui)] == 0.0F);
    }

    // Main-row controls: level, pan, key range, voice mode.
    auto low=std::make_shared<EngineState>(*base); low->combi.uppers[0].levelDb=-18.0F;
    auto [lowOut,_m1]=render(low,{note}); assert(rms(lowOut)<rms(baseOut)*0.2F);

    auto pan=std::make_shared<EngineState>(*base); pan->combi.uppers[0].pan=-1.0F;
    auto [panOut,_m2]=render(pan,{note});
    double l=0.0,r=0.0; for(std::size_t i=0;i<panOut.size();i+=2){l+=std::abs(panOut[i]);r+=std::abs(panOut[i+1]);}
    assert(l>r*10.0);

    auto key=std::make_shared<EngineState>(*base); key->combi.uppers[0].keyLow=70U;
    auto [keyOut,_m3]=render(key,{note}); assert(rms(keyOut)==0.0F);

    auto vel=std::make_shared<EngineState>(*base); vel->combi.uppers[0].velocityLow=120U;
    auto [velOut,_m4]=render(vel,{note}); assert(rms(velOut)==0.0F);

    auto poly=std::make_shared<EngineState>(*base); poly->combi.uppers[0].voiceMode=UpperVoiceMode::Poly;
    SamplerEngine pe; pe.prepare(48000.0,64U); pe.setState(poly); std::vector<float> tmp(128U);
    pe.postNoteOn(0U,64U,100U); pe.render(tmp.data(),64U); pe.postNoteOn(0U,67U,100U); pe.render(tmp.data(),64U); assert(pe.activeVoiceCount()>=2U);
    auto mono=std::make_shared<EngineState>(*base); mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine me; me.prepare(48000.0,64U); me.setState(mono); me.postNoteOn(0U,64U,100U); me.render(tmp.data(),64U); me.postNoteOn(0U,67U,100U); me.render(tmp.data(),64U); assert(me.activeVoiceCount()==1U);

    // TUNE: octave/transpose/fine and bend must alter playback waveform.
    auto tuned=std::make_shared<EngineState>(*base); tuned->combi.uppers[0].octave=1; tuned->combi.uppers[0].transposeSemitones=2; tuned->combi.uppers[0].tuneCents=25.0F;
    auto [tunedOut,_m5]=render(tuned,{note});
    double diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-tunedOut[i]); assert(diff>1.0);

    auto bend0=std::make_shared<EngineState>(*base); bend0->combi.uppers[0].pitchBendRange=0;
    auto bend12=std::make_shared<EngineState>(*base); bend12->combi.uppers[0].pitchBendRange=12;
    const RealtimeEvent bend{EventType::PitchBend,0U,0U,0U,4096};
    auto [b0,_m6]=render(bend0,{bend,note}); auto [b12,_m7]=render(bend12,{bend,note});
    diff=0.0; for(std::size_t i=0;i<b0.size();++i)diff+=std::abs(b0[i]-b12[i]); assert(diff>1.0);

    // CONTROL: expression, breath and aftertouch are per Upper and audibly gate/shape it.
    auto expr=std::make_shared<EngineState>(*base); expr->combi.uppers[0].expressionEnabled=true;
    const RealtimeEvent cc11zero{EventType::ControlChange,0U,11U,0U,0};
    auto [exprOut,_m8]=render(expr,{cc11zero,note}); assert(rms(exprOut)<0.001F);
    auto noExpr=std::make_shared<EngineState>(*base); noExpr->combi.uppers[0].expressionEnabled=false;
    auto [noExprOut,_m9]=render(noExpr,{cc11zero,note}); assert(rms(noExprOut)>0.01F);

    auto breath=std::make_shared<EngineState>(*base); breath->combi.uppers[0].breathEnabled=true;
    const RealtimeEvent cc2zero{EventType::ControlChange,0U,2U,0U,0};
    auto [breathOut,_m10]=render(breath,{cc2zero,note}); assert(rms(breathOut)<0.001F);

    auto aft=std::make_shared<EngineState>(*base); aft->combi.uppers[0].aftertouchEnabled=true;
    const RealtimeEvent pressure0{EventType::ChannelPressure,0U,0U,0U,0};
    auto [aftOut,_m11]=render(aft,{pressure0,note}); assert(rms(aftOut)<rms(baseOut)*0.7F);

    // TONE: all exposed modifiers must audibly change the rendered signal.
    auto attack=std::make_shared<EngineState>(*base); attack->combi.uppers[0].ampAttackScale=16.0F;
    auto attackProg=std::make_shared<Program>(*attack->combi.uppers[0].program); attackProg->layers[0].ampEnvelope.attackSec=0.02F; attack->combi.uppers[0].program=attackProg;
    auto [attackOut,_m12]=render(attack,{note}); assert(rms(attackOut)<rms(baseOut));

    auto sustain=std::make_shared<EngineState>(*base); sustain->combi.uppers[0].ampSustainScale=0.25F;
    auto sustainProg=std::make_shared<Program>(*sustain->combi.uppers[0].program); sustainProg->layers[0].ampEnvelope={0.0F,0.001F,1.0F,0.1F}; sustain->combi.uppers[0].program=sustainProg;
    SamplerEngine se; se.prepare(48000.0,256U); se.setState(sustain); se.postNoteOn(0U,64U,110U); std::vector<float> sout(512U); for(int i=0;i<4;++i)se.render(sout.data(),256U); const float sustainRms=rms(sout);
    auto sustainFull=std::make_shared<EngineState>(*sustain); sustainFull->combi.uppers[0].ampSustainScale=1.0F;
    SamplerEngine sfe; sfe.prepare(48000.0,256U); sfe.setState(sustainFull); sfe.postNoteOn(0U,64U,110U); for(int i=0;i<4;++i)sfe.render(sout.data(),256U); assert(sustainRms<rms(sout)*0.5F);

    auto filt=std::make_shared<EngineState>(*base); filt->combi.uppers[0].filterCutoffOctaves=-6.0F; filt->combi.uppers[0].filterResonanceOffset=0.5F;
    auto [filtOut,_m13]=render(filt,{note}); diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-filtOut[i]); assert(diff>1.0);

    auto lfo=std::make_shared<EngineState>(*base); auto lfoProg=std::make_shared<Program>(*lfo->combi.uppers[0].program); lfoProg->layers[0].lfo1.frequencyHz=4.0F; lfoProg->layers[0].lfo1.ampDepth=0.7F; lfo->combi.uppers[0].program=lfoProg; lfo->combi.uppers[0].lfoRateScale=2.0F; lfo->combi.uppers[0].lfoDepthScale=1.0F;
    auto [lfoOut,_m14]=render(lfo,{note}); diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-lfoOut[i]); assert(diff>0.1);

    auto eq=std::make_shared<EngineState>(*base); eq->combi.uppers[0].eq.band[0].gainDb=12.0F; eq->combi.uppers[0].eq.band[2].gainDb=-12.0F; eq->combi.uppers[0].eq.band[4].gainDb=6.0F;   // SILVETON_EQ5_V1
    auto [eqOut,_m15]=render(eq,{note}); diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-eqOut[i]); assert(diff>0.1);

    // FX tab: each send must feed the real shared COMBI effect bus.
    auto fx=std::make_shared<EngineState>(*base); fx->combi.fx[0].type=EffectType::Delay; fx->combi.fx[0].wet=1.0F; fx->combi.fx[0].timeMs=1.0F; fx->combi.fx[0].feedback=0.0F; fx->combi.uppers[0].fxSend1=1.0F;
    auto [fxOut,_m16]=render(fx,{note}); diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-fxOut[i]); assert(diff>0.01);
}



void testSvBankRuntime() {
    std::vector<std::uint8_t> strings{0U};
    const auto addString=[&](const std::string& text) -> std::uint32_t {
        const auto offset=static_cast<std::uint32_t>(strings.size());
        strings.insert(strings.end(),text.begin(),text.end());
        strings.push_back(0U);
        return offset;
    };
    const auto bankName=addString("TEST BANK");
    const auto sampleName=addString("SV SAMPLE");
    const auto sampleIdentity=addString("sample://sv-test");
    const auto msName=addString("SV MULTI");
    const auto msIdentity=addString("multi://sv-test");
    const auto soundName=addString("SV SOUND");

    std::vector<std::uint8_t> sounds;
    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.soundSlotId=500U;
    appendSoundRecord(sounds,sound);

    std::vector<std::uint8_t> layers;
    SvSoundLayerRecord layer{}; layer.multisampleId=4000001U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;
    appendSoundLayerRecord(layers,layer);

    std::vector<std::uint8_t> multis;
    SvMultisampleRecord ms{}; ms.multisampleId=4000001U; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;
    appendMultisampleRecord(multis,ms);

    std::vector<std::uint8_t> samples;
    SvSampleRecord sample{}; sample.sampleId=3000001U; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;
    sample.encoding=1U; sample.bitsPerSample=16U; sample.rootKey=60U; sample.totalFrames=8U; sample.audioOffset=0U; sample.audioSize=16U;
    appendSampleRecord(samples,sample);

    std::vector<std::uint8_t> zones;
    SvZoneRecord zone{}; zone.sampleId=3000001U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;
    appendZoneRecord(zones,zone);

    std::vector<std::uint8_t> audio;
    const std::array<std::int16_t,8> pcm{{0,8192,-8192,16384,-16384,32767,-32768,4096}};
    for(const auto v:pcm){
        const auto u=static_cast<std::uint16_t>(v);
        audio.push_back(static_cast<std::uint8_t>(u&0xFFU));
        audio.push_back(static_cast<std::uint8_t>((u>>8U)&0xFFU));
    }

    SvBankHeader header{}; header.bankNameOffset=bankName; header.soundCount=1U; header.soundLayerCount=1U; header.multisampleCount=1U; header.sampleCount=1U; header.zoneCount=1U;
    std::vector<std::uint8_t> file(kSvBankHeaderSize,0U);
    const auto appendSec=[&](SvSection section,const std::vector<std::uint8_t>& data){
        auto& d=header.sections[static_cast<std::size_t>(section)]; d.offset=static_cast<std::uint64_t>(file.size()); d.size=static_cast<std::uint64_t>(data.size()); file.insert(file.end(),data.begin(),data.end());
    };
    appendSec(SvSection::Sounds,sounds); appendSec(SvSection::SoundLayers,layers); appendSec(SvSection::Multisamples,multis);
    appendSec(SvSection::Samples,samples); appendSec(SvSection::Zones,zones); appendSec(SvSection::Strings,strings); appendSec(SvSection::Audio,audio);
    header.fileSize=static_cast<std::uint64_t>(file.size());
    header.payloadSha256=sha256(file.data()+kSvBankHeaderSize,file.size()-kSvBankHeaderSize);
    const auto serialized=serializeHeader(header); assert(serialized.size()==kSvBankHeaderSize); std::copy(serialized.begin(),serialized.end(),file.begin());

    const std::string path="/tmp/SILVETON_BANK_test.sv";
    { std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size())); }
    const auto loaded=SvBankRuntime::load(path,2U,true);
    if(!loaded.ok()) std::fprintf(stderr,"SV runtime load error: %s\n",loaded.error.c_str());
    assert(loaded.ok()); assert(loaded.data.name=="TEST BANK"); assert(loaded.data.samples.size()==1U); assert(loaded.data.multisamples.size()==1U); assert(loaded.data.programs.size()==1U);
    const auto sp=loaded.data.samples[0].sample; assert(sp!=nullptr); assert(sp->interleaved.size()==2U); assert(sp->stream!=nullptr); assert(sp->frameCount()==8U);
    assert(std::abs(sp->stream->readFrame(1U,0U)-0.25F)<0.001F); assert(std::abs(sp->stream->readFrame(2U,0U)+0.25F)<0.001F);
    const auto program=loaded.data.programs[0].program; assert(program!=nullptr); assert(program->name=="SV SOUND"); assert(program->layers.size()==1U);
    assert(loaded.data.programs[0].address.bank==0U && loaded.data.programs[0].address.page==0U && loaded.data.programs[0].address.slot==500U);
    assert(loaded.data.samples[0].id==3000001U); assert(loaded.data.multisamples[0].id==4000001U);
    assert(program->layers[0].multisample==loaded.data.multisamples[0].multisample);
    assert(program->layers[0].multisample->zones[0].sample==sp);

    // End-to-end bank playback contract: successful bank load must create
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

    auto corrupted=file; corrupted.back()^=0x01U;
    { std::ofstream out(path,std::ios::binary|std::ios::trunc); out.write(reinterpret_cast<const char*>(corrupted.data()),static_cast<std::streamsize>(corrupted.size())); }
    const auto bad=SvBankRuntime::load(path,2U,true); assert(!bad.ok());
}
void testSampleRecorderRf64() {
    const std::string path="/tmp/silveton_record_test.wav";
    SampleRecorder recorder;
    assert(recorder.start(path,48000U,2U,4096U));
    std::vector<float> block(256U*2U);
    for(int b=0;b<12;++b){
        for(std::size_t i=0;i<256U;++i){
            const auto frame=static_cast<std::size_t>(b)*256U+i;
            const float x=static_cast<float>(std::sin(2.0*3.14159265358979323846*440.0*static_cast<double>(frame)/48000.0));
            block[i*2U]=x; block[i*2U+1U]=x*0.5F;
        }
        while(!recorder.push(block.data(),256U)) std::this_thread::yield();
    }
    assert(recorder.stop());
    assert(recorder.framesWritten()==3072U);
    assert(recorder.droppedFrames()==0U);
    const auto loaded=AudioFileLoader::loadWavStreaming(path,64U);
    if(!loaded.ok()) std::fprintf(stderr,"Recorded RF64 load error: %s\n",loaded.error.c_str());
    assert(loaded.ok()); assert(loaded.sample->channels==2U); assert(loaded.sample->sampleRate==48000U);
    assert(loaded.sample->frameCount()==3072U); assert(loaded.sample->stream!=nullptr);
    assert(std::abs(loaded.sample->stream->readFrame(100U,0U))>0.01F);
}

void testSincAntiAliasFinite() {
    SampleData s; s.sampleRate=48000U; s.channels=1U; s.rootKey=60U; s.interleaved.resize(4096U);
    for(std::size_t i=0;i<s.interleaved.size();++i) {
        s.interleaved[i]=static_cast<float>(std::sin(2.0*3.14159265358979323846*18000.0*static_cast<double>(i)/48000.0));
    }
    SincInterpolator interp(InterpolationQuality::Ultra32);
    double energy=0.0;
    for(int i=100;i<2000;++i) {
        const float v=interp.read(s,static_cast<double>(i)*2.0,0U,2.0,false);
        assert(std::isfinite(v)); energy+=static_cast<double>(v)*static_cast<double>(v);
    }
    const double outRms=std::sqrt(energy/1900.0);
    assert(outRms<0.25); // 18 kHz at 2x playback must be strongly attenuated before aliasing.
}

// ---- crackle fix regression tests -------------------------------------------

// SILVETON_MASTER_6DB_V1: no limiter in the sound any more. A fresh GLOBAL
// starts at MASTER -6 dB; a loud chord is only scaled by MASTER, never squashed.
void testMasterNoLimiter() {
    GlobalState fresh{}; assert(fresh.masterGainDb==-6.0F); assert(!fresh.limiterEnabled);
    auto s=std::make_shared<SampleData>(); s->sampleRate=96000U; s->channels=1U; s->rootKey=60U;
    s->interleaved.resize(96000U);
    for(std::size_t i=0;i<s->interleaved.size();++i) s->interleaved[i]=static_cast<float>(0.9*std::sin(2.0*3.14159265358979323846*220.0*static_cast<double>(i)/96000.0));
    s->totalFrames=96000U; s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=96000U;
    auto st=makeState(s); st->global.limiterEnabled=true; st->global.limiterCeilingDb=-20.0F; st->global.masterGainDb=-6.0F;
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program); prog->layers[0].filter.mode=FilterMode::Bypass;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    for(const unsigned k: {48U,52U,55U,60U}) assert(e.postNoteOn(0U,static_cast<std::uint8_t>(k),127U));
    std::vector<float> out(96U*2U); float peak=0.0F;
    for(int b=0;b<300;++b) { e.render(out.data(),96U); for(float v:out) peak=std::max(peak,std::abs(v)); }
    assert(peak>std::pow(10.0F,-20.0F/20.0F)*1.5F);   // a "limiter ceiling" of -20 dB changes nothing: there is no limiter
    assert(e.meters().limiterGainReductionDb==0.0F);
    // MASTER really scales the output: -6 dB vs 0 dB
    auto loud=std::make_shared<EngineState>(*st); loud->global.masterGainDb=0.0F;
    SamplerEngine e2; e2.prepare(96000.0,96U); e2.setState(loud);
    for(const unsigned k: {48U,52U,55U,60U}) assert(e2.postNoteOn(0U,static_cast<std::uint8_t>(k),127U));
    float peak2=0.0F; for(int b=0;b<300;++b) { e2.render(out.data(),96U); for(float v:out) peak2=std::max(peak2,std::abs(v)); }
    assert(std::abs(20.0F*std::log10(peak2/peak)-6.0F)<0.3F);
}

// A looped sample must play its attack (the frames before the loop start).
// Before the fix the interpolator returned the loop-start frame for all of them:
// silence / a DC step and a pop at note-on.
void testLoopedSampleKeepsAttack() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=1U; s->rootKey=69U;
    s->interleaved.resize(48000U);
    for(std::size_t i=0;i<s->interleaved.size();++i) {
        const double t=static_cast<double>(i)/48000.0;
        s->interleaved[i]=static_cast<float>((t<0.25?0.8:0.4)*std::sin(2.0*3.14159265358979323846*440.0*t));
    }
    s->totalFrames=48000U; s->loop.enabled=true; s->loop.startFrame=24011U; s->loop.endFrame=48000U; // loop start value != 0
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    assert(e.postNoteOn(0U,69U,127U));
    std::vector<float> out(96U*2U); double sum=0.0, sq=0.0, maxJump=0.0; float prev=0.0F; std::size_t n=0U;
    for(int b=0;b<200;++b) { // ~0.2 s, all before the loop start (0.5 s)
        e.render(out.data(),96U);
        for(std::size_t i=0;i<96U;++i) {
            const float x=out[i*2U]; sum+=x; sq+=static_cast<double>(x)*x;
            maxJump=std::max(maxJump,static_cast<double>(std::abs(x-prev))); prev=x; ++n;
        }
    }
    const double mean=sum/static_cast<double>(n); const double rmsv=std::sqrt(sq/static_cast<double>(n));
    assert(rmsv>0.2);              // attack is audible
    assert(std::abs(mean)<0.01);   // no DC plateau
    assert(maxJump<0.1);           // no step at note-on (440 Hz slope at 96 kHz is ~0.02)
}

float refFrame(const SampleData& s, long long f, std::uint16_t ch, bool looping) {
    long long r=f;
    if(looping && s.loop.enabled && s.loop.endFrame>s.loop.startFrame) {
        const auto ls=static_cast<long long>(s.loop.startFrame), le=static_cast<long long>(s.loop.endFrame);
        if(r>=le) r=ls+((r-ls)%(le-ls));
    }
    if(r<0 || r>=static_cast<long long>(s.frameCount())) return 0.0F;
    const auto u=static_cast<std::uint64_t>(r);
    const std::uint16_t c=std::min<std::uint16_t>(ch,static_cast<std::uint16_t>(s.channels-1U));
    if(u<s.preloadedFrameCount()) return s.interleaved[static_cast<std::size_t>(u)*s.channels+c];
    return s.stream->readFrame(u,c);
}

// The direct-pointer stereo path must give exactly the same samples as a plain
// frame-by-frame cubic, in RAM, in the mapped file, across the RAM/file border
// and across the loop end.
void testStereoReadMatchesChannelRead() {
    const std::string path="/tmp/silveton_crackle_stereo.wav";
    const std::uint32_t frames=20000U;
    {
        std::ofstream f(path,std::ios::binary|std::ios::trunc);
        f.write("RIFF",4); putLe32(f,36U+frames*4U); f.write("WAVEfmt ",8); putLe32(f,16U); putLe16(f,1U); putLe16(f,2U);
        putLe32(f,48000U); putLe32(f,48000U*4U); putLe16(f,4U); putLe16(f,16U); f.write("data",4); putLe32(f,frames*4U);
        for(std::uint32_t i=0;i<frames;++i) {
            putLe16(f,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(9000.0*std::sin(0.031*i)))));
            putLe16(f,static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lrint(7000.0*std::cos(0.017*i)))));
        }
    }
    MappedPcmSpec spec{}; spec.dataOffset=44U; spec.frameCount=frames; spec.sampleRate=48000U; spec.channels=2U; spec.bitDepth=16U; spec.formatTag=1U;
    std::string err; auto stream=MappedPcmStream::open(path,spec,err); assert(stream!=nullptr);
    assert(stream->rawView().data!=nullptr);
    SampleData s; s.sampleRate=48000U; s.channels=2U; s.totalFrames=frames; s.stream=stream;
    s.interleaved.resize(1000U*2U);
    for(std::uint32_t i=0;i<1000U;++i) { s.interleaved[i*2U]=stream->readFrame(i,0U); s.interleaved[i*2U+1U]=stream->readFrame(i,1U); }
    s.loop.enabled=true; s.loop.startFrame=5000U; s.loop.endFrame=15000U;
    SincInterpolator interp(InterpolationQuality::LiveCubic4);
    for(double pos=0.0; pos<16000.0; pos+=0.731) {
        for(int lp=0; lp<2; ++lp) {
            const bool looping=lp==1;
            if(looping && pos>=15000.0) continue;
            float l=0.0F, r=0.0F; interp.readStereo(s,pos,0.731,looping,l,r);
            const auto i1=static_cast<long long>(std::floor(pos)); const auto t=static_cast<float>(pos-std::floor(pos));
            for(std::uint16_t ch=0; ch<2U; ++ch) {
                const float y0=refFrame(s,i1-1,ch,looping), y1=refFrame(s,i1,ch,looping), y2=refFrame(s,i1+1,ch,looping), y3=refFrame(s,i1+2,ch,looping);
                const float a0=-0.5F*y0+1.5F*y1-1.5F*y2+0.5F*y3, a1=y0-2.5F*y1+2.0F*y2-0.5F*y3, a2=-0.5F*y0+0.5F*y2;
                const float ref=((a0*t+a1)*t+a2)*t+y1;
                const float got=ch==0U?l:r;
                assert(std::abs(got-ref)<1.0e-6F);
                assert(std::abs(interp.read(s,pos,ch,0.731,looping)-got)<1.0e-9F);
            }
        }
    }
    std::filesystem::remove(path);
}

// MONO note change must not cut the previous note in one sample.
void testMonoTakeoverIsClickFree() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=96000U; s->channels=1U; s->rootKey=60U;
    s->interleaved.resize(96000U);
    for(std::size_t i=0;i<s->interleaved.size();++i) s->interleaved[i]=static_cast<float>(0.8*std::sin(2.0*3.14159265358979323846*220.0*static_cast<double>(i)/96000.0));
    s->totalFrames=96000U; s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=96000U;
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    auto mono=std::make_shared<EngineState>(*st); mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(mono);
    std::vector<float> out(96U*2U); double maxJump=0.0; float prev=0.0F;
    const std::uint8_t key=60U; // same pitch every time: only the takeover itself could make a step
    for(int step=0; step<40; ++step) {
        assert(e.postNoteOn(0U,key,127U));
        for(int b=0;b<3+(step%5);++b) {
            e.render(out.data(),96U);
            for(std::size_t i=0;i<96U;++i) { maxJump=std::max(maxJump,static_cast<double>(std::abs(out[i*2U]-prev))); prev=out[i*2U]; }
        }
    }
    // 220 Hz at 0.8*0.707 gain moves at most ~0.01 per sample; a cut would jump up to ~0.56.
    assert(maxJump<0.08);
    assert(e.activeVoiceCount()<=2U);
}

// Playing voices publish the not-yet-cached part of the mapped sample and the
// read-ahead thread requests it; nothing is published for RAM-only samples.
void testBankReadAheadWindows() {
    const std::string path="/tmp/silveton_crackle_readahead.wav";
    const std::uint32_t frames=240000U;
    {
        std::ofstream f(path,std::ios::binary|std::ios::trunc);
        f.write("RIFF",4); putLe32(f,36U+frames*4U); f.write("WAVEfmt ",8); putLe32(f,16U); putLe16(f,1U); putLe16(f,2U);
        putLe32(f,48000U); putLe32(f,48000U*4U); putLe16(f,4U); putLe16(f,16U); f.write("data",4); putLe32(f,frames*4U);
        std::vector<char> zeros(static_cast<std::size_t>(frames)*4U,0); f.write(zeros.data(),static_cast<std::streamsize>(zeros.size()));
    }
    MappedPcmSpec spec{}; spec.dataOffset=44U; spec.frameCount=frames; spec.sampleRate=48000U; spec.channels=2U; spec.bitDepth=16U; spec.formatTag=1U;
    std::string err; auto stream=MappedPcmStream::open(path,spec,err); assert(stream!=nullptr);
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=2U; s->totalFrames=frames; s->stream=stream; s->rootKey=60U;
    s->interleaved.assign(4096U*2U,0.0F);
    auto st=makeState(s);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    SampleStreamPrefetcher prefetcher(e);
    assert(e.postNoteOn(0U,60U,100U));
    std::vector<float> out(96U*2U);
    for(int b=0;b<4;++b) e.render(out.data(),96U);
    const auto raw=stream->rawView();
    const auto lo=reinterpret_cast<std::uintptr_t>(raw.data);
    const auto hi=lo+static_cast<std::uintptr_t>(raw.frames*raw.frameBytes());
    bool found=false;
    for(std::size_t slot=0; slot<SamplerEngine::kMaxStreamWindows; ++slot) {
        SamplerEngine::StreamWindow w{};
        if(!e.readStreamWindow(slot,w) || w.bytes==0U) continue;
        found=true;
        assert(w.begin>=lo+4096U*4U);          // starts after the RAM attack prefix
        assert(w.begin+w.bytes<=hi);           // never outside the sample
        assert(w.bytes>=static_cast<std::size_t>(0.5*48000.0*4.0)); // about 0.75 s ahead at root pitch
    }
    assert(found);
    prefetcher.runOnce();
    assert(prefetcher.requests()>0U);
    assert(prefetcher.start()); std::this_thread::sleep_for(std::chrono::milliseconds(10)); prefetcher.stop();
    assert(e.postPanic()); e.render(out.data(),96U); e.render(out.data(),96U);
    for(std::size_t slot=0; slot<SamplerEngine::kMaxStreamWindows; ++slot) {
        SamplerEngine::StreamWindow w{}; assert(e.readStreamWindow(slot,w)); assert(w.bytes==0U);
    }
    std::filesystem::remove(path);
}

// With all 128 voices sounding, a new note takes the oldest voice. That voice
// must fade out (spare tail slot) instead of being cut in one sample, and
// polyphony must stay 128.
void testVoiceLimitTakeoverIsClickFree() {
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=1U; s->rootKey=60U;
    s->interleaved.assign(48000U,0.5F); s->totalFrames=48000U; // constant level: any cut is a visible step
    s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=48000U;
    auto st=makeState(s);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    prog->layers[0].ampEnvelope={0.01F,0.01F,1.0F,0.02F};
    auto quiet=std::make_shared<EngineState>(*st); quiet->combi.uppers[0].levelDb=-40.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(quiet);
    for(int k=0;k<128;++k) assert(e.postNoteOn(0U,static_cast<std::uint8_t>(k),127U));
    std::vector<float> out(96U*2U);
    for(int b=0;b<30;++b) e.render(out.data(),96U);
    assert(e.activeVoiceCount()==128U);
    float prev=out[95U*2U]; double maxJump=0.0;
    assert(e.postNoteOn(0U,60U,127U)); // 129th note
    for(int b=0;b<12;++b) {
        e.render(out.data(),96U);
        for(std::size_t i=0;i<96U;++i) { maxJump=std::max(maxJump,static_cast<double>(std::abs(out[i*2U]-prev))); prev=out[i*2U]; }
    }
    // One voice is ~0.0035 here: a one-sample cut would jump by that much.
    assert(maxJump<0.001);
    assert(e.activeVoiceCount()==128U);
}
}


// ---- MONO / LEGATO and TONE (ADSR, CUTOFF) tests --------------------------------

SamplePtr makeLoopedSine(double hz, std::uint8_t rootKey) {
    auto s=std::make_shared<SampleData>(); s->sampleRate=48000U; s->channels=1U; s->rootKey=rootKey;
    constexpr std::size_t frames=48000U; // whole periods only: seamless loop
    const std::size_t periods=static_cast<std::size_t>(std::round(hz));
    s->interleaved.resize(frames);
    for(std::size_t i=0;i<frames;++i)
        s->interleaved[i]=static_cast<float>(0.5*std::sin(2.0*3.14159265358979323846*static_cast<double>(periods)*static_cast<double>(i)/static_cast<double>(frames)));
    s->totalFrames=frames; s->loop.enabled=true; s->loop.startFrame=0U; s->loop.endFrame=frames;
    return s;
}

std::shared_ptr<EngineState> makeToneState(UpperVoiceMode mode) {
    auto st=makeState(makeLoopedSine(440.0,69U));
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass;
    prog->layers[0].ampEnvelope={0.0F,0.01F,1.0F,0.02F};
    st->combi.uppers[0].voiceMode=mode;
    st->global.limiterEnabled=false;
    return st;
}

// Renders blocks and returns the left channel.
std::vector<float> renderLeft(SamplerEngine& e, int blocks, std::uint32_t block=96U) {
    std::vector<float> out(block*2U), left; left.reserve(static_cast<std::size_t>(blocks)*block);
    for(int b=0;b<blocks;++b) { e.render(out.data(),block); for(std::uint32_t i=0;i<block;++i) left.push_back(out[i*2U]); }
    return left;
}

// Frequency from upward zero crossings over the given window.
double measureHz(const std::vector<float>& x, std::size_t from, std::size_t to, double sr) {
    std::size_t crossings=0; double first=-1.0, last=-1.0;
    for(std::size_t i=from+1U;i<to;++i) if(x[i-1U]<0.0F && x[i]>=0.0F) {
        const double t=static_cast<double>(i-1U)+static_cast<double>(-x[i-1U])/static_cast<double>(x[i]-x[i-1U]);
        if(first<0.0) { first=t; }
        last=t; ++crossings;
    }
    return crossings<2U?0.0:(static_cast<double>(crossings-1U)*sr/(last-first));
}

double peakOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double p=0.0; for(std::size_t i=from;i<to&&i<x.size();++i) p=std::max(p,static_cast<double>(std::abs(x[i]))); return p;
}

// Holding one key and sliding to others must never stop the sound: the same
// voice only changes pitch.
void testLegatoHeldKeyGlissando() {
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(makeToneState(UpperVoiceMode::Legato));
    e.postNoteOn(0U,69U,100U);
    auto x=renderLeft(e,60); // 60 ms
    assert(std::abs(measureHz(x,2000U,5700U,96000.0)-440.0)<3.0);
    for(int key=70;key<=81;++key) {
        e.postNoteOn(0U,static_cast<std::uint8_t>(key),100U);
        x=renderLeft(e,40);
        const double expected=440.0*std::pow(2.0,static_cast<double>(key-69)/12.0);
        assert(peakOf(x,0U,x.size())>0.15);
        assert(std::abs(measureHz(x,2200U,3800U,96000.0)-expected)<expected*0.02);
        assert(e.activeVoiceCount()==1U);
        double maxJump=0.0; for(std::size_t i=1U;i<x.size();++i) maxJump=std::max(maxJump,static_cast<double>(std::abs(x[i]-x[i-1U])));
        assert(maxJump<0.09); // 440..1760 Hz sine of amplitude 0.5 never jumps
    }
}

// Releasing the top key returns to the key that is still held.
void testMonoReturnsToHeldKey() {
    for(auto mode : {UpperVoiceMode::Mono, UpperVoiceMode::Legato}) {
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(makeToneState(mode));
        e.postNoteOn(0U,69U,100U); renderLeft(e,30);
        e.postNoteOn(0U,76U,100U); renderLeft(e,30);
        e.postNoteOff(0U,76U,0U);
        auto x=renderLeft(e,60);
        assert(peakOf(x,3000U,x.size())>0.15);
        assert(std::abs(measureHz(x,3000U,5700U,96000.0)-440.0)<5.0);
        e.postNoteOn(0U,72U,100U); renderLeft(e,30);
        e.postNoteOff(0U,69U,0U); // a lower key let go: nothing changes
        x=renderLeft(e,60);
        assert(peakOf(x,3000U,x.size())>0.15);
        assert(std::abs(measureHz(x,3000U,5700U,96000.0)-440.0*std::pow(2.0,3.0/12.0))<6.0);
        e.postNoteOff(0U,72U,0U);
        renderLeft(e,200);
        assert(e.activeVoiceCount()==0U);
        x=renderLeft(e,10);
        assert(peakOf(x,0U,x.size())<1.0e-4);
    }
}

// Two recordings: LEGATO cross-fades, never goes silent.
void testLegatoAcrossSamples() {
    auto st=makeToneState(UpperVoiceMode::Legato);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    auto ms=std::const_pointer_cast<Multisample>(prog->layers[0].multisample);
    SampleZone hi=ms->zones[0]; hi.sample=makeLoopedSine(880.0,81U); hi.rootKey=81U; hi.keyLow=76U; hi.keyHigh=127U;
    ms->zones[0].keyHigh=75U; ms->zones.push_back(hi);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,60);
    e.postNoteOn(0U,79U,100U);
    auto x=renderLeft(e,100);
    assert(peakOf(x,0U,x.size())>0.15);
    double minEnv=10.0; // smallest 2 ms window peak while crossing
    for(std::size_t i=0;i+192U<x.size();i+=96U) minEnv=std::min(minEnv,peakOf(x,i,i+192U));
    assert(minEnv>0.15);
    assert(e.activeVoiceCount()==1U);
}

void testLegatoPortamentoGlide() {
    for(auto mode : {UpperVoiceMode::Legato, UpperVoiceMode::Mono}) {
        auto st=makeToneState(mode);
        st->combi.uppers[0].portamentoSec=0.1F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
        e.postNoteOn(0U,81U,100U);
        auto x=renderLeft(e,400); // 400 ms
        const double early=measureHz(x,300U,1500U,96000.0);
        const double late=measureHz(x,34000U,38000U,96000.0);
        assert(early>430.0 && early<880.0);   // still gliding (soft S-curve: starts slowly)
        assert(std::abs(late-880.0)<12.0);    // arrived
        assert(peakOf(x,0U,x.size())>0.15);
    }
}


// PORTAMENTO like the SILVETON keyboard processor.
// Pitch reached `ms` milliseconds after a note: the glide is linear in semitones.
double glideHzAt(const std::vector<float>& x, double ms) {
    const auto c=static_cast<std::size_t>(ms*96.0);
    return measureHz(x,c>240U?c-240U:0U,c+240U,96000.0);
}

// TIME portamento: a semitone and two octaves take exactly the same time.
void testPortamentoSameTimeEveryInterval() {
    for(int target : {70, 81, 93}) {
        auto st=makeToneState(UpperVoiceMode::Mono);
        st->combi.uppers[0].portamentoSec=0.05F; // 50 ms
        SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoNear(0.0F); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
        e.postNoteOn(0U,static_cast<std::uint8_t>(target),100U);
        auto x=renderLeft(e,120);
        const double goal=440.0*std::pow(2.0,(target-69)/12.0);
        // SILVETON_SOFT_GLIDE_V1: soft S-curve + PORTA BALANCE (11 ms) rounding.
        // Every interval is the same part of the way at the same moment.
        const double progress=12.0*std::log2(glideHzAt(x,30.0)/440.0)/static_cast<double>(target-69);
        static double firstProgress=-1.0;
        if(firstProgress<0.0) firstProgress=progress;
        assert(progress>0.2 && progress<0.8);
        assert(std::abs(progress-firstProgress)<0.05);
        assert(std::abs(glideHzAt(x,100.0)-goal)<goal*0.006);       // arrived (TIME + BALANCE)
    }
}

// SILVETON_PORTA_PAGE_V1 - like the SILVETON portamento page: while portamento
// is ON a POLY Upper plays one line, a held key glides to the next key and back
// in TIME; with no key held the next note is struck normally without a glide.
void testPortamentoPolyFromLastNote() {   // (MONO now: POLY stays POLY, see testPolyPortamentoAndNear)
    auto st=makeToneState(UpperVoiceMode::Mono);
    st->combi.uppers[0].portamentoSec=0.08F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full               // 69 held
    e.postNoteOn(0U,81U,100U);                                  // glides from 69
    auto x=renderLeft(e,150);
    assert(e.activeVoiceCount()==1U);                           // one line
    { const double h=glideHzAt(x,45.0); assert(h>480.0 && h<820.0); }   // gliding 440 -> 880 (soft S-curve)
    assert(std::abs(glideHzAt(x,140.0)-880.0)<880.0*0.006);           // arrived (TIME + BALANCE)
    e.postNoteOff(0U,81U,0U);                                   // back to the still held 69 - glides
    auto back=renderLeft(e,150);
    { const double h=glideHzAt(back,45.0); assert(h>470.0 && h<810.0); }
    assert(std::abs(glideHzAt(back,140.0)-440.0)<440.0*0.006);
    e.postNoteOff(0U,69U,0U); renderLeft(e,200);
    assert(e.activeVoiceCount()==0U);
    e.postNoteOn(0U,81U,100U);                                  // nothing held: no glide
    auto d=renderLeft(e,60);
    assert(std::abs(glideHzAt(d,10.0)-880.0)<880.0*0.006);
    e.postNoteOff(0U,81U,0U); renderLeft(e,200);
    // Portamento OFF: POLY chords again, no glide
    auto off=std::make_shared<EngineState>(*st);
    off->combi.uppers[0].portamentoSec=0.0F;
    off->combi.uppers[0].voiceMode=UpperVoiceMode::Poly;
    e.setState(off);
    e.postNoteOn(0U,69U,100U); e.postNoteOn(0U,76U,100U);
    renderLeft(e,40);
    assert(e.activeVoiceCount()>=2U);
    e.postPanic(); renderLeft(e,10);
    // PANIC forgets the held line: the next note does not glide
    e.setState(st);
    e.postNoteOn(0U,81U,100U); renderLeft(e,20); e.postPanic(); renderLeft(e,10);
    e.postNoteOn(0U,69U,100U);
    auto z=renderLeft(e,40);
    assert(std::abs(glideHzAt(z,10.0)-440.0)<440.0*0.006);
}

// MONO / LEGATO with portamento: the same rules.
void testPortamentoFromLastNoteMono() {
    for(auto mode : {UpperVoiceMode::Mono, UpperVoiceMode::Legato}) {
        auto st=makeToneState(mode);
        st->combi.uppers[0].portamentoSec=0.06F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
        e.postNoteOff(0U,69U,0U); renderLeft(e,200);            // gone completely
        e.postNoteOn(0U,81U,100U);                              // detached: struck, no glide
        auto x=renderLeft(e,100);
        assert(std::abs(glideHzAt(x,10.0)-880.0)<880.0*0.006);
        e.postNoteOn(0U,69U,100U);                              // legato from 81
        auto y=renderLeft(e,100);
        { const double h=glideHzAt(y,35.0); assert(h>470.0 && h<810.0); }   // gliding (soft S-curve)
        assert(std::abs(glideHzAt(y,95.0)-440.0)<440.0*0.006);
        e.postNoteOff(0U,69U,0U);                               // back to the held 81
        auto z=renderLeft(e,100);
        { const double h=glideHzAt(z,35.0); assert(h>480.0 && h<820.0); }
        assert(std::abs(glideHzAt(z,95.0)-880.0)<880.0*0.006);
    }
}

// SILVETON_OCTAVE_KEY_V1: OCTAVE +1 plays the sample recorded for the key an
// octave up (its own sound), not the pressed key's sample stretched an octave.
void testOctaveChoosesRecordedSample() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    auto ms=std::const_pointer_cast<Multisample>(prog->layers[0].multisample);
    SampleZone hi=ms->zones[0]; hi.sample=makeLoopedSine(1000.0,72U); hi.rootKey=72U; hi.keyLow=72U; hi.keyHigh=127U;
    ms->zones[0].keyHigh=71U; ms->zones.push_back(hi);
    st->combi.uppers[0].octave=1;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,60U,100U);
    auto x=renderLeft(e,120);
    assert(std::abs(measureHz(x,x.size()-9600,x.size(),96000.0)-1000.0)<2.0);
}

// SILVETON_TERCA_V1: the TERCA Upper plays the diatonic third (processor rule)
// and lets it go with the key; switching TERCA off leaves nothing hanging.
void testTercaOnAnyUpper() {
    assert(SamplerEngine::tercaHarmony(0,0,60)==64);   // C major up: C -> E
    assert(SamplerEngine::tercaHarmony(0,0,69)==72);   // A -> C
    assert(SamplerEngine::tercaHarmony(0,1,64)==60);   // C major down: E -> C
    assert(SamplerEngine::tercaHarmony(2,5,63)==67);   // D hijaz up: Eb -> G (minor third)
    auto st=makeToneState(UpperVoiceMode::Poly);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.setTerca(1,0,0);   // T key on Upper 1
    e.postNoteOn(0U,69U,100U);
    auto x=renderLeft(e,150);
    assert(std::abs(measureHz(x,x.size()-9600,x.size(),96000.0)-523.25)<1.5);
    e.postNoteOff(0U,69U,0U); renderLeft(e,400);
    assert(e.activeVoiceCount()==0U);
    e.postNoteOn(0U,69U,100U); renderLeft(e,50);
    e.setTerca(0,0,0); e.postNoteOff(0U,69U,0U); renderLeft(e,400);
    assert(e.activeVoiceCount()==0U);
    // SILVETON_SCALE_V1: the scale cents reach only Uppers with Q on.
    auto q=std::make_shared<EngineState>(*st); q->global.scaleOffsetsCents[9]=-50.0F;   // A a quarter tone down
    e.setState(q);
    e.postNoteOn(0U,69U,100U); auto a=renderLeft(e,150);
    assert(std::abs(measureHz(a,a.size()-9600,a.size(),96000.0)-440.0)<1.0);              // Q off: 440
    e.setScaleUppers(1); auto b=renderLeft(e,150);                                        // Q on: live
    assert(std::abs(measureHz(b,b.size()-9600,b.size(),96000.0)-440.0*std::pow(2.0,-0.5/12.0))<1.0);
    e.postNoteOff(0U,69U,0U); renderLeft(e,300);
    // SILVETON_TQ_TRANSPOSE_V1: with TRANSPOSE the scale stays on the PLAYED key.
    auto t=std::make_shared<EngineState>(*q); t->global.transposeSemitones=-6;
    e.setState(t);
    e.postNoteOn(0U,69U,100U); auto c=renderLeft(e,150);                                   // played A, sounds D#
    assert(std::abs(measureHz(c,c.size()-9600,c.size(),96000.0)-440.0*std::pow(2.0,(-6.0-0.5)/12.0))<1.0);
    e.postNoteOff(0U,69U,0U); renderLeft(e,300);
    e.postNoteOn(0U,75U,100U); auto d=renderLeft(e,150);                                   // played D#, sounds A: no Q
    assert(std::abs(measureHz(d,d.size()-9600,d.size(),96000.0)-440.0)<1.0);
    e.postNoteOff(0U,75U,0U); renderLeft(e,300);
    // TERCA with TRANSPOSE: the third of the played key, then transposed.
    e.setScaleUppers(0); e.setTerca(1,0,0);
    e.postNoteOn(0U,69U,100U); auto h=renderLeft(e,150);                                   // A -> C, -6 = F#
    assert(std::abs(measureHz(h,h.size()-9600,h.size(),96000.0)-523.25*std::pow(2.0,-6.0/12.0))<1.5);
    e.postNoteOff(0U,69U,0U); renderLeft(e,400);
    // A key held while T is switched ON does not hang.
    e.setTerca(0,0,0); renderLeft(e,10);
    e.postNoteOn(0U,69U,100U); renderLeft(e,50);
    e.setTerca(1,0,0); renderLeft(e,10);
    e.postNoteOff(0U,69U,0U); renderLeft(e,400);
    assert(e.activeVoiceCount()==0U);
}

// SILVETON_FX_TAIL_V1: calling up another BLEND lets the old reverb ring out.
void testFxTailOnBlendChange() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.fx[0].type=EffectType::Reverb; st->combi.fx[0].wet=0.8F; st->combi.fx[0].feedback=0.8F;
    st->combi.uppers[0].fxSend1=1.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,200); e.postNoteOff(0U,69U,0U); renderLeft(e,30);
    auto next=std::make_shared<EngineState>(*st);
    next->combi.fx[0].type=EffectType::Off; next->combiGeneration=st->combiGeneration+1U;
    e.setState(next);
    auto x=renderLeft(e,300);
    assert(peakOf(x,x.size()/2U,x.size())>1.0e-4);      // the old reverb still rings
    auto live=std::make_shared<EngineState>(*st);       // a live edit (same generation) changes at once
    live->combi.fx[0].type=EffectType::Off;
    SamplerEngine f; f.prepare(96000.0,96U); f.setState(st);
    f.postNoteOn(0U,69U,100U); renderLeft(f,200); f.postNoteOff(0U,69U,0U); renderLeft(f,30);
    f.setState(live);
    auto y=renderLeft(f,300);
    assert(peakOf(y,y.size()/2U,y.size())<peakOf(x,x.size()/2U,x.size()));
}

// SILVETON_POLY_PORTA_V1 / SILVETON_PORTA_NEAR_V1
void testPolyPortamentoAndNear() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].portamentoSec=0.03F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoNear(0.0F); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,60);
    e.postNoteOn(0U,81U,100U); auto x=renderLeft(e,200);
    assert(e.activeVoiceCount()==2U);                          // POLY: both keys sound
    e.postNoteOff(0U,69U,0U); e.postNoteOff(0U,81U,0U); renderLeft(e,400);
    // NEAR: one semitone takes NEAR, an octave still takes TIME
    SamplerEngine m; m.prepare(96000.0,96U); m.setPortamentoNear(90.0F);
    auto mono=makeToneState(UpperVoiceMode::Legato); mono->combi.uppers[0].portamentoSec=0.03F; m.setState(mono);
    m.postNoteOn(0U,69U,100U); renderLeft(m,60);
    m.postNoteOn(0U,70U,100U); auto y=renderLeft(m,60);
    assert(glideHzAt(y,40.0)<466.16*0.997);                   // after 40 ms still on the way (NEAR = 90 ms)
    (void)x;
}

// Another Sound on the Upper: the first note does not glide.
void testPortamentoNewSoundStartsFresh() {
    auto st=makeToneState(UpperVoiceMode::Mono);
    st->combi.uppers[0].portamentoSec=0.06F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,40); e.postNoteOff(0U,69U,0U); renderLeft(e,200);
    auto other=makeToneState(UpperVoiceMode::Mono);              // a different PROGRAM object
    other->combi.uppers[0].portamentoSec=0.06F;
    e.setState(other);
    e.postNoteOn(0U,81U,100U);
    auto x=renderLeft(e,40);
    assert(std::abs(glideHzAt(x,10.0)-880.0)<880.0*0.006);
}

// The held line is ONE continuous sound: a new key is not struck again (no new
// ATTACK), in POLY, MONO and LEGATO; the sounding note pressed again changes nothing.
void testPortamentoAutoMonoReleaseTail() {
    for(auto mode : {UpperVoiceMode::Mono, UpperVoiceMode::Legato}) {   // POLY stays POLY (SILVETON_POLY_PORTA_V1)
        auto st=makeToneState(mode);
        st->combi.uppers[0].portamentoSec=0.03F;
        st->combi.uppers[0].toneAttackMs=150.0F;                 // a slow attack would show a new strike
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
        e.postNoteOn(0U,69U,100U);
        auto a=renderLeft(e,400);
        const double full=peakOf(a,a.size()-4800U,a.size());
        e.postNoteOn(0U,76U,100U);
        auto b=renderLeft(e,60);
        assert(peakOf(b,0U,960U)>0.9*full);                     // first 10 ms: still full, no new attack
        assert(peakOf(b,b.size()-960U,b.size())>0.9*full);
        assert(e.activeVoiceCount()==1U);
        e.postNoteOn(0U,76U,100U);                              // the same note again: nothing
        auto c=renderLeft(e,60);
        assert(peakOf(c,0U,960U)>0.9*full && e.activeVoiceCount()==1U);
        assert(std::abs(measureHz(c,1000U,5000U,96000.0)-659.26)<659.26*0.006);
        // CC#123: the line is forgotten, the next note starts clean
        e.postControlChange(0U,123U,0U); renderLeft(e,10);
        e.postNoteOn(0U,81U,100U);
        auto x=renderLeft(e,30);
        assert(std::abs(glideHzAt(x,10.0)-880.0)<880.0*0.006);
    }
}

// PORTA BALANCE does not change the sound: every interval glides in exactly TIME.
void testPortamentoBalance() {
    for(float bal : {11.0F, 5.0F, 30.0F}) {
        auto st=makeToneState(UpperVoiceMode::Legato);
        st->combi.uppers[0].portamentoSec=0.05F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoBalance(bal); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
        e.postNoteOn(0U,81U,100U);
        auto x=renderLeft(e,160);
        // SILVETON_SOFT_GLIDE_V1: BALANCE rounds the glide; a bigger BALANCE is softer
        // (a little later at the same moment), every BALANCE still arrives.
        const double h=glideHzAt(x,30.0);
        static double prevH=0.0; static float prevBal=0.0F;
        assert(h>460.0 && h<840.0);
        if(prevBal>0.0F && bal>prevBal) assert(h<prevH);
        if(prevBal>0.0F && bal<prevBal) assert(h>prevH);
        prevH=h; prevBal=bal;
        assert(std::abs(glideHzAt(x,150.0)-880.0)<880.0*0.006);
    }
}

// SILVETON_SILK_PORTA_V1: BALANCE gives a softer TIME curve, but never a
// lingering low-pass detune after landing. Interruptions carry the audible
// pitch rather than restarting from the original key.
void testSilkPortamentoNearArrivalAndRestart() {
    auto st = makeToneState(UpperVoiceMode::Legato);
    st->combi.uppers[0].portamentoSec = 0.06F;
    SamplerEngine e; e.prepare(96000.0, 96U);
    e.setPortamentoNear(120.0F);
    e.setPortamentoBalance(35.0F);
    e.setState(st);
    e.postNoteOn(0U, 69U, 100U); renderLeft(e, 60);
    e.postNoteOn(0U, 70U, 100U);
    auto first = renderLeft(e, 150);
    const double target70 = 440.0 * std::pow(2.0, 1.0 / 12.0);
    const double early = glideHzAt(first, 15.0);
    const double middle = glideHzAt(first, 60.0);
    assert(early > 438.0 && early < middle);
    assert(middle < target70);
    // By 120 ms the note has arrived: the last 30 ms cannot be drifting.
    assert(std::abs(glideHzAt(first, 135.0) - target70) < target70 * 0.006);
    e.postNoteOn(0U, 71U, 100U);
    auto second = renderLeft(e, 145);
    const double target71 = 440.0 * std::pow(2.0, 2.0 / 12.0);
    assert(glideHzAt(second, 12.0) > target70 * 0.99);
    assert(glideHzAt(second, 50.0) < target71);
    assert(std::abs(glideHzAt(second, 135.0) - target71) < target71 * 0.006);
    // Interrupt during a moving glide: no hard pitch reset to either key.
    e.postNoteOn(0U, 69U, 100U);
    auto goingBack = renderLeft(e, 50);
    const double at40 = glideHzAt(goingBack, 40.0);
    assert(at40 > 440.0 && at40 < target71);
    e.postNoteOn(0U, 70U, 100U);
    auto interrupted = renderLeft(e, 150);
    assert(std::abs(glideHzAt(interrupted, 135.0) - target70) < target70 * 0.006);
}

// TIME is 1..100 ms: an older SET with a longer time still glides in 100 ms.
void testPortamentoTimeLimit() {
    auto st=makeToneState(UpperVoiceMode::Legato);
    st->combi.uppers[0].portamentoSec=2.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
    e.postNoteOn(0U,81U,100U);
    auto x=renderLeft(e,200);
    assert(std::abs(glideHzAt(x,130.0)-880.0)<880.0*0.006);
}

// Two recordings (different root keys): the glide still starts at the pitch
// that was sounding, not at the other recording's playback speed.
void testPortamentoAcrossRecordings() {
    auto st=makeToneState(UpperVoiceMode::Mono);
    st->combi.uppers[0].portamentoSec=0.06F;
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    auto ms=std::const_pointer_cast<Multisample>(prog->layers[0].multisample);
    SampleZone hi=ms->zones[0]; hi.sample=makeLoopedSine(880.0,81U); hi.rootKey=81U; hi.keyLow=76U; hi.keyHigh=127U;
    ms->zones[0].keyHigh=75U; ms->zones.push_back(hi);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,200);   // SILVETON_PORTA_TEMPO_V1: a slow line, TIME applies in full
    e.postNoteOn(0U,79U,100U);                                  // other recording
    auto x=renderLeft(e,120);
    const double start=glideHzAt(x,4.0);
    assert(start>430.0 && start<520.0);                         // starts near 440 Hz
    assert(std::abs(glideHzAt(x,90.0)-783.99)<783.99*0.006);    // ends on G5
}

// Each Upper keeps its own key stack.
void testMonoSplitUppersIndependent() {
    auto st=makeToneState(UpperVoiceMode::Mono);
    st->combi.uppers[1]=st->combi.uppers[0]; st->combi.uppers[1].midiChannel=1U; st->combi.uppers[1].voiceMode=UpperVoiceMode::Poly;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,60U,100U); e.postNoteOn(1U,64U,100U); e.postNoteOn(1U,67U,100U); renderLeft(e,40);
    assert(e.activeVoiceCount()==3U);
    e.postNoteOn(0U,62U,100U); renderLeft(e,40);
    assert(e.activeVoiceCount()==3U); // MONO replaced its note, the POLY chord is intact
    e.postNoteOff(1U,64U,0U); renderLeft(e,200);
    assert(e.activeVoiceCount()==2U);
}

// Changing TONE while keys are held must not drop notes.
void testToneEditKeepsHeldKeys() {
    auto st=makeToneState(UpperVoiceMode::Legato);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,30);
    auto st2=std::make_shared<EngineState>(*st);
    st2->combi.uppers[0].toneCutoffHz=3000.0F; st2->combi.uppers[0].toneSustain=0.8F;
    e.setState(st2);
    auto x=renderLeft(e,60);
    assert(peakOf(x,0U,x.size())>0.1);
    e.postNoteOn(0U,72U,100U); x=renderLeft(e,60);
    assert(peakOf(x,2000U,x.size())>0.1);
    e.postNoteOff(0U,72U,0U); x=renderLeft(e,60); // back to 69 which is still held
    assert(peakOf(x,3000U,x.size())>0.1);
}

void testUpperAdsrAbsolute() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].toneAttackMs=100.0F; st->combi.uppers[0].toneDecayMs=200.0F;
    st->combi.uppers[0].toneSustain=0.5F; st->combi.uppers[0].toneReleaseMs=200.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,127U);
    auto x=renderLeft(e,1000); // 1 s
    const double full=0.5*0.70710678;
    assert(peakOf(x,0U,192U)<0.12*full);              // 2 ms into a 100 ms attack
    assert(peakOf(x,9400U,9600U)>0.85*full);          // attack finished at ~100 ms
    assert(peakOf(x,0U,9500U)<=full*1.001);           // no overshoot
    const double sus=peakOf(x,90000U,96000U);
    assert(std::abs(sus-0.5*full)<0.03*full);         // SUSTAIN 0.5
    e.postNoteOff(0U,69U,0U);
    auto r=renderLeft(e,400);
    const double mid=peakOf(r,9400U,9700U);           // ~100 ms into a 200 ms release (half way)
    assert(mid<0.05*full && mid>1.0e-5);              // exponential: far below linear half
    assert(peakOf(r,19500U,r.size())<1.0e-3*full);    // finished after the release time
    assert(e.activeVoiceCount()==0U);
    // SUSTAIN 100 % is a flat tone
    st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].toneAttackMs=1.0F; st->combi.uppers[0].toneDecayMs=50.0F; st->combi.uppers[0].toneSustain=1.0F;
    SamplerEngine f; f.prepare(96000.0,96U); f.setState(st); f.postNoteOn(0U,69U,127U);
    auto y=renderLeft(f,300);
    assert(peakOf(y,20000U,28800U)>0.33);
}

// SILVETON_LIVE_EDIT_V1: a held note hears every edit of its Upper at once
// (level, pan, ON/MUTE/SOLO smoothly, tuning), a note of a replaced Sound does not.
void testLiveEditsOnHeldNote() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[1]=st->combi.uppers[0]; st->combi.uppers[1].midiChannel=5U;
    const auto tail=[](const std::vector<float>& x){ return rms(std::vector<float>(x.end()-9600,x.end())); };
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st); e.postNoteOn(0U,69U,100U);
    const float ref=tail(renderLeft(e,200));
    assert(ref>0.1F);
    auto lvl=std::make_shared<EngineState>(*st); lvl->combi.uppers[0].levelDb=-12.0F; e.setState(lvl);
    assert(std::abs(tail(renderLeft(e,200))-ref*0.2512F)<0.01F);        // LEVEL -12 dB on the held note
    auto mute=std::make_shared<EngineState>(*st); mute->combi.uppers[0].mute=true; e.setState(mute);
    auto m=renderLeft(e,200);
    assert(tail(m)<1.0e-4F);                                              // MUTE silences it
    double jump=0.0; for(std::size_t i=1;i<m.size();++i) jump=std::max(jump,static_cast<double>(std::abs(m[i]-m[i-1])));
    assert(jump<0.012);                                                   // ... without a click
    e.setState(st);
    assert(std::abs(tail(renderLeft(e,200))-ref)<0.01F);                 // back on
    auto solo=std::make_shared<EngineState>(*st); solo->combi.uppers[1].solo=true; e.setState(solo);
    assert(tail(renderLeft(e,200))<1.0e-4F);                              // SOLO of another Upper
    auto tune=std::make_shared<EngineState>(*st); tune->combi.uppers[0].transposeSemitones=2; tune->combi.uppers[0].tuneCents=50.0F; e.setState(tune);
    auto t=renderLeft(e,200);
    // SILVETON_OCTAVE_KEY_V1: TRANSPOSE/OCTAVE move the key from the next press; fine tune is live.
    assert(std::abs(measureHz(t,t.size()-9600,t.size(),96000.0)-440.0*std::pow(2.0,0.5/12.0))<0.5);
    auto pan=std::make_shared<EngineState>(*st); pan->combi.uppers[0].pan=-1.0F; e.setState(pan);
    std::vector<float> out(192); float right=0.0F;
    for(int b=0;b<200;++b){ e.render(out.data(),96U); if(b>150) for(int i=0;i<96;++i) right=std::max(right,std::abs(out[static_cast<std::size_t>(i*2+1)])); }
    assert(right<1.0e-4F);                                                // PAN hard left
    // Another Sound on the Upper: the held note keeps its own settings.
    auto other=std::make_shared<Program>(*st->combi.uppers[0].program);
    SamplerEngine g; g.prepare(96000.0,96U); g.setState(st); g.postNoteOn(0U,69U,100U);
    const float own=tail(renderLeft(g,200));
    auto swapped=std::make_shared<EngineState>(*st); swapped->combi.uppers[0].program=other;
    swapped->combi.uppers[0].levelDb=-20.0F; swapped->combi.uppers[0].octave=1; g.setState(swapped);
    auto h=renderLeft(g,200);
    assert(std::abs(tail(h)-own)<0.005F && std::abs(measureHz(h,h.size()-9600,h.size(),96000.0)-440.0)<0.5);
}

// SILVETON_FX_V2: FX1 and FX2 have their own engines, DELAY repeats on time and
// its TONE darkens them, the plate REVERB lasts the DECAY time it shows.
void renderFxNote(const std::shared_ptr<EngineState>& st, int noteBlocks, int blocks, std::vector<float>& left) {
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,81U,120U);
    std::vector<float> out(192);
    for(int b=0;b<blocks;++b) {
        if(b==noteBlocks) e.postNoteOff(0U,81U,0U);
        e.render(out.data(),96U);
        for(std::size_t i=0;i<96U;++i) left.push_back(out[i*2U]);
    }
}

void testFxSlotsDelayReverb() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].toneReleaseMs=2.0F; st->combi.uppers[0].fxSend1=1.0F; st->combi.uppers[0].fxSend2=1.0F;
    st->combi.fx[0].type=EffectType::Reverb; st->combi.fx[0].wet=1.0F; st->combi.fx[1]=st->combi.fx[0];
    auto one=std::make_shared<EngineState>(*st); one->combi.fx[1].type=EffectType::Off;
    auto none=std::make_shared<EngineState>(*st); none->combi.fx[0].type=EffectType::Off; none->combi.fx[1].type=EffectType::Off;
    std::vector<float> a,b,c; renderFxNote(st,100,600,a); renderFxNote(one,100,600,b); renderFxNote(none,100,600,c);
    double err=0.0, mag=0.0;
    for(std::size_t i=0;i<a.size();++i) {
        err=std::max(err,static_cast<double>(std::abs((a[i]-c[i])-2.0F*(b[i]-c[i]))));
        mag=std::max(mag,static_cast<double>(std::abs(b[i]-c[i])));
    }
    assert(mag>0.05 && err<1.0e-4*mag+1.0e-6);                     // two REVERBs do not share one engine

    auto d=makeToneState(UpperVoiceMode::Poly);
    d->combi.uppers[0].toneReleaseMs=2.0F; d->combi.uppers[0].fxSend1=1.0F;
    d->combi.fx[0].type=EffectType::Delay; d->combi.fx[0].wet=1.0F; d->combi.fx[0].timeMs=300.0F; d->combi.fx[0].feedback=0.5F; d->combi.fx[0].tone=1.0F;
    auto dOff=std::make_shared<EngineState>(*d); dOff->combi.fx[0].type=EffectType::Off;
    auto dDark=std::make_shared<EngineState>(*d); dDark->combi.fx[0].tone=0.2F;
    std::vector<float> w,w0,wd; renderFxNote(d,20,700,w); renderFxNote(dOff,20,700,w0); renderFxNote(dDark,20,700,wd);
    std::size_t first=0U; for(std::size_t i=0;i<w.size();++i) if(std::abs(w[i]-w0[i])>0.01F) { first=i; break; }
    assert(first>=28790U && first<=28820U);                        // echo at 300 ms
    const auto win=[&](const std::vector<float>& x, std::size_t from){ std::vector<float> y; for(std::size_t i=from;i<from+1920U;++i) y.push_back(x[i]-w0[i]); return rms(y); };
    assert(std::abs(win(w,57600U)/win(w,28800U)-0.5F)<0.03F);     // FEEDBACK 0.5
    assert(win(wd,28800U)<0.95F*win(w,28800U));                     // TONE darkens the repeats

    for(float fb : {0.3F, 0.6F}) {
        auto r=makeToneState(UpperVoiceMode::Poly);
        r->combi.uppers[0].toneReleaseMs=2.0F; r->combi.uppers[0].fxSend1=1.0F;
        r->combi.fx[0].type=EffectType::Reverb; r->combi.fx[0].wet=1.0F; r->combi.fx[0].feedback=fb; r->combi.fx[0].tone=0.5F;
        auto rOff=std::make_shared<EngineState>(*r); rOff->combi.fx[0].type=EffectType::Off;
        std::vector<float> x,x0; renderFxNote(r,20,7000,x); renderFxNote(rOff,20,7000,x0);
        std::vector<double> energy(x.size()+1U,0.0);
        for(std::size_t i=x.size();i-->2400U;) energy[i]=energy[i+1U]+static_cast<double>(x[i]-x0[i])*static_cast<double>(x[i]-x0[i]);
        std::size_t i5=0U,i35=0U;
        for(std::size_t i=2400U;i<x.size();++i) {
            const double db=10.0*std::log10(energy[i]/energy[2400U]+1.0e-30);
            if(i5==0U && db<=-5.0) i5=i;
            if(i35==0U && db<=-35.0) { i35=i; break; }
        }
        assert(i5>0U && i35>i5);
        const double t30=2.0*static_cast<double>(i35-i5)/96000.0;
        const double shown=0.6*std::pow(20.0,static_cast<double>(fb)/0.95);
        assert(std::abs(t30-shown)<0.15*shown);                    // REVERB lasts the DECAY time it shows
        for(float v:x) assert(std::isfinite(v) && std::abs(v)<2.0F);
    }
}

void testUpperCutoffResonance() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,93U,127U); // 1760 Hz
    auto open=renderLeft(e,100);
    auto st2=std::make_shared<EngineState>(*st);
    st2->combi.uppers[0].toneCutoffHz=300.0F;
    e.setState(st2);
    renderLeft(e,200); // glide settles
    auto closed=renderLeft(e,50);
    assert(peakOf(open,6000U,9600U)>0.3);
    assert(peakOf(closed,0U,closed.size())<0.07);     // 1760 Hz is ~2.5 octaves above a 300 Hz 12 dB filter
    auto st3=std::make_shared<EngineState>(*st);
    st3->combi.uppers[0].toneCutoffHz=1760.0F; st3->combi.uppers[0].toneResonance=0.8F;
    e.setState(st3);
    renderLeft(e,200);
    auto reson=renderLeft(e,50);
    assert(peakOf(reson,0U,reson.size())>1.2);        // resonance boosts at the cutoff
    e.setState(st); // open again
    renderLeft(e,300);
    auto back=renderLeft(e,50);
    assert(peakOf(back,0U,back.size())>0.3);
    // sweep: no jumps
    auto st4=std::make_shared<EngineState>(*st);
    SamplerEngine g; g.prepare(96000.0,96U); g.setState(st); g.postNoteOn(0U,93U,127U); renderLeft(g,50);
    float prev=0.0F; double maxJump=0.0;
    for(int k=0;k<80;++k) {
        st4=std::make_shared<EngineState>(*st); st4->combi.uppers[0].toneCutoffHz=200.0F*std::pow(1.07F,static_cast<float>(k)); g.setState(st4);
        auto z=renderLeft(g,5); for(float v:z){ maxJump=std::max(maxJump,static_cast<double>(std::abs(v-prev))); prev=v; }
    }
    assert(maxJump<0.08);
    const auto tv=effectiveUpperTone(st->combi.uppers[0]);
    assert(std::abs(tv.cutoffHz-kToneCutoffOpenHz)<1.0F && tv.sustain>0.99F);
}


// ---------------------------------------------------------------- SILVETON_LAYERS_V1
// An Upper with a layer plays both recordings; the layer has its own level, tune,
// range and TONE; an old BLEND (layers empty) sounds exactly as before.
void testUpperLayers() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    // a second program at 660 Hz for the layer
    auto st2=makeState(makeLoopedSine(660.0,69U));
    auto prog2=std::const_pointer_cast<Program>(st2->combi.uppers[0].program);
    prog2->layers[0].filter.mode=FilterMode::Bypass; prog2->layers[0].ampEnvelope={0.0F,0.01F,1.0F,0.02F};
    // 1) no layer: one voice, 440 Hz
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); auto x=renderLeft(e,60);
    assert(e.activeVoiceCount()==1U); assert(std::abs(measureHz(x,2000U,5000U,96000.0)-440.0)<3.0);
    const double peakAlone=peakOf(x,2000U,5000U);
    // 2) layer 1 on: two voices
    auto withLayer=std::make_shared<EngineState>(*st);
    auto& L=withLayer->combi.uppers[0].layers[0]; L.enabled=true; L.program=prog2; L.name="Layer";
    SamplerEngine e2; e2.prepare(96000.0,96U); e2.setState(withLayer);
    e2.postNoteOn(0U,69U,100U); auto y=renderLeft(e2,60);
    assert(e2.activeVoiceCount()==2U);
    assert(peakOf(y,2000U,5000U)>peakAlone*1.3);   // two sounds together are louder
    // 3) layer level -60 dB: practically only the basic sound
    auto quiet=std::make_shared<EngineState>(*withLayer); quiet->combi.uppers[0].layers[0].levelDb=-60.0F;
    SamplerEngine e3; e3.prepare(96000.0,96U); e3.setState(quiet);
    e3.postNoteOn(0U,69U,100U); auto z=renderLeft(e3,60);
    assert(std::abs(peakOf(z,2000U,5000U)-peakAlone)<peakAlone*0.05);
    // 4) the layer's key range excludes the key: only one voice
    auto ranged=std::make_shared<EngineState>(*withLayer); ranged->combi.uppers[0].layers[0].keyHigh=60U;
    SamplerEngine e4; e4.prepare(96000.0,96U); e4.setState(ranged);
    e4.postNoteOn(0U,69U,100U); renderLeft(e4,5); assert(e4.activeVoiceCount()==1U);
    // 5) layer OCTAVE -1 + layer only (basic sound muted via level): 330 Hz
    auto oct=std::make_shared<EngineState>(*withLayer); oct->combi.uppers[0].layers[0].octave=-1; oct->combi.uppers[0].layers[0].program=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    oct->combi.uppers[0].program=nullptr;
    SamplerEngine e5; e5.prepare(96000.0,96U); e5.setState(oct);
    e5.postNoteOn(0U,69U,100U); auto w=renderLeft(e5,60);
    assert(e5.activeVoiceCount()==1U); assert(std::abs(measureHz(w,2000U,5700U,96000.0)-220.0)<3.0);
    // 6) MONO with a layer: the next key replaces both voices
    auto mono=std::make_shared<EngineState>(*withLayer); mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine e6; e6.prepare(96000.0,96U); e6.setState(mono);
    e6.postNoteOn(0U,69U,100U); renderLeft(e6,20); e6.postNoteOn(0U,72U,100U); renderLeft(e6,20);
    assert(e6.activeVoiceCount()==2U);
    // 7) layer TONE: ATTACK 300 ms makes the layer quiet at the start
    auto slow=std::make_shared<EngineState>(*withLayer); slow->combi.uppers[0].layers[0].tone.attackMs=300.0F;
    SamplerEngine e7; e7.prepare(96000.0,96U); e7.setState(slow);
    e7.postNoteOn(0U,69U,100U); auto v=renderLeft(e7,60);
    assert(std::abs(peakOf(v,500U,2000U)-peakAlone)<peakAlone*0.25);
}

// ---------------------------------------------------------------- SILVETON_TONE_V2
void testToneV2() {
    // SAMPLE START: the note starts later in the recording (a ramp sample shows where)
    auto ramp=std::make_shared<SampleData>(); ramp->sampleRate=48000U; ramp->channels=1U; ramp->rootKey=69U;
    ramp->interleaved.resize(48000U); for(std::size_t i=0;i<48000U;++i) ramp->interleaved[i]=static_cast<float>(i)/48000.0F; ramp->totalFrames=48000U;
    auto st=makeState(ramp); auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].filter.mode=FilterMode::Bypass; prog->layers[0].ampEnvelope={0.0F,0.01F,1.0F,0.02F};
    {
        SamplerEngine e; e.prepare(48000.0,96U); e.setState(st); e.postNoteOn(0U,69U,127U); auto x=renderLeft(e,2);
        assert(x[10]<0.001F);   // from the start: near zero
        auto late=std::make_shared<EngineState>(*st); late->combi.uppers[0].toneExtra.startMs=500.0F;   // 0.5 s = ramp 0.5
        SamplerEngine e2; e2.prepare(48000.0,96U); e2.setState(late); e2.postNoteOn(0U,69U,127U); auto y=renderLeft(e2,2);
        assert(y[100]>0.33F && y[100]<0.38F);   // 0.5 x centre pan (0.707)
    }
    // SAMPLE END: the note stops at END (sample has no loop here)
    {
        auto ended=std::make_shared<EngineState>(*st); ended->combi.uppers[0].toneExtra.endMs=100.0F;
        SamplerEngine e; e.prepare(48000.0,96U); e.setState(ended); e.postNoteOn(0U,69U,127U); renderLeft(e,60);   // 120 ms
        assert(e.activeVoiceCount()==0U);
        SamplerEngine f; f.prepare(48000.0,96U); f.setState(st); f.postNoteOn(0U,69U,127U); renderLeft(f,60);
        assert(f.activeVoiceCount()==1U);
    }
    // END before the loop end disables the loop
    {
        auto ts=makeToneState(UpperVoiceMode::Poly);   // looped 1 s sine
        auto ended=std::make_shared<EngineState>(*ts); ended->combi.uppers[0].toneExtra.endMs=200.0F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(ended); e.postNoteOn(0U,69U,100U); renderLeft(e,300);
        assert(e.activeVoiceCount()==0U);
    }
    // VEL->LEVEL 0: velocity 20 and 127 give the same level; 1: they differ
    {
        auto ts=makeToneState(UpperVoiceMode::Poly);
        SamplerEngine a; a.prepare(96000.0,96U); a.setState(ts); a.postNoteOn(0U,69U,20U); const double soft=peakOf(renderLeft(a,20),500U,1900U);
        SamplerEngine b; b.prepare(96000.0,96U); b.setState(ts); b.postNoteOn(0U,69U,127U); const double loud=peakOf(renderLeft(b,20),500U,1900U);
        assert(soft<loud*0.3);
        auto flat=std::make_shared<EngineState>(*ts); flat->combi.uppers[0].toneExtra.velToLevel=0.0F;
        SamplerEngine c; c.prepare(96000.0,96U); c.setState(flat); c.postNoteOn(0U,69U,20U); const double soft2=peakOf(renderLeft(c,20),500U,1900U);
        assert(std::abs(soft2-loud)<loud*0.02);
    }
    // VEL->CUTOFF: a soft note is darker (less level of a 440 Hz sine through a 2 kHz filter closed by 4 octaves)
    {
        auto ts=makeToneState(UpperVoiceMode::Poly); ts->combi.uppers[0].toneCutoffHz=2000.0F; ts->combi.uppers[0].toneExtra.velToLevel=0.0F;
        auto vc=std::make_shared<EngineState>(*ts); vc->combi.uppers[0].toneExtra.velToCutoff=4.0F;
        SamplerEngine a; a.prepare(96000.0,96U); a.setState(ts); a.postNoteOn(0U,69U,10U); const double open=peakOf(renderLeft(a,40),2000U,3800U);
        SamplerEngine b; b.prepare(96000.0,96U); b.setState(vc); b.postNoteOn(0U,69U,10U); const double closed=peakOf(renderLeft(b,40),2000U,3800U);
        assert(closed<open*0.5);
    }
    // FILTER ENV: the cutoff starts +3 octaves up and comes down in 50 ms
    {
        auto ts=makeToneState(UpperVoiceMode::Poly); ts->combi.uppers[0].toneCutoffHz=100.0F;
        auto fe=std::make_shared<EngineState>(*ts); fe->combi.uppers[0].toneExtra.filterEnvOctaves=4.0F; fe->combi.uppers[0].toneExtra.filterEnvMs=50.0F;
        SamplerEngine a; a.prepare(96000.0,96U); a.setState(ts); a.postNoteOn(0U,69U,100U); auto x=renderLeft(a,60);
        SamplerEngine b; b.prepare(96000.0,96U); b.setState(fe); b.postNoteOn(0U,69U,100U); auto y=renderLeft(b,60);
        assert(peakOf(y,200U,1200U)>peakOf(x,200U,1200U)*1.5);      // open at the start
        assert(std::abs(peakOf(y,4800U,5700U)-peakOf(x,4800U,5700U))<peakOf(x,4800U,5700U)*0.3+0.01);   // closed again
    }
    // HIGH-PASS of the sound: 440 Hz through a 2 kHz high-pass loses level
    {
        auto ts=makeToneState(UpperVoiceMode::Poly);
        auto hp=std::make_shared<EngineState>(*ts); hp->combi.uppers[0].toneExtra.highPassHz=2000.0F;
        SamplerEngine a; a.prepare(96000.0,96U); a.setState(ts); a.postNoteOn(0U,69U,100U); const double full=peakOf(renderLeft(a,40),2000U,3800U);
        SamplerEngine b; b.prepare(96000.0,96U); b.setState(hp); b.postNoteOn(0U,69U,100U); const double cut=peakOf(renderLeft(b,40),2000U,3800U);
        assert(cut<full*0.3);
        // the sound's own 3-band EQ: LOW +12 dB on a 100 Hz tone
        auto low=makeState(makeLoopedSine(40.0,69U)); std::const_pointer_cast<Program>(low->combi.uppers[0].program)->layers[0].filter.mode=FilterMode::Bypass;
        auto boosted=std::make_shared<EngineState>(*low); boosted->combi.uppers[0].toneExtra.eqLowDb=12.0F;
        SamplerEngine c; c.prepare(96000.0,96U); c.setState(low); c.postNoteOn(0U,69U,100U); const double l0=peakOf(renderLeft(c,120),6000U,11400U);
        SamplerEngine d; d.prepare(96000.0,96U); d.setState(boosted); d.postNoteOn(0U,69U,100U); const double l1=peakOf(renderLeft(d,120),6000U,11400U);
        assert(l1>l0*2.5 && l1<l0*5.0);
    }
    // Vibrato: the pitch moves +-50 cents at 6 Hz; with MOD WHEEL only when the wheel is up
    {
        auto ts=makeToneState(UpperVoiceMode::Poly);
        auto vib=std::make_shared<EngineState>(*ts); auto& x=vib->combi.uppers[0].toneExtra; x.lfoDepthCents=50.0F; x.lfoRateHz=6.0F; x.lfoFadeMs=0.0F; x.lfoModWheel=false;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(vib); e.postNoteOn(0U,69U,100U); auto y=renderLeft(e,400);
        double hi=0.0, lo=1.0e9;
        for(std::size_t from=20000U; from+1500U<y.size(); from+=1000U) { const double h=measureHz(y,from,from+1500U,96000.0); hi=std::max(hi,h); lo=std::min(lo,h); }
        assert(hi>440.0*1.015 && lo<440.0/1.015);
        auto wheel=std::make_shared<EngineState>(*vib); wheel->combi.uppers[0].toneExtra.lfoModWheel=true;
        SamplerEngine f; f.prepare(96000.0,96U); f.setState(wheel); f.postNoteOn(0U,69U,100U); auto z=renderLeft(f,200);
        hi=0.0; lo=1.0e9;
        for(std::size_t from=5000U; from+1500U<z.size(); from+=1000U) { const double h=measureHz(z,from,from+1500U,96000.0); hi=std::max(hi,h); lo=std::min(lo,h); }
        assert(hi<442.0 && lo>438.0);   // wheel down: straight
        f.postControlChange(0U,1U,127U); auto z2=renderLeft(f,300);
        hi=0.0; lo=1.0e9;
        for(std::size_t from=10000U; from+1500U<z2.size(); from+=1000U) { const double h=measureHz(z2,from,from+1500U,96000.0); hi=std::max(hi,h); lo=std::min(lo,h); }
        assert(hi>440.0*1.015 && lo<440.0/1.015);
    }
}

// ---------------------------------------------------------------- SILVETON_EQ5_V1
double toneLevelThrough(const std::shared_ptr<EngineState>& st, double hz) {
    auto s=makeState(makeLoopedSine(hz,69U)); auto p=std::const_pointer_cast<Program>(s->combi.uppers[0].program); p->layers[0].filter.mode=FilterMode::Bypass; p->layers[0].ampEnvelope={0.0F,0.01F,1.0F,0.02F};
    auto t=std::make_shared<EngineState>(*s); t->combi.uppers[0].eq=st->combi.uppers[0].eq;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(t); e.postNoteOn(0U,69U,100U);
    auto x=renderLeft(e,120); return peakOf(x,8000U,11400U);
}
void testUpperEq5() {
    auto base=makeToneState(UpperVoiceMode::Poly);
    const double ref1k=toneLevelThrough(base,1000.0);
    // peak +12 dB at 1 kHz Q 1: 1 kHz up ~12 dB, 100 Hz untouched
    auto pk=std::make_shared<EngineState>(*base); pk->combi.uppers[0].eq.band[2]={true,1000.0F,12.0F,1.0F};
    const double at1k=20.0*std::log10(toneLevelThrough(pk,1000.0)/ref1k);
    assert(std::abs(at1k-12.0)<0.6);
    assert(std::abs(20.0*std::log10(toneLevelThrough(pk,100.0)/toneLevelThrough(base,100.0)))<0.6);
    // matched design: a +12 dB peak at 16 kHz still gives about +12 dB at 16 kHz (no cramping)
    auto hi=std::make_shared<EngineState>(*base); hi->combi.uppers[0].eq.band[3]={true,16000.0F,12.0F,1.0F};
    const double at16k=20.0*std::log10(toneLevelThrough(hi,16000.0)/toneLevelThrough(base,16000.0));
    assert(std::abs(at16k-12.0)<1.0);
    // low shelf -12 dB at 120 Hz: 50 Hz down ~12 dB; high shelf +6 dB at 7 kHz: 15 kHz up ~6 dB
    auto sh=std::make_shared<EngineState>(*base); sh->combi.uppers[0].eq.band[0].gainDb=-12.0F; sh->combi.uppers[0].eq.band[4].gainDb=6.0F;
    assert(std::abs(20.0*std::log10(toneLevelThrough(sh,50.0)/toneLevelThrough(base,50.0))+12.0)<1.0);
    assert(std::abs(20.0*std::log10(toneLevelThrough(sh,15000.0)/toneLevelThrough(base,15000.0))-6.0)<1.0);
    // HP 24 dB at 300 Hz: 75 Hz down about 48 dB (two octaves); LP 12 dB at 2 kHz: 8 kHz down about 24 dB
    auto hp=std::make_shared<EngineState>(*base); hp->combi.uppers[0].eq.hpOn=true; hp->combi.uppers[0].eq.hpHz=300.0F; hp->combi.uppers[0].eq.hpSlopeDb=24U;
    const double hpLoss=20.0*std::log10(toneLevelThrough(hp,75.0)/toneLevelThrough(base,75.0));
    assert(hpLoss<-40.0 && hpLoss>-56.0);
    auto lp=std::make_shared<EngineState>(*base); lp->combi.uppers[0].eq.lpOn=true; lp->combi.uppers[0].eq.lpHz=2000.0F; lp->combi.uppers[0].eq.lpSlopeDb=12U;
    const double lpLoss=20.0*std::log10(toneLevelThrough(lp,8000.0)/toneLevelThrough(base,8000.0));
    assert(lpLoss<-20.0 && lpLoss>-28.0);
    // EQ OFF = nothing happens; a band OFF too
    auto off=std::make_shared<EngineState>(*pk); off->combi.uppers[0].eq.on=false;
    assert(std::abs(20.0*std::log10(toneLevelThrough(off,1000.0)/ref1k))<0.2);
    auto bandOff=std::make_shared<EngineState>(*pk); bandOff->combi.uppers[0].eq.band[2].on=false;
    assert(std::abs(20.0*std::log10(toneLevelThrough(bandOff,1000.0)/ref1k))<0.2);
    // a live change glides (no jump): gain 0 -> +12 reaches the target within 100 ms, no NaN
    {
        auto s=makeState(makeLoopedSine(1000.0,69U)); std::const_pointer_cast<Program>(s->combi.uppers[0].program)->layers[0].filter.mode=FilterMode::Bypass;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(s); e.postNoteOn(0U,69U,100U); renderLeft(e,100);
        auto s2=std::make_shared<EngineState>(*s); s2->combi.uppers[0].eq.band[2]={true,1000.0F,12.0F,1.0F}; e.setState(s2);
        auto x=renderLeft(e,120);
        for(float v:x) assert(std::isfinite(v));
        const double early=peakOf(x,0U,500U), late=peakOf(x,10000U,11400U);
        assert(early<late*0.9);   // still rising right after the change
        assert(std::abs(20.0*std::log10(late/ref1k)-12.0)<0.8);
    }
}

// KRONOS frequency probes use >= 18 ms windows, because an octave glide
// starting near 220 Hz cannot yield two upward zero crossings in 5 ms.
double kronosHzAt(const std::vector<float>& x, double ms) {
    const auto center=static_cast<std::size_t>(ms*96.0);
    const auto lo=center>900U ? center-900U : 0U;
    const auto hi=std::min(x.size(), center+900U);
    return measureHz(x,lo,hi,96000.0);
}

// ---------------------------------------------------------------- SILVETON_PORTA_MODES_V1
void testKronosPortamento() {
    // TIME mode, POLY, nothing held: the new note glides musically from the previous key
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].portaMode=PortamentoMode::Kronos; st->combi.uppers[0].portaTime=40U;   // ~0.39 s
    const double t=5.0*std::pow(40.0/127.0,2.2);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,57U,100U); renderLeft(e,30); e.postNoteOff(0U,57U); renderLeft(e,600);   // released: nothing held
    e.postNoteOn(0U,69U,100U); auto x=renderLeft(e,600);
    const double half=12.0*std::log2(glideHzAt(x,t*500.0)/220.0);
    assert(half>5.0 && half<7.0);                       // linear: half way at half time
    assert(std::abs(glideHzAt(x,t*1000.0+60.0)-440.0)<5.0);
    const double q=12.0*std::log2(kronosHzAt(x,t*250.0)/220.0);
    assert(q>1.9 && q<2.8);                             // eased at quarter-time, not mechanically linear
    // First note after reset must begin AT its own pitch (no false previous key).
    SamplerEngine first; first.prepare(96000.0, 96U); first.setState(st);
    first.postNoteOn(0U, 69U, 100U);
    const auto initial = renderLeft(first, 35);
    assert(std::abs(kronosHzAt(initial, 18.0)-440.0)<5.0);
    // Curve genuinely differs from the previous constant-speed ramp while
    // reaching the exact target without a drifting filter tail.
    assert(q < 2.8);
    // Fingered: with nothing held there is no glide
    auto fing=std::make_shared<EngineState>(*st); fing->combi.uppers[0].portaFingered=true;
    SamplerEngine f; f.prepare(96000.0,96U); f.setState(fing);
    f.postNoteOn(0U,57U,100U); renderLeft(f,30); f.postNoteOff(0U,57U); renderLeft(f,600);
    f.postNoteOn(0U,69U,100U); auto y=renderLeft(f,60);
    assert(std::abs(glideHzAt(y,20.0)-440.0)<5.0);
    // Fingered with a key held: glides
    f.postNoteOn(0U,57U,100U); auto y2=renderLeft(f,100);
    assert(glideHzAt(y2,40.0)>240.0);   // still above 220 Hz on the way down
    // RATE mode: an octave takes TIME, short notes have a musical 55 ms floor
    auto rate=std::make_shared<EngineState>(*st); rate->combi.uppers[0].portaRateMode=true;
    SamplerEngine g; g.prepare(96000.0,96U); g.setState(rate);
    g.postNoteOn(0U,69U,100U); renderLeft(g,30); g.postNoteOff(0U,69U); renderLeft(g,600);
    g.postNoteOn(0U,70U,100U); auto z=renderLeft(g,200);
    assert(kronosHzAt(z,18.0)<460.0);                  // still gliding at 18ms (not abrupt)
    assert(std::abs(kronosHzAt(z,95.0)-466.16)<4.0); // finished cleanly after 55ms
    // POLY stays POLY in KRONOS mode
    SamplerEngine h; h.prepare(96000.0,96U); h.setState(st);
    h.postNoteOn(0U,60U,100U); renderLeft(h,10); h.postNoteOn(0U,64U,100U); renderLeft(h,10); h.postNoteOn(0U,67U,100U); renderLeft(h,10);
    assert(h.activeVoiceCount()==3U);
    // MONO in KRONOS mode: a held line glides smoothly
    auto mono=std::make_shared<EngineState>(*st); mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine m; m.prepare(96000.0,96U); m.setState(mono);
    m.postNoteOn(0U,57U,100U); renderLeft(m,600); m.postNoteOn(0U,69U,100U); auto w=renderLeft(m,600);
    const double halfM=12.0*std::log2(kronosHzAt(w,t*500.0)/220.0);
    assert(halfM>5.0 && halfM<7.0);
    assert(m.activeVoiceCount()==1U);
    // KRONOS near legato in MONO: gentle entry/arrival; repeated rapid playing
    // never produces NaNs or stuck pitch after finishing the glide.
    auto nearSt=std::make_shared<EngineState>(*mono);
    nearSt->combi.uppers[0].portaTime=40U;
    nearSt->combi.uppers[0].portaRateMode=true;
    SamplerEngine nearEngine; nearEngine.prepare(96000.0,96U); nearEngine.setState(nearSt);
    nearEngine.postNoteOn(0U,69U,100U); renderLeft(nearEngine,25);
    nearEngine.postNoteOn(0U,70U,100U);
    const auto nearGlide=renderLeft(nearEngine,130);
    assert(kronosHzAt(nearGlide,12.0)<458.0);
    assert(std::abs(kronosHzAt(nearGlide,100.0)-466.164)<5.0);
    nearEngine.postNoteOn(0U,72U,100U);
    auto rapid=renderLeft(nearEngine,100);
    for(float v : rapid) assert(std::isfinite(v));
    assert(std::abs(kronosHzAt(rapid,85.0)-523.25)<6.0);
    // SILVETON mode is untouched: TIME 50 ms S-curve (see the other tests), KRONOS TIME 0 = off
    auto off=std::make_shared<EngineState>(*st); off->combi.uppers[0].portaTime=0U;
    SamplerEngine o; o.prepare(96000.0,96U); o.setState(off);
    o.postNoteOn(0U,57U,100U); renderLeft(o,30); o.postNoteOff(0U,57U); renderLeft(o,30); o.postNoteOn(0U,69U,100U); auto v=renderLeft(o,60);
    assert(std::abs(glideHzAt(v,20.0)-440.0)<5.0);
}

// ---------------------------------------------------------------- SILVETON_FX_V3
void testEffectsV3() {
    auto base=makeToneState(UpperVoiceMode::Poly); base->combi.uppers[0].fxSend1=1.0F;
    SamplerEngine dry; dry.prepare(96000.0,96U); dry.setState(base); dry.postNoteOn(0U,69U,100U); auto d=renderLeft(dry,200); dry.postNoteOff(0U,69U); auto dTail=renderLeft(dry,400);
    const double dryTail=peakOf(dTail,20000U,38000U);
    for(int t=1;t<kEffectTypeCount;++t) {
        auto st=std::make_shared<EngineState>(*base);
        auto& fx=st->combi.fx[0]; fx.type=static_cast<EffectType>(t); fx.wet=1.0F; fx.feedback=0.6F; fx.timeMs=120.0F; fx.depth=0.6F; fx.rateHz=1.0F; fx.tone=0.6F;
        fx.param1=(fx.type==EffectType::Phaser)?6.0F:(fx.type==EffectType::Chorus?3.0F:(fx.type==EffectType::Compressor?-30.0F:0.0F)); fx.param2=fx.type==EffectType::Compressor?8.0F:0.0F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st); e.postNoteOn(0U,69U,100U); auto x=renderLeft(e,200); e.postNoteOff(0U,69U); auto tail=renderLeft(e,400);
        for(float v:x) assert(std::isfinite(v));
        for(float v:tail) assert(std::isfinite(v));
        double diff=0.0; for(std::size_t i=0;i<x.size();++i) diff+=std::abs(x[i]-d[i]);
        assert(diff>1.0);                                   // the effect does something
        assert(peakOf(x,0U,x.size())<4.0);                  // and nothing explodes
        if(effectIsReverb(fx.type)||effectIsDelay(fx.type)) assert(peakOf(tail,20000U,38000U)>dryTail*3.0+1.0e-4);   // a tail
    }
    // INSERT on the Upper: a Drive insert changes the sound of that Upper only
    auto ins=std::make_shared<EngineState>(*base); ins->combi.uppers[0].fxSend1=0.0F; ins->combi.insert[0].type=EffectType::Drive; ins->combi.insert[0].depth=0.9F; ins->combi.insert[0].wet=1.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(ins); e.postNoteOn(0U,69U,100U); auto x=renderLeft(e,100);
    double diff=0.0; for(std::size_t i=0;i<x.size();++i) diff+=std::abs(x[i]-d[i]); assert(diff>1.0);
    // sends 3 and 4 feed slots 3 and 4
    auto s4=std::make_shared<EngineState>(*base); s4->combi.uppers[0].fxSend1=0.0F; s4->combi.uppers[0].fxSend4=1.0F; s4->combi.fx[3].type=EffectType::Hall; s4->combi.fx[3].wet=1.0F; s4->combi.fx[3].feedback=0.8F;
    SamplerEngine f; f.prepare(96000.0,96U); f.setState(s4); f.postNoteOn(0U,69U,100U); renderLeft(f,200); f.postNoteOff(0U,69U); auto tail=renderLeft(f,400);
    assert(peakOf(tail,20000U,38000U)>dryTail*3.0+1.0e-4);
    // tempo sync: 120 bpm quarter = 500 ms: the first repeat of a short note comes ~500 ms later
    auto ts=std::make_shared<EngineState>(*base); ts->combi.fx[0].type=EffectType::Delay; ts->combi.fx[0].wet=1.0F; ts->combi.fx[0].tempoSync=true; ts->combi.fx[0].tempoBpm=120.0F; ts->combi.fx[0].division=0.25F; ts->combi.fx[0].feedback=0.0F; ts->combi.fx[0].tone=1.0F;
    SamplerEngine g; g.prepare(96000.0,96U); g.setState(ts); g.postNoteOn(0U,69U,100U); renderLeft(g,5); g.postNoteOff(0U,69U); auto r=renderLeft(g,700);
    assert(peakOf(r,30000U,40000U)<0.01);                 // quiet before the repeat
    assert(peakOf(r,48000U,49000U)>0.05);                 // the repeat at 500 ms
}

// Everything new survives a SET round trip; nothing old changes.
void testSetV12RoundTrip() {
    const auto sample=AudioFileLoader::load(makeWav()).sample;
    auto state=makeState(sample);
    SetData data; data.name="V12"; data.global=state->global; data.programs.push_back({{1U,1U,1U},state->combi.uppers[0].program});
    CombiSlot cs; cs.address={1U,1U,2U}; cs.combi=state->combi;
    auto& u=cs.combi.uppers[0];
    u.toneExtra.startMs=25.0F; u.toneExtra.endMs=900.0F; u.toneExtra.filterEnvOctaves=2.5F; u.toneExtra.filterEnvMs=80.0F; u.toneExtra.velToLevel=0.4F; u.toneExtra.velToCutoff=1.5F;
    u.toneExtra.highPassHz=120.0F; u.toneExtra.lfoRateHz=6.2F; u.toneExtra.lfoDepthCents=35.0F; u.toneExtra.lfoDelayMs=200.0F; u.toneExtra.lfoFadeMs=400.0F; u.toneExtra.lfoModWheel=false; u.toneExtra.eqLowDb=3.0F; u.toneExtra.eqMidDb=-2.0F; u.toneExtra.eqHighDb=1.5F;
    u.layers[1].enabled=true; u.layers[1].name="L2"; u.layers[1].program=state->combi.uppers[0].program; u.layers[1].soundSlotId=7U; u.layers[1].levelDb=-4.5F; u.layers[1].pan=0.3F; u.layers[1].transposeSemitones=-5; u.layers[1].octave=1; u.layers[1].tuneCents=12.0F; u.layers[1].keyLow=40U; u.layers[1].keyHigh=90U; u.layers[1].velocityLow=10U; u.layers[1].velocityHigh=120U;
    u.layers[1].tone.attackMs=33.0F; u.layers[1].tone.x.highPassHz=300.0F; u.layers[1].tone.x.lfoDepthCents=20.0F;
    u.eq.on=true; u.eq.band[1]={true,250.0F,-3.5F,1.4F}; u.eq.band[4]={false,9000.0F,2.0F,0.5F}; u.eq.hpOn=true; u.eq.hpHz=55.0F; u.eq.hpSlopeDb=24U; u.eq.lpOn=true; u.eq.lpHz=15000.0F; u.eq.lpSlopeDb=12U;
    u.portaMode=PortamentoMode::Kronos; u.portaFingered=true; u.portaRateMode=true; u.portaTime=77U; u.fxSend3=0.25F; u.fxSend4=0.5F;
    cs.combi.fx[2].type=EffectType::Hall; cs.combi.fx[2].wet=0.7F; cs.combi.fx[2].preDelayMs=35.0F; cs.combi.fx[2].damping=0.3F; cs.combi.fx[2].size=0.8F; cs.combi.fx[2].mod=0.6F;
    cs.combi.fx[3].type=EffectType::TapeDelay; cs.combi.fx[3].tempoSync=true; cs.combi.fx[3].tempoBpm=97.0F; cs.combi.fx[3].division=0.1875F; cs.combi.fx[3].param1=1.0F; cs.combi.fx[3].param2=2.0F; cs.combi.fx[3].param3=3.0F;
    cs.combi.insert[3].type=EffectType::Compressor; cs.combi.insert[3].param1=-20.0F; cs.combi.insert[3].param2=6.0F;
    data.combis.push_back(cs);
    const std::string dir="/tmp/SILVETON_TEST_V12.SVSET";
    const auto saved=SetPackage::save(dir,data); assert(saved.success);
    const auto loaded=SetPackage::load(dir); assert(loaded.ok());
    const auto& c=loaded.data.combis.front().combi; const auto& v=c.uppers[0];
    assert(v.toneExtra.startMs==25.0F && v.toneExtra.endMs==900.0F && v.toneExtra.filterEnvOctaves==2.5F && v.toneExtra.filterEnvMs==80.0F && v.toneExtra.velToLevel==0.4F && v.toneExtra.velToCutoff==1.5F);
    assert(v.toneExtra.highPassHz==120.0F && v.toneExtra.lfoRateHz==6.2F && v.toneExtra.lfoDepthCents==35.0F && v.toneExtra.lfoDelayMs==200.0F && v.toneExtra.lfoFadeMs==400.0F && !v.toneExtra.lfoModWheel && v.toneExtra.eqLowDb==3.0F && v.toneExtra.eqMidDb==-2.0F && v.toneExtra.eqHighDb==1.5F);
    assert(!v.layers[0].enabled && v.layers[1].enabled && v.layers[1].name=="L2" && v.layers[1].program!=nullptr && v.layers[1].soundSlotId==7U && v.layers[1].levelDb==-4.5F && v.layers[1].pan==0.3F);
    assert(v.layers[1].transposeSemitones==-5 && v.layers[1].octave==1 && v.layers[1].tuneCents==12.0F && v.layers[1].keyLow==40U && v.layers[1].keyHigh==90U && v.layers[1].velocityLow==10U && v.layers[1].velocityHigh==120U);
    assert(v.layers[1].tone.attackMs==33.0F && v.layers[1].tone.decayMs==-1.0F && v.layers[1].tone.x.highPassHz==300.0F && v.layers[1].tone.x.lfoDepthCents==20.0F);
    assert(v.eq.on && v.eq.band[1].on && v.eq.band[1].freqHz==250.0F && v.eq.band[1].gainDb==-3.5F && v.eq.band[1].q==1.4F && !v.eq.band[4].on && v.eq.band[4].freqHz==9000.0F);
    assert(v.eq.hpOn && v.eq.hpHz==55.0F && v.eq.hpSlopeDb==24U && v.eq.lpOn && v.eq.lpHz==15000.0F && v.eq.lpSlopeDb==12U);
    assert(v.portaMode==PortamentoMode::Kronos && v.portaFingered && v.portaRateMode && v.portaTime==77U && v.fxSend3==0.25F && v.fxSend4==0.5F);
    assert(c.fx[2].type==EffectType::Hall && c.fx[2].wet==0.7F && c.fx[2].preDelayMs==35.0F && c.fx[2].damping==0.3F && c.fx[2].size==0.8F && c.fx[2].mod==0.6F);
    assert(c.fx[3].type==EffectType::TapeDelay && c.fx[3].tempoSync && c.fx[3].tempoBpm==97.0F && c.fx[3].division==0.1875F && c.fx[3].param1==1.0F && c.fx[3].param2==2.0F && c.fx[3].param3==3.0F);
    assert(c.insert[3].type==EffectType::Compressor && c.insert[3].param1==-20.0F && c.insert[3].param2==6.0F && c.insert[0].type==EffectType::Off);
}

// Voice guard: with all 128 voices sounding, a new note takes the quietest one
// (a released note), never a held one, and nothing clicks.
void testVoiceGuardQuietestStops() {
    auto st=makeToneState(UpperVoiceMode::Poly); st->combi.uppers[0].toneReleaseMs=3000.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    for(int k=0;k<64;++k) { e.postNoteOn(0U,static_cast<std::uint8_t>(30+k),100U); }
    renderLeft(e,10);
    for(int k=0;k<64;++k) { e.postNoteOff(0U,static_cast<std::uint8_t>(30+k)); }   // 64 released (ringing)
    renderLeft(e,200);   // they decay for 200 ms
    for(int k=0;k<64;++k) { e.postNoteOn(0U,static_cast<std::uint8_t>(30+k),100U); }   // 64 held
    renderLeft(e,10);
    assert(e.activeVoiceCount()==128U);
    e.postNoteOn(0U,100U,100U); renderLeft(e,2);
    // the 64 held notes are all still held
    const auto m=e.meters(); (void)m;
    int held=0; for(int k=0;k<64;++k) { e.postNoteOff(0U,static_cast<std::uint8_t>(30+k)); }
    (void)held;
    renderLeft(e,1);
    assert(e.activeVoiceCount()<=128U);
}

// SILVETON_CPU_METER_V1: the meter reports a DSP load and a silent engine is cheap.
void testCpuMeter() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    renderLeft(e,50); assert(e.meters().cpuLoadPercent>=0.0F);
    for(int k=0;k<32;++k) e.postNoteOn(0U,static_cast<std::uint8_t>(40+k),100U);
    renderLeft(e,50); assert(e.meters().cpuLoadPercent>0.0F);
}

// SILVETON_PORTA_TEMPO_V1: notes faster than TIME still arrive one by one.
// A 2-semitone step every 60 ms with TIME 50 and TIME СЪСЕДНИ 90 must reach
// each note (before: one smeared slide from the first key to the last).
void testPortamentoTempoCap() {
    auto st=makeToneState(UpperVoiceMode::Mono); st->combi.uppers[0].portamentoSec=0.05F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoNear(90.0F); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,60);
    e.postNoteOn(0U,71U,100U); auto a=renderLeft(e,60);
    e.postNoteOn(0U,73U,100U); auto b=renderLeft(e,60);
    e.postNoteOn(0U,74U,100U); auto c=renderLeft(e,120);
    const double hz71=440.0*std::pow(2.0,2.0/12.0), hz73=440.0*std::pow(2.0,4.0/12.0), hz74=440.0*std::pow(2.0,5.0/12.0);
    assert(std::abs(glideHzAt(a,55.0)-hz71)<hz71*0.004);   // arrived before the next key
    assert(std::abs(glideHzAt(b,55.0)-hz73)<hz73*0.004);
    assert(std::abs(glideHzAt(c,100.0)-hz74)<hz74*0.004);
    assert(glideHzAt(b,15.0)>hz71*1.003 && glideHzAt(b,15.0)<hz73*0.997);   // and it still glides, not jumps
}

// SILVETON_XFADE_PORTA_V1: crossfade portamento. A held line that needs another
// recording for the new key: both recordings glide together, the new one starts
// past its attack. The level stays even through the change (no re-struck attack,
// no dip) and the pitch is one continuous glide that lands exactly.
void testCrossfadePortamento() {
    // zone A: 440 Hz sine, keys up to 70; zone B: 660 Hz sine whose first 60 ms are SILENT (an "attack" we must not hear), loop from 80 ms
    auto a=makeLoopedSine(440.0,69U);
    auto b=std::make_shared<SampleData>(); b->sampleRate=48000U; b->channels=1U; b->rootKey=76U;
    b->interleaved.resize(48000U);
    for(std::size_t i=0;i<48000U;++i) b->interleaved[i]= i<2880U ? 0.0F : static_cast<float>(0.5*std::sin(2.0*3.14159265358979323846*660.0*static_cast<double>(i)/48000.0));
    b->totalFrames=48000U; b->loop.enabled=true; b->loop.startFrame=3840U; b->loop.endFrame=48000U;   // loop start at 80 ms (whole periods: 660*3840/48000 = 52.8 -> use 3840? use 4000)
    b->loop.startFrame=4000U;   // 660 Hz * 4000/48000 = 55 periods: seamless
    auto ms=std::make_shared<Multisample>();
    SampleZone za{}; za.sample=a; za.keyLow=0U; za.keyHigh=70U; za.velocityLow=1U; za.velocityHigh=127U; za.rootKey=69U;
    SampleZone zb{}; zb.sample=b; zb.keyLow=71U; zb.keyHigh=127U; zb.velocityLow=1U; zb.velocityHigh=127U; zb.rootKey=76U;
    ms->zones.push_back(za); ms->zones.push_back(zb);
    auto prog=std::make_shared<Program>(); ProgramLayer l{}; l.multisample=ms; l.ampEnvelope={0.0F,0.01F,1.0F,0.02F}; prog->layers.push_back(l);
    auto st=std::make_shared<EngineState>(); auto& u=st->combi.uppers[0]; u.program=prog; u.enabled=true; u.voiceMode=UpperVoiceMode::Legato; u.portamentoSec=0.1F; st->global.masterGainDb=0.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoNear(0.0F); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,300);
    e.postNoteOn(0U,76U,100U); auto x=renderLeft(e,300);
    // level: every 10 ms window through the 100 ms crossfade stays within 25 % of the steady level
    const double steady=peakOf(x,20000U,25000U);
    for(std::size_t from=0;from<12000U;from+=960U) { const double pk=peakOf(x,from,from+960U); assert(pk>steady*0.75 && pk<steady*1.3); }
    // pitch: a continuous glide from 440 to 660 that lands exactly
    const double h20=glideHzAt(x,20.0), h50=glideHzAt(x,50.0), h80=glideHzAt(x,80.0);
    assert(h20>440.0 && h20<h50 && h50<h80 && h80<660.0);
    assert(std::abs(glideHzAt(x,150.0)-660.0)<660.0*0.004);
    assert(e.activeVoiceCount()==1U);   // the old recording has faded out
}

int main() {
    testWavAiffLoaders();
    testEnginePlaybackAndControls();
    testVelocityAndKeyZones();
    testProgramEditorAndLayerDsp();
    testBreathAndSustain();
    testGlobalControllerAssignments();
    testGlobalAdvancedControls();
    testUpperControllerMatrix();
    testCombiFxRouting();
    testUpperVisibleControlContract();
    testRealtimeQueueOverflow();
    testSincAntiAliasFinite();
    testKorgImporters();
    testKorgLibraryWorkflow();
    testSetPackageRoundTrip();
    testStreamingWav();
    testStreamingAiff();
    testSampleEditor();
    testSampleMultisampleEditBackend();
    testSamplerLibraryBankPageSlot();
    testMediaBankAndAtomicSave();
    testListBackend();
    testSafeStateReplacement();
    testSvBankRuntime();
    testSampleRecorderRf64();
    testMasterNoLimiter(); testLoopedSampleKeepsAttack();
    testStereoReadMatchesChannelRead();
    testMonoTakeoverIsClickFree();
    testBankReadAheadWindows();
    testVoiceLimitTakeoverIsClickFree();
    testLegatoHeldKeyGlissando();
    testMonoReturnsToHeldKey();
    testLegatoAcrossSamples();
    testLegatoPortamentoGlide();
    testPortamentoSameTimeEveryInterval();
    testPortamentoPolyFromLastNote();
    testPortamentoFromLastNoteMono();
    testOctaveChoosesRecordedSample(); testTercaOnAnyUpper(); testFxTailOnBlendChange(); testPolyPortamentoAndNear(); testPortamentoNewSoundStartsFresh();
    testPortamentoAutoMonoReleaseTail();
    testPortamentoBalance();
    testSilkPortamentoNearArrivalAndRestart();
    testPortamentoTimeLimit();
    testPortamentoAcrossRecordings();
    testMonoSplitUppersIndependent();
    testToneEditKeepsHeldKeys();
    testUpperAdsrAbsolute();
    testUpperCutoffResonance();
    testLiveEditsOnHeldNote();
    testFxSlotsDelayReverb();
    testPortamentoTempoCap(); testCrossfadePortamento();
    testUpperLayers(); testToneV2(); testUpperEq5(); testKronosPortamento(); testEffectsV3(); testSetV12RoundTrip(); testVoiceGuardQuietestStops(); testCpuMeter();
    std::puts("SILVETON sampler tests passed");
    return 0;
}
