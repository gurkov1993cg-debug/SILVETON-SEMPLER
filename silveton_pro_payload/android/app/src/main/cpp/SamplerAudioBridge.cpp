#ifdef __ANDROID__

#include <jni.h>
#include <android/log.h>
#include <sound/asound.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>

#include "silveton/sampler/AudioFileLoader.h"
#include "silveton/sampler/KorgImporter.h"
#include "silveton/sampler/MultisampleEditor.h"
#include "silveton/sampler/SampleEditor.h"
#include "silveton/sampler/ProgramEditor.h"
#include "silveton/sampler/SamplerEngine.h"
#include "silveton/sampler/SamplerLibrary.h"
#include "silveton/sampler/SetPackage.h"
#include "silveton/sampler/SvBankRuntime.h"
#include "silveton/sampler/SampleStreamPrefetcher.h"

#include <algorithm>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace silveton::sampler;

constexpr const char* kTag = "SilvetonSampler";

// Built-in playable fallback instrument. It is deliberately generated in the
// native realtime path so a fresh APK can verify MIDI -> DSP -> Direct ALSA
// without requiring a bank file. As soon as a real PROGRAM/COMBI is loaded,
// installState() disables this fallback automatically.
class BuiltinTestInstrument {
public:
    static constexpr std::size_t kVoiceCount = 24U;
    static constexpr std::size_t kTableSize = 2048U;
    static constexpr std::size_t kEventCount = 128U;

    BuiltinTestInstrument() noexcept {
        constexpr double pi = 3.14159265358979323846;
        for (std::size_t i = 0; i < kTableSize; ++i) {
            const double p = 2.0 * pi * static_cast<double>(i) / static_cast<double>(kTableSize);
            table_[i] = static_cast<float>(
                0.78 * std::sin(p) +
                0.16 * std::sin(2.0 * p) +
                0.06 * std::sin(3.0 * p));
        }
    }

    void setEnabled(bool enabled) noexcept {
        enabled_.store(enabled, std::memory_order_release);
        if (!enabled) push({3U, 0U, 0U});
    }

    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    void noteOn(std::uint8_t key, std::uint8_t velocity) noexcept {
        if (enabled() && velocity != 0U) push({1U, key, velocity});
    }
    void noteOff(std::uint8_t key) noexcept {
        if (enabled()) push({2U, key, 0U});
    }
    void panic() noexcept { push({3U, 0U, 0U}); }

    void render(float* interleavedStereo, std::uint32_t frames, int sampleRate) noexcept {
        if (interleavedStereo == nullptr || frames == 0U || sampleRate <= 0) return;
        consumeEvents(sampleRate);
        if (!enabled()) return;

        const float sr = static_cast<float>(sampleRate);
        const float attackStep = 1.0F / std::max(1.0F, sr * 0.004F);
        const float decayK = 1.0F - std::exp(-1.0F / std::max(1.0F, sr * 0.24F));
        const float releaseMul = std::exp(std::log(0.0001F) / std::max(1.0F, sr * 0.32F));

        for (std::uint32_t f = 0; f < frames; ++f) {
            float mono = 0.0F;
            for (auto& v : voices_) {
                if (!v.active) continue;

                if (!v.releasing) {
                    if (v.env < 0.999F && v.attack) {
                        v.env += attackStep;
                        if (v.env >= 1.0F) { v.env = 1.0F; v.attack = false; }
                    } else {
                        v.env += (0.48F - v.env) * decayK;
                    }
                } else {
                    v.env *= releaseMul;
                    if (v.env < 0.0001F) { v.active = false; continue; }
                }

                const auto i0 = static_cast<std::size_t>(v.phase) & (kTableSize - 1U);
                const auto i1 = (i0 + 1U) & (kTableSize - 1U);
                const float frac = v.phase - static_cast<float>(static_cast<std::size_t>(v.phase));
                const float osc = table_[i0] + (table_[i1] - table_[i0]) * frac;
                mono += osc * v.env * v.velocityGain * 0.035F;

                v.phase += v.phaseStep;
                if (v.phase >= static_cast<float>(kTableSize)) v.phase -= static_cast<float>(kTableSize);
            }

            interleavedStereo[static_cast<std::size_t>(f) * 2U] += mono;
            interleavedStereo[static_cast<std::size_t>(f) * 2U + 1U] += mono;
        }
    }

private:
    struct Event { std::uint8_t type; std::uint8_t key; std::uint8_t velocity; };
    struct Voice {
        bool active{false};
        bool releasing{false};
        bool attack{true};
        std::uint8_t key{0U};
        float phase{0.0F};
        float phaseStep{0.0F};
        float env{0.0F};
        float velocityGain{0.0F};
        std::uint64_t age{0U};
    };

    void push(Event e) noexcept {
        if (writeLock_.test_and_set(std::memory_order_acquire)) return;
        const unsigned w = write_.load(std::memory_order_relaxed);
        const unsigned next = (w + 1U) % static_cast<unsigned>(kEventCount);
        if (next != read_.load(std::memory_order_acquire)) {
            events_[w] = e;
            write_.store(next, std::memory_order_release);
        }
        writeLock_.clear(std::memory_order_release);
    }

    void consumeEvents(int sampleRate) noexcept {
        unsigned r = read_.load(std::memory_order_relaxed);
        const unsigned w = write_.load(std::memory_order_acquire);
        while (r != w) {
            const Event e = events_[r];
            r = (r + 1U) % static_cast<unsigned>(kEventCount);
            if (e.type == 1U) startVoice(e.key, e.velocity, sampleRate);
            else if (e.type == 2U) releaseKey(e.key);
            else if (e.type == 3U) for (auto& v : voices_) v = Voice{};
        }
        read_.store(r, std::memory_order_release);
    }

    void startVoice(std::uint8_t key, std::uint8_t velocity, int sampleRate) noexcept {
        Voice* chosen = nullptr;
        for (auto& v : voices_) if (!v.active) { chosen = &v; break; }
        if (chosen == nullptr) {
            chosen = &voices_[0];
            for (auto& v : voices_) if (v.env < chosen->env || (v.env == chosen->env && v.age < chosen->age)) chosen = &v;
        }
        const double frequency = 440.0 * std::pow(2.0, (static_cast<int>(key) - 69) / 12.0);
        chosen->active = true;
        chosen->releasing = false;
        chosen->attack = true;
        chosen->key = key;
        chosen->phase = 0.0F;
        chosen->phaseStep = static_cast<float>(frequency * static_cast<double>(kTableSize) / static_cast<double>(sampleRate));
        chosen->env = 0.0F;
        chosen->velocityGain = std::sqrt(static_cast<float>(velocity) / 127.0F);
        chosen->age = ++age_;
    }

    void releaseKey(std::uint8_t key) noexcept {
        for (auto& v : voices_) if (v.active && v.key == key) v.releasing = true;
    }

    std::array<float, kTableSize> table_{};
    std::array<Voice, kVoiceCount> voices_{};
    std::array<Event, kEventCount> events_{};
    std::atomic<unsigned> write_{0U};
    std::atomic<unsigned> read_{0U};
    std::atomic<bool> enabled_{true};
    std::atomic_flag writeLock_ = ATOMIC_FLAG_INIT;
    std::uint64_t age_{0U};
};

static void configureRealtimeThread(const char* name, int firstCpu, int lastCpu, int fifoPriority) noexcept {
    if (name != nullptr) (void)::pthread_setname_np(::pthread_self(), name);

    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = std::max(0, firstCpu); cpu <= std::max(firstCpu, lastCpu); ++cpu) CPU_SET(cpu, &set);
    (void)::sched_setaffinity(0, sizeof(set), &set);

    sched_param param{};
    param.sched_priority = fifoPriority;
    if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        // Ordinary Android processes may not hold CAP_SYS_NICE. Root mode will
        // promote the named TID from outside; nice still gives a useful fallback.
        (void)::setpriority(PRIO_PROCESS, 0, -16);
    }
}

std::string jstringToUtf8(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const char* raw = env->GetStringUTFChars(s, nullptr);
    if (raw == nullptr) return {};
    std::string out(raw);
    env->ReleaseStringUTFChars(s, raw);
    return out;
}

class AndroidSamplerAudio {
public:
    AndroidSamplerAudio() = default;
    ~AndroidSamplerAudio() { close(); }

