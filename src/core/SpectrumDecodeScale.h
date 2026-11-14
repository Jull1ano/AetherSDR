#pragma once

#include "DbmRangePlausibility.h"
#include <QMetaType>
#include <QtGlobal>

namespace AetherSDR {

// The scale used to decode THIS observation, captured with the samples.
// An empty scale denotes absolute-level samples with no pixel-range encoding.
struct SpectrumDecodeScale {
    float minDbm{0};
    float maxDbm{0};
    quint64 generation{0};
    // brick3-angelia: the RF span the frame's bins really cover, when the
    // producer knows it (SpectrumCoverage::frameLowMhz/HighMhz); 0 = the
    // pan's current view. SpectrumWidget reprojects a frame computed for an
    // earlier view onto the one on screen. Not part of valid().
    double frameLowMhz{0};
    double frameHighMhz{0};

    bool valid() const
    {
        return generation != 0 && dbmRangeLooksPlausible(minDbm, maxDbm);
    }
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::SpectrumDecodeScale)
