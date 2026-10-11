#include "silveton/sampler/SetPackage.h"
#include "silveton/sampler/AudioFileLoader.h"
#include "silveton/sampler/KorgImporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace silveton::sampler {
namespace {

constexpr std::uint32_t kVersion = 12U;   // SILVETON_SET_V12: layers, TONE v2, EQ5, porta modes, FX v3
constexpr std::array<char,8> kMagic{{'S','I','L','V','S','E','T','1'}};

struct Writer {
    std::ofstream f;
    explicit Writer(const std::filesystem::path& p) : f(p, std::ios::binary) {}
    [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(f); }
    void bytes(const void* p, std::size_t n) { f.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); }
    void u8(std::uint8_t v) { bytes(&v,1U); }
    void i8(std::int8_t v) { bytes(&v,1U); }
    void u16(std::uint16_t v) { std::array<std::uint8_t,2> b{{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v>>8U)}}; bytes(b.data(),2U); }
    void u32(std::uint32_t v) { std::array<std::uint8_t,4> b{{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v>>8U), static_cast<std::uint8_t>(v>>16U), static_cast<std::uint8_t>(v>>24U)}}; bytes(b.data(),4U); }
    void f32(float v) { std::uint32_t u=0U; static_assert(sizeof(u)==sizeof(v)); std::memcpy(&u,&v,sizeof(v)); u32(u); }
    void str(const std::string& s) { u32(static_cast<std::uint32_t>(s.size())); if(!s.empty()) bytes(s.data(),s.size()); }
};

struct Reader {
    std::ifstream f;
    std::string error;
    explicit Reader(const std::filesystem::path& p) : f(p, std::ios::binary) {}
    bool bytes(void* p, std::size_t n) { if(error.empty() && f.read(static_cast<char*>(p), static_cast<std::streamsize>(n))) return true; if(error.empty()) error="Unexpected end of SET manifest"; return false; }
    std::uint8_t u8() { std::uint8_t v=0U; bytes(&v,1U); return v; }
    std::int8_t i8() { std::int8_t v=0; bytes(&v,1U); return v; }
    std::uint16_t u16() { std::array<std::uint8_t,2>b{}; bytes(b.data(),2U); return static_cast<std::uint16_t>(b[0]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[1])<<8U); }
    std::uint32_t u32() { std::array<std::uint8_t,4>b{}; bytes(b.data(),4U); return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1])<<8U) | (static_cast<std::uint32_t>(b[2])<<16U) | (static_cast<std::uint32_t>(b[3])<<24U); }
    float f32() { const auto u=u32(); float v=0.0F; std::memcpy(&v,&u,sizeof(v)); return v; }
    std::string str() { const auto n=u32(); if(n>16U*1024U*1024U){ if(error.empty())error="String field too large"; return {}; } std::string s(n,'\0'); if(n>0U)bytes(s.data(),n); return s; }
};

void writeEnvelope(Writer& w,const EnvelopeParams& p){w.f32(p.attackSec);w.f32(p.decaySec);w.f32(p.sustain);w.f32(p.releaseSec);} 
EnvelopeParams readEnvelope(Reader& r){EnvelopeParams p; p.attackSec=r.f32();p.decaySec=r.f32();p.sustain=r.f32();p.releaseSec=r.f32();return p;}
void writeFilter(Writer& w,const FilterParams& p){w.u8(static_cast<std::uint8_t>(p.mode));w.f32(p.cutoffHz);w.f32(p.resonance);w.f32(p.keyTrack);w.f32(p.velocityAmount);w.f32(p.envelopeAmount);w.f32(p.lfoAmount);} 
FilterParams readFilter(Reader& r){FilterParams p; p.mode=static_cast<FilterMode>(r.u8());p.cutoffHz=r.f32();p.resonance=r.f32();p.keyTrack=r.f32();p.velocityAmount=r.f32();p.envelopeAmount=r.f32();p.lfoAmount=r.f32();return p;}
void writeLfo(Writer&w,const LfoParams&p){w.u8(static_cast<std::uint8_t>(p.wave));w.f32(p.frequencyHz);w.f32(p.delaySec);w.f32(p.fadeSec);w.u8(p.keySync?1U:0U);w.f32(p.pitchCents);w.f32(p.filterOctaves);w.f32(p.ampDepth);} 
LfoParams readLfo(Reader&r){LfoParams p;p.wave=static_cast<LfoWave>(r.u8());p.frequencyHz=r.f32();p.delaySec=r.f32();p.fadeSec=r.f32();p.keySync=r.u8()!=0U;p.pitchCents=r.f32();p.filterOctaves=r.f32();p.ampDepth=r.f32();return p;}
void writeFx(Writer&w,const EffectSlot&p){w.u8(static_cast<std::uint8_t>(p.type));w.f32(p.wet);w.f32(p.timeMs);w.f32(p.feedback);w.f32(p.rateHz);w.f32(p.depth);w.f32(p.tone);
    w.f32(p.preDelayMs);w.f32(p.damping);w.f32(p.size);w.f32(p.mod);w.u8(p.tempoSync?1U:0U);w.f32(p.tempoBpm);w.f32(p.division);w.f32(p.param1);w.f32(p.param2);w.f32(p.param3);}