    bool open() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        // MIDI is an independent realtime input path. Start it first so a
        // temporary PCM failure never makes the keyboard disappear.
        const bool midiOk = startDirectMidiLocked();
        // Bank read-ahead runs on an ordinary thread (CPU 0-5), never on the audio core.
        prefetcher_.setCpuRange(0, 5);
        if (!prefetcher_.start()) {
            __android_log_print(ANDROID_LOG_WARN, kTag, "Bank read-ahead thread could not start");
        }
        const bool audioOk = openLocked();
        if (!audioOk) return false;
        if (!midiOk) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                "Direct MIDI did not start while audio opened successfully");
        }
        return true;
    }

    bool start() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        wantStarted_.store(true, std::memory_order_release);
        return startLocked();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        wantStarted_.store(false, std::memory_order_release);
        (void)stopLocked();
    }

    void close() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        wantStarted_.store(false, std::memory_order_release);
        (void)stopLocked();
        stopDirectMidiLocked();
        prefetcher_.stop();
        closePcmLocked();
    }

    bool recover() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (!recoverRequested_.load(std::memory_order_acquire)) return true;
        const bool resume = wantStarted_.load(std::memory_order_acquire);
        (void)stopLocked();
        closePcmLocked();
        if (!openLocked()) return false;
        recoverRequested_.store(false, std::memory_order_release);
        return !resume || startLocked();
    }

    bool loadAudioFile(const std::string& path) {
        // Two seconds at 48 kHz is preloaded for instant attack; the remainder
        // stays mmap-backed. Higher-rate files preload the same frame count,
        // still avoiding full-library RAM residency on the 4 GB target tablet.
        const auto loaded = AudioFileLoader::loadStreaming(path, 96000U);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        return installMultisample(makeSingleSampleMultisample(loaded.sample, loaded.sample->name), loaded.sample->name);
    }

    bool loadKmp(const std::string& path) {
        const auto imported = library_.importKmp(path);
        if (!imported.success || imported.multisampleIds.empty()) {
            setTextError(imported.error.empty() ? "KMP library import returned no multisample" : imported.error);
            return false;
        }
        const auto ms = library_.multisample(imported.multisampleIds.front());
        if (ms == nullptr) { setTextError("Imported KMP multisample is missing from library"); return false; }
        return installMultisample(ms, ms->name);
    }

    std::vector<int> importKmpToLibrary(const std::string& path) {
        const auto imported = library_.importKmp(path);
        if (!imported.success) { setTextError(imported.error); return {}; }
        std::vector<int> ids; ids.reserve(imported.multisampleIds.size());
        for (const auto id : imported.multisampleIds) ids.push_back(static_cast<int>(id));
        setTextError({});
        return ids;
    }

    std::vector<int> importKscToLibrary(const std::string& path) {
        const auto imported = library_.importKsc(path);
        if (!imported.success) { setTextError(imported.error); return {}; }
        std::vector<int> ids; ids.reserve(imported.multisampleIds.size());
        for (const auto id : imported.multisampleIds) ids.push_back(static_cast<int>(id));
        setTextError({});
        return ids;
    }

    std::vector<int> multisampleIds() const {
        const auto raw = library_.multisampleIds();
        std::vector<int> ids; ids.reserve(raw.size());
        for (const auto id : raw) ids.push_back(static_cast<int>(id));
        return ids;
    }

    std::vector<std::string> multisampleNames() const { return library_.multisampleNames(); }

    std::vector<std::string> kscNames(const std::string& path) {
        if (!ensureKscCache(path)) return {};
        std::vector<std::string> names;
        names.reserve(kscCache_.multisamples.size());
        for (const auto& ms : kscCache_.multisamples) names.push_back(ms != nullptr ? ms->name : std::string{});
        return names;
    }

    bool loadKscItem(const std::string& path, int index) {
        if (!ensureKscCache(path)) return false;
        if (index < 0 || static_cast<std::size_t>(index) >= kscCache_.multisamples.size()) {
            setTextError("KSC multisample index is out of range");
            return false;
        }
        const auto& ms = kscCache_.multisamples[static_cast<std::size_t>(index)];
        if (ms == nullptr) { setTextError("KSC multisample is empty"); return false; }
        return installMultisample(ms, ms->name);
    }

    bool loadSvBank(const std::string& path) {
        const auto loaded = SvBankRuntime::load(path, 65536U, false);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        if (loaded.data.programs.empty() || loaded.data.programs.front().program == nullptr) {
            setTextError("SILVETON bank contains no playable SOUND");
            return false;
        }

        // Preserve user performance objects across a SOUND-bank replacement.
        // COMBI Upper parts persist fixed soundSlotId references, so after the
        // new SOUND resources are installed we rebind every Upper to the new
        // ProgramPtr from the same 1..500 SOUND position.
        const auto preserved = library_.snapshot();
        auto merged = loaded.data;
        merged.global = preserved.global;
        merged.combis = preserved.combis;
        merged.lists = preserved.lists;
        library_.replace(merged);

        for (const auto& saved : preserved.combis) {
            Combi rebound = saved.combi;
            for (auto& upper : rebound.uppers) {
                if (upper.soundSlotId == kNoSoundSlot) continue;
                if (upper.soundSlotId > kFixedSoundSlotCount) {
                    upper.program.reset();
                    upper.enabled = false;
                    continue;
                }
                upper.program = library_.program(SlotAddress{0U, 0U, upper.soundSlotId});
                if (upper.program != nullptr) {
                    upper.name = upper.program->name;
                } else {
                    upper.enabled = false;
                }
            }
            if (!library_.writeCombi(saved.address, rebound)) {
                setTextError("Failed to restore COMBI after SILVETON bank load");
                return false;
            }
        }
        // Do not accept a bank merely because SOUND names are visible. Select
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
    }

    bool loadSet(const std::string& path) {
        const auto loaded = SetPackage::load(path);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        library_.replace(loaded.data);
        auto state = std::make_shared<EngineState>();
        state->global = loaded.data.global;
        if (!loaded.data.combis.empty()) {
            state->combi = loaded.data.combis.front().combi;
        } else if (!loaded.data.programs.empty() && loaded.data.programs.front().program != nullptr) {
            state->combi.name = loaded.data.programs.front().program->name;
            state->combi.uppers[0].enabled = true;
            state->combi.uppers[0].program = loaded.data.programs.front().program;
            state->combi.uppers[0].midiChannel = 0U;
        } else {
            setTextError("SET contains no playable COMBI or PROGRAM");
            return false;
        }
        return installState(std::move(state));
    }

    bool saveSet(const std::string& path) {
        syncCurrentStateToLibrary();
        const auto saved = SetPackage::save(path, library_.snapshot());
        if (!saved.success) { setTextError(saved.error); return false; }
        setTextError({});
        return true;
    }

    bool saveUserPerformanceState(const std::string& path) {
        syncCurrentStateToLibrary();
        auto data = library_.snapshot();
        data.samples.clear();
        data.multisamples.clear();
        data.programs.clear();
        for (auto& slot : data.combis) {
            for (auto& upper : slot.combi.uppers) upper.program.reset();
        }
        const auto saved = SetPackage::save(path, data);
        if (!saved.success) { setTextError(saved.error); return false; }
        setTextError({});
        return true;
    }

    bool loadUserPerformanceState(const std::string& path) {
        const auto loaded = SetPackage::load(path);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }

        for (const auto& saved : loaded.data.combis) {
            Combi rebound = saved.combi;
            for (auto& upper : rebound.uppers) {
                if (upper.soundSlotId == kNoSoundSlot) {
                    upper.program.reset();
                    continue;
                }
                if (upper.soundSlotId > kFixedSoundSlotCount) {
                    upper.program.reset();
                    upper.enabled = false;
                    continue;
                }
                upper.program = library_.program(SlotAddress{0U, 0U, upper.soundSlotId});
                if (upper.program != nullptr) upper.name = upper.program->name;
                else upper.enabled = false;
            }
            if (!library_.writeCombi(saved.address, rebound)) {
                setTextError("Could not restore saved BLEND/COMBI");
                return false;
            }
        }
        for (const auto& saved : loaded.data.lists) {
            if (!library_.writeList(saved.address, saved.list)) {
                setTextError("Could not restore saved LIST");
                return false;
            }
        }
        setTextError({});
        return true;
    }

    bool saveBank(const std::string& path, int bank) {
        if (bank < 0 || bank > 65535) { setTextError("Bank number is out of range"); return false; }
        syncCurrentStateToLibrary();
        const auto bankData = library_.snapshotBank(static_cast<std::uint16_t>(bank));
        if (bankData.programs.empty() && bankData.combis.empty() && bankData.lists.empty()) {
            setTextError("Selected bank is empty");
            return false;
        }
        const auto saved = SetPackage::save(path, bankData);
        if (!saved.success) { setTextError(saved.error); return false; }
        setTextError({});
        return true;
    }

    bool loadBank(const std::string& path, int targetBank, bool overwrite) {
        if (targetBank < 0 || targetBank > 65535) { setTextError("Target bank is out of range"); return false; }
        const auto loaded = SetPackage::load(path);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }
        const auto merged = library_.mergeBank(loaded.data, static_cast<std::uint16_t>(targetBank), overwrite);
        if (!merged.success) { setTextError(merged.error); return false; }
        setTextError({});
        return true;
    }

    bool writeCurrentProgram(const SlotAddress& address, int upperIndex, const std::string& name) {
        if (upperIndex < 0 || upperIndex >= kUpperCount || state_ == nullptr) {
            setTextError("Invalid Upper index for Write Program");
            return false;
        }
        const auto& source = state_->combi.uppers[static_cast<std::size_t>(upperIndex)].program;
        if (source == nullptr) { setTextError("Selected Upper has no PROGRAM"); return false; }
        auto copy = std::make_shared<Program>(*source);
        copy->name = name.empty() ? source->name : name;
        if (!library_.writeProgram(address, copy)) { setTextError("Write Program failed"); return false; }
        setTextError({});
        return true;
    }

    bool updateProgramBasic(const SlotAddress& address, bool mono, bool legato, float portamentoSec) {
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::updateBasic(source, mono, legato, portamentoSec));
    }

    bool addProgramLayer(const SlotAddress& address, std::uint32_t multisampleId, const std::string& name) {
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        const auto ms = library_.multisample(multisampleId);
        if (ms == nullptr) { setTextError("MULTISAMPLE id is not available"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::addLayer(source, name, ms));
    }

    bool removeProgramLayer(const SlotAddress& address, int layerIndex) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::removeLayer(source, static_cast<std::size_t>(layerIndex)));
    }

    bool moveProgramLayer(const SlotAddress& address, int fromIndex, int toIndex) {
        if (fromIndex < 0 || toIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::moveLayer(source, static_cast<std::size_t>(fromIndex), static_cast<std::size_t>(toIndex)));
    }

    bool assignProgramMultisample(const SlotAddress& address, int layerIndex, std::uint32_t multisampleId) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        const auto ms = library_.multisample(multisampleId);
        if (ms == nullptr) { setTextError("MULTISAMPLE id is not available"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::assignMultisample(source, static_cast<std::size_t>(layerIndex), ms));
    }

    bool updateProgramLayerBasic(const SlotAddress& address, int layerIndex, bool enabled,
                                 int keyLow, int keyHigh, int velocityLow, int velocityHigh,
                                 int velocityFadeIn, int velocityFadeOut, int transposeSemitones,
                                 float tuneCents, float levelDb, float pan, float delaySec, bool damperEnabled) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::updateLayerBasic(
            source, static_cast<std::size_t>(layerIndex), enabled, keyLow, keyHigh, velocityLow, velocityHigh,
            velocityFadeIn, velocityFadeOut, transposeSemitones, tuneCents, levelDb, pan, delaySec, damperEnabled));
    }

    bool updateProgramAmpEnvelope(const SlotAddress& address, int layerIndex,
                                  float attack, float decay, float sustain, float release) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::updateLayerAmpEnvelope(
            source, static_cast<std::size_t>(layerIndex), EnvelopeParams{attack, decay, sustain, release}));
    }

    bool updateProgramPitch(const SlotAddress& address, int layerIndex, float amountCents,
                            float attack, float decay, float sustain, float release) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        return commitProgramEdit(address, source, ProgramEditor::updateLayerPitch(
            source, static_cast<std::size_t>(layerIndex), amountCents, EnvelopeParams{attack, decay, sustain, release}));
    }

    bool updateProgramFilter(const SlotAddress& address, int layerIndex, int mode,
                             float cutoffHz, float resonance, float keyTrack, float velocityAmount,
                             float envelopeAmount, float lfoAmount,
                             float attack, float decay, float sustain, float release) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        if (mode < static_cast<int>(FilterMode::LowPass) || mode > static_cast<int>(FilterMode::Bypass)) {
            setTextError("PROGRAM FILTER mode is invalid"); return false;
        }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        FilterParams filter{};
        filter.mode = static_cast<FilterMode>(mode);
        filter.cutoffHz = cutoffHz;
        filter.resonance = resonance;
        filter.keyTrack = keyTrack;
        filter.velocityAmount = velocityAmount;
        filter.envelopeAmount = envelopeAmount;
        filter.lfoAmount = lfoAmount;
        return commitProgramEdit(address, source, ProgramEditor::updateLayerFilter(
            source, static_cast<std::size_t>(layerIndex), filter, EnvelopeParams{attack, decay, sustain, release}));
    }

    bool updateProgramLfo(const SlotAddress& address, int layerIndex, int lfoIndex, int wave,
                          float frequencyHz, float delaySec, float fadeSec, bool keySync,
                          float pitchCents, float filterOctaves, float ampDepth) {
        if (layerIndex < 0) { setTextError("PROGRAM layer index is out of range"); return false; }
        if (wave < static_cast<int>(LfoWave::Sine) || wave > static_cast<int>(LfoWave::RandomHold)) {
            setTextError("PROGRAM LFO waveform is invalid"); return false;
        }
        const auto source = library_.program(address);
        if (source == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        LfoParams lfo{};
        lfo.wave = static_cast<LfoWave>(wave);
        lfo.frequencyHz = frequencyHz;
        lfo.delaySec = delaySec;
        lfo.fadeSec = fadeSec;
        lfo.keySync = keySync;
        lfo.pitchCents = pitchCents;
        lfo.filterOctaves = filterOctaves;
        lfo.ampDepth = ampDepth;
        return commitProgramEdit(address, source, ProgramEditor::updateLayerLfo(
            source, static_cast<std::size_t>(layerIndex), lfoIndex, lfo));
    }

    bool writeCurrentSample(std::uint32_t id, int upperIndex, int layerIndex, int zoneIndex, const std::string& name) {
        if (upperIndex < 0 || upperIndex >= kUpperCount || state_ == nullptr) {
            setTextError("Invalid Upper index for Write Sample"); return false;
        }
        const auto& program = state_->combi.uppers[static_cast<std::size_t>(upperIndex)].program;
        if (program == nullptr || layerIndex < 0 || static_cast<std::size_t>(layerIndex) >= program->layers.size()) {
            setTextError("Selected PROGRAM layer is invalid"); return false;
        }
        const auto& ms = program->layers[static_cast<std::size_t>(layerIndex)].multisample;
        if (ms == nullptr || zoneIndex < 0 || static_cast<std::size_t>(zoneIndex) >= ms->zones.size() || ms->zones[static_cast<std::size_t>(zoneIndex)].sample == nullptr) {
            setTextError("Selected MULTISAMPLE zone has no SAMPLE"); return false;
        }
        const auto source = ms->zones[static_cast<std::size_t>(zoneIndex)].sample;
        auto edited = SampleEditor::updateMetadata(*source, name.empty() ? source->name : name, source->rootKey, source->fineTuneCents);
        if (!edited.ok() || !library_.writeSample(id, edited.sample)) { setTextError(edited.error.empty() ? "Write Sample failed" : edited.error); return false; }
        setTextError({}); return true;
    }

    bool updateSampleMetadata(std::uint32_t id, const std::string& name, int rootKey, float fineTuneCents) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::updateMetadata(*source, name, static_cast<std::uint8_t>(std::clamp(rootKey,0,127)), fineTuneCents));
    }
    bool trimSample(std::uint32_t id, std::uint64_t startFrame, std::uint64_t endFrame) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::trim(*source, startFrame, endFrame));
    }
    bool normalizeSample(std::uint32_t id, float targetPeakDb) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::normalize(*source, targetPeakDb));
    }
    bool reverseSample(std::uint32_t id) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::reverse(*source));
    }
    bool fadeSample(std::uint32_t id, std::uint64_t fadeInFrames, std::uint64_t fadeOutFrames) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::fade(*source, fadeInFrames, fadeOutFrames));
    }
    bool setSampleLoop(std::uint32_t id, std::uint64_t startFrame, std::uint64_t endFrame, std::uint32_t crossfadeFrames) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::setLoop(*source, startFrame, endFrame, crossfadeFrames));
    }
    bool clearSampleLoop(std::uint32_t id) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        return commitSampleEdit(id, source, SampleEditor::clearLoop(*source));
    }
    std::int64_t sampleNearestZeroCrossing(std::uint32_t id, std::uint64_t aroundFrame, std::uint64_t radius) {
        const auto source = library_.sample(id); if (source == nullptr) { setTextError("SAMPLE id is not available"); return -1; }
        setTextError({}); return static_cast<std::int64_t>(SampleEditor::nearestZeroCrossing(*source, aroundFrame, radius));
    }

    bool renameMultisample(std::uint32_t id, const std::string& name) {
        const auto source = library_.multisample(id); if (source == nullptr) { setTextError("MULTISAMPLE id is not available"); return false; }
        return commitMultisampleEdit(id, source, MultisampleEditor::rename(source, name));
    }
    bool addMultisampleZone(std::uint32_t id, std::uint32_t sampleId, int insertIndex,
                            int keyLow, int keyHigh, int velocityLow, int velocityHigh, int rootKey,
                            int transpose, float tuneCents, float gainDb, float pan,
                            bool oneShot, bool reverse, bool nonTranspose) {
        const auto source = library_.multisample(id); if (source == nullptr) { setTextError("MULTISAMPLE id is not available"); return false; }
        const auto sample = library_.sample(sampleId); if (sample == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        SampleZone z{}; z.sample=sample; z.keyLow=static_cast<std::uint8_t>(std::clamp(keyLow,0,127)); z.keyHigh=static_cast<std::uint8_t>(std::clamp(keyHigh,0,127));
        z.velocityLow=static_cast<std::uint8_t>(std::clamp(velocityLow,1,127)); z.velocityHigh=static_cast<std::uint8_t>(std::clamp(velocityHigh,1,127)); z.rootKey=static_cast<std::uint8_t>(std::clamp(rootKey,0,127));
        z.transposeSemitones=static_cast<std::int8_t>(std::clamp(transpose,-64,63)); z.tuneCents=tuneCents; z.gainDb=gainDb; z.pan=pan; z.oneShot=oneShot; z.reverse=reverse; z.nonTranspose=nonTranspose;
        const auto result = insertIndex < 0 ? MultisampleEditor::addZone(source,z) : MultisampleEditor::insertZone(source,static_cast<std::size_t>(insertIndex),z);
        return commitMultisampleEdit(id, source, result);
    }
    bool updateMultisampleZone(std::uint32_t id, int zoneIndex, std::uint32_t sampleId,
                               int keyLow, int keyHigh, int velocityLow, int velocityHigh, int rootKey,
                               int transpose, float tuneCents, float gainDb, float pan,
                               bool oneShot, bool reverse, bool nonTranspose) {
        if (zoneIndex < 0) { setTextError("Zone index is out of range"); return false; }
        const auto source = library_.multisample(id); if (source == nullptr) { setTextError("MULTISAMPLE id is not available"); return false; }
        const auto sample = library_.sample(sampleId); if (sample == nullptr) { setTextError("SAMPLE id is not available"); return false; }
        SampleZone z{}; z.sample=sample; z.keyLow=static_cast<std::uint8_t>(std::clamp(keyLow,0,127)); z.keyHigh=static_cast<std::uint8_t>(std::clamp(keyHigh,0,127));
        z.velocityLow=static_cast<std::uint8_t>(std::clamp(velocityLow,1,127)); z.velocityHigh=static_cast<std::uint8_t>(std::clamp(velocityHigh,1,127)); z.rootKey=static_cast<std::uint8_t>(std::clamp(rootKey,0,127));
        z.transposeSemitones=static_cast<std::int8_t>(std::clamp(transpose,-64,63)); z.tuneCents=tuneCents; z.gainDb=gainDb; z.pan=pan; z.oneShot=oneShot; z.reverse=reverse; z.nonTranspose=nonTranspose;
        return commitMultisampleEdit(id, source, MultisampleEditor::updateZone(source,static_cast<std::size_t>(zoneIndex),z));
    }
    bool removeMultisampleZone(std::uint32_t id, int zoneIndex) {
        if (zoneIndex < 0) { setTextError("Zone index is out of range"); return false; }
        const auto source=library_.multisample(id); if(source==nullptr){setTextError("MULTISAMPLE id is not available");return false;}
        return commitMultisampleEdit(id,source,MultisampleEditor::removeZone(source,static_cast<std::size_t>(zoneIndex)));
    }
    bool moveMultisampleZone(std::uint32_t id, int fromIndex, int toIndex) {
        if(fromIndex<0||toIndex<0){setTextError("Zone index is out of range");return false;}
        const auto source=library_.multisample(id); if(source==nullptr){setTextError("MULTISAMPLE id is not available");return false;}
        return commitMultisampleEdit(id,source,MultisampleEditor::moveZone(source,static_cast<std::size_t>(fromIndex),static_cast<std::size_t>(toIndex)));
    }

    bool writeCurrentMultisample(std::uint32_t id, int upperIndex, int layerIndex, const std::string& name) {
        if (upperIndex < 0 || upperIndex >= kUpperCount || state_ == nullptr) {
            setTextError("Invalid Upper index for Write Multisample");
            return false;
        }
        const auto& program = state_->combi.uppers[static_cast<std::size_t>(upperIndex)].program;
        if (program == nullptr || layerIndex < 0 || static_cast<std::size_t>(layerIndex) >= program->layers.size()) {
            setTextError("Selected PROGRAM layer has no MULTISAMPLE");
            return false;
        }
        const auto& source = program->layers[static_cast<std::size_t>(layerIndex)].multisample;
        if (source == nullptr) { setTextError("Selected PROGRAM layer has no MULTISAMPLE"); return false; }
        auto copy = std::make_shared<Multisample>(*source);
        if (!name.empty()) copy->name = name;
        if (!library_.writeMultisample(id, copy)) { setTextError("Write Multisample failed"); return false; }
        setTextError({});
        return true;
    }

    bool writeCurrentCombi(const SlotAddress& address, const std::string& name) {
        if (state_ == nullptr) { setTextError("No active COMBI"); return false; }
        Combi copy = state_->combi;
        if (!name.empty()) copy.name = name;
        if (!library_.writeCombi(address, copy)) { setTextError("Write Combi failed"); return false; }
        setTextError({});
        return true;
    }

    bool updateUpperMain(int upperIndex, bool enabled, bool solo, bool mute,
                         float levelDb, float pan, int keyLow, int keyHigh, int voiceMode) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        if (keyLow < 0 || keyLow > 127 || keyHigh < 0 || keyHigh > 127 || keyLow > keyHigh) { setTextError("Invalid Upper key range"); return false; }
        if (voiceMode < static_cast<int>(UpperVoiceMode::Poly) || voiceMode > static_cast<int>(UpperVoiceMode::Legato)) { setTextError("Invalid Upper voice mode"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        u.enabled = enabled;
        u.solo = solo;
        u.mute = mute;
        u.levelDb = std::clamp(levelDb, -60.0F, 18.0F);
        u.pan = std::clamp(pan, -1.0F, 1.0F);
        u.keyLow = static_cast<std::uint8_t>(keyLow);
        u.keyHigh = static_cast<std::uint8_t>(keyHigh);
        u.voiceMode = static_cast<UpperVoiceMode>(voiceMode);
        return installState(std::move(next));
    }

    bool updateUpperTune(int upperIndex, int octave, int transpose, float fineCents,
                         int pitchBendRange, float portamentoSec) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        u.octave = static_cast<std::int8_t>(std::clamp(octave, -4, 4));
        u.transposeSemitones = static_cast<std::int8_t>(std::clamp(transpose, -48, 48));
        u.tuneCents = std::clamp(fineCents, -100.0F, 100.0F);
        u.pitchBendRange = static_cast<std::int8_t>(std::clamp(pitchBendRange, 0, 24));
        u.portamentoSec = std::clamp(portamentoSec, 0.0F, 5.0F);
        return installState(std::move(next));
    }

    bool updateUpperRange(int upperIndex, int velocityLow, int velocityHigh) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        if (velocityLow < 1 || velocityLow > 127 || velocityHigh < 1 || velocityHigh > 127 || velocityLow > velocityHigh) { setTextError("Invalid Upper velocity range"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        u.velocityLow = static_cast<std::uint8_t>(velocityLow);
        u.velocityHigh = static_cast<std::uint8_t>(velocityHigh);
        return installState(std::move(next));
    }

    bool updateUpperControl(int upperIndex, bool breath, bool expression, bool aftertouch,
                            bool damper, int midiChannel) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        if (midiChannel < 0 || midiChannel > 15) { setTextError("Invalid Upper MIDI channel"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        u.breathEnabled = breath;
        u.expressionEnabled = expression;
        u.aftertouchEnabled = aftertouch;
        u.damperEnabled = damper;
        u.midiChannel = static_cast<std::uint8_t>(midiChannel);
        return installState(std::move(next));
    }

    bool updateUpperTone(int upperIndex, float ampAttackScale, float ampDecayScale,
                         float ampSustainScale, float ampReleaseScale,
                         float filterCutoffOctaves, float filterResonanceOffset,
                         float lfoRateScale, float lfoDepthScale,
                         float eqLowDb, float eqMidDb, float eqHighDb) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        const float values[] = {ampAttackScale, ampDecayScale, ampSustainScale, ampReleaseScale,
                                filterCutoffOctaves, filterResonanceOffset, lfoRateScale, lfoDepthScale,
                                eqLowDb, eqMidDb, eqHighDb};
        for (float v : values) if (!std::isfinite(v)) { setTextError("Upper TONE contains non-finite value"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        u.ampAttackScale = std::clamp(ampAttackScale, 0.0F, 16.0F);
        u.ampDecayScale = std::clamp(ampDecayScale, 0.0F, 16.0F);
        u.ampSustainScale = std::clamp(ampSustainScale, 0.0F, 2.0F);
        u.ampReleaseScale = std::clamp(ampReleaseScale, 0.0F, 16.0F);
        u.filterCutoffOctaves = std::clamp(filterCutoffOctaves, -10.0F, 10.0F);
        u.filterResonanceOffset = std::clamp(filterResonanceOffset, -1.0F, 1.0F);
        u.lfoRateScale = std::clamp(lfoRateScale, 0.0F, 16.0F);
        u.lfoDepthScale = std::clamp(lfoDepthScale, 0.0F, 8.0F);
        u.eqLowDb = std::clamp(eqLowDb, -18.0F, 18.0F);
        u.eqMidDb = std::clamp(eqMidDb, -18.0F, 18.0F);
        u.eqHighDb = std::clamp(eqHighDb, -18.0F, 18.0F);
        return installState(std::move(next));
    }

    // TONE page: 0 attack ms, 1 decay ms, 2 sustain 0..1, 3 release ms, 4 cutoff Hz, 5 resonance 0..1.
    // A negative value clears the setting (the PROGRAM default is used again).
    bool setUpperToneParam(int upperIndex, int param, float value) {
        if (upperIndex < 0 || upperIndex >= kUpperCount || param < 0 || param > 5) { setTextError("Invalid TONE parameter"); return false; }
        if (!std::isfinite(value)) { setTextError("TONE value is not finite"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& u = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        const float v = value < 0.0F ? -1.0F : value;
        switch (param) {
            case 0: u.toneAttackMs = v < 0.0F ? v : std::min(v, kToneMaxTimeMs); break;
            case 1: u.toneDecayMs = v < 0.0F ? v : std::min(v, kToneMaxTimeMs); break;
            case 2: u.toneSustain = v < 0.0F ? v : std::min(v, 1.0F); break;
            case 3: u.toneReleaseMs = v < 0.0F ? v : std::min(v, kToneMaxTimeMs); break;
            case 4: u.toneCutoffHz = v < 0.0F ? v : std::clamp(v, 20.0F, kToneCutoffOpenHz); break;
            default: u.toneResonance = v < 0.0F ? v : std::min(v, 1.0F); break;
        }
        return installState(std::move(next));
    }

    std::vector<float> upperState(int upperIndex) const {
        std::vector<float> out;
        if (upperIndex < 0 || upperIndex >= kUpperCount || state_ == nullptr) return out;
        const auto& u = state_->combi.uppers[static_cast<std::size_t>(upperIndex)];
        out = {
            u.enabled ? 1.0F : 0.0F, u.solo ? 1.0F : 0.0F, u.mute ? 1.0F : 0.0F,
            u.levelDb, u.pan, static_cast<float>(u.keyLow), static_cast<float>(u.keyHigh),
            static_cast<float>(u.velocityLow), static_cast<float>(u.velocityHigh),
            static_cast<float>(u.voiceMode), static_cast<float>(u.octave),
            static_cast<float>(u.transposeSemitones), u.tuneCents, static_cast<float>(u.pitchBendRange),
            u.portamentoSec, u.breathEnabled ? 1.0F : 0.0F, u.expressionEnabled ? 1.0F : 0.0F,
            u.aftertouchEnabled ? 1.0F : 0.0F, u.damperEnabled ? 1.0F : 0.0F,
            static_cast<float>(u.midiChannel), u.ampAttackScale, u.ampDecayScale,
            u.ampSustainScale, u.ampReleaseScale, u.filterCutoffOctaves, u.filterResonanceOffset,
            u.lfoRateScale, u.lfoDepthScale, u.eqLowDb, u.eqMidDb, u.eqHighDb, u.fxSend1, u.fxSend2
        };
        const UpperToneValues tone = effectiveUpperTone(u);
        out.insert(out.end(), {tone.attackMs, tone.decayMs, tone.sustain, tone.releaseMs, tone.cutoffHz, tone.resonance,
                               u.toneAttackMs >= 0.0F ? 1.0F : 0.0F, u.toneDecayMs >= 0.0F ? 1.0F : 0.0F,
                               u.toneSustain >= 0.0F ? 1.0F : 0.0F, u.toneReleaseMs >= 0.0F ? 1.0F : 0.0F,
                               u.toneCutoffHz >= 0.0F ? 1.0F : 0.0F, u.toneResonance >= 0.0F ? 1.0F : 0.0F});
        return out;
    }

    std::string upperSoundName(int upperIndex) const {
        if (upperIndex < 0 || upperIndex >= kUpperCount || state_ == nullptr) return {};
        const auto& p = state_->combi.uppers[static_cast<std::size_t>(upperIndex)].program;
        return p == nullptr ? std::string{} : p->name;
    }

    std::string combiName() const { return state_ == nullptr ? std::string{} : state_->combi.name; }


    std::vector<int> combiCatalogSlots() const {
        std::vector<int> out;
        out.reserve(500U);
        for (int slot = 1; slot <= 500; ++slot) {
            SlotAddress address{};
            address.bank = 0;
            address.page = 0;
            address.slot = static_cast<std::uint16_t>(slot);
            if (library_.combi(address).has_value()) out.push_back(slot);
        }
        return out;
    }

    std::vector<std::string> combiCatalogNames() const {
        std::vector<std::string> out;
        out.reserve(500U);
        for (int slot = 1; slot <= 500; ++slot) {
            SlotAddress address{};
            address.bank = 0;
            address.page = 0;
            address.slot = static_cast<std::uint16_t>(slot);
            const auto combi = library_.combi(address);
            if (combi.has_value()) out.push_back(combi->name);
        }
        return out;
    }

    std::vector<int> soundCatalogAddresses() const {
        std::vector<int> out;
        for (const auto& item : library_.programCatalog()) {
            out.push_back(static_cast<int>(item.address.bank));
            out.push_back(static_cast<int>(item.address.page));
            out.push_back(static_cast<int>(item.address.slot));
        }
        return out;
    }

    std::vector<std::string> soundCatalogNames() const {
        std::vector<std::string> out;
        for (const auto& item : library_.programCatalog()) out.push_back(item.name);
        return out;
    }

    bool updateUpperFxSends(int upperIndex, float send1, float send2) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.fxSend1 = std::clamp(send1, 0.0F, 1.0F);
        upper.fxSend2 = std::clamp(send2, 0.0F, 1.0F);
        return installState(std::move(next));
    }

    bool updateUpperControllerRoute(int upperIndex, int routeIndex, int source, int destination,
                                    int cc, float amount, bool bipolar) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        if (routeIndex < 0 || routeIndex >= kMaxUpperControllerRoutes) { setTextError("Invalid controller route index"); return false; }
        if (source < static_cast<int>(ControllerSource::Off) || source > static_cast<int>(ControllerSource::ControlChange)) { setTextError("Invalid controller source"); return false; }
        if (destination < static_cast<int>(ModDestination::LevelDb) || destination > static_cast<int>(ModDestination::FxSend2)) { setTextError("Invalid modulation destination"); return false; }
        if (!std::isfinite(amount)) { setTextError("Controller route amount is not finite"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& route = next->combi.uppers[static_cast<std::size_t>(upperIndex)].controllerRoutes[static_cast<std::size_t>(routeIndex)];
        route.source = static_cast<ControllerSource>(source);
        route.destination = static_cast<ModDestination>(destination);
        route.cc = static_cast<std::uint8_t>(std::clamp(cc, 0, 127));
        route.amount = std::clamp(amount, -96.0F, 96.0F);
        route.bipolar = bipolar;
        return installState(std::move(next));
    }

    bool updateCombiFx(int slot, int type, float wet, float timeMs, float feedback,
                       float rateHz, float depth, float tone) {
        if (slot < 0 || slot > 1) { setTextError("Invalid COMBI FX slot"); return false; }
        if (type < static_cast<int>(EffectType::Off) || type > static_cast<int>(EffectType::Reverb)) { setTextError("Invalid COMBI FX type"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& fx = slot == 0 ? next->combi.fx1 : next->combi.fx2;
        fx.type = static_cast<EffectType>(type);
        fx.wet = std::clamp(wet, 0.0F, 1.0F);
        fx.timeMs = std::clamp(timeMs, 1.0F, 2500.0F);
        fx.feedback = std::clamp(feedback, 0.0F, 0.95F);
        fx.rateHz = std::clamp(rateHz, 0.05F, 8.0F);
        fx.depth = std::clamp(depth, 0.0F, 1.0F);
        fx.tone = std::clamp(tone, 0.0F, 1.0F);
        return installState(std::move(next));
    }

    bool updateCombiFxRouting(int routing) {
        if (routing < static_cast<int>(EffectRouting::Parallel) || routing > static_cast<int>(EffectRouting::Fx1IntoFx2)) { setTextError("Invalid COMBI FX routing"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        next->combi.fxRouting = static_cast<EffectRouting>(routing);
        return installState(std::move(next));
    }

    std::vector<float> combiFxState() const {
        std::vector<float> out;
        if (state_ == nullptr) return out;
        const auto append = [&out](const EffectSlot& fx) {
            out.push_back(static_cast<float>(fx.type));
            out.push_back(fx.wet); out.push_back(fx.timeMs); out.push_back(fx.feedback);
            out.push_back(fx.rateHz); out.push_back(fx.depth); out.push_back(fx.tone);
        };
        append(state_->combi.fx1); append(state_->combi.fx2);
        out.push_back(static_cast<float>(state_->combi.fxRouting));
        return out;
    }

    std::vector<float> globalState() const {
        std::vector<float> out;
        if (state_ == nullptr) return out;
        const auto& g = state_->global;
        out = {g.masterTuneHz, static_cast<float>(g.transposeSemitones), g.masterGainDb,
               g.eqLowDb, g.eqMidDb, g.eqHighDb, g.limiterEnabled ? 1.0F : 0.0F,
               g.limiterCeilingDb, static_cast<float>(g.velocityCurve),
               static_cast<float>(g.breathCc), static_cast<float>(g.expressionCc),
               static_cast<float>(g.damperCc), g.pitchBendEnabled ? 1.0F : 0.0F,
               g.aftertouchEnabled ? 1.0F : 0.0F};
        return out;
    }

    bool writeGlobal() {
        if (state_ == nullptr) { setTextError("No active GLOBAL state"); return false; }
        library_.setGlobal(state_->global);
        setTextError({});
        return true;
    }

    bool updateGlobal(float masterTuneHz, int transposeSemitones, float masterGainDb,
                      float eqLowDb, float eqMidDb, float eqHighDb, bool limiterEnabled,
                      float limiterCeilingDb, int velocityCurve, int breathCc, int expressionCc,
                      int damperCc, bool pitchBendEnabled, bool aftertouchEnabled) {
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& g = next->global;
        g.masterTuneHz = std::clamp(masterTuneHz, 400.0F, 480.0F);
        g.transposeSemitones = static_cast<std::int8_t>(std::clamp(transposeSemitones, -48, 48));
        g.masterGainDb = std::clamp(masterGainDb, -60.0F, 18.0F);
        g.eqLowDb = std::clamp(eqLowDb, -18.0F, 18.0F);
        g.eqMidDb = std::clamp(eqMidDb, -18.0F, 18.0F);
        g.eqHighDb = std::clamp(eqHighDb, -18.0F, 18.0F);
        g.limiterEnabled = limiterEnabled;
        g.limiterCeilingDb = std::clamp(limiterCeilingDb, -12.0F, 0.0F);
        g.velocityCurve = static_cast<std::uint8_t>(std::clamp(velocityCurve, 0, 4));
        g.breathCc = static_cast<std::uint8_t>(std::clamp(breathCc, 0, 127));
        g.expressionCc = static_cast<std::uint8_t>(std::clamp(expressionCc, 0, 127));
        g.damperCc = static_cast<std::uint8_t>(std::clamp(damperCc, 0, 127));
        g.pitchBendEnabled = pitchBendEnabled;
        g.aftertouchEnabled = aftertouchEnabled;
        library_.setGlobal(g);
        return installState(std::move(next));
    }

    bool updateGlobalAdvanced(int scaleRoot, const std::array<float, 12>& scaleOffsetsCents,
                              std::uint16_t midiInputChannelMask, bool filterNotes,
                              bool filterControlChange, bool filterPitchBend, bool filterAftertouch,
                              float breathMin, float breathMax, float breathCurve,
                              float expressionMin, float expressionMax, float expressionCurve,
                              bool damperPolarityReversed) {
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& g = next->global;
        g.scaleRoot = static_cast<std::uint8_t>(std::clamp(scaleRoot, 0, 11));
        for (std::size_t i = 0; i < g.scaleOffsetsCents.size(); ++i) {
            g.scaleOffsetsCents[i] = std::clamp(scaleOffsetsCents[i], -100.0F, 100.0F);
        }
        g.midiInputChannelMask = midiInputChannelMask;
        g.filterNotes = filterNotes;
        g.filterControlChange = filterControlChange;
        g.filterPitchBend = filterPitchBend;
        g.filterAftertouch = filterAftertouch;
        g.breathMin = std::clamp(breathMin, 0.0F, 0.9999F);
        g.breathMax = std::clamp(breathMax, g.breathMin + 0.0001F, 1.0F);
        g.breathCurve = std::clamp(breathCurve, 0.1F, 8.0F);
        g.expressionMin = std::clamp(expressionMin, 0.0F, 0.9999F);
        g.expressionMax = std::clamp(expressionMax, g.expressionMin + 0.0001F, 1.0F);
        g.expressionCurve = std::clamp(expressionCurve, 0.1F, 8.0F);
        g.damperPolarityReversed = damperPolarityReversed;
        library_.setGlobal(g);
        return installState(std::move(next));
    }

    bool writeList(const SlotAddress& address, const SetList& list) {
        if (!library_.writeList(address, list)) { setTextError("Write List failed"); return false; }
        setTextError({});
        return true;
    }

    bool setListEntry(const SlotAddress& listAddress, int index, int number, const std::string& name, const SlotAddress& combiAddress) {
        if (index < 0) { setTextError("LIST entry index is invalid"); return false; }
        const auto combi = library_.combi(combiAddress);
        if (!combi.has_value()) { setTextError("LIST target COMBI slot is empty"); return false; }
        ListEntry entry{};
        entry.number = static_cast<std::uint16_t>(std::clamp(number, 0, 65535));
        entry.name = name.empty() ? combi->name : name;
        entry.combiBank = combiAddress.bank; entry.combiPage = combiAddress.page; entry.combiSlot = combiAddress.slot;
        if (!library_.setListEntry(listAddress, static_cast<std::size_t>(index), entry)) { setTextError("LIST entry update failed"); return false; }
        setTextError({}); return true;
    }

    bool insertListEntry(const SlotAddress& listAddress, int index, int number, const std::string& name, const SlotAddress& combiAddress) {
        if (index < 0) { setTextError("LIST entry index is invalid"); return false; }
        const auto combi = library_.combi(combiAddress);
        if (!combi.has_value()) { setTextError("LIST target COMBI slot is empty"); return false; }
        ListEntry entry{};
        entry.number = static_cast<std::uint16_t>(std::clamp(number, 0, 65535));
        entry.name = name.empty() ? combi->name : name;
        entry.combiBank = combiAddress.bank; entry.combiPage = combiAddress.page; entry.combiSlot = combiAddress.slot;
        if (!library_.insertListEntry(listAddress, static_cast<std::size_t>(index), entry)) { setTextError("LIST entry insert failed"); return false; }
        if (activeListAddress_.has_value() && *activeListAddress_ == listAddress && activeListEntry_ >= index) ++activeListEntry_;
        setTextError({}); return true;
    }

    bool removeListEntry(const SlotAddress& listAddress, int index) {
        if (index < 0 || !library_.removeListEntry(listAddress, static_cast<std::size_t>(index))) { setTextError("LIST entry remove failed"); return false; }
        if (activeListAddress_.has_value() && *activeListAddress_ == listAddress) {
            if (activeListEntry_ == index) activeListEntry_ = -1;
            else if (activeListEntry_ > index) --activeListEntry_;
        }
        setTextError({}); return true;
    }

    bool moveListEntry(const SlotAddress& listAddress, int fromIndex, int toIndex) {
        if (fromIndex < 0 || toIndex < 0 || !library_.moveListEntry(listAddress, static_cast<std::size_t>(fromIndex), static_cast<std::size_t>(toIndex))) { setTextError("LIST entry move failed"); return false; }
        if (activeListAddress_.has_value() && *activeListAddress_ == listAddress) {
            if (activeListEntry_ == fromIndex) activeListEntry_ = toIndex;
            else if (fromIndex < activeListEntry_ && activeListEntry_ <= toIndex) --activeListEntry_;
            else if (toIndex <= activeListEntry_ && activeListEntry_ < fromIndex) ++activeListEntry_;
        }
        setTextError({}); return true;
    }

    bool moveList(const SlotAddress& from, const SlotAddress& to, bool overwrite) {
        if (!library_.moveList(from, to, overwrite)) { setTextError("LIST move failed"); return false; }
        if (activeListAddress_.has_value() && *activeListAddress_ == from) activeListAddress_ = to;
        setTextError({}); return true;
    }

    bool activateListEntry(const SlotAddress& listAddress, int index) {
        if (index < 0) { setTextError("LIST entry index is invalid"); return false; }
        const auto list = library_.list(listAddress);
        if (!list.has_value() || static_cast<std::size_t>(index) >= list->entries.size()) { setTextError("LIST entry is not available"); return false; }
        const auto& entry = list->entries[static_cast<std::size_t>(index)];
        const SlotAddress combiAddress{entry.combiBank, entry.combiPage, entry.combiSlot};
        if (!activateCombi(combiAddress)) return false;
        activeListAddress_ = listAddress;
        activeListEntry_ = index;
        setTextError({}); return true;
    }

    int activeListEntry(const SlotAddress& listAddress) const noexcept {
        return activeListAddress_.has_value() && *activeListAddress_ == listAddress ? activeListEntry_ : -1;
    }

    std::vector<int> listEntryNumbers(const SlotAddress& address) const {
        std::vector<int> out; const auto list = library_.list(address); if (!list.has_value()) return out;
        out.reserve(list->entries.size()); for (const auto& e : list->entries) out.push_back(static_cast<int>(e.number)); return out;
    }

    std::vector<std::string> listEntryNames(const SlotAddress& address) const {
        std::vector<std::string> out; const auto list = library_.list(address); if (!list.has_value()) return out;
        out.reserve(list->entries.size()); for (const auto& e : list->entries) out.push_back(e.name); return out;
    }

    std::vector<int> listEntryCombiAddresses(const SlotAddress& address) const {
        std::vector<int> out; const auto list = library_.list(address); if (!list.has_value()) return out;
        out.reserve(list->entries.size()*3U); for (const auto& e : list->entries) { out.push_back(e.combiBank); out.push_back(e.combiPage); out.push_back(e.combiSlot); } return out;
    }

    bool clearProgram(const SlotAddress& address) { return library_.clearProgram(address); }
    bool clearCombi(const SlotAddress& address) { return library_.clearCombi(address); }
    bool clearList(const SlotAddress& address) {
        const bool ok = library_.clearList(address);
        if (ok && activeListAddress_.has_value() && *activeListAddress_ == address) { activeListAddress_.reset(); activeListEntry_ = -1; }
        return ok;
    }
    bool clearMultisample(std::uint32_t id) { return library_.clearMultisample(id); }

    bool programSlotOccupied(const SlotAddress& address) const { return library_.hasProgram(address); }
    bool combiSlotOccupied(const SlotAddress& address) const { return library_.hasCombi(address); }
    bool listSlotOccupied(const SlotAddress& address) const { return library_.hasList(address); }
    bool renameProgram(const SlotAddress& address, const std::string& name) { return library_.renameProgram(address, name); }
    bool renameCombi(const SlotAddress& address, const std::string& name) { return library_.renameCombi(address, name); }
    bool renameList(const SlotAddress& address, const std::string& name) { return library_.renameList(address, name); }
    bool copyProgram(const SlotAddress& from, const SlotAddress& to, bool overwrite) { return library_.copyProgram(from, to, overwrite); }
    bool copyCombi(const SlotAddress& from, const SlotAddress& to, bool overwrite) { return library_.copyCombi(from, to, overwrite); }
    bool copyList(const SlotAddress& from, const SlotAddress& to, bool overwrite) { return library_.copyList(from, to, overwrite); }

    bool activateProgram(const SlotAddress& address, int upperIndex) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        const auto program = library_.program(address);
        if (program == nullptr) { setTextError("PROGRAM slot is empty"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program = program;
        upper.soundSlotId = (address.bank == 0U && address.page == 0U && address.slot >= 1U && address.slot <= kFixedSoundSlotCount) ? address.slot : kNoSoundSlot;
        upper.name = program->name; // visible name arrives 1:1 from SILVETON_BANK.sv
        upper.enabled = true;
        return installState(std::move(next));
    }

    bool clearUpperSound(int upperIndex) {
        if (upperIndex < 0 || upperIndex >= kUpperCount) { setTextError("Invalid Upper index"); return false; }
        auto next = std::make_shared<EngineState>(state_ != nullptr ? *state_ : EngineState{});
        auto& upper = next->combi.uppers[static_cast<std::size_t>(upperIndex)];
        upper.program.reset();
        upper.soundSlotId = kNoSoundSlot;
        upper.enabled = false;
        upper.solo = false;
        upper.mute = false;
        return installState(std::move(next));
    }

    bool activateCombi(const SlotAddress& address) {
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
    }

    bool setSetName(const std::string& name) {
        if (name.empty()) { setTextError("SET name cannot be empty"); return false; }
        library_.setSetName(name);
        return true;
    }

    bool midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {
        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);
        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);
        midiLastStatus_.store(static_cast<int>(status), std::memory_order_relaxed);
        midiLastData1_.store(static_cast<int>(data1), std::memory_order_relaxed);
        midiLastData2_.store(static_cast<int>(data2), std::memory_order_relaxed);
        midiActivitySerial_.fetch_add(1U, std::memory_order_release);
        if (kind == 0x80U || (kind == 0x90U && data2 == 0U)) testInstrument_.noteOff(data1);
        else if (kind == 0x90U) testInstrument_.noteOn(data1, data2);
        else if (kind == 0xB0U && (data1 == 120U || data1 == 123U)) testInstrument_.panic();
        switch (kind) {
            case 0x80U: return engine_.postNoteOff(ch, data1, data2);
            case 0x90U: return data2 == 0U ? engine_.postNoteOff(ch, data1, 0U) : engine_.postNoteOn(ch, data1, data2);
            case 0xB0U: return engine_.postControlChange(ch, data1, data2);
            case 0xD0U: return engine_.postChannelPressure(ch, data1);
            case 0xE0U: {
                const int raw = static_cast<int>(data1) | (static_cast<int>(data2) << 7);
                return engine_.postPitchBend(ch, static_cast<std::int16_t>(raw - 8192));
            }
            default: return true;
        }
    }

    bool panic() noexcept { testInstrument_.panic(); return engine_.postPanic(); }

    MeterSnapshot meters() const noexcept { return engine_.meters(); }
    int sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int framesPerBurst() const noexcept { return framesPerBurst_.load(std::memory_order_relaxed); }
    int bufferSizeFrames() const noexcept { return bufferSizeFrames_.load(std::memory_order_relaxed); }
    int xrunCount() const noexcept { return xrunCount_.load(std::memory_order_relaxed); }
    int midiDeviceCount() const noexcept { return midiDeviceCount_.load(std::memory_order_relaxed); }
    int midiLastError() const noexcept { return midiLastError_.load(std::memory_order_relaxed); }
    std::uint64_t midiActivitySerial() const noexcept { return midiActivitySerial_.load(std::memory_order_acquire); }
    int midiLastStatus() const noexcept { return midiLastStatus_.load(std::memory_order_relaxed); }
    int midiLastData1() const noexcept { return midiLastData1_.load(std::memory_order_relaxed); }
    int midiLastData2() const noexcept { return midiLastData2_.load(std::memory_order_relaxed); }
    bool testInstrumentEnabled() const noexcept { return testInstrument_.enabled(); }
    bool testNoteOn(std::uint8_t note, std::uint8_t velocity) noexcept {
        if (!testInstrument_.enabled() || velocity == 0U) return false;
        testInstrument_.noteOn(note, velocity);
        return true;
    }
    void testNoteOff(std::uint8_t note) noexcept { testInstrument_.noteOff(note); }

    // The Kotlin status structure predates the Direct-ALSA backend. These
    // AAudio-specific fields remain in the JNI array for ABI compatibility and
    // deliberately report "not applicable" instead of inventing fake values.
    int sharingMode() const noexcept { return -1; }
    int performanceMode() const noexcept { return -1; }
    int deviceId() const noexcept { return -1; }

    int lastError() const noexcept { return lastError_.load(std::memory_order_relaxed); }
    bool needsRecovery() const noexcept { return recoverRequested_.load(std::memory_order_acquire); }
    bool started() const noexcept { return started_.load(std::memory_order_acquire); }

    double estimatedOutputLatencyMs() const noexcept {
        const int fd = pcmFd_.load(std::memory_order_acquire);
        const int sr = sampleRate();
        if (fd < 0 || sr <= 0) return -1.0;
        snd_pcm_sframes_t delayFrames = 0;
        if (::ioctl(fd, SNDRV_PCM_IOCTL_DELAY, &delayFrames) < 0) return -1.0;
        const double queued = std::max(0.0, static_cast<double>(delayFrames));
        return queued * 1000.0 / static_cast<double>(sr);
    }

    std::string textError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return textError_;
    }

private:
    struct PcmConfig {
        unsigned requestedRate{0U};
        unsigned requestedPeriods{0U};
        unsigned rate{0U};
        unsigned periodFrames{0U};
        unsigned periods{0U};
        unsigned bufferFrames{0U};
    };

    struct MidiParserState {
        std::uint8_t runningStatus{0U};
        std::uint8_t pendingStatus{0U};
        std::uint8_t needed{0U};
        std::uint8_t count{0U};
        std::array<std::uint8_t, 2> data{};
        bool inSysEx{false};
    };

    struct RawMidiPort {
        std::string path;
        int fd{-1};
        MidiParserState parser{};
    };

    static constexpr const char* kPcmDevice = "/dev/snd/pcmC0D0p";
    static constexpr const char* kControlDevice = "/dev/snd/controlC0";
    static constexpr const char* kAlsaDeviceDirectory = "/dev/snd";
    static constexpr unsigned kChannels = 2U;
    static constexpr unsigned kPreferredMinimumPeriodFrames = 96U;
    static constexpr std::size_t kMaxRawMidiPorts = 16U;

    static bool isRawMidiNodeName(const char* name) noexcept {
        if (name == nullptr || std::strncmp(name, "midiC", 5U) != 0) return false;
        const char* p = name + 5;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
        if (*p != 'D') return false;
        ++p;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
        return *p == '\0';
    }

    static std::uint8_t midiDataBytesForStatus(std::uint8_t status) noexcept {
        if (status >= 0x80U && status <= 0xBFU) return 2U;
        if (status >= 0xC0U && status <= 0xDFU) return 1U;
        if (status >= 0xE0U && status <= 0xEFU) return 2U;
        if (status == 0xF1U || status == 0xF3U) return 1U;
        if (status == 0xF2U) return 2U;
        return 0U;
    }

    static void resetMidiParser(MidiParserState& parser) noexcept {
        parser = MidiParserState{};
    }

    std::vector<std::string> discoverRawMidiPaths() noexcept {
        std::vector<std::string> paths;
        DIR* dir = ::opendir(kAlsaDeviceDirectory);
        if (dir == nullptr) {
            midiLastError_.store(-errno, std::memory_order_relaxed);
            return paths;
        }

        while (dirent* entry = ::readdir(dir)) {
            if (!isRawMidiNodeName(entry->d_name)) continue;
            paths.emplace_back(std::string(kAlsaDeviceDirectory) + "/" + entry->d_name);
        }
        (void)::closedir(dir);
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        return paths;
    }

    static int openRawMidiInput(const std::string& path) noexcept {
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return -1;

        snd_rawmidi_params params{};
        params.stream = SNDRV_RAWMIDI_STREAM_INPUT;
        params.buffer_size = 4096U;
        params.avail_min = 1U;
        params.no_active_sensing = 0U;
        params.mode = 0U;
        if (::ioctl(fd, SNDRV_RAWMIDI_IOCTL_PARAMS, &params) < 0) {
            const int saved = errno;
            (void)::close(fd);
            errno = saved;
            return -1;
        }
        return fd;
    }

    void closeRawMidiPort(RawMidiPort& port) noexcept {
        if (port.fd >= 0) (void)::close(port.fd);
        port.fd = -1;
        resetMidiParser(port.parser);
    }

    void syncRawMidiPorts() noexcept {
        const auto discovered = discoverRawMidiPaths();

        for (auto it = rawMidiPorts_.begin(); it != rawMidiPorts_.end();) {
            if (std::find(discovered.begin(), discovered.end(), it->path) != discovered.end()) {
                ++it;
                continue;
            }
            __android_log_print(ANDROID_LOG_INFO, kTag, "Direct MIDI detached: %s", it->path.c_str());
            closeRawMidiPort(*it);
            it = rawMidiPorts_.erase(it);
        }

        int firstOpenError = 0;
        for (const auto& path : discovered) {
            const bool alreadyOpen = std::any_of(rawMidiPorts_.begin(), rawMidiPorts_.end(), [&](const RawMidiPort& port) {
                return port.path == path && port.fd >= 0;
            });
            if (alreadyOpen || rawMidiPorts_.size() >= kMaxRawMidiPorts) continue;

            const int fd = openRawMidiInput(path);
            if (fd < 0) {
                if (firstOpenError == 0) firstOpenError = -errno;
                continue;
            }

            RawMidiPort port{};
            port.path = path;
            port.fd = fd;
            rawMidiPorts_.push_back(std::move(port));
            __android_log_print(ANDROID_LOG_INFO, kTag, "Direct MIDI attached: %s", path.c_str());
        }

        midiDeviceCount_.store(static_cast<int>(rawMidiPorts_.size()), std::memory_order_relaxed);
        if (!rawMidiPorts_.empty() || discovered.empty()) {
            midiLastError_.store(0, std::memory_order_relaxed);
        } else if (firstOpenError != 0) {
            midiLastError_.store(firstOpenError, std::memory_order_relaxed);
        }
    }

    void dispatchRawMidiMessage(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {
        if (status >= 0xF0U) return;
        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);
        switch (kind) {
            case 0x80U:
            case 0x90U:
            case 0xB0U:
            case 0xD0U:
            case 0xE0U:
                (void)midi(status, data1, data2);
                break;
            default:
                break;
        }
    }

    void feedRawMidiByte(RawMidiPort& port, std::uint8_t byte) noexcept {
        auto& parser = port.parser;

        // System realtime bytes can be interleaved anywhere and do not alter
        // channel-message running status. The sampler does not consume them.
        if (byte >= 0xF8U) return;

        if (parser.inSysEx) {
            if (byte == 0xF7U) parser.inSysEx = false;
            return;
        }

        if ((byte & 0x80U) != 0U) {
            if (byte == 0xF0U) {
                resetMidiParser(parser);
                parser.inSysEx = true;
                return;
            }

            parser.pendingStatus = byte;
            parser.count = 0U;
            parser.needed = midiDataBytesForStatus(byte);
            parser.runningStatus = byte < 0xF0U ? byte : 0U;
            if (parser.needed == 0U) parser.pendingStatus = 0U;
            return;
        }

        if (parser.pendingStatus == 0U) {
            if (parser.runningStatus == 0U) return;
            parser.pendingStatus = parser.runningStatus;
            parser.needed = midiDataBytesForStatus(parser.runningStatus);
            parser.count = 0U;
        }

        if (parser.count < parser.data.size()) parser.data[parser.count] = static_cast<std::uint8_t>(byte & 0x7FU);
        ++parser.count;
        if (parser.count < parser.needed) return;

        const std::uint8_t status = parser.pendingStatus;
        const std::uint8_t data1 = parser.needed >= 1U ? parser.data[0] : 0U;
        const std::uint8_t data2 = parser.needed >= 2U ? parser.data[1] : 0U;
        dispatchRawMidiMessage(status, data1, data2);

        parser.pendingStatus = parser.runningStatus;
        parser.count = 0U;
        parser.needed = parser.pendingStatus != 0U ? midiDataBytesForStatus(parser.pendingStatus) : 0U;
    }

    void directMidiLoop() noexcept {
        configureRealtimeThread("SilvMidiRT", 0, 5, 70);
        auto nextScan = std::chrono::steady_clock::now();
        std::array<pollfd, kMaxRawMidiPorts> pollFds{};
        std::array<std::uint8_t, 256U> readBuffer{};

        while (midiThreadRun_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextScan) {
                syncRawMidiPorts();
                nextScan = now + std::chrono::milliseconds(250);
            }

            const std::size_t count = std::min(rawMidiPorts_.size(), pollFds.size());
            if (count == 0U) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            for (std::size_t i = 0U; i < count; ++i) {
                pollFds[i].fd = rawMidiPorts_[i].fd;
                pollFds[i].events = POLLIN;
                pollFds[i].revents = 0;
            }

            const int ready = ::poll(pollFds.data(), static_cast<nfds_t>(count), 1);
            if (ready < 0) {
                if (errno == EINTR) continue;
                midiLastError_.store(-errno, std::memory_order_relaxed);
                continue;
            }
            if (ready == 0) continue;

            bool rescanSoon = false;
            for (std::size_t i = 0U; i < count; ++i) {
                if ((pollFds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    closeRawMidiPort(rawMidiPorts_[i]);
                    rescanSoon = true;
                    continue;
                }
                if ((pollFds[i].revents & POLLIN) == 0) continue;

                for (;;) {
                    const ssize_t bytesRead = ::read(rawMidiPorts_[i].fd, readBuffer.data(), readBuffer.size());
                    if (bytesRead > 0) {
                        for (ssize_t b = 0; b < bytesRead; ++b) feedRawMidiByte(rawMidiPorts_[i], readBuffer[static_cast<std::size_t>(b)]);
                        continue;
                    }
                    if (bytesRead == 0) {
                        closeRawMidiPort(rawMidiPorts_[i]);
                        rescanSoon = true;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        midiLastError_.store(-errno, std::memory_order_relaxed);
                        closeRawMidiPort(rawMidiPorts_[i]);
                        rescanSoon = true;
                    }
                    break;
                }
            }

            if (rescanSoon) {
                rawMidiPorts_.erase(
                    std::remove_if(rawMidiPorts_.begin(), rawMidiPorts_.end(), [](const RawMidiPort& port) { return port.fd < 0; }),
                    rawMidiPorts_.end());
                midiDeviceCount_.store(static_cast<int>(rawMidiPorts_.size()), std::memory_order_relaxed);
                nextScan = std::chrono::steady_clock::now();
            }
        }

        for (auto& port : rawMidiPorts_) closeRawMidiPort(port);
        rawMidiPorts_.clear();
        midiDeviceCount_.store(0, std::memory_order_relaxed);
    }

    bool startDirectMidiLocked() {
        if (midiThreadRun_.load(std::memory_order_acquire)) return true;
        midiLastError_.store(0, std::memory_order_relaxed);
        midiThreadRun_.store(true, std::memory_order_release);
        try {
            midiThread_ = std::thread(&AndroidSamplerAudio::directMidiLoop, this);
        } catch (...) {
            midiThreadRun_.store(false, std::memory_order_release);
            midiLastError_.store(-EAGAIN, std::memory_order_relaxed);
            setTextError("Direct ALSA MIDI thread could not be created");
            return false;
        }
        return true;
    }

    void stopDirectMidiLocked() noexcept {
        midiThreadRun_.store(false, std::memory_order_release);
        if (midiThread_.joinable()) midiThread_.join();
        midiDeviceCount_.store(0, std::memory_order_relaxed);
    }

    static bool isMaskParam(int param) noexcept {
        return param >= SNDRV_PCM_HW_PARAM_FIRST_MASK && param <= SNDRV_PCM_HW_PARAM_LAST_MASK;
    }

    static bool isIntervalParam(int param) noexcept {
        return param >= SNDRV_PCM_HW_PARAM_FIRST_INTERVAL && param <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL;
    }

    static snd_mask* maskParam(snd_pcm_hw_params& params, int param) noexcept {
        return &params.masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    }

    static snd_interval* intervalParam(snd_pcm_hw_params& params, int param) noexcept {
        return &params.intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    }

    static void initHwParams(snd_pcm_hw_params& params) noexcept {
        std::memset(&params, 0, sizeof(params));
        for (int param = SNDRV_PCM_HW_PARAM_FIRST_MASK; param <= SNDRV_PCM_HW_PARAM_LAST_MASK; ++param) {
            auto* mask = maskParam(params, param);
            for (auto& word : mask->bits) word = ~0U;
        }
        for (int param = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; param <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; ++param) {
            auto* interval = intervalParam(params, param);
            interval->min = 0U;
            interval->max = ~0U;
        }
        params.rmask = ~0U;
        params.cmask = 0U;
        params.info = ~0U;
    }

    static void setMaskParam(snd_pcm_hw_params& params, int param, unsigned bit) noexcept {
        if (!isMaskParam(param) || bit >= SNDRV_MASK_MAX) return;
        auto* mask = maskParam(params, param);
        std::memset(mask, 0, sizeof(*mask));
        mask->bits[bit >> 5U] |= (1U << (bit & 31U));
    }

    static void setIntParam(snd_pcm_hw_params& params, int param, unsigned value) noexcept {
        if (!isIntervalParam(param)) return;
        auto* interval = intervalParam(params, param);
        std::memset(interval, 0, sizeof(*interval));
        interval->min = value;
        interval->max = value;
        interval->integer = 1U;
    }

    static void setMinParam(snd_pcm_hw_params& params, int param, unsigned value) noexcept {
        if (!isIntervalParam(param)) return;
        auto* interval = intervalParam(params, param);
        interval->min = value;
        interval->openmin = 0U;
    }

    static unsigned getMinParam(snd_pcm_hw_params& params, int param) noexcept {
        return isIntervalParam(param) ? intervalParam(params, param)->min : 0U;
    }

    static std::optional<snd_ctl_elem_id> findControlId(int fd, const char* name) noexcept {
        if (fd < 0 || name == nullptr) return std::nullopt;
        snd_ctl_elem_list list{};
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || list.count == 0U) return std::nullopt;

        std::vector<snd_ctl_elem_id> ids(list.count);
        list.offset = 0U;
        list.space = list.count;
        list.pids = ids.data();
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0) return std::nullopt;

        const unsigned used = std::min<unsigned>(list.used, static_cast<unsigned>(ids.size()));
        for (unsigned i = 0U; i < used; ++i) {
            const char* controlName = reinterpret_cast<const char*>(ids[i].name);
            if (std::strncmp(controlName, name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN) == 0) return ids[i];
        }
        return std::nullopt;
    }

    static bool setIntegerControl(int fd, const char* name, long first, long second, int& errorOut) noexcept {
        const auto id = findControlId(fd, name);
        if (!id.has_value()) { errorOut = -ENOENT; return false; }

        snd_ctl_elem_info info{};
        info.id = *id;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }
        if (info.type != SNDRV_CTL_ELEM_TYPE_BOOLEAN && info.type != SNDRV_CTL_ELEM_TYPE_INTEGER) {
            errorOut = -EINVAL;
            return false;
        }

        snd_ctl_elem_value value{};
        value.id = *id;
        const unsigned count = std::min<unsigned>(info.count, 128U);
        for (unsigned i = 0U; i < count; ++i) value.value.integer.value[i] = (i == 0U) ? first : second;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }
        errorOut = 0;
        return true;
    }

    static bool setEnumControl(int fd, const char* name, const char* itemName, int& errorOut) noexcept {
        const auto id = findControlId(fd, name);
        if (!id.has_value()) { errorOut = -ENOENT; return false; }

        snd_ctl_elem_info info{};
        info.id = *id;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { errorOut = -errno; return false; }
        if (info.type != SNDRV_CTL_ELEM_TYPE_ENUMERATED || info.value.enumerated.items == 0U) {
            errorOut = -EINVAL;
            return false;
        }

        unsigned selected = info.value.enumerated.items;
        for (unsigned item = 0U; item < info.value.enumerated.items; ++item) {
            snd_ctl_elem_info itemInfo{};
            itemInfo.id = *id;
            itemInfo.value.enumerated.item = item;
            if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &itemInfo) < 0) { errorOut = -errno; return false; }
            if (std::strncmp(itemInfo.value.enumerated.name, itemName, sizeof(itemInfo.value.enumerated.name)) == 0) {
                selected = item;
                break;
            }
        }
        if (selected >= info.value.enumerated.items) { errorOut = -ENOENT; return false; }

        snd_ctl_elem_value value{};
        value.id = *id;
        const unsigned count = std::min<unsigned>(info.count, 128U);
        for (unsigned i = 0U; i < count; ++i) value.value.enumerated.item[i] = selected;
        if (::ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) < 0) { errorOut = -errno; return false; }
        errorOut = 0;
        return true;
    }

    bool configureOutputRouteLocked() {
        const int fd = ::open(kControlDevice, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            lastError_.store(-errno, std::memory_order_relaxed);
            return false;
        }

        int error = 0;
        const bool ok = setIntegerControl(fd, "ADDA_DL_CH1 DL1_CH1", 1L, 1L, error)
            && setIntegerControl(fd, "ADDA_DL_CH2 DL1_CH2", 1L, 1L, error)
            && setEnumControl(fd, "DAC In Mux", "Normal Path", error)
            && setEnumControl(fd, "HPL Mux", "Audio Playback", error)
            && setEnumControl(fd, "HPR Mux", "Audio Playback", error);

        if (ok) {
            int volumeError = 0;
            (void)setIntegerControl(fd, "Headset Volume", 12L, 12L, volumeError);
        }

        const int closeResult = ::close(fd);
        if (!ok) {
            lastError_.store(error == 0 ? -EIO : error, std::memory_order_relaxed);
            return false;
        }
        if (closeResult < 0) {
            lastError_.store(-errno, std::memory_order_relaxed);
            return false;
        }

        __android_log_print(ANDROID_LOG_INFO, kTag, "Direct ALSA SM-X130 output route configured");
        return true;
    }

    static int openPcmNode() noexcept {
        return ::open(kPcmDevice, O_RDWR | O_CLOEXEC);
    }

    static void releasePcmFd(int fd) noexcept {
        if (fd < 0) return;
        (void)::ioctl(fd, SNDRV_PCM_IOCTL_DROP);
        (void)::ioctl(fd, SNDRV_PCM_IOCTL_HW_FREE);
        (void)::close(fd);
    }

    static bool configurePcmFd(
        int fd,
        unsigned requestedRate,
        unsigned requestedPeriods,
        PcmConfig& config,
        int& errorOut) noexcept {
        if (fd < 0) {
            errorOut = -EBADF;
            return false;
        }

        snd_pcm_hw_params params{};
        initHwParams(params);
        setMaskParam(params, SNDRV_PCM_HW_PARAM_ACCESS, static_cast<unsigned>(SNDRV_PCM_ACCESS_RW_INTERLEAVED));
        setMaskParam(params, SNDRV_PCM_HW_PARAM_FORMAT, static_cast<unsigned>(SNDRV_PCM_FORMAT_S16_LE));
        setMaskParam(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, static_cast<unsigned>(SNDRV_PCM_SUBFORMAT_STD));
        setIntParam(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16U);
        setIntParam(params, SNDRV_PCM_HW_PARAM_FRAME_BITS, 16U * kChannels);
        setIntParam(params, SNDRV_PCM_HW_PARAM_CHANNELS, kChannels);
        setIntParam(params, SNDRV_PCM_HW_PARAM_RATE, requestedRate);
        // Physical SM-X130 validation proved exactly 96 frames x 2 at 96 kHz.
        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, kPreferredMinimumPeriodFrames);
        setIntParam(params, SNDRV_PCM_HW_PARAM_PERIODS, requestedPeriods);
        params.flags |= SNDRV_PCM_HW_PARAMS_NORESAMPLE;

        if (::ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
            errorOut = -errno;
            return false;
        }

        config.requestedRate = requestedRate;
        config.requestedPeriods = requestedPeriods;
        config.rate = getMinParam(params, SNDRV_PCM_HW_PARAM_RATE);
        config.periodFrames = getMinParam(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
        config.periods = getMinParam(params, SNDRV_PCM_HW_PARAM_PERIODS);
        config.bufferFrames = getMinParam(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

        if (config.rate == 0U) config.rate = requestedRate;
        if (config.periodFrames == 0U) config.periodFrames = kPreferredMinimumPeriodFrames;
        if (config.periods == 0U) config.periods = requestedPeriods;
        if (config.bufferFrames == 0U) config.bufferFrames = config.periodFrames * config.periods;

        if (config.rate != requestedRate
            || config.periodFrames != kPreferredMinimumPeriodFrames
            || config.periods != requestedPeriods
            || config.bufferFrames != config.periodFrames * config.periods) {
            errorOut = -EINVAL;
            return false;
        }

        snd_pcm_sw_params sw{};
        sw.tstamp_mode = SNDRV_PCM_TSTAMP_ENABLE;
        sw.period_step = 1U;
        sw.avail_min = static_cast<snd_pcm_uframes_t>(config.periodFrames);
        sw.start_threshold = static_cast<snd_pcm_uframes_t>(
            config.bufferFrames - config.periodFrames);
        sw.stop_threshold = static_cast<snd_pcm_uframes_t>(config.bufferFrames);
        sw.xfer_align = 1U;
        sw.silence_threshold = 0U;
        sw.silence_size = 0U;

        if (::ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
            errorOut = -errno;
            return false;
        }
        if (::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            errorOut = -errno;
            return false;
        }

        errorOut = 0;
        return true;
    }

    static double queueCapacityMs(const PcmConfig& config) noexcept {
        if (config.rate == 0U) return std::numeric_limits<double>::infinity();
        return static_cast<double>(config.bufferFrames) * 1000.0 / static_cast<double>(config.rate);
    }

    bool chooseAndOpenPcmLocked() {
        // Direct ALSA bypasses Android AudioPolicy. Lock the exact hardware
        // profile already proven on the physical SM-X130: 96 kHz / 96 x 2.
        constexpr unsigned kRate = 96000U;
        constexpr unsigned kPeriods = 2U;

        const int fd = openPcmNode();
        if (fd < 0) {
            const int error = -errno;
            lastError_.store(error, std::memory_order_relaxed);
            if (error == -EACCES || error == -EPERM) {
                recoverRequested_.store(true, std::memory_order_release);
            }
            return false;
        }

        PcmConfig selected{};
        int error = 0;
        if (!configurePcmFd(fd, kRate, kPeriods, selected, error)) {
            releasePcmFd(fd);
            lastError_.store(error, std::memory_order_relaxed);
            return false;
        }

        pcmFd_.store(fd, std::memory_order_release);
        sampleRate_.store(static_cast<int>(selected.rate), std::memory_order_relaxed);
        framesPerBurst_.store(static_cast<int>(selected.periodFrames), std::memory_order_relaxed);
        bufferSizeFrames_.store(static_cast<int>(selected.bufferFrames), std::memory_order_relaxed);
        periodCount_.store(static_cast<int>(selected.periods), std::memory_order_relaxed);

        const std::size_t sampleCount = static_cast<std::size_t>(selected.periodFrames) * kChannels;
        renderBuffer_.assign(sampleCount, 0.0F);
        pcmBuffer_.assign(sampleCount, 0);

        __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "Direct ALSA locked %s: %u Hz, period=%u, periods=%u, buffer=%u (%.3f ms)",
            kPcmDevice, selected.rate, selected.periodFrames, selected.periods,
            selected.bufferFrames, queueCapacityMs(selected));
        return true;
    }

    bool openLocked() {
        if (pcmFd_.load(std::memory_order_acquire) >= 0) return true;
        // Mixer route is configured by the privileged pre-open root gate.
        // The APK itself only owns the PCM realtime path.
        if (!chooseAndOpenPcmLocked()) {
            const int error = lastError_.load(std::memory_order_relaxed);
            if (error == -EACCES || error == -EPERM) {
                recoverRequested_.store(true, std::memory_order_release);
                setTextError("Direct ALSA permission denied for /dev/snd. Install the SILVETON SM-X130 audio privilege module.");
            } else {
                setTextError(
                    std::string("AUDIO DRIVER: PCM open/config failed ") + kPcmDevice
                    + " (errno " + std::to_string(-error) + ")"
                    + (error == -EBUSY ? " [PCM BUSY]" : ""));
            }
            return false;
        }

        const int sr = sampleRate();
        const int period = framesPerBurst();
        engine_.prepare(
            static_cast<double>(std::max(8000, sr)),
            static_cast<std::uint32_t>(std::max(64, period)));
        if (state_ != nullptr) engine_.setState(state_);

        xrunCount_.store(0, std::memory_order_relaxed);
        lastError_.store(0, std::memory_order_relaxed);
        recoverRequested_.store(false, std::memory_order_release);
        setTextError({});
        return true;
    }

    bool startLocked() {
        if (started_.load(std::memory_order_acquire)) return true;
        if (pcmFd_.load(std::memory_order_acquire) < 0 && !openLocked()) return false;

        const int fd = pcmFd_.load(std::memory_order_acquire);
        if (fd < 0) {
            lastError_.store(-ENODEV, std::memory_order_relaxed);
            return false;
        }
        if (::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            const int error = -errno;
            lastError_.store(error, std::memory_order_relaxed);
            recoverRequested_.store(true, std::memory_order_release);
            setTextError("AUDIO DRIVER: PREPARE failed (errno " + std::to_string(-error) + ")");
            return false;
        }

        threadRun_.store(true, std::memory_order_release);
        try {
            audioThread_ = std::thread(&AndroidSamplerAudio::audioLoop, this);
        } catch (...) {
            threadRun_.store(false, std::memory_order_release);
            lastError_.store(-EAGAIN, std::memory_order_relaxed);
            setTextError("Direct ALSA audio thread could not be created");
            return false;
        }
        started_.store(true, std::memory_order_release);
        setTextError({});
        return true;
    }

    bool stopLocked() {
        const int fd = pcmFd_.load(std::memory_order_acquire);
        threadRun_.store(false, std::memory_order_release);

        // DROP wakes a blocked WRITEI ioctl so the thread can terminate without
        // waiting for the hardware queue to drain.
        if (fd >= 0) (void)::ioctl(fd, SNDRV_PCM_IOCTL_DROP);
        if (audioThread_.joinable()) audioThread_.join();

        started_.store(false, std::memory_order_release);
        if (fd >= 0 && ::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            const int error = -errno;
            lastError_.store(error, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void closePcmLocked() noexcept {
        const int fd = pcmFd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) releasePcmFd(fd);
        sampleRate_.store(0, std::memory_order_relaxed);
        framesPerBurst_.store(0, std::memory_order_relaxed);
        bufferSizeFrames_.store(0, std::memory_order_relaxed);
        periodCount_.store(0, std::memory_order_relaxed);
        renderBuffer_.clear();
        pcmBuffer_.clear();
    }

    bool recoverXrunOnAudioThread(int fd) noexcept {
        xrunCount_.fetch_add(1, std::memory_order_relaxed);
        if (::ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            lastError_.store(-errno, std::memory_order_relaxed);
            recoverRequested_.store(true, std::memory_order_release);
            return false;
        }
        return true;
    }

    bool writePeriodOnAudioThread(int fd, std::uint32_t frames) noexcept {
        std::uint32_t offset = 0U;
        while (offset < frames && threadRun_.load(std::memory_order_acquire)) {
            snd_xferi transfer{};
            transfer.buf = pcmBuffer_.data() + static_cast<std::size_t>(offset) * kChannels;
            transfer.frames = static_cast<snd_pcm_uframes_t>(frames - offset);

            const int rc = ::ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &transfer);
            snd_pcm_sframes_t written = transfer.result;
            if (rc < 0) written = static_cast<snd_pcm_sframes_t>(-errno);

            // A deliberate stop() issues DROP to wake this ioctl. Do not count
            // that controlled wake-up as a hardware XRUN.
            if (!threadRun_.load(std::memory_order_acquire)) return false;

            if (written == -EPIPE || written == -ESTRPIPE) {
                if (!recoverXrunOnAudioThread(fd)) return false;
                continue;
            }
            if (written < 0) {
                lastError_.store(static_cast<int>(written), std::memory_order_relaxed);
                recoverRequested_.store(true, std::memory_order_release);
                return false;
            }
            if (written == 0) {
                std::this_thread::yield();
                continue;
            }

            const auto advance = static_cast<std::uint32_t>(
                std::min<snd_pcm_sframes_t>(written, static_cast<snd_pcm_sframes_t>(frames - offset)));
            offset += advance;
        }
        return offset == frames;
    }

    void audioLoop() noexcept {
        configureRealtimeThread("SilvAudioRT", 7, 7, 80);
#if defined(__aarch64__)
        // Flush denormal floats to zero on the audio thread (reverb/delay tails
        // decay into denormals; they cost extra cycles and are inaudible).
        {
            std::uint64_t fpcr = 0U;
            __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
            fpcr |= (std::uint64_t{1} << 24);
            __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
        }
#endif

        const int fd = pcmFd_.load(std::memory_order_acquire);
        const int period = framesPerBurst();
        if (fd < 0 || period <= 0 || renderBuffer_.empty() || pcmBuffer_.empty()) {
            lastError_.store(-EINVAL, std::memory_order_relaxed);
            recoverRequested_.store(true, std::memory_order_release);
            started_.store(false, std::memory_order_release);
            threadRun_.store(false, std::memory_order_release);
            return;
        }

        const auto frames = static_cast<std::uint32_t>(period);
        while (threadRun_.load(std::memory_order_acquire)) {
            engine_.render(renderBuffer_.data(), frames);
            testInstrument_.render(renderBuffer_.data(), frames, sampleRate());

            const std::size_t samples = static_cast<std::size_t>(frames) * kChannels;
            for (std::size_t i = 0U; i < samples; ++i) {
                const float clipped = std::clamp(renderBuffer_[i], -1.0F, 1.0F);
                const long quantized = std::lrint(static_cast<double>(clipped) * 32767.0);
                pcmBuffer_[i] = static_cast<std::int16_t>(
                    std::clamp<long>(quantized, -32768L, 32767L));
            }

            if (!writePeriodOnAudioThread(fd, frames)) break;
        }

        threadRun_.store(false, std::memory_order_release);
        started_.store(false, std::memory_order_release);
    }

    bool commitSampleEdit(std::uint32_t id, const SamplePtr& previous, SampleEditResult result) {
        if (!result.ok()) { setTextError(result.error); return false; }
        if (!library_.writeSample(id, result.sample)) { setTextError("SAMPLE write-back failed"); return false; }
        if (state_ != nullptr) {
            auto next = std::make_shared<EngineState>(*state_); bool changed=false;
            for (auto& upper : next->combi.uppers) {
                if (upper.program == nullptr) continue;
                std::shared_ptr<Program> pc;
                for (std::size_t li=0U; li<upper.program->layers.size(); ++li) {
                    const auto& oldMs=upper.program->layers[li].multisample; if(oldMs==nullptr) continue;
                    std::shared_ptr<Multisample> mc;
                    for(std::size_t zi=0U; zi<oldMs->zones.size(); ++zi){
                        if(oldMs->zones[zi].sample!=previous) continue;
                        if(mc==nullptr) mc=std::make_shared<Multisample>(*oldMs);
                        mc->zones[zi].sample=result.sample;
                    }
                    if(mc!=nullptr){ if(pc==nullptr) pc=std::make_shared<Program>(*upper.program); pc->layers[li].multisample=mc; }
                }
                if(pc!=nullptr){ upper.program=pc; changed=true; }
            }
            if(changed && !installState(std::move(next))) return false;
        }
        setTextError({}); return true;
    }

    bool commitMultisampleEdit(std::uint32_t id, const MultisamplePtr& previous, MultisampleEditResult result) {
        if(!result.ok()){setTextError(result.error);return false;}
        if(!library_.writeMultisample(id,result.multisample)){setTextError("MULTISAMPLE write-back failed");return false;}
        if(state_!=nullptr){auto next=std::make_shared<EngineState>(*state_);bool changed=false;for(auto& upper:next->combi.uppers){if(upper.program==nullptr)continue;std::shared_ptr<Program> pc;for(std::size_t li=0U;li<upper.program->layers.size();++li){if(upper.program->layers[li].multisample!=previous)continue;if(pc==nullptr)pc=std::make_shared<Program>(*upper.program);pc->layers[li].multisample=result.multisample;}if(pc!=nullptr){upper.program=pc;changed=true;}}if(changed&&!installState(std::move(next)))return false;}
        setTextError({});return true;
    }

    bool commitProgramEdit(const SlotAddress& address, const ProgramPtr& previous, ProgramEditResult result) {
        if (!result.ok()) { setTextError(result.error); return false; }
        if (!library_.writeProgram(address, result.program)) { setTextError("PROGRAM write-back failed"); return false; }
        if (state_ != nullptr) {
            auto next = std::make_shared<EngineState>(*state_);
            bool activeChanged = false;
            for (auto& upper : next->combi.uppers) {
                if (upper.program == previous) {
                    upper.program = result.program;
                    activeChanged = true;
                }
            }
            if (activeChanged && !installState(std::move(next))) return false;
        }
        setTextError({});
        return true;
    }

    bool ensureKscCache(const std::string& path) {
        if (path == kscCachePath_ && kscCache_.ok()) return true;
        auto imported = KorgImporter::loadKsc(path);
        if (!imported.ok()) { setTextError(imported.error); return false; }
        kscCachePath_ = path;
        kscCache_ = std::move(imported);
        setTextError({});
        return true;
    }

    MultisamplePtr makeSingleSampleMultisample(const SamplePtr& sample, const std::string& name) {
        auto ms = std::make_shared<Multisample>();
        ms->name = name;
        SampleZone z{};
        z.sample = sample;
        z.keyLow = 0U; z.keyHigh = 127U; z.velocityLow = 1U; z.velocityHigh = 127U;
        z.rootKey = sample->rootKey;
        ms->zones.push_back(z);
        return ms;
    }

    bool installMultisample(const MultisamplePtr& ms, const std::string& name) {
        if (ms == nullptr) return false;
        auto program = std::make_shared<Program>();
        program->name = name;
        ProgramLayer layer{};
        layer.name = "Layer 1";
        layer.multisample = ms;
        program->layers.push_back(layer);
        auto state = std::make_shared<EngineState>();
        state->combi.name = name;
        auto& u = state->combi.uppers[0];
        u.name = "Upper 1";
        u.enabled = true;
        u.program = program;
        u.midiChannel = 0U;
        return installState(std::move(state));
    }

    bool installState(std::shared_ptr<const EngineState> state) {
        if (state == nullptr) return false;
        std::lock_guard<std::mutex> lock(controlMutex_);
        state_ = std::move(state);
        // Real bank SOUND is authoritative. The old diagnostic TEST EP is retired.
        testInstrument_.setEnabled(false);
        engine_.setState(state_);
        return true;
    }

    void syncCurrentStateToLibrary() {
        if (state_ == nullptr) return;
        library_.setGlobal(state_->global);
    }

    void setTextError(const std::string& s) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        textError_ = s;
    }

    SamplerEngine engine_;
    SampleStreamPrefetcher prefetcher_{engine_};
    BuiltinTestInstrument testInstrument_{};
    std::atomic<std::uint64_t> midiActivitySerial_{0U};
    std::atomic<int> midiLastStatus_{0};
    std::atomic<int> midiLastData1_{0};
    std::atomic<int> midiLastData2_{0};
    SamplerLibrary library_{"SILVETON SET"};
    KorgCollectionResult kscCache_{};
    std::string kscCachePath_;
    std::shared_ptr<const EngineState> state_{std::make_shared<EngineState>()};
    std::optional<SlotAddress> activeListAddress_{};
    int activeListEntry_{-1};
    std::atomic<int> pcmFd_{-1};
    std::thread audioThread_{};
    std::thread midiThread_{};
    std::vector<RawMidiPort> rawMidiPorts_{};
    std::vector<float> renderBuffer_{};
    std::vector<std::int16_t> pcmBuffer_{};
    std::atomic<bool> threadRun_{false};
    std::atomic<bool> midiThreadRun_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> wantStarted_{false};
    std::atomic<bool> recoverRequested_{false};
    std::atomic<int> sampleRate_{0};
    std::atomic<int> framesPerBurst_{0};
    std::atomic<int> bufferSizeFrames_{0};
    std::atomic<int> periodCount_{0};
    std::atomic<int> xrunCount_{0};
    std::atomic<int> midiDeviceCount_{0};
    std::atomic<int> midiLastError_{0};
    std::atomic<int> lastError_{0};
    mutable std::mutex controlMutex_;
    mutable std::mutex errorMutex_;
    std::string textError_;
};

