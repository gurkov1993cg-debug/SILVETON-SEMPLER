from pathlib import Path
import sys
root = Path(sys.argv[1])
eng=root/'core/src/sampler/SamplerEngine.cpp'
h=root/'core/include/silveton/sampler/SincInterpolator.h'
cpp=root/'core/src/sampler/SincInterpolator.cpp'

# cubic live interpolation
s=h.read_text()
s=s.replace('enum class InterpolationQuality : int { Fast8 = 8, High16 = 16, Ultra32 = 32 };','enum class InterpolationQuality : int { LiveCubic4 = 4, Fast8 = 8, High16 = 16, Ultra32 = 32 };')
s=s.replace('explicit SincInterpolator(InterpolationQuality quality = InterpolationQuality::High16) noexcept;','explicit SincInterpolator(InterpolationQuality quality = InterpolationQuality::LiveCubic4) noexcept;')
h.write_text(s)
s=cpp.read_text()
needle='''    if (!sample.valid()) return 0.0F;\n\n    const int taps = static_cast<int>(quality_);\n'''
repl='''    if (!sample.valid()) return 0.0F;\n\n    if (quality_ == InterpolationQuality::LiveCubic4) {\n        const long long i1 = static_cast<long long>(std::floor(position));\n        const float t = static_cast<float>(position - static_cast<double>(i1));\n        const float y0 = readFrame(sample, i1 - 1LL, channel, looping);\n        const float y1 = readFrame(sample, i1, channel, looping);\n        const float y2 = readFrame(sample, i1 + 1LL, channel, looping);\n        const float y3 = readFrame(sample, i1 + 2LL, channel, looping);\n        const float a0 = -0.5F*y0 + 1.5F*y1 - 1.5F*y2 + 0.5F*y3;\n        const float a1 = y0 - 2.5F*y1 + 2.0F*y2 - 0.5F*y3;\n        const float a2 = -0.5F*y0 + 0.5F*y2;\n        return ((a0*t + a1)*t + a2)*t + y1;\n    }\n\n    const int taps = static_cast<int>(quality_);\n'''
if needle not in s: raise SystemExit('sinc needle')
s=s.replace(needle,repl)
cpp.write_text(s)

s=eng.read_text()
s=s.replace('interpolator_(InterpolationQuality::High16)','interpolator_(InterpolationQuality::LiveCubic4)')
# add masks
s=s.replace('''    std::array<float, kUpperCount> upperPeakR{};\n    Biquad lowEq{}, midEq{}, highEq{};\n''','''    std::array<float, kUpperCount> upperPeakR{};\n    std::array<std::uint16_t, kUpperCount> upperRouteMask{};\n    Biquad lowEq{}, midEq{}, highEq{};\n''')
s=s.replace('''        upperPeakL.fill(0.0F);\n        upperPeakR.fill(0.0F);\n''','''        upperPeakL.fill(0.0F);\n        upperPeakR.fill(0.0F);\n        upperRouteMask.fill(0U);\n''')
# set masks in setState loop
old='''    for (int i = 0; i < kUpperCount; ++i) {\n        const auto& u = state_->combi.uppers[static_cast<std::size_t>(i)];\n        impl_->upperLowEq[static_cast<std::size_t>(i)].setShelf(impl_->sampleRate, 120.0F, u.eqLowDb, false);\n'''
new='''    for (int i = 0; i < kUpperCount; ++i) {\n        const auto index = static_cast<std::size_t>(i);\n        const auto& u = state_->combi.uppers[index];\n        std::uint16_t routeMask = 0U;\n        for (const auto& route : u.controllerRoutes) {\n            if (route.source == ControllerSource::Off || std::abs(route.amount) <= 1.0e-9F) continue;\n            routeMask = static_cast<std::uint16_t>(routeMask | static_cast<std::uint16_t>(1U << static_cast<unsigned>(route.destination)));\n        }\n        impl_->upperRouteMask[index] = routeMask;\n        impl_->upperLowEq[index].setShelf(impl_->sampleRate, 120.0F, u.eqLowDb, false);\n'''
if old not in s: raise SystemExit('setState mask anchor')
s=s.replace(old,new)
s=s.replace('''        impl_->upperMidEq[static_cast<std::size_t>(i)].setPeaking(impl_->sampleRate, 1200.0F, 0.75F, u.eqMidDb);\n        impl_->upperHighEq[static_cast<std::size_t>(i)].setShelf(impl_->sampleRate, 7000.0F, u.eqHighDb, true);\n''','''        impl_->upperMidEq[index].setPeaking(impl_->sampleRate, 1200.0F, 0.75F, u.eqMidDb);\n        impl_->upperHighEq[index].setShelf(impl_->sampleRate, 7000.0F, u.eqHighDb, true);\n''')