EffectSlot readFx(Reader&r,std::uint32_t version){EffectSlot p;const auto t=r.u8();p.type=t<static_cast<std::uint8_t>(kEffectTypeCount)?static_cast<EffectType>(t):EffectType::Off;p.wet=r.f32();p.timeMs=r.f32();p.feedback=r.f32();p.rateHz=r.f32();p.depth=r.f32();p.tone=r.f32();
    if(version>=12U){p.preDelayMs=r.f32();p.damping=r.f32();p.size=r.f32();p.mod=r.f32();p.tempoSync=r.u8()!=0U;p.tempoBpm=r.f32();p.division=r.f32();p.param1=r.f32();p.param2=r.f32();p.param3=r.f32();}
    else if(p.type==EffectType::Reverb){p.damping=1.0F-std::clamp(p.tone,0.0F,1.0F);p.tone=0.6F;}   // the old PLATE used TONE as damping
    return p;}
void writeToneExtra(Writer&w,const ToneExtra&x){w.f32(x.startMs);w.f32(x.endMs);w.f32(x.filterEnvOctaves);w.f32(x.filterEnvMs);w.f32(x.velToLevel);w.f32(x.velToCutoff);w.f32(x.highPassHz);w.f32(x.lfoRateHz);w.f32(x.lfoDepthCents);w.f32(x.lfoDelayMs);w.f32(x.lfoFadeMs);w.u8(x.lfoModWheel?1U:0U);w.f32(x.eqLowDb);w.f32(x.eqMidDb);w.f32(x.eqHighDb);}
ToneExtra readToneExtra(Reader&r){ToneExtra x;x.startMs=r.f32();x.endMs=r.f32();x.filterEnvOctaves=r.f32();x.filterEnvMs=r.f32();x.velToLevel=r.f32();x.velToCutoff=r.f32();x.highPassHz=r.f32();x.lfoRateHz=r.f32();x.lfoDepthCents=r.f32();x.lfoDelayMs=r.f32();x.lfoFadeMs=r.f32();x.lfoModWheel=r.u8()!=0U;x.eqLowDb=r.f32();x.eqMidDb=r.f32();x.eqHighDb=r.f32();
    const auto fin=[](float&v,float d){if(!std::isfinite(v))v=d;};fin(x.startMs,0.0F);fin(x.endMs,0.0F);fin(x.filterEnvOctaves,0.0F);fin(x.filterEnvMs,120.0F);fin(x.velToLevel,1.0F);fin(x.velToCutoff,0.0F);fin(x.highPassHz,0.0F);fin(x.lfoRateHz,5.5F);fin(x.lfoDepthCents,0.0F);fin(x.lfoDelayMs,0.0F);fin(x.lfoFadeMs,300.0F);fin(x.eqLowDb,0.0F);fin(x.eqMidDb,0.0F);fin(x.eqHighDb,0.0F);return x;}
void writeSoundTone(Writer&w,const SoundTone&t){w.f32(t.attackMs);w.f32(t.decayMs);w.f32(t.sustain);w.f32(t.releaseMs);w.f32(t.cutoffHz);w.f32(t.resonance);writeToneExtra(w,t.x);}
SoundTone readSoundTone(Reader&r){SoundTone t;const auto tone=[&r](float hi){const float v=r.f32();return(std::isfinite(v)&&v>=0.0F)?std::min(v,hi):-1.0F;};
    t.attackMs=tone(kToneMaxTimeMs);t.decayMs=tone(kToneMaxTimeMs);t.sustain=tone(1.0F);t.releaseMs=tone(kToneMaxTimeMs);t.cutoffHz=tone(kToneCutoffOpenHz);t.resonance=tone(1.0F);t.x=readToneExtra(r);return t;}
void writeEq(Writer&w,const UpperEq&e){w.u8(e.on?1U:0U);for(const auto&b:e.band){w.u8(b.on?1U:0U);w.f32(b.freqHz);w.f32(b.gainDb);w.f32(b.q);}w.u8(e.hpOn?1U:0U);w.f32(e.hpHz);w.u8(e.hpSlopeDb);w.u8(e.lpOn?1U:0U);w.f32(e.lpHz);w.u8(e.lpSlopeDb);}
UpperEq readEq(Reader&r){UpperEq e;e.on=r.u8()!=0U;for(auto&b:e.band){b.on=r.u8()!=0U;b.freqHz=std::clamp(r.f32(),20.0F,20000.0F);b.gainDb=std::clamp(r.f32(),-18.0F,18.0F);b.q=std::clamp(r.f32(),0.1F,10.0F);}
    e.hpOn=r.u8()!=0U;e.hpHz=std::clamp(r.f32(),10.0F,2000.0F);e.hpSlopeDb=r.u8()>=24U?24U:12U;e.lpOn=r.u8()!=0U;e.lpHz=std::clamp(r.f32(),1000.0F,20000.0F);e.lpSlopeDb=r.u8()>=24U?24U:12U;return e;}
