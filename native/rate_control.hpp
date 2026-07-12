#pragma once

#include <vector>

#include <ares/ares.hpp>

// Dynamic rate control — port of ares' reference implementation,
// ruby/audio/audio.cpp Audio::output() at v132 (the SDL3 audio rewrite
// dropped it; in v140+ the settings toggle sets a flag nothing reads).
//
// Emulation is wall-clock paced while the device DAC drains the ring at its
// own 48 kHz; the clocks drift, so a fixed resampler ratio slowly walks the
// buffer toward overflow or underrun. Upstream corrects this where the final
// resampler feeds the device buffer: skew the claimed input frequency by the
// buffer fill so production tracks the DAC clock. Our final stage is the
// stream resamplers feeding our ring, so the skew lands there.
//
// Upstream calls Stream-level setResamplerFrequency() nowhere for this —
// it fully resets the resampler (drops history + queued samples). Like
// upstream, we call Cubic::setInputFrequency(), a ratio-only recompute; it
// sits behind the protected Stream::_channels, hence the derived-class
// access idiom.
namespace RateControl {

struct StreamChannels : ares::Core::Audio::Stream {
    auto setInputFrequency(f64 dynamicFrequency) -> void {
        for (auto& channel : _channels) {
            channel.resampler.setInputFrequency(dynamicFrequency);
        }
    }
};

// fillLevel is ring occupancy in [0, 1]. Formula and constant are upstream's
// verbatim: maxDelta 0.005 (±0.5%, < 9 cents of pitch), linear in fill,
// unity at half full — empty ring claims a slower input (produce more),
// full ring a faster one (produce less).
inline auto apply(std::vector<ares::Node::Audio::Stream>& streams,
                  f64 fillLevel) -> void {
    f64 maxDelta = 0.005;
    for (auto& stream : streams) {
        f64 dynamicFrequency =
            ((1.0 - maxDelta) + 2.0 * fillLevel * maxDelta) * stream->frequency();
        static_cast<StreamChannels&>(*stream).setInputFrequency(dynamicFrequency);
    }
}

} // namespace RateControl