std::unique_ptr<AndroidSamplerAudio> gSampler;
AndroidSamplerAudio& sampler() { if (!gSampler) gSampler = std::make_unique<AndroidSamplerAudio>(); return *gSampler; }

SlotAddress makeAddress(jint bank, jint page, jint slot) noexcept {
    SlotAddress a{};
    a.bank = static_cast<std::uint16_t>(std::clamp(bank, 0, 65535));
    a.page = static_cast<std::uint16_t>(std::clamp(page, 0, 65535));
    a.slot = static_cast<std::uint16_t>(std::clamp(slot, 0, 65535));
    return a;
}

jfloatArray metersArray(JNIEnv* env, const MeterSnapshot& m) {
    std::array<jfloat, 5 + kUpperCount * 2> v{};
    v[0]=m.peakLeft; v[1]=m.peakRight; v[2]=m.limiterGainReductionDb;
    v[3]=static_cast<jfloat>(m.activeVoices); v[4]=static_cast<jfloat>(m.droppedEvents);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kUpperCount); ++i) {
        const std::size_t base = 5U + i * 2U;
        v[base] = m.upperPeakLeft[i];
        v[base + 1U] = m.upperPeakRight[i];
    }
    jfloatArray a=env->NewFloatArray(static_cast<jsize>(v.size()));
    if(a!=nullptr)env->SetFloatArrayRegion(a,0,static_cast<jsize>(v.size()),v.data());
    return a;
}

