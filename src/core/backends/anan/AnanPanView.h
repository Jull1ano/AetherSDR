#pragma once

// brick3-angelia: the panadapter's VIEW inside the sampled band.
//
// The sample rate (the DDC's) says how much spectrum is captured; the view is
// the part of it on screen. They are separate, as in Thetis: zooming changes
// the view and leaves the rate alone. A view is a pair of fractions of the
// sampled band, 0 at its lowest frequency and 1 at its highest.
//
// The analyzer (AnanPanAnalyzer) is asked for a window somewhat wider than
// the view, so a small pan needs nothing from it, and is re-aimed only once a
// gesture has settled -- re-aiming empties its input, and the display would
// freeze for one FFT's worth of samples on every step of a drag otherwise.
// Between the two, each frame is read from the window it has onto the view
// that is wanted: resamplePanWindow(). Pure functions, so tests can pin them.

#include "core/backends/anan/AnanDroopCorrection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace AetherSDR::anan {

// applyDroopCorrectionDbResampled() for a frame that covers only [low, high]
// of the sampled band. The table spans the whole band, so point i reads it at
// the band fraction the point really sits on. Both grids have their end
// points on the edges of what they span (see droopCurveAt()).
inline void applyDroopCorrectionDbWindowed(std::vector<float>& pointsDb,
                                           const DroopCorrectionTable& table,
                                           double low, double high) noexcept
{
    const std::size_t n = pointsDb.size();
    if (n < 2 || !(high > low))
        return;
    const double last = static_cast<double>(kDroopCorrectionFftSize - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double fraction =
            low + (high - low) * static_cast<double>(i) / static_cast<double>(n - 1);
        const double x = std::clamp(fraction, 0.0, 1.0) * last;
        const auto j = std::min(static_cast<std::size_t>(x),
                                static_cast<std::size_t>(kDroopCorrectionFftSize - 2));
        const float t = static_cast<float>(x - static_cast<double>(j));
        pointsDb[i] += table[j] + t * (table[j + 1] - table[j]);
    }
}

// A frame of dB points covering [srcLow, srcHigh] of the sampled band, read
// onto `count` points covering [dstLow, dstHigh]. Where several source points
// fall on one output point their POWER is averaged (the analyzer's own
// detector, so a zoomed-out read keeps the noise floor where it was); where
// the two grids are about as fine, each output point takes its nearest source
// point; where the source is coarser, the output is interpolated between the
// two nearest. Output points outside the source take the frame's lowest
// value: nothing is known there until the analyzer has been re-aimed.
inline void resamplePanWindow(const std::vector<float>& src, double srcLow, double srcHigh,
                              double dstLow, double dstHigh, std::size_t count,
                              std::vector<float>& out)
{
    out.resize(count);
    const std::size_t m = src.size();
    if (m < 2 || count < 2 || !(srcHigh > srcLow) || !(dstHigh > dstLow)) {
        std::fill(out.begin(), out.end(), m > 0 ? src.front() : -160.0f);
        return;
    }
    const float floorDb = *std::min_element(src.begin(), src.end());
    // Band fraction per point, on each grid.
    const double srcStep = (srcHigh - srcLow) / static_cast<double>(m - 1);
    const double dstStep = (dstHigh - dstLow) / static_cast<double>(count - 1);
    // Half an output point, measured in source points.
    const double half = 0.5 * dstStep / srcStep;
    const double lastIndex = static_cast<double>(m - 1);
    for (std::size_t i = 0; i < count; ++i) {
        const double fraction = dstLow + dstStep * static_cast<double>(i);
        const double x = (fraction - srcLow) / srcStep;   // position among the source points
        if (x < -0.5 || x > lastIndex + 0.5) {
            out[i] = floorDb;
            continue;
        }
        if (half >= 0.75) {
            const double a = std::max(0.0, std::ceil(x - half));
            const double b = std::min(lastIndex, std::floor(x + half));
            if (b >= a) {
                const auto first = static_cast<std::size_t>(a);
                const auto end = static_cast<std::size_t>(b);
                double sum = 0.0;
                for (std::size_t j = first; j <= end; ++j)
                    sum += std::pow(10.0, static_cast<double>(src[j]) / 10.0);
                const double mean = sum / static_cast<double>(end - first + 1);
                out[i] = mean > 0.0 ? static_cast<float>(10.0 * std::log10(mean)) : floorDb;
                continue;
            }
        }
        const double c = std::clamp(x, 0.0, lastIndex);
        if (half >= 0.35) {
            // About one source point per output point -- the settled state,
            // where the analyzer is asked for exactly that. Take the nearest
            // as it is: interpolating between a carrier's peak and its
            // neighbour would lower the peak by an amount that changes with
            // every sub-point shift of the view.
            out[i] = src[static_cast<std::size_t>(std::lround(c))];
            continue;
        }
        const auto j = std::min(static_cast<std::size_t>(c), m - 2);
        const double t = c - static_cast<double>(j);
        out[i] = static_cast<float>(static_cast<double>(src[j])
                                    + t * (static_cast<double>(src[j + 1])
                                           - static_cast<double>(src[j])));
    }
}

}  // namespace AetherSDR::anan
