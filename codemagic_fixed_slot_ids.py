#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(sys.argv[1]).resolve()

def load(rel):
    p = ROOT / rel
    if not p.is_file():
        raise SystemExit(f"missing {rel}")
    return p, p.read_text(encoding="utf-8")

def save(p, text):
    p.write_text(text, encoding="utf-8")

def rep(text, old, new, label):
    if old not in text:
        raise SystemExit(f"patch anchor not found: {label}")
    if text.count(old) != 1:
        raise SystemExit(f"patch anchor not unique: {label} ({text.count(old)})")
    return text.replace(old, new, 1)

# -----------------------------------------------------------------------------
# Runtime types: 500 fixed SOUND / COMBI positions; COMBI Upper stores SoundSlotID.
# SAMPLE and MULTISAMPLE remain uint32 resource IDs (not fixed slot arrays).
# -----------------------------------------------------------------------------
p, s = load("core/include/silveton/sampler/SamplerTypes.h")
s = rep(s,
'''constexpr int kMaxUpperControllerRoutes = 8;''',
'''constexpr int kMaxUpperControllerRoutes = 8;
constexpr std::uint16_t kFixedSoundSlotCount = 500U;
constexpr std::uint16_t kFixedCombiSlotCount = 500U;
constexpr std::uint16_t kNoSoundSlot = 0U;''',
"fixed slot constants")
s = rep(s,
'''struct UpperPart {
    std::string name;
    ProgramPtr program;''',
'''struct UpperPart {
    std::string name;
    ProgramPtr program;
    // Stable 1..500 SOUND position. 0 means no fixed-slot assignment / legacy state.
    std::uint16_t soundSlotId{kNoSoundSlot};''',
"UpperPart soundSlotId")
save(p, s)

# -----------------------------------------------------------------------------
# SET persistence: keep COMBI->SOUND relationship by fixed SoundSlotID.
# Version 10 adds uint16 soundSlotId to every Upper. Legacy SETs remain readable.
# -----------------------------------------------------------------------------
p, s = load("core/src/sampler/SetPackage.cpp")
s = rep(s, "constexpr std::uint32_t kVersion = 9U;", "constexpr std::uint32_t kVersion = 10U;", "SET version 10")
s = rep(s,
'''void writeUpper(Writer&w,const UpperPart&u,const Graph&g){
    w.str(u.name);const auto it=g.programIds.find(u.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);''',
'''void writeUpper(Writer&w,const UpperPart&u,const Graph&g){
    w.str(u.name);const auto it=g.programIds.find(u.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);w.u16(u.soundSlotId);''',
"write Upper soundSlotId")
s = rep(s,
'''UpperPart readUpper(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){
    UpperPart u;u.name=r.str();const auto id=r.u32();if(id<p.size())u.program=p[id];''',
'''UpperPart readUpper(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){
    UpperPart u;u.name=r.str();const auto id=r.u32();if(id<p.size())u.program=p[id];if(version>=10U)u.soundSlotId=r.u16();''',
"read Upper soundSlotId")
# Raise old arbitrary resource caps. Stable IDs are uint32; these limits are only corruption guards.
s = s.replace("sampleCount>100000U", "sampleCount>10000000U")
s = s.replace("msCount>100000U", "msCount>10000000U")
s = s.replace("sampleCatalogCount>100000U", "sampleCatalogCount>10000000U")
s = s.replace("msCatalogCount>100000U", "msCatalogCount>10000000U")
save(p, s)