void writeAddress(Writer&w,const SlotAddress&a){w.u16(a.bank);w.u16(a.page);w.u16(a.slot);} 
SlotAddress readAddress(Reader&r){return {r.u16(),r.u16(),r.u16()};}

bool writeFloatWav(const std::filesystem::path& p,const SampleData& s) {
    if(!s.valid()) return false;
    std::ofstream f(p,std::ios::binary); if(!f)return false;
    const std::uint32_t dataBytes=static_cast<std::uint32_t>(s.interleaved.size()*sizeof(float));
    const std::uint32_t riffSize=4U+(8U+16U)+(8U+dataBytes);
    auto le16=[&](std::uint16_t v){std::array<char,2>b{{static_cast<char>(v&0xFFU),static_cast<char>((v>>8U)&0xFFU)}};f.write(b.data(),2);};
    auto le32=[&](std::uint32_t v){std::array<char,4>b{{static_cast<char>(v&0xFFU),static_cast<char>((v>>8U)&0xFFU),static_cast<char>((v>>16U)&0xFFU),static_cast<char>((v>>24U)&0xFFU)}};f.write(b.data(),4);};
    f.write("RIFF",4);le32(riffSize);f.write("WAVE",4);f.write("fmt ",4);le32(16U);le16(3U);le16(s.channels);le32(s.sampleRate);le32(s.sampleRate*static_cast<std::uint32_t>(s.channels)*4U);le16(static_cast<std::uint16_t>(s.channels*4U));le16(32U);f.write("data",4);le32(dataBytes);
    for(float x:s.interleaved){std::uint32_t u=0U;std::memcpy(&u,&x,sizeof(x));std::array<char,4>b{{static_cast<char>(u&0xFFU),static_cast<char>((u>>8U)&0xFFU),static_cast<char>((u>>16U)&0xFFU),static_cast<char>((u>>24U)&0xFFU)}};f.write(b.data(),4);}return static_cast<bool>(f);
}

SamplePtr loadAsset(const std::filesystem::path& p,std::string& error) {
    auto ext=p.extension().string();std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(ext==".ksf"){auto r=KorgImporter::loadKsfStreaming(p.string()); if(!r.ok()){error=r.error;return nullptr;} return r.sample;}
    AudioLoadResult r;
    if(ext==".wav"||ext==".wave") r=AudioFileLoader::loadWavStreaming(p.string());
    else if(ext==".aif"||ext==".aiff"||ext==".aifc") r=AudioFileLoader::loadAiffStreaming(p.string());
    else r=AudioFileLoader::load(p.string());
    if(!r.ok()){error=r.error;return nullptr;}return r.sample;
}

struct Graph {
    std::vector<SamplePtr> samples;
    std::vector<MultisamplePtr> multisamples;
    std::vector<ProgramPtr> programs;
    std::unordered_map<const SampleData*,std::uint32_t> sampleIds;
    std::unordered_map<const Multisample*,std::uint32_t> multisampleIds;
    std::unordered_map<const Program*,std::uint32_t> programIds;

    std::uint32_t addSample(const SamplePtr& p){if(!p)return 0xFFFFFFFFU;auto it=sampleIds.find(p.get());if(it!=sampleIds.end())return it->second;auto id=static_cast<std::uint32_t>(samples.size());samples.push_back(p);sampleIds.emplace(p.get(),id);return id;}
    std::uint32_t addMs(const MultisamplePtr& p){if(!p)return 0xFFFFFFFFU;auto it=multisampleIds.find(p.get());if(it!=multisampleIds.end())return it->second;auto id=static_cast<std::uint32_t>(multisamples.size());multisamples.push_back(p);multisampleIds.emplace(p.get(),id);for(const auto&z:p->zones)addSample(z.sample);return id;}
    std::uint32_t addProgram(const ProgramPtr& p){if(!p)return 0xFFFFFFFFU;auto it=programIds.find(p.get());if(it!=programIds.end())return it->second;auto id=static_cast<std::uint32_t>(programs.size());programs.push_back(p);programIds.emplace(p.get(),id);for(const auto&l:p->layers)addMs(l.multisample);return id;}
};

