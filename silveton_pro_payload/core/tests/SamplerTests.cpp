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
    st->global.limiterEnabled=true; st->global.limiterCeilingDb=-0.3F;
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
    parallel->combi.fx1.type=EffectType::Delay;
    parallel->combi.fx1.wet=0.8F;
    parallel->combi.fx1.timeMs=1.0F;
    parallel->combi.fx1.feedback=0.0F;
    parallel->combi.fx2.type=EffectType::Delay;
    parallel->combi.fx2.wet=0.8F;
    parallel->combi.fx2.timeMs=1.0F;
    parallel->combi.fx2.feedback=0.0F;
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

    auto eq=std::make_shared<EngineState>(*base); eq->combi.uppers[0].eqLowDb=12.0F; eq->combi.uppers[0].eqMidDb=-12.0F; eq->combi.uppers[0].eqHighDb=6.0F;
    auto [eqOut,_m15]=render(eq,{note}); diff=0.0; for(std::size_t i=0;i<baseOut.size();++i)diff+=std::abs(baseOut[i]-eqOut[i]); assert(diff>0.1);

    // FX tab: each send must feed the real shared COMBI effect bus.
    auto fx=std::make_shared<EngineState>(*base); fx->combi.fx1.type=EffectType::Delay; fx->combi.fx1.wet=1.0F; fx->combi.fx1.timeMs=1.0F; fx->combi.fx1.feedback=0.0F; fx->combi.uppers[0].fxSend1=1.0F;
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
        e.postNoteOn(0U,69U,100U); renderLeft(e,40);
        e.postNoteOn(0U,81U,100U);
        auto x=renderLeft(e,400); // 400 ms
        const double early=measureHz(x,300U,1500U,96000.0);
        const double late=measureHz(x,34000U,38000U,96000.0);
        assert(early>450.0 && early<880.0);   // still gliding
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
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,40);
        e.postNoteOn(0U,static_cast<std::uint8_t>(target),100U);
        auto x=renderLeft(e,120);
        const double goal=440.0*std::pow(2.0,(target-69)/12.0);
        const double half=440.0*std::pow(2.0,(target-69)/24.0);   // half way in semitones
        assert(std::abs(glideHzAt(x,25.0)-half)<half*0.03);         // exactly half way at 25 ms
        assert(std::abs(glideHzAt(x,70.0)-goal)<goal*0.006);        // arrived after 50 ms
    }
}

// Like the SILVETON keyboard processor: while portamento is ON a POLY Upper
// becomes MONO (one line, last key wins) and every note glides from the last
// note - also after the key was released. OFF = POLY again, no glide.
void testPortamentoPolyFromLastNote() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    st->combi.uppers[0].portamentoSec=0.08F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,40);               // 69 held
    e.postNoteOn(0U,81U,100U);                                  // 69 is taken over, 81 glides from it
    auto x=renderLeft(e,150);
    assert(e.activeVoiceCount()==1U);                           // automatic MONO: one line
    assert(std::abs(glideHzAt(x,40.0)-622.25)<622.25*0.03);    // half way 440 -> 880
    assert(std::abs(glideHzAt(x,110.0)-880.0)<880.0*0.006);
    e.postNoteOff(0U,81U,0U);                                   // back to the still held 69 - glides
    auto back=renderLeft(e,150);
    assert(std::abs(glideHzAt(back,40.0)-622.25)<622.25*0.03);
    assert(std::abs(glideHzAt(back,110.0)-440.0)<440.0*0.006);
    e.postNoteOff(0U,69U,0U); renderLeft(e,200);
    assert(e.activeVoiceCount()==0U);
    e.postNoteOn(0U,81U,100U);                                  // detached: still glides from 69
    auto d=renderLeft(e,150);
    assert(std::abs(glideHzAt(d,40.0)-622.25)<622.25*0.03);
    assert(std::abs(glideHzAt(d,110.0)-880.0)<880.0*0.006);
    // Portamento OFF: POLY chords again, no glide
    auto off=std::make_shared<EngineState>(*st);
    off->combi.uppers[0].portamentoSec=0.0F;
    e.setState(off);
    e.postNoteOn(0U,69U,100U); e.postNoteOn(0U,76U,100U);
    auto y=renderLeft(e,40);
    assert(e.activeVoiceCount()>=2U);
    (void)y;
    e.postPanic(); renderLeft(e,10);
    // Panic forgets the last note: the next one does not glide
    e.setState(st);
    e.postNoteOn(0U,81U,100U); renderLeft(e,20); e.postPanic(); renderLeft(e,10);
    e.postNoteOn(0U,69U,100U);
    auto z=renderLeft(e,40);
    assert(std::abs(glideHzAt(z,10.0)-440.0)<440.0*0.006);
}