jintArray intVectorArray(JNIEnv* env, const std::vector<int>& values) {
    jintArray out = env->NewIntArray(static_cast<jsize>(values.size()));
    if (out == nullptr || values.empty()) return out;
    std::vector<jint> raw; raw.reserve(values.size());
    for (const int value : values) raw.push_back(static_cast<jint>(value));
    env->SetIntArrayRegion(out, 0, static_cast<jsize>(raw.size()), raw.data());
    return out;
}

jfloatArray floatVectorArray(JNIEnv* env, const std::vector<float>& values) {
    jfloatArray out = env->NewFloatArray(static_cast<jsize>(values.size()));
    if (out == nullptr || values.empty()) return out;
    env->SetFloatArrayRegion(out, 0, static_cast<jsize>(values.size()), values.data());
    return out;
}

jobjectArray stringVectorArray(JNIEnv* env, const std::vector<std::string>& values) {
    jclass cls = env->FindClass("java/lang/String");
    if (cls == nullptr) return nullptr;
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(values.size()), cls, nullptr);
    if (out == nullptr) return nullptr;
    for (std::size_t i = 0; i < values.size(); ++i) {
        jstring js = env->NewStringUTF(values[i].c_str());
        env->SetObjectArrayElement(out, static_cast<jsize>(i), js);
        env->DeleteLocalRef(js);
    }
    return out;
}
}