# -----------------------------------------------------------------------------
# SILVETON_BANK.sv v2: stable IDs instead of positional indexes.
# - SOUND uses fixed soundSlotId 1..500.
# - SOUND layer references multisampleId (uint32).
# - MULTISAMPLE record owns multisampleId (uint32).
# - ZONE references sampleId (uint32).
# - SAMPLE record owns sampleId (uint32).
# Audio offsets/sizes remain uint64.
# -----------------------------------------------------------------------------
p, s = load("core/include/silveton/sampler/SvBankFormat.h")
s = rep(s, "constexpr std::uint16_t kSvBankVersionMajor = 1U;", "constexpr std::uint16_t kSvBankVersionMajor = 2U;", "SV major v2")
s = rep(s, "constexpr std::array<std::uint8_t, 8> kSvBankMagic{{'S','I','L','V','S','V','1',0}};", "constexpr std::array<std::uint8_t, 8> kSvBankMagic{{'S','I','L','V','S','V','2',0}};", "SV magic v2")
s = rep(s,
'''struct SvSoundRecord {
    std::uint32_t nameOffset{0};
    std::uint32_t firstLayer{0};
    std::uint32_t layerCount{0};
    std::uint16_t bank{0};
    std::uint16_t page{0};
    std::uint16_t slot{0};
    std::uint16_t category{0};
};''',
'''struct SvSoundRecord {
    std::uint32_t nameOffset{0};
    std::uint32_t firstLayer{0};
    std::uint32_t layerCount{0};
    std::uint16_t soundSlotId{0}; // fixed 1..500
    std::uint16_t category{0};
    std::uint16_t reserved0{0};
    std::uint16_t reserved1{0};
};''',
"SvSoundRecord v2")
s = s.replace("std::uint32_t multisampleIndex{0};", "std::uint32_t multisampleId{0};", 1)
s = rep(s,
'''struct SvMultisampleRecord {
    std::uint32_t nameOffset{0};
    std::uint32_t sourceIdentityOffset{0};
    std::uint32_t firstZone{0};
    std::uint32_t zoneCount{0};
};''',
'''struct SvMultisampleRecord {
    std::uint32_t multisampleId{0};
    std::uint32_t nameOffset{0};
    std::uint32_t sourceIdentityOffset{0};
    std::uint32_t firstZone{0};
    std::uint32_t zoneCount{0};
};''',
"SvMultisampleRecord id")
s = rep(s,
'''struct SvSampleRecord {
    std::uint32_t nameOffset{0};''',
'''struct SvSampleRecord {
    std::uint32_t sampleId{0};
    std::uint32_t nameOffset{0};''',
"SvSampleRecord id")
s = s.replace("std::uint32_t sampleIndex{0};", "std::uint32_t sampleId{0};", 1)
save(p, s)

p, s = load("core/src/sampler/SvBankFormat.cpp")
s = rep(s,
'''void appendSoundRecord(std::vector<std::uint8_t>& out, const SvSoundRecord& r) {
    const std::size_t start = out.size();
    appendU32(out, r.nameOffset); appendU32(out, r.firstLayer); appendU32(out, r.layerCount);
    appendU16(out, r.bank); appendU16(out, r.page); appendU16(out, r.slot); appendU16(out, r.category);
    padTo(out, start + kSvSoundRecordSize);
}''',
'''void appendSoundRecord(std::vector<std::uint8_t>& out, const SvSoundRecord& r) {
    const std::size_t start = out.size();
    appendU32(out, r.nameOffset); appendU32(out, r.firstLayer); appendU32(out, r.layerCount);
    appendU16(out, r.soundSlotId); appendU16(out, r.category); appendU16(out, r.reserved0); appendU16(out, r.reserved1);
    padTo(out, start + kSvSoundRecordSize);
}''',
"append SOUND v2")
s = s.replace("appendU32(out, r.multisampleIndex);", "appendU32(out, r.multisampleId);", 1)
s = rep(s,
'''void appendMultisampleRecord(std::vector<std::uint8_t>& out, const SvMultisampleRecord& r) {
    const std::size_t start = out.size();
    appendU32(out, r.nameOffset); appendU32(out, r.sourceIdentityOffset); appendU32(out, r.firstZone); appendU32(out, r.zoneCount);
    padTo(out, start + kSvMultisampleRecordSize);
}''',
'''void appendMultisampleRecord(std::vector<std::uint8_t>& out, const SvMultisampleRecord& r) {
    const std::size_t start = out.size();
    appendU32(out, r.multisampleId); appendU32(out, r.nameOffset); appendU32(out, r.sourceIdentityOffset); appendU32(out, r.firstZone); appendU32(out, r.zoneCount);
    padTo(out, start + kSvMultisampleRecordSize);
}''',
"append MULTISAMPLE id")
s = rep(s,
'''    appendU32(out, r.nameOffset); appendU32(out, r.sourceIdentityOffset); appendU32(out, r.sampleRate); appendU16(out, r.channels);''',
'''    appendU32(out, r.sampleId); appendU32(out, r.nameOffset); appendU32(out, r.sourceIdentityOffset); appendU32(out, r.sampleRate); appendU16(out, r.channels);''',
"append SAMPLE id")
s = s.replace("appendU32(out, r.sampleIndex);", "appendU32(out, r.sampleId);", 1)
s = rep(s,
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.firstLayer) && readU32(p,end,r.layerCount) &&
           readU16(p,end,r.bank) && readU16(p,end,r.page) && readU16(p,end,r.slot) && readU16(p,end,r.category);''',
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.firstLayer) && readU32(p,end,r.layerCount) &&
           readU16(p,end,r.soundSlotId) && readU16(p,end,r.category) && readU16(p,end,r.reserved0) && readU16(p,end,r.reserved1);''',