void writeGlobal(Writer&w,const GlobalState&g){
    w.f32(g.masterTuneHz);w.i8(g.transposeSemitones);w.f32(g.masterGainDb);w.f32(g.eqLowDb);w.f32(g.eqMidDb);w.f32(g.eqHighDb);w.u8(g.limiterEnabled?1U:0U);w.f32(g.limiterCeilingDb);w.u8(g.velocityCurve);w.u8(g.breathCc);w.u8(g.expressionCc);w.u8(g.damperCc);w.u8(g.pitchBendEnabled?1U:0U);w.u8(g.aftertouchEnabled?1U:0U);
    w.u8(g.scaleRoot);for(float cents:g.scaleOffsetsCents)w.f32(cents);
    w.u16(g.midiInputChannelMask);w.u8(g.filterNotes?1U:0U);w.u8(g.filterControlChange?1U:0U);w.u8(g.filterPitchBend?1U:0U);w.u8(g.filterAftertouch?1U:0U);
    w.f32(g.breathMin);w.f32(g.breathMax);w.f32(g.breathCurve);w.f32(g.expressionMin);w.f32(g.expressionMax);w.f32(g.expressionCurve);w.u8(g.damperPolarityReversed?1U:0U);
}
GlobalState readGlobal(Reader&r,std::uint32_t version){
    GlobalState g;g.masterTuneHz=r.f32();g.transposeSemitones=r.i8();g.masterGainDb=r.f32();g.eqLowDb=r.f32();g.eqMidDb=r.f32();g.eqHighDb=r.f32();g.limiterEnabled=r.u8()!=0U;g.limiterCeilingDb=r.f32();g.velocityCurve=r.u8();
    if(version>=3U){g.breathCc=r.u8();g.expressionCc=r.u8();g.damperCc=r.u8();g.pitchBendEnabled=r.u8()!=0U;g.aftertouchEnabled=r.u8()!=0U;}
    if(version>=5U){
        g.scaleRoot=static_cast<std::uint8_t>(r.u8()%12U);for(float& cents:g.scaleOffsetsCents)cents=std::clamp(r.f32(),-100.0F,100.0F);
        g.midiInputChannelMask=r.u16();g.filterNotes=r.u8()!=0U;g.filterControlChange=r.u8()!=0U;g.filterPitchBend=r.u8()!=0U;g.filterAftertouch=r.u8()!=0U;
        g.breathMin=std::clamp(r.f32(),0.0F,1.0F);g.breathMax=std::clamp(r.f32(),0.0F,1.0F);g.breathCurve=std::clamp(r.f32(),0.1F,8.0F);
        g.expressionMin=std::clamp(r.f32(),0.0F,1.0F);g.expressionMax=std::clamp(r.f32(),0.0F,1.0F);g.expressionCurve=std::clamp(r.f32(),0.1F,8.0F);g.damperPolarityReversed=r.u8()!=0U;
        if(g.breathMax<=g.breathMin)g.breathMax=std::min(1.0F,g.breathMin+0.0001F);
        if(g.expressionMax<=g.expressionMin)g.expressionMax=std::min(1.0F,g.expressionMin+0.0001F);
    }
    return g;
}

void writeProgramBody(Writer&w,const Program&p,const Graph&g){w.str(p.name);w.u8(p.mono?1U:0U);w.u8(p.legato?1U:0U);w.f32(p.portamentoSec);w.u32(static_cast<std::uint32_t>(p.layers.size()));for(const auto&l:p.layers){w.str(l.name);const auto it=g.multisampleIds.find(l.multisample.get());w.u32(it==g.multisampleIds.end()?0xFFFFFFFFU:it->second);w.u8(l.enabled?1U:0U);w.u8(l.keyLow);w.u8(l.keyHigh);w.u8(l.velocityLow);w.u8(l.velocityHigh);w.i8(l.transposeSemitones);w.f32(l.tuneCents);w.f32(l.levelDb);w.f32(l.pan);w.u8(l.velocityFadeIn);w.u8(l.velocityFadeOut);w.f32(l.delaySec);w.u8(l.damperEnabled?1U:0U);writeEnvelope(w,l.ampEnvelope);writeEnvelope(w,l.filterEnvelope);writeEnvelope(w,l.pitchEnvelope);w.f32(l.pitchEnvelopeCents);writeFilter(w,l.filter);writeLfo(w,l.lfo1);writeLfo(w,l.lfo2);}}

ProgramPtr readProgramBody(Reader&r,const std::vector<MultisamplePtr>&ms,std::uint32_t version){auto p=std::make_shared<Program>();p->name=r.str();p->mono=r.u8()!=0U;p->legato=r.u8()!=0U;p->portamentoSec=r.f32();const auto n=r.u32();if(n>10000U){r.error="Program layer count too large";return nullptr;}p->layers.reserve(n);for(std::uint32_t i=0U;i<n;++i){ProgramLayer l;l.name=r.str();const auto id=r.u32();if(id<ms.size())l.multisample=ms[id];l.enabled=r.u8()!=0U;l.keyLow=r.u8();l.keyHigh=r.u8();l.velocityLow=r.u8();l.velocityHigh=r.u8();l.transposeSemitones=r.i8();l.tuneCents=r.f32();l.levelDb=r.f32();l.pan=r.f32();if(version>=7U){l.velocityFadeIn=r.u8();l.velocityFadeOut=r.u8();l.delaySec=r.f32();l.damperEnabled=r.u8()!=0U;}l.ampEnvelope=readEnvelope(r);l.filterEnvelope=readEnvelope(r);l.pitchEnvelope=readEnvelope(r);l.pitchEnvelopeCents=r.f32();l.filter=readFilter(r);l.lfo1=readLfo(r);l.lfo2=readLfo(r);p->layers.push_back(l);}return p;}

