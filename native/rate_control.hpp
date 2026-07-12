#pragma once

#include <algorithm>
#include <vector>

#include <ares/ares.hpp>

// Dynamic rate control — the classic byuu algorithm on our own AV pipeline.
//
// Emulation is paced by the wall clock while the device DAC drains the ring
// at its own 48 kHz; the two clocks drift, so a fixed resampler ratio slowly
// walks the ring buffer toward overflow (drop-oldest glitches) or underrun
// (starvation). Each tick we read the ring fill and skew every stream's
// resampler ratio toward a half-full ring instead.
//
// Upstream has nothing to port: in this snapshot Audio::setDynamic() sets a
// flag nothing consumes. And the public knob, Stream::setResamplerFrequency(),
// fully resets the resampler — dropping its history and queued samples — so
// nudging it per frame would glitch. Cubic::setInputFrequency() recomputes
// only the ratio, which is exactly what rate control needs; it sits behind
// the protected Stream::_channels, reached via the derived-class access idiom.
namespace RateControl {

struct StreamChannels : ares::Core::Audio::Stream {
    auto skewResamplerRatio(f64 skew) -> void {
        for (auto& channel : _channels) {
            channel.resampler.setInputFrequency(frequency() * skew);
        }
    }
};

// Maximum skew ±0.5% (< 9 cents of pitch — inaudible), orders of magnitude
// above real wall-clock↔DAC drift, so the fill converges from anywhere.
constexpr f64 kMaxSkew = 0.005;

// fill is current ring occupancy in [0, 1]; the target is a half-full ring
// (equal latency headroom against overflow and underrun). Claiming a faster
// input lowers output-per-input (Cubic::write steps mu by input/output per
// emitted sample), so fill above target skews production down — negative
// feedback in both directions.
inline auto apply(std::vector<ares::Node::Audio::Stream>& streams,
                  f64 fill) -> void {
    const f64 error = std::clamp((fill - 0.5) * 2.0, -1.0, 1.0);
    const f64 skew  = 1.0 + kMaxSkew * error;
    for (auto& stream : streams) {
        static_cast<StreamChannels&>(*stream).skewResamplerRatio(skew);
    }
}

} // namespace RateControl