"read SOUND v2")
s = s.replace("readU32(p,end,r.multisampleIndex)", "readU32(p,end,r.multisampleId)", 1)
s = rep(s,
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.firstZone) && readU32(p,end,r.zoneCount);''',
'''    return readU32(p,end,r.multisampleId) && readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.firstZone) && readU32(p,end,r.zoneCount);''',
"read MULTISAMPLE id")
s = rep(s,
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.sampleRate) && readU16(p,end,r.channels) &&''',
'''    return readU32(p,end,r.sampleId) && readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.sampleRate) && readU16(p,end,r.channels) &&''',
"read SAMPLE id")
s = s.replace("readU32(p,end,r.sampleIndex)", "readU32(p,end,r.sampleId)", 1)
save(p, s)

# -----------------------------------------------------------------------------
# SV runtime resolver: IDs are dictionary keys, never implicit vector positions.
# -----------------------------------------------------------------------------
p, s = load("core/src/sampler/SvBankRuntime.cpp")
s = rep(s, "#include <memory>\n#include <set>", "#include <memory>\n#include <map>\n#include <set>", "map include")
s = rep(s,
'''    std::vector<SamplePtr> samples;
    samples.reserve(header.sampleCount);
    out.data.samples.reserve(header.sampleCount);''',
'''    std::vector<SamplePtr> samples;
    samples.reserve(header.sampleCount);
    std::map<std::uint32_t, SamplePtr> sampleById;
    out.data.samples.reserve(header.sampleCount);''',
"sample ID map")
s = rep(s,
'''        if (!sample->valid()) { out.error = "SV sample failed semantic validation"; return out; }
        samples.push_back(sample);
        out.data.samples.push_back({i + 1U, sample});''',
'''        if (!sample->valid()) { out.error = "SV sample failed semantic validation"; return out; }
        if (r.sampleId == 0U || !sampleById.emplace(r.sampleId, sample).second) {
            out.error = "SV bank contains invalid or duplicate SAMPLE id";
            return out;
        }
        samples.push_back(sample);
        out.data.samples.push_back({r.sampleId, sample});''',
"store SAMPLE id")
s = rep(s,
'''        if (r.sampleIndex >= samples.size() || r.keyLow > r.keyHigh || r.velocityLow == 0U || r.velocityLow > r.velocityHigh) {
            out.error = "SV zone failed semantic validation";
            return out;
        }
        SampleZone z{};
        z.sample = samples[r.sampleIndex];''',
'''        const auto sampleIt = sampleById.find(r.sampleId);
        if (r.sampleId == 0U || sampleIt == sampleById.end() || r.keyLow > r.keyHigh || r.velocityLow == 0U || r.velocityLow > r.velocityHigh) {
            out.error = "SV zone failed semantic validation or references unknown SAMPLE id";
            return out;
        }
        SampleZone z{};
        z.sample = sampleIt->second;''',
"resolve SAMPLE id")
s = rep(s,
'''    std::vector<MultisamplePtr> multisamples;
    multisamples.reserve(header.multisampleCount);
    out.data.multisamples.reserve(header.multisampleCount);''',
'''    std::vector<MultisamplePtr> multisamples;
    multisamples.reserve(header.multisampleCount);
    std::map<std::uint32_t, MultisamplePtr> multisampleById;
    out.data.multisamples.reserve(header.multisampleCount);''',
"multisample ID map")
s = rep(s,
'''        multisamples.push_back(ms);
        out.data.multisamples.push_back({i + 1U, ms});''',
'''        if (r.multisampleId == 0U || !multisampleById.emplace(r.multisampleId, ms).second) {
            out.error = "SV bank contains invalid or duplicate MULTISAMPLE id";
            return out;
        }
        multisamples.push_back(ms);
        out.data.multisamples.push_back({r.multisampleId, ms});''',
"store MULTISAMPLE id")
s = rep(s,
'''        if (r.multisampleIndex >= multisamples.size() || r.keyLow > r.keyHigh ||
            r.velocityLow == 0U || r.velocityLow > r.velocityHigh) {
            out.error = "SV SOUND layer failed semantic validation";
            return out;
        }
        ProgramLayer layer{};
        layer.multisample = multisamples[r.multisampleIndex];''',
'''        const auto multisampleIt = multisampleById.find(r.multisampleId);
        if (r.multisampleId == 0U || multisampleIt == multisampleById.end() || r.keyLow > r.keyHigh ||
            r.velocityLow == 0U || r.velocityLow > r.velocityHigh) {
            out.error = "SV SOUND layer failed semantic validation or references unknown MULTISAMPLE id";
            return out;
        }
        ProgramLayer layer{};
        layer.multisample = multisampleIt->second;''',
"resolve MULTISAMPLE id")
s = rep(s,
'''    std::set<std::tuple<std::uint16_t, std::uint16_t, std::uint16_t>> addresses;''',
'''    std::set<std::uint16_t> soundSlots;''',
"sound slot set")
s = rep(s,
'''        const auto key = std::make_tuple(r.bank, r.page, r.slot);
        if (!addresses.insert(key).second) {
            out.error = "SV bank contains duplicate SOUND slot address";
            return out;
        }''',
'''        if (r.soundSlotId < 1U || r.soundSlotId > kFixedSoundSlotCount || !soundSlots.insert(r.soundSlotId).second) {
            out.error = "SV bank contains invalid or duplicate fixed SOUND slot";
            return out;
        }''',
"validate fixed SOUND slot")
s = rep(s,
'''        out.data.programs.push_back({SlotAddress{r.bank, r.page, r.slot}, program});''',
'''        out.data.programs.push_back({SlotAddress{0U, 0U, r.soundSlotId}, program});''',
"install fixed SOUND slot")
save(p, s)