void writeUpper(Writer&w,const UpperPart&u,const Graph&g){
    w.str(u.name);const auto it=g.programIds.find(u.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);w.u16(u.soundSlotId);
    w.u8(u.enabled?1U:0U);w.u8(u.mute?1U:0U);w.u8(u.solo?1U:0U);w.u8(u.keyLow);w.u8(u.keyHigh);w.u8(u.velocityLow);w.u8(u.velocityHigh);
    w.f32(u.levelDb);w.f32(u.pan);w.i8(u.transposeSemitones);w.f32(u.tuneCents);w.i8(u.octave);w.i8(u.pitchBendRange);
    w.u8(u.damperEnabled?1U:0U);w.u8(u.expressionEnabled?1U:0U);w.u8(u.breathEnabled?1U:0U);w.u8(u.aftertouchEnabled?1U:0U);w.u8(u.midiChannel);
    w.f32(u.eqLowDb);w.f32(u.eqMidDb);w.f32(u.eqHighDb);w.f32(u.fxSend1);w.f32(u.fxSend2);
    w.f32(u.ampAttackScale);w.f32(u.ampDecayScale);w.f32(u.ampReleaseScale);w.f32(u.filterCutoffOctaves);w.f32(u.filterResonanceOffset);w.f32(u.lfoRateScale);w.f32(u.lfoDepthScale);w.f32(u.portamentoSec);
    w.f32(u.ampSustainScale);w.u8(static_cast<std::uint8_t>(u.voiceMode));
    w.f32(u.toneAttackMs);w.f32(u.toneDecayMs);w.f32(u.toneSustain);w.f32(u.toneReleaseMs);w.f32(u.toneCutoffHz);w.f32(u.toneResonance);
    for(const auto&route:u.controllerRoutes){w.u8(static_cast<std::uint8_t>(route.source));w.u8(static_cast<std::uint8_t>(route.destination));w.u8(route.cc);w.f32(route.amount);w.u8(route.bipolar?1U:0U);}
    // SILVETON_SET_V12
    writeToneExtra(w,u.toneExtra);
    for(const auto&l:u.layers){w.u8(l.enabled?1U:0U);w.str(l.name);const auto lit=g.programIds.find(l.program.get());w.u32(lit==g.programIds.end()?0xFFFFFFFFU:lit->second);w.u16(l.soundSlotId);
        w.f32(l.levelDb);w.f32(l.pan);w.i8(l.transposeSemitones);w.i8(l.octave);w.f32(l.tuneCents);w.u8(l.keyLow);w.u8(l.keyHigh);w.u8(l.velocityLow);w.u8(l.velocityHigh);writeSoundTone(w,l.tone);}
    writeEq(w,u.eq);
    w.u8(static_cast<std::uint8_t>(u.portaMode));w.u8(u.portaFingered?1U:0U);w.u8(u.portaRateMode?1U:0U);w.u8(u.portaTime);w.f32(u.fxSend3);w.f32(u.fxSend4);
}

