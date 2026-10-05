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
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"patch anchor not unique: {label} ({count})")
    return text.replace(old, new, 1)

# -----------------------------------------------------------------------------
# Fixed visible positions + stable internal resource identity.
# SOUND / COMBI are user-facing fixed positions 1..500.
# SAMPLE / MULTISAMPLE are stable uint32 IDs and are never limited to 500.
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
    // Persistent SOUND position. 1..500 = bank SOUND; 0 = no fixed SOUND.
    std::uint16_t soundSlotId{kNoSoundSlot};''',
"UpperPart sound slot")
save(p, s)

# -----------------------------------------------------------------------------
# SET persistence. v10 adds the persistent SoundSlotID in each COMBI Upper.
# Old SET versions remain readable.
# -----------------------------------------------------------------------------
p, s = load("core/src/sampler/SetPackage.cpp")
s = rep(s, "constexpr std::uint32_t kVersion = 9U;", "constexpr std::uint32_t kVersion = 10U;", "SET v10")
s = rep(s,
'''void writeUpper(Writer&w,const UpperPart&u,const Graph&g){
    w.str(u.name);const auto it=g.programIds.find(u.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);''',
'''void writeUpper(Writer&w,const UpperPart&u,const Graph&g){
    w.str(u.name);const auto it=g.programIds.find(u.program.get());w.u32(it==g.programIds.end()?0xFFFFFFFFU:it->second);w.u16(u.soundSlotId);''',
"write Upper SoundSlotID")
s = rep(s,
'''UpperPart readUpper(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){
    UpperPart u;u.name=r.str();const auto id=r.u32();if(id<p.size())u.program=p[id];''',
'''UpperPart readUpper(Reader&r,const std::vector<ProgramPtr>&p,std::uint32_t version){
    UpperPart u;u.name=r.str();const auto id=r.u32();if(id<p.size())u.program=p[id];if(version>=10U)u.soundSlotId=r.u16();''',
"read Upper SoundSlotID")
# Corruption guards only. They are deliberately far above realistic Korg-sized sets.
s = s.replace("sampleCount>100000U", "sampleCount>10000000U")
s = s.replace("msCount>100000U", "msCount>10000000U")
s = s.replace("sampleCatalogCount>100000U", "sampleCatalogCount>10000000U")
s = s.replace("msCatalogCount>100000U", "msCatalogCount>10000000U")
save(p, s)

# -----------------------------------------------------------------------------
# SILVETON_BANK.sv v2: references are stable IDs, never array positions.
# -----------------------------------------------------------------------------
p, s = load("core/include/silveton/sampler/SvBankFormat.h")
s = rep(s, "constexpr std::uint16_t kSvBankVersionMajor = 1U;", "constexpr std::uint16_t kSvBankVersionMajor = 2U;", "SV major")
s = rep(s, "constexpr std::array<std::uint8_t, 8> kSvBankMagic{{'S','I','L','V','S','V','1',0}};", "constexpr std::array<std::uint8_t, 8> kSvBankMagic{{'S','I','L','V','S','V','2',0}};", "SV magic")
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
"SV SOUND record")
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
"SV MULTISAMPLE record")
s = rep(s,
'''struct SvSampleRecord {
    std::uint32_t nameOffset{0};''',
'''struct SvSampleRecord {
    std::uint32_t sampleId{0};
    std::uint32_t nameOffset{0};''',
"SV SAMPLE record")
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
"append SOUND")
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
"append MULTISAMPLE")
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
"read SOUND")
s = s.replace("readU32(p,end,r.multisampleIndex)", "readU32(p,end,r.multisampleId)", 1)
s = rep(s,
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.firstZone) && readU32(p,end,r.zoneCount);''',
'''    return readU32(p,end,r.multisampleId) && readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.firstZone) && readU32(p,end,r.zoneCount);''',
"read MULTISAMPLE")
s = rep(s,
'''    return readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.sampleRate) && readU16(p,end,r.channels) &&''',
'''    return readU32(p,end,r.sampleId) && readU32(p,end,r.nameOffset) && readU32(p,end,r.sourceIdentityOffset) && readU32(p,end,r.sampleRate) && readU16(p,end,r.channels) &&''',
"read SAMPLE")
s = s.replace("readU32(p,end,r.sampleIndex)", "readU32(p,end,r.sampleId)", 1)
save(p, s)

# -----------------------------------------------------------------------------
# v2 bank resolver. Full sample audio stays mmap-backed. Every SAMPLE only keeps
# the existing 4096-frame attack prefix in RAM so any SOUND can start instantly.
# -----------------------------------------------------------------------------
p, s = load("core/src/sampler/SvBankRuntime.cpp")
s = rep(s, "#include <memory>\n#include <set>", "#include <memory>\n#include <map>\n#include <set>", "SV map include")
s = rep(s,
'''    std::vector<SamplePtr> samples;
    samples.reserve(header.sampleCount);
    out.data.samples.reserve(header.sampleCount);''',
'''    std::vector<SamplePtr> samples;
    samples.reserve(header.sampleCount);
    std::map<std::uint32_t, SamplePtr> sampleById;
    out.data.samples.reserve(header.sampleCount);''',
"SAMPLE lookup")
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
"store SAMPLE stable id")
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
"resolve SAMPLE stable id")
s = rep(s,
'''    std::vector<MultisamplePtr> multisamples;
    multisamples.reserve(header.multisampleCount);
    out.data.multisamples.reserve(header.multisampleCount);''',
'''    std::vector<MultisamplePtr> multisamples;
    multisamples.reserve(header.multisampleCount);
    std::map<std::uint32_t, MultisamplePtr> multisampleById;
    out.data.multisamples.reserve(header.multisampleCount);''',
"MULTISAMPLE lookup")
s = rep(s,
'''        multisamples.push_back(ms);
        out.data.multisamples.push_back({i + 1U, ms});''',
'''        if (r.multisampleId == 0U || !multisampleById.emplace(r.multisampleId, ms).second) {
            out.error = "SV bank contains invalid or duplicate MULTISAMPLE id";
            return out;
        }
        multisamples.push_back(ms);
        out.data.multisamples.push_back({r.multisampleId, ms});''',
"store MULTISAMPLE stable id")
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
"resolve MULTISAMPLE stable id")
s = rep(s,
'''    std::set<std::tuple<std::uint16_t, std::uint16_t, std::uint16_t>> addresses;''',
'''    std::set<std::uint16_t> soundSlots;''',
"SOUND slot set")
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
"validate fixed SOUND")
s = rep(s,
'''        out.data.programs.push_back({SlotAddress{r.bank, r.page, r.slot}, program});''',
'''        out.data.programs.push_back({SlotAddress{0U, 0U, r.soundSlotId}, program});''',
"install fixed SOUND")
save(p, s)

# -----------------------------------------------------------------------------
# Stable resource allocation. Deleted IDs become persistent tombstones in SET
# state; new imports allocate above the current maximum instead of filling gaps.
# -----------------------------------------------------------------------------
p, s = load("core/src/sampler/SamplerLibrary.cpp")
s = rep(s,
'''template <typename Map>
std::optional<std::uint32_t> firstFreeId(const Map& map, const std::unordered_set<std::uint32_t>& reserved) {
    for (std::uint32_t candidate = 1U; candidate < std::numeric_limits<std::uint32_t>::max(); ++candidate) {
        if (map.find(candidate) == map.end() && reserved.find(candidate) == reserved.end()) return candidate;
    }
    return std::nullopt;
}''',
'''template <typename Map>
std::optional<std::uint32_t> firstFreeId(const Map& map, const std::unordered_set<std::uint32_t>& reserved) {
    std::uint64_t candidate = 1U;
    if (!map.empty()) candidate = static_cast<std::uint64_t>(map.rbegin()->first) + 1U;
    while (candidate <= static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        const auto id = static_cast<std::uint32_t>(candidate);
        if (id != 0U && map.find(id) == map.end() && reserved.find(id) == reserved.end()) return id;
        ++candidate;
    }
    return std::nullopt;
}''',
"monotonic resource IDs")
s = s.replace(
'''bool SamplerLibrary::writeSample(std::uint32_t id, SamplePtr sample) {
    if (sample == nullptr || !sample->valid()) return false;''',
'''bool SamplerLibrary::writeSample(std::uint32_t id, SamplePtr sample) {
    if (id == 0U || sample == nullptr || !sample->valid()) return false;''', 1)
s = s.replace(
'''bool SamplerLibrary::writeMultisample(std::uint32_t id, MultisamplePtr multisample) {
    if (multisample == nullptr || multisample->name.empty() || multisample->zones.empty()) return false;''',
'''bool SamplerLibrary::writeMultisample(std::uint32_t id, MultisamplePtr multisample) {
    if (id == 0U || multisample == nullptr || multisample->name.empty() || multisample->zones.empty()) return false;''', 1)
s = rep(s,
'''bool SamplerLibrary::clearSample(std::uint32_t id) { return samples_.erase(id) != 0U; }
bool SamplerLibrary::clearMultisample(std::uint32_t id) { return multisamples_.erase(id) != 0U; }''',
'''bool SamplerLibrary::clearSample(std::uint32_t id) {
    const auto it = samples_.find(id);
    if (it == samples_.end()) return false;
    it->second.reset(); // tombstone: this ID must never silently become another SAMPLE
    return true;
}
bool SamplerLibrary::clearMultisample(std::uint32_t id) {
    const auto it = multisamples_.find(id);
    if (it == multisamples_.end()) return false;
    it->second.reset(); // tombstone: preserve stable MULTISAMPLE identity
    return true;
}''',
"resource tombstones")
s = rep(s,
'''    for (const auto& item : data.samples) if (item.sample != nullptr) samples_[item.id] = item.sample;
    for (const auto& item : data.multisamples) if (item.multisample != nullptr) multisamples_[item.id] = item.multisample;''',
'''    for (const auto& item : data.samples) if (item.id != 0U) samples_[item.id] = item.sample;
    for (const auto& item : data.multisamples) if (item.id != 0U) multisamples_[item.id] = item.multisample;''',
"preserve tombstones on replace")
save(p, s)

# -----------------------------------------------------------------------------
# Realtime graph switching. The old implementation stopped/reopened Direct ALSA
# every time a SOUND/COMBI/control changed. Publish immutable state on control
# thread, then swap it at the next audio block boundary without stopping ALSA.
# Existing voices own the graph they were created from, so tails remain valid.
# -----------------------------------------------------------------------------
p, s = load("core/include/silveton/sampler/SamplerEngine.h")
s = rep(s,
'''    // State replacement is intentionally an out-of-audio-thread operation.
    // The Android wrapper stops the stream before replacing the immutable graph.
    void setState(std::shared_ptr<const EngineState> state) noexcept;
    [[nodiscard]] std::shared_ptr<const EngineState> state() const noexcept { return state_; }''',
'''    // Control thread publishes a fully prepared immutable graph. The audio thread
    // adopts it at a block boundary; Direct ALSA is never stopped for SOUND/COMBI changes.
    void setState(std::shared_ptr<const EngineState> state) noexcept;
    [[nodiscard]] std::shared_ptr<const EngineState> state() const noexcept {
        return std::atomic_load_explicit(&state_, std::memory_order_acquire);
    }''',
"SamplerEngine state API")
s = rep(s,
'''    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const EngineState> state_;
    RealtimeEventQueue events_;''',
'''    std::unique_ptr<Impl> impl_;
    std::shared_ptr<const EngineState> state_;
    std::shared_ptr<const EngineState> pendingState_;
    RealtimeEventQueue events_;''',
"pending state member")
save(p, s)

p, s = load("core/src/sampler/SamplerEngine.cpp")
s = rep(s,
'''        int upperIndex{0};
        SamplePtr sample;
        const ProgramLayer* layer{nullptr};''',
'''        int upperIndex{0};
        std::uint16_t routeMask{0U};
        SamplePtr sample;
        std::shared_ptr<const EngineState> ownerState;
        const ProgramLayer* layer{nullptr};''',
"voice graph ownership")
s = rep(s,
'''void SamplerEngine::setState(std::shared_ptr<const EngineState> state) noexcept {
    if (state == nullptr) return;
    // Voices keep raw pointers into the immutable state graph. State replacement
    // is therefore a hard graph boundary: caller stops audio, and we clear all
    // voice/DSP state before releasing the old graph so no dangling pointer can
    // survive a PROGRAM/COMBI/SET switch.
    impl_->reset(impl_->sampleRate);
    events_.clear();
    state_ = std::move(state);
    // Updating EQ coefficients here is safe because caller must stop audio first.
    const auto& g = state_->global;
    impl_->lowEq.setShelf(impl_->sampleRate, 120.0F, g.eqLowDb, false);
    impl_->midEq.setPeaking(impl_->sampleRate, 1200.0F, 0.75F, g.eqMidDb);
    impl_->highEq.setShelf(impl_->sampleRate, 7000.0F, g.eqHighDb, true);
    for (int i = 0; i < kUpperCount; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto& u = state_->combi.uppers[index];
        std::uint16_t routeMask = 0U;
        for (const auto& route : u.controllerRoutes) {
            if (route.source == ControllerSource::Off || std::abs(route.amount) <= 1.0e-9F) continue;
            routeMask = static_cast<std::uint16_t>(routeMask | static_cast<std::uint16_t>(1U << static_cast<unsigned>(route.destination)));
        }
        impl_->upperRouteMask[index] = routeMask;
        impl_->upperLowEq[index].setShelf(impl_->sampleRate, 120.0F, u.eqLowDb, false);
        impl_->upperMidEq[index].setPeaking(impl_->sampleRate, 1200.0F, 0.75F, u.eqMidDb);
        impl_->upperHighEq[index].setShelf(impl_->sampleRate, 7000.0F, u.eqHighDb, true);
    }
}''',
'''void SamplerEngine::setState(std::shared_ptr<const EngineState> state) noexcept {
    if (state == nullptr) return;
    // No file I/O, no locks and no ALSA stop here. Latest complete graph wins.
    std::atomic_store_explicit(&pendingState_, std::move(state), std::memory_order_release);
}''',
"publish pending graph")
s = rep(s,
'''void startVoice(SamplerEngine::Impl& impl,
                 SamplerEngine::Impl::Voice& v,
                 const UpperPart& upper,
                 const ProgramLayer& layer,
                 const SampleZone& zone,
                 int upperIndex,
                 std::uint8_t channel,
                 std::uint8_t key,
                 std::uint8_t velocity,
                 const GlobalState& global) noexcept {''',
'''void startVoice(SamplerEngine::Impl& impl,
                 SamplerEngine::Impl::Voice& v,
                 const UpperPart& upper,
                 const ProgramLayer& layer,
                 const SampleZone& zone,
                 int upperIndex,
                 std::uint8_t channel,
                 std::uint8_t key,
                 std::uint8_t velocity,
                 const GlobalState& global,
                 const std::shared_ptr<const EngineState>& ownerState) noexcept {''',
"startVoice owner")
s = rep(s,
'''    v.upperIndex = upperIndex;
    v.sample = zone.sample;
    v.layer = &layer;''',
'''    v.upperIndex = upperIndex;
    v.routeMask = impl.upperRouteMask[static_cast<std::size_t>(std::clamp(upperIndex, 0, kUpperCount - 1))];
    v.sample = zone.sample;
    v.ownerState = ownerState;
    v.layer = &layer;''',
"voice owner assignment")
s = rep(s,
'''void processEvent(SamplerEngine::Impl& impl, const EngineState& state, const RealtimeEvent& e) noexcept {
    const std::uint8_t channel = static_cast<std::uint8_t>(e.channel & 0x0FU);''',
'''void processEvent(SamplerEngine::Impl& impl, const std::shared_ptr<const EngineState>& stateOwner, const RealtimeEvent& e) noexcept {
    if (stateOwner == nullptr) return;
    const auto& state = *stateOwner;
    const std::uint8_t channel = static_cast<std::uint8_t>(e.channel & 0x0FU);''',
"processEvent owner")
s = rep(s,
'''                        startVoice(impl, *v, upper, layer, zone, ui, e.channel, e.data1, e.data2, state.global);''',
'''                        startVoice(impl, *v, upper, layer, zone, ui, e.channel, e.data1, e.data2, state.global, stateOwner);''',
"startVoice call")
s = rep(s,
'''void SamplerEngine::render(float* out, std::uint32_t frames) noexcept {
    if (out == nullptr || frames == 0U) return;
    std::memset(out, 0, static_cast<std::size_t>(frames) * 2U * sizeof(float));
    const auto current = state_;
    if (current == nullptr) return;

    RealtimeEvent ev{};
    while (events_.pop(ev)) processEvent(*impl_, *current, ev);''',
'''void SamplerEngine::render(float* out, std::uint32_t frames) noexcept {
    if (out == nullptr || frames == 0U) return;
    std::memset(out, 0, static_cast<std::size_t>(frames) * 2U * sizeof(float));

    // Adopt a complete control-thread graph only at a realtime block boundary.
    // Do not reset voices or FX: old voices own their immutable source graph and
    // continue naturally while new notes immediately use the new SOUND/COMBI.
    auto pending = std::atomic_exchange_explicit(
        &pendingState_, std::shared_ptr<const EngineState>{}, std::memory_order_acq_rel);
    if (pending != nullptr) {
        std::atomic_store_explicit(&state_, pending, std::memory_order_release);
        const auto& g = pending->global;
        impl_->lowEq.setShelf(impl_->sampleRate, 120.0F, g.eqLowDb, false);
        impl_->midEq.setPeaking(impl_->sampleRate, 1200.0F, 0.75F, g.eqMidDb);
        impl_->highEq.setShelf(impl_->sampleRate, 7000.0F, g.eqHighDb, true);
        for (int i = 0; i < kUpperCount; ++i) {
            const auto index = static_cast<std::size_t>(i);
            const auto& u = pending->combi.uppers[index];
            std::uint16_t routeMask = 0U;
            for (const auto& route : u.controllerRoutes) {
                if (route.source == ControllerSource::Off || std::abs(route.amount) <= 1.0e-9F) continue;
                routeMask = static_cast<std::uint16_t>(routeMask | static_cast<std::uint16_t>(1U << static_cast<unsigned>(route.destination)));
            }
            impl_->upperRouteMask[index] = routeMask;
            impl_->upperLowEq[index].setShelf(impl_->sampleRate, 120.0F, u.eqLowDb, false);
            impl_->upperMidEq[index].setPeaking(impl_->sampleRate, 1200.0F, 0.75F, u.eqMidDb);
            impl_->upperHighEq[index].setShelf(impl_->sampleRate, 7000.0F, u.eqHighDb, true);
        }
    }

    const auto current = std::atomic_load_explicit(&state_, std::memory_order_acquire);
    if (current == nullptr) return;

    RealtimeEvent ev{};
    while (events_.pop(ev)) processEvent(*impl_, current, ev);''',
"block-boundary state swap")
s = rep(s,
'''            const std::uint16_t routeMask = impl_->upperRouteMask[ui];''',
'''            const std::uint16_t routeMask = v.routeMask;''',
"per-voice route mask")
save(p, s)

# -----------------------------------------------------------------------------
# Android native wrapper: SOUND/COMBI changes no longer stop/reopen Direct ALSA.
# The visible Upper name always comes from the exact bank SOUND name.
# -----------------------------------------------------------------------------
p, s = load("android/app/src/main/cpp/SamplerAudioBridge.cpp")
s = rep(s,
'''        state->combi.uppers[0].name = "UPPER 1";
        state->combi.uppers[0].enabled = true;
        state->combi.uppers[0].program = loaded.data.programs.front().program;
        state->combi.uppers[0].midiChannel = 0U;''',
'''        state->combi.uppers[0].name = loaded.data.programs.front().program->name;
        state->combi.uppers[0].enabled = true;
        state->combi.uppers[0].program = loaded.data.programs.front().program;
        state->combi.uppers[0].soundSlotId = loaded.data.programs.front().address.slot;
        state->combi.uppers[0].midiChannel = 0U;''',
"initial bank SOUND exact name")
s = rep(s,
'''        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program = program;
        upper.enabled = true;
        if (upper.name.empty()) upper.name = "Upper " + std::to_string(upperIndex + 1);''',
'''        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program = program;
        upper.soundSlotId = (address.bank == 0U && address.page == 0U && address.slot >= 1U && address.slot <= kFixedSoundSlotCount) ? address.slot : kNoSoundSlot;
        upper.name = program->name; // visible name arrives 1:1 from SILVETON_BANK.sv
        upper.enabled = true;''',
"activate fixed SOUND")
s = rep(s,
'''        upper.program.reset();
        upper.enabled = false;''',
'''        upper.program.reset();
        upper.soundSlotId = kNoSoundSlot;
        upper.enabled = false;''',
"clear fixed SOUND")
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
            upper.name = upper.program->name;
        }
        return installState(std::move(next));
    }''',
"COMBI resolves fixed SOUND")
s = rep(s,
'''    bool installState(std::shared_ptr<const EngineState> state) {
        std::lock_guard<std::mutex> lock(controlMutex_);
        const bool resume = started_.load(std::memory_order_acquire);
        if (!stopLocked()) {
            setTextError("Direct ALSA did not stop cleanly before state replacement");
            return false;
        }
        state_ = std::move(state);
        // Keep TEST EP enabled for hardware validation. External MIDI note-on/off
        // always reaches this native instrument, independently of bank content.
        testInstrument_.setEnabled(true);
        engine_.setState(state_);
        return !resume || startLocked();
    }''',
'''    bool installState(std::shared_ptr<const EngineState> state) {
        if (state == nullptr) return false;
        std::lock_guard<std::mutex> lock(controlMutex_);
        state_ = std::move(state);
        const bool hasPlayableProgram = std::any_of(
            state_->combi.uppers.begin(), state_->combi.uppers.end(),
            [](const UpperPart& u) { return u.enabled && u.program != nullptr; });
        // Fallback is only for an empty fresh install. It must never layer over a real bank SOUND.
        testInstrument_.setEnabled(!hasPlayableProgram);
        engine_.setState(state_);
        return true;
    }''',
"no-stop state install")
save(p, s)

# -----------------------------------------------------------------------------
# Core test: deliberately huge non-positional IDs prove that no array index is
# being mistaken for a persistent resource ID.
# -----------------------------------------------------------------------------
test_candidates = [
    ROOT / "core/tests/CoreTests.cpp",
    ROOT / "core/tests/SamplerCoreTests.cpp",
]
test_path = next((q for q in test_candidates if q.is_file()), None)
if test_path is None:
    raise SystemExit("missing sampler core test source")
s = test_path.read_text(encoding="utf-8")
s = rep(s,
'''    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.bank=2U; sound.page=3U; sound.slot=4U;''',
'''    SvSoundRecord sound{}; sound.nameOffset=soundName; sound.firstLayer=0U; sound.layerCount=1U; sound.soundSlotId=500U;''',
"test SOUND 500")
s = rep(s,
'''    SvSoundLayerRecord layer{}; layer.multisampleIndex=0U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;''',
'''    SvSoundLayerRecord layer{}; layer.multisampleId=4000001U; layer.enabled=1U; layer.keyLow=0U; layer.keyHigh=127U; layer.velocityLow=1U; layer.velocityHigh=127U;''',
"test MULTISAMPLE ref")
s = rep(s,
'''    SvMultisampleRecord ms{}; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;''',
'''    SvMultisampleRecord ms{}; ms.multisampleId=4000001U; ms.nameOffset=msName; ms.sourceIdentityOffset=msIdentity; ms.firstZone=0U; ms.zoneCount=1U;''',
"test MULTISAMPLE id")
s = rep(s,
'''    SvSampleRecord sample{}; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;''',
'''    SvSampleRecord sample{}; sample.sampleId=3000001U; sample.nameOffset=sampleName; sample.sourceIdentityOffset=sampleIdentity; sample.sampleRate=48000U; sample.channels=1U;''',
"test SAMPLE id")
s = rep(s,
'''    SvZoneRecord zone{}; zone.sampleIndex=0U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;''',
'''    SvZoneRecord zone{}; zone.sampleId=3000001U; zone.keyLow=0U; zone.keyHigh=127U; zone.velocityLow=1U; zone.velocityHigh=127U; zone.rootKey=60U;''',
"test SAMPLE ref")
s = rep(s,
'''    assert(loaded.data.programs[0].address.bank==2U && loaded.data.programs[0].address.page==3U && loaded.data.programs[0].address.slot==4U);''',
'''    assert(loaded.data.programs[0].address.bank==0U && loaded.data.programs[0].address.page==0U && loaded.data.programs[0].address.slot==500U);
    assert(loaded.data.samples[0].id==3000001U); assert(loaded.data.multisamples[0].id==4000001U);''',
"test stable IDs")
test_path.write_text(s, encoding="utf-8")

# Final build-time contract checks.
checks = {
    "core/include/silveton/sampler/SamplerTypes.h": [
        "kFixedSoundSlotCount = 500U", "kFixedCombiSlotCount = 500U", "soundSlotId{kNoSoundSlot}"
    ],
    "core/include/silveton/sampler/SvBankFormat.h": [
        "kSvBankVersionMajor = 2U", "'S','I','L','V','S','V','2',0", "sampleId{0}", "multisampleId{0}"
    ],
    "core/src/sampler/SamplerEngine.cpp": [
        "std::atomic_store_explicit(&pendingState_", "std::atomic_exchange_explicit(", "v.ownerState = ownerState", "const std::uint16_t routeMask = v.routeMask"
    ],
    "android/app/src/main/cpp/SamplerAudioBridge.cpp": [
        "testInstrument_.setEnabled(!hasPlayableProgram)", "upper.name = program->name", "COMBI references an empty SOUND slot"
    ],
}
for rel, tokens in checks.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for token in tokens:
        if token not in text:
            raise SystemExit(f"bank runtime v2 contract missing in {rel}: {token}")

bridge = (ROOT / "android/app/src/main/cpp/SamplerAudioBridge.cpp").read_text(encoding="utf-8")
install_begin = bridge.find("    bool installState(std::shared_ptr<const EngineState> state) {")
install_end = bridge.find("    void syncCurrentStateToLibrary()", install_begin)
if install_begin < 0 or install_end < 0:
    raise SystemExit("cannot locate final installState")
install_block = bridge[install_begin:install_end]
if "stopLocked()" in install_block or "startLocked()" in install_block:
    raise SystemExit("SOUND/COMBI state install still restarts Direct ALSA")

print("PASS: SV v2 stable IDs + 500 SOUND/COMBI identity + exact bank names + mmap attack preload + seamless block-boundary switching")
