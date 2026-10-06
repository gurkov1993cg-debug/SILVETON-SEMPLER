#pragma once

#include "silveton/sampler/SamplerTypes.h"
#include <cstddef>

namespace silveton::sampler {

enum class InterpolationQuality : int { Studio = 1, LiveCubic4 = 4, Fast8 = 8, High16 = 16, Ultra32 = 32 };

// SILVETON_STUDIO_INTERP_V1
// Studio is the live default: a Kaiser-windowed sinc whose kernel is chosen
// from the playback ratio (12 kernels per octave, 16..64 points). At the
// 96 kHz engine rate it keeps 0..20 kHz flat (within 0.001 dB) and rejects
// every image/alias that could land in 0..20 kHz by 100 dB or more, for any
// transposition from the pitched-down range up to about +3 octaves.
// LiveCubic4 is the old 4-point cubic. Fast8/High16/Ultra32 use a
// Blackman-windowed sinc. All tables are built once on the control thread
// (no sin/cos in the audio thread).
//
// Loop rule: when a sample loops, only frames at or after the loop END wrap
// back into the loop. Frames before the loop START are the attack and are
// played as recorded.
class SincInterpolator {
public:
    explicit SincInterpolator(InterpolationQuality quality = InterpolationQuality::LiveCubic4) noexcept;

    void setQuality(InterpolationQuality quality) noexcept { quality_ = quality; }
    [[nodiscard]] InterpolationQuality quality() const noexcept { return quality_; }

    [[nodiscard]] float read(const SampleData& sample,
                             double position,
                             std::uint16_t channel,
                             double playbackRatio,
                             bool looping) const noexcept;

    // Left and right in one pass (mono samples return the same value twice).
    void readStereo(const SampleData& sample,
                    double position,
                    double playbackRatio,
                    bool looping,
                    float& left,
                    float& right) const noexcept;

private:
    InterpolationQuality quality_;
};

} // namespace silveton::sampler