UpperPart readUpper(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){
    UpperPart u;u.name=r.str();const auto id=r.u32();if(id<p.size())u.program=p[id];if(version>=10U)u.soundSlotId=r.u16();
    u.enabled=r.u8()!=0U;u.mute=r.u8()!=0U;u.solo=r.u8()!=0U;u.keyLow=r.u8();u.keyHigh=r.u8();u.velocityLow=r.u8();u.velocityHigh=r.u8();
    u.levelDb=r.f32();u.pan=r.f32();u.transposeSemitones=r.i8();u.tuneCents=r.f32();u.octave=r.i8();u.pitchBendRange=r.i8();
    u.damperEnabled=r.u8()!=0U;u.expressionEnabled=r.u8()!=0U;u.breathEnabled=r.u8()!=0U;u.aftertouchEnabled=r.u8()!=0U;u.midiChannel=r.u8();
    u.eqLowDb=r.f32();u.eqMidDb=r.f32();u.eqHighDb=r.f32();u.fxSend1=r.f32();u.fxSend2=r.f32();
    if(version>=2U){u.ampAttackScale=r.f32();u.ampDecayScale=r.f32();u.ampReleaseScale=r.f32();u.filterCutoffOctaves=r.f32();u.filterResonanceOffset=r.f32();u.lfoRateScale=r.f32();u.lfoDepthScale=r.f32();u.portamentoSec=r.f32();}
    if(version>=9U){u.ampSustainScale=std::clamp(r.f32(),0.0F,2.0F);const auto vm=r.u8();u.voiceMode=vm<=static_cast<std::uint8_t>(UpperVoiceMode::Legato)?static_cast<UpperVoiceMode>(vm):UpperVoiceMode::Poly;}
    else if(u.program!=nullptr){u.voiceMode=u.program->legato?UpperVoiceMode::Legato:(u.program->mono?UpperVoiceMode::Mono:UpperVoiceMode::Poly);if(version<2U)u.portamentoSec=u.program->portamentoSec;}
    if(version>=11U){
        const auto tone=[&r](float hi){const float v=r.f32();return(std::isfinite(v)&&v>=0.0F)?std::min(v,hi):-1.0F;};
        u.toneAttackMs=tone(kToneMaxTimeMs);u.toneDecayMs=tone(kToneMaxTimeMs);u.toneSustain=tone(1.0F);u.toneReleaseMs=tone(kToneMaxTimeMs);
        u.toneCutoffHz=tone(kToneCutoffOpenHz);u.toneResonance=tone(1.0F);
    }
    if(version>=6U){for(auto&route:u.controllerRoutes){route.source=static_cast<ControllerSource>(r.u8());route.destination=static_cast<ModDestination>(r.u8());route.cc=static_cast<std::uint8_t>(r.u8()&0x7FU);route.amount=r.f32();route.bipolar=r.u8()!=0U;}}
    if(version>=12U){
        u.toneExtra=readToneExtra(r);
        for(auto&l:u.layers){l.enabled=r.u8()!=0U;l.name=r.str();const auto lid=r.u32();if(lid<p.size())l.program=p[lid];l.soundSlotId=r.u16();
            l.levelDb=std::clamp(r.f32(),-60.0F,24.0F);l.pan=std::clamp(r.f32(),-1.0F,1.0F);l.transposeSemitones=r.i8();l.octave=r.i8();l.tuneCents=r.f32();l.keyLow=r.u8();l.keyHigh=r.u8();l.velocityLow=r.u8();l.velocityHigh=r.u8();l.tone=readSoundTone(r);}
        u.eq=readEq(r);
        const auto pm=r.u8();u.portaMode=pm==1U?PortamentoMode::Kronos:PortamentoMode::Silveton;u.portaFingered=r.u8()!=0U;u.portaRateMode=r.u8()!=0U;u.portaTime=static_cast<std::uint8_t>(std::min<unsigned>(r.u8(),127U));
        u.fxSend3=std::clamp(r.f32(),0.0F,1.0F);u.fxSend4=std::clamp(r.f32(),0.0F,1.0F);
    } else {
        // an old BLEND: its LOW / MID / HIGH become bands 1 / 3 / 5 of the new EQ
        u.eq.band[0].gainDb=std::clamp(u.eqLowDb,-18.0F,18.0F);u.eq.band[2].gainDb=std::clamp(u.eqMidDb,-18.0F,18.0F);u.eq.band[4].gainDb=std::clamp(u.eqHighDb,-18.0F,18.0F);
    }
    return u;
}

void writeCombi(Writer&w,const Combi&c,const Graph&g){w.str(c.name);for(const auto&u:c.uppers)writeUpper(w,u,g);writeFx(w,c.fx[0]);writeFx(w,c.fx[1]);w.u8(static_cast<std::uint8_t>(c.fxRouting));
    writeFx(w,c.fx[2]);writeFx(w,c.fx[3]);for(const auto&ins:c.insert)writeFx(w,ins);}   // SILVETON_SET_V12
Combi readCombi(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){Combi c;c.name=r.str();for(auto&u:c.uppers)u=readUpper(r,p,version);c.fx[0]=readFx(r,version);c.fx[1]=readFx(r,version);if(version>=6U)c.fxRouting=static_cast<EffectRouting>(r.u8());
    if(version>=12U){c.fx[2]=readFx(r,version);c.fx[3]=readFx(r,version);for(auto&ins:c.insert)ins=readFx(r,version);}return c;}

} // namespace