start=s.index('''            const auto& ch = impl_->channels[v.channel & 0x0FU];\n''')
end=s.index('''            if (v.zone->reverse) v.position -= ratio; else v.position += ratio;\n''', start)
oldblock=s[start:end]
newblock=r'''            const auto& ch = impl_->channels[v.channel & 0x0FU];
            const auto ui = static_cast<std::size_t>(std::clamp(v.upperIndex, 0, kUpperCount - 1));
            const std::uint16_t routeMask = impl_->upperRouteMask[ui];
            const auto routeActive = [routeMask](ModDestination d) noexcept {
                return (routeMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(d))) != 0U;
            };

            const bool filterActive = v.layer->filter.mode != FilterMode::Bypass;
            const bool lfoDepthRouted = routeActive(ModDestination::LfoDepth);
            const bool lfo1Needed = lfoDepthRouted || v.layer->lfo1.pitchCents != 0.0F ||
                v.layer->lfo1.filterOctaves != 0.0F || v.layer->lfo1.ampDepth != 0.0F ||
                (filterActive && v.layer->filter.lfoAmount != 0.0F);
            const bool lfo2Needed = lfoDepthRouted || v.layer->lfo2.pitchCents != 0.0F ||
                v.layer->lfo2.filterOctaves != 0.0F || v.layer->lfo2.ampDepth != 0.0F;

            LfoParams lfo1Params = v.layer->lfo1;
            LfoParams lfo2Params = v.layer->lfo2;
            float lfo1 = 0.0F, lfo2 = 0.0F;
            if (lfo1Needed || lfo2Needed) {
                const float lfoRateScale = std::clamp(v.upper->lfoRateScale, 0.0F, 16.0F);
                const float controllerLfoDepth = lfoDepthRouted
                    ? controllerAmount(*v.upper, ch, ModDestination::LfoDepth) : 0.0F;
                const float lfoDepthScale = std::clamp(
                    v.upper->lfoDepthScale * std::max(0.0F, 1.0F + controllerLfoDepth), 0.0F, 8.0F);
                if (lfo1Needed) {
                    lfo1Params.frequencyHz *= lfoRateScale;
                    lfo1Params.pitchCents *= lfoDepthScale;
                    lfo1Params.filterOctaves *= lfoDepthScale;
                    lfo1Params.ampDepth *= lfoDepthScale;
                    lfo1 = v.lfo1.process(lfo1Params, impl_->sampleRate);
                }
                if (lfo2Needed) {
                    lfo2Params.frequencyHz *= lfoRateScale;
                    lfo2Params.pitchCents *= lfoDepthScale;
                    lfo2Params.filterOctaves *= lfoDepthScale;
                    lfo2Params.ampDepth *= lfoDepthScale;
                    lfo2 = v.lfo2.process(lfo2Params, impl_->sampleRate);
                }
            }

            const bool pitchEnvNeeded = v.layer->pitchEnvelopeCents != 0.0F;
            const bool filterEnvNeeded = filterActive && v.layer->filter.envelopeAmount != 0.0F;
            const float pitchEnv = pitchEnvNeeded ? v.pitchEnv.process(impl_->sampleRate) : 0.0F;
            const float ampEnv = v.ampEnv.process(impl_->sampleRate);
            const float filterEnv = filterEnvNeeded ? v.filterEnv.process(impl_->sampleRate) : 0.0F;
            if (!v.ampEnv.active()) { v.active = false; continue; }

            if (v.glideFramesRemaining > 0U) {
                v.ratio *= std::exp(v.glideLogStep);
                --v.glideFramesRemaining;
                if (v.glideFramesRemaining == 0U) v.ratio = v.targetRatio;
            }

            double ratio = v.ratio;
            const bool dynamicPitchActive = ch.pitchBend != 0 || routeActive(ModDestination::PitchSemitones) ||
                pitchEnvNeeded || (lfo1Needed && lfo1Params.pitchCents != 0.0F) ||
                (lfo2Needed && lfo2Params.pitchCents != 0.0F);
            if (dynamicPitchActive) {
                const float bendNorm = static_cast<float>(ch.pitchBend) / 8192.0F;
                const float bendSemis = bendNorm * static_cast<float>(v.upper->pitchBendRange);
                const float controllerPitchSemis = routeActive(ModDestination::PitchSemitones)
                    ? controllerAmount(*v.upper, ch, ModDestination::PitchSemitones) : 0.0F;
                const float lfoPitchCents = lfo1 * lfo1Params.pitchCents + lfo2 * lfo2Params.pitchCents;
                const float envPitchCents = pitchEnv * v.layer->pitchEnvelopeCents;
                const double dynamicPitch = static_cast<double>(bendSemis + controllerPitchSemis) +
                    static_cast<double>(lfoPitchCents + envPitchCents) / 100.0;
                ratio *= std::pow(2.0, dynamicPitch / 12.0);
            }

            float sL = sampleWithLoopCrossfade(interpolator_, sample, v.position, 0U, ratio, looping);
            float sR = sample.channels > 1U
                ? sampleWithLoopCrossfade(interpolator_, sample, v.position, 1U, ratio, looping)
                : sL;

            if (filterActive) {
                const float controllerCutoff = routeActive(ModDestination::FilterCutoffOctaves)
                    ? controllerAmount(*v.upper, ch, ModDestination::FilterCutoffOctaves) : 0.0F;
                float cutoff = v.layer->filter.cutoffHz * std::pow(
                    2.0F, std::clamp(v.upper->filterCutoffOctaves + controllerCutoff, -10.0F, 10.0F));
                const float keyDelta = (static_cast<float>(v.key) - 60.0F) / 12.0F;
                const float velocityNorm = static_cast<float>(v.velocity) * kInv127;
                const float filterOct = keyDelta * v.layer->filter.keyTrack +
                                        velocityNorm * v.layer->filter.velocityAmount +
                                        filterEnv * v.layer->filter.envelopeAmount +
                                        lfo1 * v.layer->filter.lfoAmount +
                                        lfo1 * lfo1Params.filterOctaves +
                                        lfo2 * lfo2Params.filterOctaves;
                if (filterOct != 0.0F) cutoff *= std::pow(2.0F, filterOct);
                const float controllerResonance = routeActive(ModDestination::FilterResonance)
                    ? controllerAmount(*v.upper, ch, ModDestination::FilterResonance) : 0.0F;
                const float resonance = std::clamp(
                    v.layer->filter.resonance + v.upper->filterResonanceOffset + controllerResonance, 0.0F, 1.0F);
                v.filter.process(sL, sR, v.layer->filter.mode, cutoff, resonance, impl_->sampleRate);
            }

            float modGain = ampEnv;
            if ((lfo1Needed && lfo1Params.ampDepth != 0.0F) || (lfo2Needed && lfo2Params.ampDepth != 0.0F)) {
                modGain *= std::max(0.0F, 1.0F + lfo1 * lfo1Params.ampDepth + lfo2 * lfo2Params.ampDepth);
            }
            if (v.upper->expressionEnabled) modGain *= ch.expression;
            if (v.upper->breathEnabled) modGain *= ch.breath;
            if (v.upper->aftertouchEnabled) modGain *= (0.5F + 0.5F * ch.pressure);

            const float controllerLevelDb = routeActive(ModDestination::LevelDb)
                ? controllerAmount(*v.upper, ch, ModDestination::LevelDb) : 0.0F;
            float panLeft = v.leftGain;
            float panRight = v.rightGain;
            if (routeActive(ModDestination::Pan)) {
                const float controllerPan = controllerAmount(*v.upper, ch, ModDestination::Pan);
                const float dynamicPan = std::clamp(v.basePan + controllerPan, -1.0F, 1.0F);
                const float panAngle = (dynamicPan + 1.0F) * static_cast<float>(kPi * 0.25);
                panLeft = std::cos(panAngle);
                panRight = std::sin(panAngle);
            }
            float g = v.baseGain * modGain;
            if (controllerLevelDb != 0.0F) g *= dbToGain(std::clamp(controllerLevelDb, -96.0F, 24.0F));
            const float oL = sL * g * panLeft;
            const float oR = sR * g * panRight;
            upperL[ui] += oL;
            upperR[ui] += oR;

'''
s=s[:start]+newblock+s[end:]
# skip identity upper/global EQ and controller FX route calls
old='''            impl_->upperLowEq[index].process(l, r);\n            impl_->upperMidEq[index].process(l, r);\n            impl_->upperHighEq[index].process(l, r);\n'''
new='''            const auto& u = current->combi.uppers[index];\n            if (u.eqLowDb != 0.0F) impl_->upperLowEq[index].process(l, r);\n            if (u.eqMidDb != 0.0F) impl_->upperMidEq[index].process(l, r);\n            if (u.eqHighDb != 0.0F) impl_->upperHighEq[index].process(l, r);\n'''
if old not in s: raise SystemExit('upper eq block')
s=s.replace(old,new)
s=s.replace('''            const auto& u = current->combi.uppers[index];\n            const auto& upperChannel = impl_->channels[static_cast<std::size_t>(u.midiChannel & 0x0FU)];\n            const float s1 = std::clamp(u.fxSend1 + controllerAmount(u, upperChannel, ModDestination::FxSend1), 0.0F, 1.0F);\n            const float s2 = std::clamp(u.fxSend2 + controllerAmount(u, upperChannel, ModDestination::FxSend2), 0.0F, 1.0F);\n''','''            const auto& upperChannel = impl_->channels[static_cast<std::size_t>(u.midiChannel & 0x0FU)];\n            const std::uint16_t upperMask = impl_->upperRouteMask[index];\n            const bool routeSend1 = (upperMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(ModDestination::FxSend1))) != 0U;\n            const bool routeSend2 = (upperMask & static_cast<std::uint16_t>(1U << static_cast<unsigned>(ModDestination::FxSend2))) != 0U;\n            const float s1 = std::clamp(u.fxSend1 + (routeSend1 ? controllerAmount(u, upperChannel, ModDestination::FxSend1) : 0.0F), 0.0F, 1.0F);\n            const float s2 = std::clamp(u.fxSend2 + (routeSend2 ? controllerAmount(u, upperChannel, ModDestination::FxSend2) : 0.0F), 0.0F, 1.0F);\n''')
s=s.replace('''        impl_->lowEq.process(l, r); impl_->midEq.process(l, r); impl_->highEq.process(l, r);\n''','''        if (current->global.eqLowDb != 0.0F) impl_->lowEq.process(l, r);\n        if (current->global.eqMidDb != 0.0F) impl_->midEq.process(l, r);\n        if (current->global.eqHighDb != 0.0F) impl_->highEq.process(l, r);\n''')
eng.write_text(s)


# Source-contract checks for the live path.
checks = {
    "live cubic enum": "LiveCubic4 = 4" in h.read_text(),
    "live cubic default": "InterpolationQuality::LiveCubic4" in eng.read_text(),
    "route masks": "upperRouteMask" in eng.read_text(),
    "cached pan fast path": "float panLeft = v.leftGain;" in eng.read_text(),
    "bypass filter fast path": "if (filterActive)" in eng.read_text(),
}
for name, ok in checks.items():
    if not ok:
        raise SystemExit(f"missing live DSP optimization: {name}")
print("PASS: live DSP optimized (cubic default + modulation/filter/EQ fast paths)")