# -----------------------------------------------------------------------------
# Android bridge: assigning a SOUND records the stable slot; activating a COMBI
# re-resolves every Upper from its 1..500 SoundSlotID.
# -----------------------------------------------------------------------------
p, s = load("android/app/src/main/cpp/SamplerAudioBridge.cpp")
s = rep(s,
'''        state->combi.uppers[0].program = loaded.data.programs.front().program;
        state->combi.uppers[0].midiChannel = 0U;''',
'''        state->combi.uppers[0].program = loaded.data.programs.front().program;
        state->combi.uppers[0].soundSlotId = loaded.data.programs.front().address.slot;
        state->combi.uppers[0].midiChannel = 0U;''',
"initial bank SOUND slot")
s = rep(s,
'''        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program = program;
        upper.enabled = true;''',
'''        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program = program;
        upper.soundSlotId = (address.bank == 0U && address.page == 0U && address.slot >= 1U && address.slot <= kFixedSoundSlotCount) ? address.slot : kNoSoundSlot;
        upper.enabled = true;''',
"activateProgram stores SoundSlotID")
s = rep(s,
'''        upper.program.reset();
        upper.enabled = false;''',
'''        upper.program.reset();
        upper.soundSlotId = kNoSoundSlot;
        upper.enabled = false;''',
"clear Upper slot")
s = rep(s,
'''    bool activateCombi(const SlotAddress& address) {
        const auto combi = library_.combi(address);
        if (!combi.has_value()) { setTextError("COMBI slot is empty"); return false; }
        auto next = std::make_shared<EngineState>();
        next->global = state_ != nullptr ? state_->global : library_.global();
        next->combi = *combi;
        return installState(std::move(next));
    }''',
'''    bool activateCombi(const SlotAddress& address) {
        const auto combi = library_.combi(address);
        if (!combi.has_value()) { setTextError("COMBI slot is empty"); return false; }
        auto next = std::make_shared<EngineState>();
        next->global = state_ != nullptr ? state_->global : library_.global();
        next->combi = *combi;
        for (auto& upper : next->combi.uppers) {
            if (upper.soundSlotId == kNoSoundSlot) continue;
            if (upper.soundSlotId > kFixedSoundSlotCount) {
                setTextError("COMBI references SOUND slot outside 1..500");
                return false;
            }
            upper.program = library_.program(SlotAddress{0U, 0U, upper.soundSlotId});
            if (upper.program == nullptr) {
                setTextError("COMBI references an empty SOUND slot");
                return false;
            }
        }
        return installState(std::move(next));
    }''',
"activate COMBI by stable SoundSlotID")
save(p, s)