namespace {
SetPackageResult savePackageDirectory(const std::string& packageDirectory,const SetData& data){
    namespace fs=std::filesystem;
    try{
        fs::path root(packageDirectory);fs::remove_all(root);fs::create_directories(root/"audio");
        Graph g;for(const auto&s:data.samples)g.addSample(s.sample);for(const auto&m:data.multisamples)g.addMs(m.multisample);for(const auto&s:data.programs)g.addProgram(s.program);for(const auto&cs:data.combis)for(const auto&u:cs.combi.uppers){g.addProgram(u.program);for(const auto&l:u.layers)g.addProgram(l.program);}
        Writer w(root/"manifest.svs");if(!w.ok())return{false,"Cannot create SET manifest"};w.bytes(kMagic.data(),kMagic.size());w.u32(kVersion);w.str(data.name);writeGlobal(w,data.global);
        w.u32(static_cast<std::uint32_t>(g.samples.size()));
        for(std::uint32_t i=0U;i<g.samples.size();++i){const auto&s=*g.samples[i];w.str(s.name);w.str(s.sourceIdentity);w.u8(s.rootKey);w.f32(s.fineTuneCents);w.u8(s.loop.enabled?1U:0U);w.u32(static_cast<std::uint32_t>(s.loop.startFrame));w.u32(static_cast<std::uint32_t>(s.loop.endFrame));w.u32(s.loop.crossfadeFrames);
            fs::path src(s.sourcePath);std::string ext=src.extension().string();if(ext.empty())ext=".wav";const fs::path rel=fs::path("audio")/("sample_"+std::to_string(i)+ext);const fs::path dst=root/rel;bool copied=false;if(!s.sourcePath.empty()&&fs::exists(src)){fs::copy_file(src,dst,fs::copy_options::overwrite_existing);copied=true;}if(!copied&&!writeFloatWav(dst,s))return{false,"Failed to write embedded sample audio"};w.str(rel.generic_string());}
        w.u32(static_cast<std::uint32_t>(g.multisamples.size()));for(const auto&msp:g.multisamples){w.str(msp->name);w.str(msp->sourceIdentity);w.u32(static_cast<std::uint32_t>(msp->zones.size()));for(const auto&z:msp->zones){const auto it=g.sampleIds.find(z.sample.get());w.u32(it==g.sampleIds.end()?0xFFFFFFFFU:it->second);w.u8(z.keyLow);w.u8(z.keyHigh);w.u8(z.velocityLow);w.u8(z.velocityHigh);w.u8(z.rootKey);w.i8(z.transposeSemitones);w.f32(z.tuneCents);w.f32(z.gainDb);w.f32(z.pan);w.u8(z.oneShot?1U:0U);w.u8(z.reverse?1U:0U);w.u8(z.nonTranspose?1U:0U);w.i8(z.sourceLevelRaw);w.i8(z.sourceFilterRaw);}}
        w.u32(static_cast<std::uint32_t>(g.programs.size()));for(const auto&p:g.programs)writeProgramBody(w,*p,g);
        w.u32(static_cast<std::uint32_t>(data.programs.size()));for(const auto&s:data.programs){writeAddress(w,s.address);const auto it=g.programIds.find(s.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);}
        w.u32(static_cast<std::uint32_t>(data.combis.size()));for(const auto&s:data.combis){writeAddress(w,s.address);writeCombi(w,s.combi,g);}
        w.u32(static_cast<std::uint32_t>(data.lists.size()));for(const auto&s:data.lists){writeAddress(w,s.address);w.str(s.list.name);w.u32(static_cast<std::uint32_t>(s.list.entries.size()));for(const auto&e:s.list.entries){w.u16(e.number);w.str(e.name);w.u16(e.combiBank);w.u16(e.combiPage);w.u16(e.combiSlot);}}
        w.u32(static_cast<std::uint32_t>(data.samples.size()));for(const auto&item:data.samples){w.u32(item.id);const auto it=g.sampleIds.find(item.sample.get());w.u32(it==g.sampleIds.end()?0xFFFFFFFFU:it->second);}
        w.u32(static_cast<std::uint32_t>(data.multisamples.size()));for(const auto&item:data.multisamples){w.u32(item.id);const auto it=g.multisampleIds.find(item.multisample.get());w.u32(it==g.multisampleIds.end()?0xFFFFFFFFU:it->second);}
        if (!w.f) return {false, "Failed while writing SET manifest"};
        return {true, {}};
    }catch(const std::exception&e){return{false,e.what()};}
}


} // namespace