extern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSamplerInit(JNIEnv*,jobject){gSampler=std::make_unique<AndroidSamplerAudio>();}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioOpen(JNIEnv*,jobject){return sampler().open()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStart(JNIEnv*,jobject){return sampler().start()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStop(JNIEnv*,jobject){sampler().stop();}
extern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioClose(JNIEnv*,jobject){sampler().close();}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioRecover(JNIEnv*,jobject){return sampler().recover()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioNeedsRecovery(JNIEnv*,jobject){return sampler().needsRecovery()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadAudio(JNIEnv* env,jobject,jstring path){return sampler().loadAudioFile(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadKmp(JNIEnv* env,jobject,jstring path){return sampler().loadKmp(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeImportKmpToLibrary(JNIEnv* env,jobject,jstring path){return intVectorArray(env,sampler().importKmpToLibrary(jstringToUtf8(env,path)));}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeImportKscToLibrary(JNIEnv* env,jobject,jstring path){return intVectorArray(env,sampler().importKscToLibrary(jstringToUtf8(env,path)));}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMultisampleIds(JNIEnv* env,jobject){return intVectorArray(env,sampler().multisampleIds());}
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMultisampleNames(JNIEnv* env,jobject){
    const auto names=sampler().multisampleNames();
    jclass stringClass=env->FindClass("java/lang/String");
    if(stringClass==nullptr)return nullptr;
    jobjectArray out=env->NewObjectArray(static_cast<jsize>(names.size()),stringClass,nullptr);
    if(out==nullptr)return nullptr;
    for(std::size_t i=0;i<names.size();++i){jstring v=env->NewStringUTF(names[i].c_str());env->SetObjectArrayElement(out,static_cast<jsize>(i),v);env->DeleteLocalRef(v);}
    return out;
}
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeKscNames(JNIEnv* env,jobject,jstring path){
    const auto names=sampler().kscNames(jstringToUtf8(env,path));
    jclass stringClass=env->FindClass("java/lang/String");
    if(stringClass==nullptr)return nullptr;
    jobjectArray out=env->NewObjectArray(static_cast<jsize>(names.size()),stringClass,nullptr);
    if(out==nullptr)return nullptr;
    for(std::size_t i=0;i<names.size();++i){jstring v=env->NewStringUTF(names[i].c_str());env->SetObjectArrayElement(out,static_cast<jsize>(i),v);env->DeleteLocalRef(v);}
    return out;
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadKscItem(JNIEnv* env,jobject,jstring path,jint index){return sampler().loadKscItem(jstringToUtf8(env,path),index)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadSvBank(JNIEnv* env,jobject,jstring path){return sampler().loadSvBank(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadSet(JNIEnv* env,jobject,jstring path){return sampler().loadSet(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSaveUserPerformanceState(JNIEnv* env,jobject,jstring path){return sampler().saveUserPerformanceState(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadUserPerformanceState(JNIEnv* env,jobject,jstring path){return sampler().loadUserPerformanceState(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSaveBank(JNIEnv* env,jobject,jstring path,jint bank){return sampler().saveBank(jstringToUtf8(env,path),bank)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadBank(JNIEnv* env,jobject,jstring path,jint targetBank,jboolean overwrite){return sampler().loadBank(jstringToUtf8(env,path),targetBank,overwrite==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteSample(JNIEnv* env,jobject,jint id,jint upper,jint layer,jint zone,jstring name){return sampler().writeCurrentSample(static_cast<std::uint32_t>(std::max(0,id)),upper,layer,zone,jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateSampleMetadata(JNIEnv* env,jobject,jint id,jstring name,jint rootKey,jfloat fineTune){return sampler().updateSampleMetadata(static_cast<std::uint32_t>(std::max(0,id)),jstringToUtf8(env,name),rootKey,fineTune)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTrimSample(JNIEnv*,jobject,jint id,jlong start,jlong end){if(start<0||end<0)return JNI_FALSE;return sampler().trimSample(static_cast<std::uint32_t>(std::max(0,id)),static_cast<std::uint64_t>(start),static_cast<std::uint64_t>(end))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeNormalizeSample(JNIEnv*,jobject,jint id,jfloat targetDb){return sampler().normalizeSample(static_cast<std::uint32_t>(std::max(0,id)),targetDb)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeReverseSample(JNIEnv*,jobject,jint id){return sampler().reverseSample(static_cast<std::uint32_t>(std::max(0,id)))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeFadeSample(JNIEnv*,jobject,jint id,jlong fadeIn,jlong fadeOut){if(fadeIn<0||fadeOut<0)return JNI_FALSE;return sampler().fadeSample(static_cast<std::uint32_t>(std::max(0,id)),static_cast<std::uint64_t>(fadeIn),static_cast<std::uint64_t>(fadeOut))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSetSampleLoop(JNIEnv*,jobject,jint id,jlong start,jlong end,jint crossfade){if(start<0||end<0||crossfade<0)return JNI_FALSE;return sampler().setSampleLoop(static_cast<std::uint32_t>(std::max(0,id)),static_cast<std::uint64_t>(start),static_cast<std::uint64_t>(end),static_cast<std::uint32_t>(crossfade))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearSampleLoop(JNIEnv*,jobject,jint id){return sampler().clearSampleLoop(static_cast<std::uint32_t>(std::max(0,id)))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jlong JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSampleNearestZeroCrossing(JNIEnv*,jobject,jint id,jlong around,jlong radius){if(around<0||radius<0)return static_cast<jlong>(-1);return static_cast<jlong>(sampler().sampleNearestZeroCrossing(static_cast<std::uint32_t>(std::max(0,id)),static_cast<std::uint64_t>(around),static_cast<std::uint64_t>(radius)));}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteMultisample(JNIEnv* env,jobject,jint id,jint upper,jint layer,jstring name){return sampler().writeCurrentMultisample(static_cast<std::uint32_t>(std::max(0,id)),upper,layer,jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRenameMultisample(JNIEnv* env,jobject,jint id,jstring name){return sampler().renameMultisample(static_cast<std::uint32_t>(std::max(0,id)),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAddMultisampleZone(JNIEnv*,jobject,jint id,jint sampleId,jint insertIndex,jint keyLow,jint keyHigh,jint velLow,jint velHigh,jint rootKey,jint transpose,jfloat tune,jfloat gain,jfloat pan,jboolean oneShot,jboolean reverse,jboolean nonTranspose){return sampler().addMultisampleZone(static_cast<std::uint32_t>(std::max(0,id)),static_cast<std::uint32_t>(std::max(0,sampleId)),insertIndex,keyLow,keyHigh,velLow,velHigh,rootKey,transpose,tune,gain,pan,oneShot==JNI_TRUE,reverse==JNI_TRUE,nonTranspose==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateMultisampleZone(JNIEnv*,jobject,jint id,jint zoneIndex,jint sampleId,jint keyLow,jint keyHigh,jint velLow,jint velHigh,jint rootKey,jint transpose,jfloat tune,jfloat gain,jfloat pan,jboolean oneShot,jboolean reverse,jboolean nonTranspose){return sampler().updateMultisampleZone(static_cast<std::uint32_t>(std::max(0,id)),zoneIndex,static_cast<std::uint32_t>(std::max(0,sampleId)),keyLow,keyHigh,velLow,velHigh,rootKey,transpose,tune,gain,pan,oneShot==JNI_TRUE,reverse==JNI_TRUE,nonTranspose==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRemoveMultisampleZone(JNIEnv*,jobject,jint id,jint zoneIndex){return sampler().removeMultisampleZone(static_cast<std::uint32_t>(std::max(0,id)),zoneIndex)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMoveMultisampleZone(JNIEnv*,jobject,jint id,jint fromIndex,jint toIndex){return sampler().moveMultisampleZone(static_cast<std::uint32_t>(std::max(0,id)),fromIndex,toIndex)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearMultisample(JNIEnv*,jobject,jint id){return sampler().clearMultisample(static_cast<std::uint32_t>(std::max(0,id)))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeProgramSlotOccupied(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().programSlotOccupied(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiSlotOccupied(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().combiSlotOccupied(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeListSlotOccupied(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().listSlotOccupied(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRenameProgram(JNIEnv* env,jobject,jint bank,jint page,jint slot,jstring name){return sampler().renameProgram(makeAddress(bank,page,slot),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRenameCombi(JNIEnv* env,jobject,jint bank,jint page,jint slot,jstring name){return sampler().renameCombi(makeAddress(bank,page,slot),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRenameList(JNIEnv* env,jobject,jint bank,jint page,jint slot,jstring name){return sampler().renameList(makeAddress(bank,page,slot),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCopyProgram(JNIEnv*,jobject,jint fb,jint fp,jint fs,jint tb,jint tp,jint ts,jboolean overwrite){return sampler().copyProgram(makeAddress(fb,fp,fs),makeAddress(tb,tp,ts),overwrite==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCopyCombi(JNIEnv*,jobject,jint fb,jint fp,jint fs,jint tb,jint tp,jint ts,jboolean overwrite){return sampler().copyCombi(makeAddress(fb,fp,fs),makeAddress(tb,tp,ts),overwrite==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCopyList(JNIEnv*,jobject,jint fb,jint fp,jint fs,jint tb,jint tp,jint ts,jboolean overwrite){return sampler().copyList(makeAddress(fb,fp,fs),makeAddress(tb,tp,ts),overwrite==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidi(JNIEnv*,jobject,jint status,jint d1,jint d2){return sampler().midi(static_cast<std::uint8_t>(status&0xFF),static_cast<std::uint8_t>(d1&0x7F),static_cast<std::uint8_t>(d2&0x7F))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiDeviceCount(JNIEnv*,jobject){return sampler().midiDeviceCount();}
extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastError(JNIEnv*,jobject){return sampler().midiLastError();}
extern "C" JNIEXPORT jlong JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiActivitySerial(JNIEnv*,jobject){return static_cast<jlong>(sampler().midiActivitySerial());}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMidiLastMessage(JNIEnv* env,jobject){const std::array<jint,3>v{{sampler().midiLastStatus(),sampler().midiLastData1(),sampler().midiLastData2()}};jintArray a=env->NewIntArray(3);if(a!=nullptr)env->SetIntArrayRegion(a,0,3,v.data());return a;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioStarted(JNIEnv*,jobject){return sampler().started()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestInstrumentEnabled(JNIEnv*,jobject){return sampler().testInstrumentEnabled()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOn(JNIEnv*,jobject,jint note,jint velocity){return sampler().testNoteOn(static_cast<std::uint8_t>(std::clamp(note,0,127)),static_cast<std::uint8_t>(std::clamp(velocity,0,127)))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT void JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeTestNoteOff(JNIEnv*,jobject,jint note){sampler().testNoteOff(static_cast<std::uint8_t>(std::clamp(note,0,127)));}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativePanic(JNIEnv*,jobject){return sampler().panic()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMeters(JNIEnv* env,jobject){return metersArray(env,sampler().meters());}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAudioInfo(JNIEnv* env,jobject){const std::array<jint,7>v{{sampler().sampleRate(),sampler().framesPerBurst(),sampler().bufferSizeFrames(),sampler().xrunCount(),sampler().sharingMode(),sampler().performanceMode(),sampler().deviceId()}};jintArray a=env->NewIntArray(static_cast<jsize>(v.size()));if(a!=nullptr)env->SetIntArrayRegion(a,0,static_cast<jsize>(v.size()),v.data());return a;}
extern "C" JNIEXPORT jdouble JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLatencyMs(JNIEnv*,jobject){return sampler().estimatedOutputLatencyMs();}
extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLastAudioError(JNIEnv*,jobject){return sampler().lastError();}
extern "C" JNIEXPORT jstring JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLastTextError(JNIEnv* env,jobject){const auto s=sampler().textError();return env->NewStringUTF(s.c_str());}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSaveSet(JNIEnv* env,jobject,jstring path){return sampler().saveSet(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSetSetName(JNIEnv* env,jobject,jstring name){return sampler().setSetName(jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteProgram(JNIEnv* env,jobject,jint bank,jint page,jint slot,jint upper,jstring name){return sampler().writeCurrentProgram(makeAddress(bank,page,slot),upper,jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramBasic(JNIEnv*,jobject,jint bank,jint page,jint slot,jboolean mono,jboolean legato,jfloat portamentoSec){return sampler().updateProgramBasic(makeAddress(bank,page,slot),mono==JNI_TRUE,legato==JNI_TRUE,portamentoSec)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAddProgramLayer(JNIEnv* env,jobject,jint bank,jint page,jint slot,jint multisampleId,jstring name){return sampler().addProgramLayer(makeAddress(bank,page,slot),static_cast<std::uint32_t>(std::max(0,multisampleId)),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRemoveProgramLayer(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer){return sampler().removeProgramLayer(makeAddress(bank,page,slot),layer)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMoveProgramLayer(JNIEnv*,jobject,jint bank,jint page,jint slot,jint fromLayer,jint toLayer){return sampler().moveProgramLayer(makeAddress(bank,page,slot),fromLayer,toLayer)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAssignProgramMultisample(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jint multisampleId){return sampler().assignProgramMultisample(makeAddress(bank,page,slot),layer,static_cast<std::uint32_t>(std::max(0,multisampleId)))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramLayerBasic(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jboolean enabled,jint keyLow,jint keyHigh,jint velocityLow,jint velocityHigh,jint velocityFadeIn,jint velocityFadeOut,jint transpose,jfloat tuneCents,jfloat levelDb,jfloat pan,jfloat delaySec,jboolean damperEnabled){return sampler().updateProgramLayerBasic(makeAddress(bank,page,slot),layer,enabled==JNI_TRUE,keyLow,keyHigh,velocityLow,velocityHigh,velocityFadeIn,velocityFadeOut,transpose,tuneCents,levelDb,pan,delaySec,damperEnabled==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramAmpEnvelope(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jfloat attack,jfloat decay,jfloat sustain,jfloat release){return sampler().updateProgramAmpEnvelope(makeAddress(bank,page,slot),layer,attack,decay,sustain,release)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramPitch(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jfloat amountCents,jfloat attack,jfloat decay,jfloat sustain,jfloat release){return sampler().updateProgramPitch(makeAddress(bank,page,slot),layer,amountCents,attack,decay,sustain,release)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramFilter(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jint mode,jfloat cutoffHz,jfloat resonance,jfloat keyTrack,jfloat velocityAmount,jfloat envelopeAmount,jfloat lfoAmount,jfloat attack,jfloat decay,jfloat sustain,jfloat release){return sampler().updateProgramFilter(makeAddress(bank,page,slot),layer,mode,cutoffHz,resonance,keyTrack,velocityAmount,envelopeAmount,lfoAmount,attack,decay,sustain,release)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateProgramLfo(JNIEnv*,jobject,jint bank,jint page,jint slot,jint layer,jint lfoIndex,jint wave,jfloat frequencyHz,jfloat delaySec,jfloat fadeSec,jboolean keySync,jfloat pitchCents,jfloat filterOctaves,jfloat ampDepth){return sampler().updateProgramLfo(makeAddress(bank,page,slot),layer,lfoIndex,wave,frequencyHz,delaySec,fadeSec,keySync==JNI_TRUE,pitchCents,filterOctaves,ampDepth)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteCombi(JNIEnv* env,jobject,jint bank,jint page,jint slot,jstring name){return sampler().writeCurrentCombi(makeAddress(bank,page,slot),jstringToUtf8(env,name))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperMain(JNIEnv*,jobject,jint upper,jboolean enabled,jboolean solo,jboolean mute,jfloat levelDb,jfloat pan,jint keyLow,jint keyHigh,jint voiceMode){return sampler().updateUpperMain(upper,enabled==JNI_TRUE,solo==JNI_TRUE,mute==JNI_TRUE,levelDb,pan,keyLow,keyHigh,voiceMode)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperTune(JNIEnv*,jobject,jint upper,jint octave,jint transpose,jfloat fineCents,jint pitchBendRange,jfloat portamentoSec){return sampler().updateUpperTune(upper,octave,transpose,fineCents,pitchBendRange,portamentoSec)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperRange(JNIEnv*,jobject,jint upper,jint velocityLow,jint velocityHigh){return sampler().updateUpperRange(upper,velocityLow,velocityHigh)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperControl(JNIEnv*,jobject,jint upper,jboolean breath,jboolean expression,jboolean aftertouch,jboolean damper,jint midiChannel){return sampler().updateUpperControl(upper,breath==JNI_TRUE,expression==JNI_TRUE,aftertouch==JNI_TRUE,damper==JNI_TRUE,midiChannel)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperTone(JNIEnv*,jobject,jint upper,jfloat attack,jfloat decay,jfloat sustain,jfloat release,jfloat cutoff,jfloat resonance,jfloat lfoRate,jfloat lfoDepth,jfloat eqLow,jfloat eqMid,jfloat eqHigh){return sampler().updateUpperTone(upper,attack,decay,sustain,release,cutoff,resonance,lfoRate,lfoDepth,eqLow,eqMid,eqHigh)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSetUpperToneParam(JNIEnv*,jobject,jint upper,jint param,jfloat value){return sampler().setUpperToneParam(upper,param,value)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpperState(JNIEnv* env,jobject,jint upper){return floatVectorArray(env,sampler().upperState(upper));}
extern "C" JNIEXPORT jstring JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpperSoundName(JNIEnv* env,jobject,jint upper){const auto s=sampler().upperSoundName(upper);return env->NewStringUTF(s.c_str());}
extern "C" JNIEXPORT jstring JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiName(JNIEnv* env,jobject){const auto s=sampler().combiName();return env->NewStringUTF(s.c_str());}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiCatalogSlots(JNIEnv* env,jobject){return intVectorArray(env,sampler().combiCatalogSlots());}
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiCatalogNames(JNIEnv* env,jobject){return stringVectorArray(env,sampler().combiCatalogNames());}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSoundCatalogAddresses(JNIEnv* env,jobject){return intVectorArray(env,sampler().soundCatalogAddresses());}
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSoundCatalogNames(JNIEnv* env,jobject){return stringVectorArray(env,sampler().soundCatalogNames());}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperFxSends(JNIEnv*,jobject,jint upper,jfloat send1,jfloat send2){return sampler().updateUpperFxSends(upper,send1,send2)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateUpperControllerRoute(JNIEnv*,jobject,jint upper,jint route,jint source,jint destination,jint cc,jfloat amount,jboolean bipolar){return sampler().updateUpperControllerRoute(upper,route,source,destination,cc,amount,bipolar==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateCombiFx(JNIEnv*,jobject,jint slot,jint type,jfloat wet,jfloat timeMs,jfloat feedback,jfloat rateHz,jfloat depth,jfloat tone){return sampler().updateCombiFx(slot,type,wet,timeMs,feedback,rateHz,depth,tone)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateCombiFxRouting(JNIEnv*,jobject,jint routing){return sampler().updateCombiFxRouting(routing)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiFxState(JNIEnv* env,jobject){return floatVectorArray(env,sampler().combiFxState());}
extern "C" JNIEXPORT jfloatArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeGlobalState(JNIEnv* env,jobject){return floatVectorArray(env,sampler().globalState());}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteGlobal(JNIEnv*,jobject){return sampler().writeGlobal()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateGlobal(JNIEnv*,jobject,jfloat tune,jint transpose,jfloat gain,jfloat low,jfloat mid,jfloat high,jboolean limiter,jfloat ceiling,jint velocityCurve,jint breathCc,jint expressionCc,jint damperCc,jboolean pitchBend,jboolean aftertouch){return sampler().updateGlobal(tune,transpose,gain,low,mid,high,limiter==JNI_TRUE,ceiling,velocityCurve,breathCc,expressionCc,damperCc,pitchBend==JNI_TRUE,aftertouch==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeUpdateGlobalAdvanced(JNIEnv* env,jobject,jint scaleRoot,jfloatArray scaleOffsets,jint midiChannelMask,jboolean filterNotes,jboolean filterCc,jboolean filterPitchBend,jboolean filterAftertouch,jfloat breathMin,jfloat breathMax,jfloat breathCurve,jfloat expressionMin,jfloat expressionMax,jfloat expressionCurve,jboolean damperReverse){
    if(scaleOffsets==nullptr||env->GetArrayLength(scaleOffsets)<12)return JNI_FALSE;
    std::array<jfloat,12> raw{};env->GetFloatArrayRegion(scaleOffsets,0,12,raw.data());
    std::array<float,12> offsets{};for(std::size_t i=0;i<offsets.size();++i)offsets[i]=raw[i];
    return sampler().updateGlobalAdvanced(scaleRoot,offsets,static_cast<std::uint16_t>(midiChannelMask&0xFFFF),filterNotes==JNI_TRUE,filterCc==JNI_TRUE,filterPitchBend==JNI_TRUE,filterAftertouch==JNI_TRUE,breathMin,breathMax,breathCurve,expressionMin,expressionMax,expressionCurve,damperReverse==JNI_TRUE)?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeWriteList(JNIEnv* env,jobject,jint bank,jint page,jint slot,jstring listName,jintArray numbers,jobjectArray names,jintArray combiAddresses){
    if(numbers==nullptr||names==nullptr||combiAddresses==nullptr){return JNI_FALSE;}
    const jsize count=env->GetArrayLength(numbers);
    if(env->GetArrayLength(names)!=count||env->GetArrayLength(combiAddresses)!=count*3){return JNI_FALSE;}
    std::vector<jint> nums(static_cast<std::size_t>(count));
    std::vector<jint> addr(static_cast<std::size_t>(count)*3U);
    env->GetIntArrayRegion(numbers,0,count,nums.data());
    env->GetIntArrayRegion(combiAddresses,0,count*3,addr.data());
    SetList list; list.name=jstringToUtf8(env,listName); list.entries.reserve(static_cast<std::size_t>(count));
    for(jsize i=0;i<count;++i){
        auto js=static_cast<jstring>(env->GetObjectArrayElement(names,i));
        ListEntry e; e.number=static_cast<std::uint16_t>(std::clamp(nums[static_cast<std::size_t>(i)],0,65535)); e.name=jstringToUtf8(env,js);
        if(js!=nullptr)env->DeleteLocalRef(js);
        const std::size_t base=static_cast<std::size_t>(i)*3U;
        e.combiBank=static_cast<std::uint16_t>(std::clamp(addr[base],0,65535));
        e.combiPage=static_cast<std::uint16_t>(std::clamp(addr[base+1U],0,65535));
        e.combiSlot=static_cast<std::uint16_t>(std::clamp(addr[base+2U],0,65535));
        list.entries.push_back(std::move(e));
    }
    return sampler().writeList(makeAddress(bank,page,slot),list)?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSetListEntry(JNIEnv* env,jobject,jint lb,jint lp,jint ls,jint index,jint number,jstring name,jint cb,jint cp,jint cs){return sampler().setListEntry(makeAddress(lb,lp,ls),index,number,jstringToUtf8(env,name),makeAddress(cb,cp,cs))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeInsertListEntry(JNIEnv* env,jobject,jint lb,jint lp,jint ls,jint index,jint number,jstring name,jint cb,jint cp,jint cs){return sampler().insertListEntry(makeAddress(lb,lp,ls),index,number,jstringToUtf8(env,name),makeAddress(cb,cp,cs))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeRemoveListEntry(JNIEnv*,jobject,jint lb,jint lp,jint ls,jint index){return sampler().removeListEntry(makeAddress(lb,lp,ls),index)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMoveListEntry(JNIEnv*,jobject,jint lb,jint lp,jint ls,jint fromIndex,jint toIndex){return sampler().moveListEntry(makeAddress(lb,lp,ls),fromIndex,toIndex)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeMoveList(JNIEnv*,jobject,jint fb,jint fp,jint fs,jint tb,jint tp,jint ts,jboolean overwrite){return sampler().moveList(makeAddress(fb,fp,fs),makeAddress(tb,tp,ts),overwrite==JNI_TRUE)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeActivateListEntry(JNIEnv*,jobject,jint lb,jint lp,jint ls,jint index){return sampler().activateListEntry(makeAddress(lb,lp,ls),index)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jint JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeActiveListEntry(JNIEnv*,jobject,jint lb,jint lp,jint ls){return static_cast<jint>(sampler().activeListEntry(makeAddress(lb,lp,ls)));}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeListEntryNumbers(JNIEnv* env,jobject,jint lb,jint lp,jint ls){return intVectorArray(env,sampler().listEntryNumbers(makeAddress(lb,lp,ls)));}
extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeListEntryNames(JNIEnv* env,jobject,jint lb,jint lp,jint ls){
    const auto names=sampler().listEntryNames(makeAddress(lb,lp,ls)); jclass stringClass=env->FindClass("java/lang/String"); if(stringClass==nullptr)return nullptr; jobjectArray out=env->NewObjectArray(static_cast<jsize>(names.size()),stringClass,nullptr); if(out==nullptr)return nullptr; for(std::size_t i=0;i<names.size();++i){jstring v=env->NewStringUTF(names[i].c_str());env->SetObjectArrayElement(out,static_cast<jsize>(i),v);env->DeleteLocalRef(v);} return out;
}
extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeListEntryCombiAddresses(JNIEnv* env,jobject,jint lb,jint lp,jint ls){return intVectorArray(env,sampler().listEntryCombiAddresses(makeAddress(lb,lp,ls)));}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeActivateProgram(JNIEnv*,jobject,jint bank,jint page,jint slot,jint upper){return sampler().activateProgram(makeAddress(bank,page,slot),upper)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeAssignUpperSound(JNIEnv*,jobject,jint bank,jint page,jint slot,jint upper){return sampler().activateProgram(makeAddress(bank,page,slot),upper)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearUpperSound(JNIEnv*,jobject,jint upper){return sampler().clearUpperSound(upper)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeActivateCombi(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().activateCombi(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearProgram(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().clearProgram(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearCombi(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().clearCombi(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeClearList(JNIEnv*,jobject,jint bank,jint page,jint slot){return sampler().clearList(makeAddress(bank,page,slot))?JNI_TRUE:JNI_FALSE;}

#endif