# -----------------------------------------------------------------------------
# Core compatibility test: deliberately non-positional IDs prove dictionary lookup.
# -----------------------------------------------------------------------------
p, s = load("core/tests/SamplerCoreTests.cpp")
s = rep(s,
'''    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.bank=2U; sound.page=3U; sound.slot=4U;''',
'''    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.soundSlotId=500U;''',
"test SOUND slot 500")
s = rep(s,
'''    SvSoundLayerRecord layer{}; layer.multisampleIndex=0U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;''',
'''    SvSoundLayerRecord layer{}; layer.multisampleId=4000001U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;''',
"test multisample stable id")
s = rep(s,
'''    SvMultisampleRecord ms{}; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;''',
'''    SvMultisampleRecord ms{}; ms.multisampleId=4000001U; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;''',
"test multisample record id")
s = rep(s,
'''    SvSampleRecord sample{}; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;''',
'''    SvSampleRecord sample{}; sample.sampleId=3000001U; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;''',
"test sample record id")
s = rep(s,
'''    SvZoneRecord zone{}; zone.sampleIndex=0U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;''',
'''    SvZoneRecord zone{}; zone.sampleId=3000001U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;''',
"test sample stable id")
s = rep(s,
'''    assert(loaded.data.programs[0].address.bank==2U && loaded.data.programs[0].address.page==3U && loaded.data.programs[0].address.slot==4U);''',
'''    assert(loaded.data.programs[0].address.bank==0U && loaded.data.programs[0].address.page==0U && loaded.data.programs[0].address.slot==500U);
    assert(loaded.data.samples[0].id==3000001U); assert(loaded.data.multisamples[0].id==4000001U);''',
"test fixed slot and stable IDs")
save(p, s)

print("PASS: fixed 500 SOUND/COMBI slot identity + uint32 SAMPLE/MULTISAMPLE IDs + SV v2 stable references")
