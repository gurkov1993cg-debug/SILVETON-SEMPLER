from pathlib import Path
import sys

root = Path(sys.argv[1])
cpp = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
if not cpp.is_file():
    raise SystemExit(f'missing {cpp}')

s = cpp.read_text(encoding='utf-8')

anchor = 'constexpr const char* kTag = "SilvetonSampler";\n\n'
insert = r'''constexpr const char* kTag = "SilvetonSampler";

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
                mono += osc * v.env * v.velocityGain * 0.065F;

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

'''
if 'class BuiltinTestInstrument' not in s:
    if anchor not in s:
        raise SystemExit('tag anchor not found')
    s = s.replace(anchor, insert, 1)

old_midi = '''    bool midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {\n        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);\n        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);\n        switch (kind) {\n            case 0x80U: return engine_.postNoteOff(ch, data1, data2);\n            case 0x90U: return data2 == 0U ? engine_.postNoteOff(ch, data1, 0U) : engine_.postNoteOn(ch, data1, data2);\n            case 0xB0U: return engine_.postControlChange(ch, data1, data2);\n'''
new_midi = '''    bool midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept {\n        const std::uint8_t kind = static_cast<std::uint8_t>(status & 0xF0U);\n        const std::uint8_t ch = static_cast<std::uint8_t>(status & 0x0FU);\n        if (kind == 0x80U || (kind == 0x90U && data2 == 0U)) testInstrument_.noteOff(data1);\n        else if (kind == 0x90U) testInstrument_.noteOn(data1, data2);\n        else if (kind == 0xB0U && (data1 == 120U || data1 == 123U)) testInstrument_.panic();\n        switch (kind) {\n            case 0x80U: return engine_.postNoteOff(ch, data1, data2);\n            case 0x90U: return data2 == 0U ? engine_.postNoteOff(ch, data1, 0U) : engine_.postNoteOn(ch, data1, data2);\n            case 0xB0U: return engine_.postControlChange(ch, data1, data2);\n'''
if old_midi in s:
    s = s.replace(old_midi, new_midi, 1)
elif 'testInstrument_.noteOn' not in s:
    raise SystemExit('midi block changed')

s = s.replace(
    '    bool panic() noexcept { return engine_.postPanic(); }\n',
    '    bool panic() noexcept { testInstrument_.panic(); return engine_.postPanic(); }\n'
)

render_old = '            engine_.render(renderBuffer_.data(), frames);\n\n            const std::size_t samples = static_cast<std::size_t>(frames) * kChannels;\n'
render_new = '            engine_.render(renderBuffer_.data(), frames);\n            testInstrument_.render(renderBuffer_.data(), frames, sampleRate());\n\n            const std::size_t samples = static_cast<std::size_t>(frames) * kChannels;\n'
if render_old in s:
    s = s.replace(render_old, render_new, 1)
elif 'testInstrument_.render(renderBuffer_.data(), frames, sampleRate());' not in s:
    raise SystemExit('audio render anchor changed')

state_old = '''        state_ = std::move(state);\n        engine_.setState(state_);\n        return !resume || startLocked();\n'''
state_new = '''        state_ = std::move(state);\n        bool hasRealSound = false;\n        if (state_ != nullptr) {\n            for (const auto& upper : state_->combi.uppers) {\n                if (upper.enabled && upper.program != nullptr) { hasRealSound = true; break; }\n            }\n        }\n        testInstrument_.setEnabled(!hasRealSound);\n        engine_.setState(state_);\n        return !resume || startLocked();\n'''
if state_old in s:
    s = s.replace(state_old, state_new, 1)
elif 'testInstrument_.setEnabled(!hasRealSound);' not in s:
    raise SystemExit('installState anchor changed')

members_anchor = '    SamplerEngine engine_;\n    SamplerLibrary library_{"SILVETON SET"};\n'
members_new = '    SamplerEngine engine_;\n    BuiltinTestInstrument testInstrument_{};\n    SamplerLibrary library_{"SILVETON SET"};\n'
if members_anchor in s:
    s = s.replace(members_anchor, members_new, 1)
elif 'BuiltinTestInstrument testInstrument_' not in s:
    raise SystemExit('member anchor changed')

cpp.write_text(s, encoding='utf-8')
print('PASS: built-in playable TEST EP instrument installed')