SetPackageResult SetPackage::save(const std::string& packageDirectory,const SetData& data){
    namespace fs=std::filesystem;
    try {
        const fs::path target(packageDirectory);
        if (target.empty()) return {false, "SET destination is empty"};
        const fs::path parent = target.has_parent_path() ? target.parent_path() : fs::current_path();
        fs::create_directories(parent);

        const std::string stem = target.filename().string();
        fs::path temp = parent / (stem + ".tmp-write");
        fs::path backup = parent / (stem + ".previous");
        fs::remove_all(temp);
        fs::remove_all(backup);

        const auto staged = savePackageDirectory(temp.string(), data);
        if (!staged.success) {
            fs::remove_all(temp);
            return staged;
        }

        // Verify the complete staged package before replacing the current SET.
        const auto verified = SetPackage::load(temp.string());
        if (!verified.ok()) {
            fs::remove_all(temp);
            return {false, "Staged SET verification failed: " + verified.error};
        }

        const bool hadTarget = fs::exists(target);
        if (hadTarget) fs::rename(target, backup);
        try {
            fs::rename(temp, target);
        } catch (...) {
            if (hadTarget && fs::exists(backup) && !fs::exists(target)) fs::rename(backup, target);
            fs::remove_all(temp);
            throw;
        }
        fs::remove_all(backup);
        return {true, {}};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

SetLoadResult SetPackage::load(const std::string& packageDirectory){
    namespace fs=std::filesystem;SetLoadResult result;try{const fs::path root(packageDirectory);Reader r(root/"manifest.svs");if(!r.f){result.error="Cannot open SET manifest";return result;}std::array<char,8>magic{};r.bytes(magic.data(),magic.size());if(magic!=kMagic){result.error="Not a SILVETON SET manifest";return result;}const auto version=r.u32();if(version<1U||version>kVersion){result.error="Unsupported SILVETON SET version";return result;}result.data.name=r.str();result.data.global=readGlobal(r,version);
        const auto sampleCount=r.u32();if(sampleCount>10000000U){result.error="SET sample count too large";return result;}std::vector<SamplePtr>samples;samples.reserve(sampleCount);for(std::uint32_t i=0U;i<sampleCount;++i){const std::string name=r.str();const std::string sourceIdentity=version>=8U?r.str():std::string{};const auto rootKey=r.u8();const float fine=r.f32();const bool loopEnabled=r.u8()!=0U;const auto loopStart=r.u32();const auto loopEnd=r.u32();const auto cross=r.u32();const auto rel=r.str();std::string err;auto sp=loadAsset(root/rel,err);if(!sp){result.error="Sample load failed: "+err;return result;}auto m=std::make_shared<SampleData>(*sp);m->name=name;m->sourceIdentity=sourceIdentity;m->rootKey=rootKey;m->fineTuneCents=fine;m->loop.enabled=loopEnabled;m->loop.startFrame=loopStart;m->loop.endFrame=loopEnd;m->loop.crossfadeFrames=cross;samples.push_back(m);} 
        const auto msCount=r.u32();if(msCount>10000000U){result.error="SET multisample count too large";return result;}std::vector<MultisamplePtr>mss;mss.reserve(msCount);for(std::uint32_t i=0U;i<msCount;++i){auto ms=std::make_shared<Multisample>();ms->name=r.str();if(version>=8U)ms->sourceIdentity=r.str();const auto zn=r.u32();if(zn>1000000U){result.error="SET zone count too large";return result;}ms->zones.reserve(zn);for(std::uint32_t zidx=0U;zidx<zn;++zidx){SampleZone z;const auto sid=r.u32();if(sid<samples.size())z.sample=samples[sid];z.keyLow=r.u8();z.keyHigh=r.u8();z.velocityLow=r.u8();z.velocityHigh=r.u8();z.rootKey=r.u8();z.transposeSemitones=r.i8();z.tuneCents=r.f32();z.gainDb=r.f32();z.pan=r.f32();z.oneShot=r.u8()!=0U;z.reverse=r.u8()!=0U;z.nonTranspose=r.u8()!=0U;z.sourceLevelRaw=r.i8();z.sourceFilterRaw=r.i8();ms->zones.push_back(z);}mss.push_back(ms);} 
        const auto progCount=r.u32();if(progCount>100000U){result.error="SET program count too large";return result;}std::vector<ProgramPtr>programs;programs.reserve(progCount);for(std::uint32_t i=0U;i<progCount;++i)programs.push_back(readProgramBody(r,mss,version));
        const auto progSlots=r.u32();for(std::uint32_t i=0U;i<progSlots;++i){ProgramSlot s;s.address=readAddress(r);const auto id=r.u32();if(id<programs.size())s.program=programs[id];result.data.programs.push_back(s);} 
        const auto combiCount=r.u32();for(std::uint32_t i=0U;i<combiCount;++i){CombiSlot s;s.address=readAddress(r);s.combi=readCombi(r,programs,version);result.data.combis.push_back(s);} 
        const auto listCount=r.u32();for(std::uint32_t i=0U;i<listCount;++i){ListSlot s;s.address=readAddress(r);s.list.name=r.str();const auto n=r.u32();if(n>100000U){result.error="SET list entry count too large";return result;}for(std::uint32_t j=0U;j<n;++j){ListEntry e;e.number=r.u16();e.name=r.str();e.combiBank=r.u16();e.combiPage=r.u16();e.combiSlot=r.u16();s.list.entries.push_back(e);}result.data.lists.push_back(s);} 
        if(version>=4U){const auto sampleCatalogCount=r.u32();if(sampleCatalogCount>10000000U){result.error="SET sample catalog too large";return result;}for(std::uint32_t i=0U;i<sampleCatalogCount;++i){SampleCatalogEntry item;item.id=r.u32();const auto sid=r.u32();if(sid<samples.size())item.sample=samples[sid];result.data.samples.push_back(item);}const auto msCatalogCount=r.u32();if(msCatalogCount>10000000U){result.error="SET multisample catalog too large";return result;}for(std::uint32_t i=0U;i<msCatalogCount;++i){MultisampleCatalogEntry item;item.id=r.u32();const auto mid=r.u32();if(mid<mss.size())item.multisample=mss[mid];result.data.multisamples.push_back(item);}}
        if(!r.error.empty()){result.error=r.error;return result;}return result;
    }catch(const std::exception&e){result.error=e.what();return result;}}

} // namespace silveton::sampler