// MONO / LEGATO: detached and legato notes both glide from the last note.
void testPortamentoFromLastNoteMono() {
    for(auto mode : {UpperVoiceMode::Mono, UpperVoiceMode::Legato}) {
        auto st=makeToneState(mode);
        st->combi.uppers[0].portamentoSec=0.06F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,40);
        e.postNoteOff(0U,69U,0U); renderLeft(e,200);            // gone completely
        e.postNoteOn(0U,81U,100U);                              // detached: glides from 69
        auto x=renderLeft(e,100);
        assert(std::abs(glideHzAt(x,30.0)-622.25)<622.25*0.03);
        assert(std::abs(glideHzAt(x,80.0)-880.0)<880.0*0.006);
        e.postNoteOn(0U,69U,100U);                              // legato from 81
        auto y=renderLeft(e,100);
        assert(std::abs(glideHzAt(y,30.0)-622.25)<622.25*0.03);
        assert(std::abs(glideHzAt(y,80.0)-440.0)<440.0*0.006);
        e.postNoteOff(0U,69U,0U);                               // back to the held 81
        auto z=renderLeft(e,100);
        assert(std::abs(glideHzAt(z,30.0)-622.25)<622.25*0.03);
        assert(std::abs(glideHzAt(z,80.0)-880.0)<880.0*0.006);
    }
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

// Automatic MONO of a POLY Upper: like the processor (new note, then Note Off
// of the old one) the old note ends with its own release; a MONO Upper cuts it.
// CC#123 forgets the last note.
void testPortamentoAutoMonoReleaseTail() {
    auto st=makeToneState(UpperVoiceMode::Poly);
    auto prog=std::const_pointer_cast<Program>(st->combi.uppers[0].program);
    prog->layers[0].ampEnvelope={0.0F,0.01F,1.0F,0.3F};          // 300 ms release
    st->combi.uppers[0].portamentoSec=0.03F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,40);
    e.postNoteOn(0U,76U,100U); renderLeft(e,50);
    assert(e.activeVoiceCount()==2U);                             // old note still in its release
    renderLeft(e,600);
    assert(e.activeVoiceCount()==1U);
    auto mono=std::make_shared<EngineState>(*st);
    mono->combi.uppers[0].voiceMode=UpperVoiceMode::Mono;
    SamplerEngine f; f.prepare(96000.0,96U); f.setState(mono);
    f.postNoteOn(0U,69U,100U); renderLeft(f,40);
    f.postNoteOn(0U,76U,100U); renderLeft(f,20);
    assert(f.activeVoiceCount()==1U);                             // MONO: the old one is cut short
    // CC#123 on the channel: the next note starts clean
    e.postControlChange(0U,123U,0U); renderLeft(e,10);
    e.postNoteOn(0U,81U,100U);
    auto x=renderLeft(e,30);
    assert(std::abs(glideHzAt(x,10.0)-880.0)<880.0*0.006);
}

// PORTA BALANCE (the processor's number, default 11): 11 = every interval in
// TIME; a smaller number makes the processor send a smaller CC#5, so the
// PA600's glide - and ours - gets shorter.
void testPortamentoBalance() {
    SamplerEngine probe; assert(std::abs(probe.portamentoBalance()-11.0F)<1.0e-6F);
    for(float bal : {11.0F, 8.0F}) {
        auto st=makeToneState(UpperVoiceMode::Legato);
        st->combi.uppers[0].portamentoSec=0.05F;
        SamplerEngine e; e.prepare(96000.0,96U); e.setPortamentoBalance(bal); e.setState(st);
        e.postNoteOn(0U,69U,100U); renderLeft(e,40);
        e.postNoteOn(0U,81U,100U);
        auto x=renderLeft(e,80);
        const double at30=glideHzAt(x,30.0);
        if(bal>10.0F) assert(at30<850.0);                          // 11: still gliding at 30 of 50 ms
        else assert(std::abs(at30-880.0)<880.0*0.006);             // 8: already there (~22 ms)
        assert(std::abs(glideHzAt(x,70.0)-880.0)<880.0*0.006);
    }
    SamplerEngine c; c.setPortamentoBalance(100.0F); assert(std::abs(c.portamentoBalance()-40.0F)<1.0e-6F);
}

// TIME is 1..100 ms: an older SET with a longer time still glides in 100 ms.
void testPortamentoTimeLimit() {
    auto st=makeToneState(UpperVoiceMode::Legato);
    st->combi.uppers[0].portamentoSec=2.0F;
    SamplerEngine e; e.prepare(96000.0,96U); e.setState(st);
    e.postNoteOn(0U,69U,100U); renderLeft(e,40);
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
    e.postNoteOn(0U,69U,100U); renderLeft(e,40);
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
    testLoopedSampleKeepsAttack();
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
    testPortamentoNewSoundStartsFresh();
    testPortamentoAutoMonoReleaseTail();
    testPortamentoBalance();
    testPortamentoTimeLimit();
    testPortamentoAcrossRecordings();
    testMonoSplitUppersIndependent();
    testToneEditKeepsHeldKeys();
    testUpperAdsrAbsolute();
    testUpperCutoffResonance();
    std::puts("SILVETON sampler tests passed");
    return 0;
}
