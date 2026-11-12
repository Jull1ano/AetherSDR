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
#include <limits>
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
// two nearest. Output points outside the source take the frame's median --
// its noise floor, near enough: nothing is known there until the analyzer has
// been re-aimed, and the floor is the least surprising thing to draw.
// `outside`, when given, is the value for output points outside the source
// instead of the frame's lowest (a NaN there lets the caller fill them in).
inline void resamplePanWindow(const std::vector<float>& src, double srcLow, double srcHigh,
                              double dstLow, double dstHigh, std::size_t count,
                              std::vector<float>& out, const float* outside = nullptr)
{
    out.resize(count);
    const std::size_t m = src.size();
    if (m < 2 || count < 2 || !(srcHigh > srcLow) || !(dstHigh > dstLow)) {
        std::fill(out.begin(), out.end(), m > 0 ? src.front() : -160.0f);
        return;
    }
    // Worked out only if an output point really falls outside the source.
    float floorDb = 0.0f;
    bool haveFloor = false;
    const auto fill = [&]() {
        if (!haveFloor) {
            if (outside) {
                floorDb = *outside;
            } else {
                std::vector<float> sorted(src);
                const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
                std::nth_element(sorted.begin(), mid, sorted.end());
                floorDb = *mid;
            }
            haveFloor = true;
        }
        return floorDb;
    };
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
            out[i] = fill();
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
                out[i] = mean > 0.0 ? static_cast<float>(10.0 * std::log10(mean)) : fill();
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

// ---- the trace's movement in time ----
//
// The share of a new frame in a recursive average with time constant `tau`,
// for frames `dt` apart (both in seconds). No time constant means no
// averaging: the new frame is taken whole.
inline float smoothingAlpha(double dt, double tau) noexcept
{
    if (!(tau > 0.0) || !(dt > 0.0))
        return 1.0f;
    return static_cast<float>(1.0 - std::exp(-dt / tau));
}

// One step of a recursive time average of `frame` into `state` (same size).
// logDomain averages the dB values, which is WDSP's "log recursive" and the
// calm look; otherwise the POWER is averaged ("recursive"), which reads noise
// and weak signals at their true level.
inline void averageTowards(std::vector<float>& state, const std::vector<float>& frame,
                           float alpha, bool logDomain)
{
    const std::size_t n = std::min(state.size(), frame.size());
    if (alpha >= 1.0f) {
        std::copy(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(n), state.begin());
        return;
    }
    if (logDomain) {
        for (std::size_t i = 0; i < n; ++i)
            state[i] += alpha * (frame[i] - state[i]);
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const double power = (1.0 - alpha) * std::pow(10.0, static_cast<double>(state[i]) / 10.0)
                             + alpha * std::pow(10.0, static_cast<double>(frame[i]) / 10.0);
        state[i] = power > 0.0 ? static_cast<float>(10.0 * std::log10(power)) : frame[i];
    }
}

// Attack and decay: `trace` follows `target` (same size) with one share for a
// point that has to rise and another for one that has to fall. A quick attack
// with a slower decay lets a signal come up at once and sink back gently.
inline void followWithBallistics(std::vector<float>& trace, const std::vector<float>& target,
                                 float attackAlpha, float decayAlpha) noexcept
{
    const std::size_t n = std::min(trace.size(), target.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float d = target[i] - trace[i];
        trace[i] += (d > 0.0f ? attackAlpha : decayAlpha) * d;
    }
}

// brick3-angelia: Flatten -- the noise floor's "grass" drawn shorter, with
// nothing smoothed across frequency.
//
// How tall the grass is, is physics: every point is a power estimate over the
// time and the bandwidth behind it, and only more of either -- a slower trace
// or a coarser one -- makes it truly steadier. This does neither. It finds the
// floor under the trace (the lower quartile over a few blocks of the width,
// so a signal does not lift it, raised by the noise's own spread to where the
// noise's middle sits) and draws every point within a few noise widths of
// that floor closer to it by `amount`, point by point. A point keeps its
// frequency, so a carrier is exactly as narrow as before, and the mapping is
// monotonic, so no peak appears or disappears. From about 7.5 noise widths
// away it is the identity: a signal reads its true level. A weak signal, a
// few widths up, is drawn lower along with the noise around it.
//
// Drawing only: the S-meter, the audio and the droop sweep never see it.
struct FlattenState {
    float sigmaDb = 0.0f;              // the noise's spread, followed over frames
    std::vector<float> scratch;
    std::vector<float> blockFloor;
    std::vector<float> blockSpread;
};

inline float flattenSmoothStep(float t) noexcept
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// `amount` 0..0.95: the share of the grass taken away (0.5 halves it).
inline void flattenNoiseFloor(std::vector<float>& points, float amount, FlattenState& state)
{
    const std::size_t n = points.size();
    amount = std::clamp(amount, 0.0f, 0.95f);
    if (n < 32 || !(amount > 0.0f))
        return;

    // Blocks of about 1/48 of the width; each block's floor is read over it
    // and two blocks either side, wide enough that a broadcast station fills
    // no more than half of it on a zoomed-out view.
    constexpr std::size_t kBlocks = 48;
    constexpr float kQuartileToSigma = 0.6745f;   // Gaussian: median - q25
    const std::size_t block = std::max<std::size_t>(4, n / kBlocks);
    const std::size_t nb = (n + block - 1) / block;
    state.blockFloor.assign(nb, std::numeric_limits<float>::quiet_NaN());
    state.blockSpread.assign(nb, std::numeric_limits<float>::quiet_NaN());
    for (std::size_t k = 0; k < nb; ++k) {
        const std::size_t lo = k >= 2 ? (k - 2) * block : 0;
        const std::size_t hi = std::min(n, (k + 3) * block);
        state.scratch.clear();
        for (std::size_t i = lo; i < hi; ++i) {
            if (std::isfinite(points[i]))
                state.scratch.push_back(points[i]);
        }
        const std::size_t m = state.scratch.size();
        if (m < 8)
            continue;
        const auto first = state.scratch.begin();
        const std::size_t i50 = m / 2;
        const std::size_t i25 = m / 4;
        std::nth_element(first, first + static_cast<std::ptrdiff_t>(i50), state.scratch.end());
        const float q50 = state.scratch[i50];
        std::nth_element(first, first + static_cast<std::ptrdiff_t>(i25),
                         first + static_cast<std::ptrdiff_t>(i50));
        const float q25 = state.scratch[i25];
        state.blockFloor[k] = q25;
        state.blockSpread[k] = (q50 - q25) / kQuartileToSigma;
    }

    // The noise's spread: the median over the blocks, so the blocks a signal
    // fills (whose spread is the signal's) do not count. Followed over frames
    // so the strength of the effect does not flicker.
    state.scratch.clear();
    for (const float v : state.blockSpread) {
        if (std::isfinite(v))
            state.scratch.push_back(v);
    }
    if (state.scratch.empty())
        return;
    const std::size_t mid = state.scratch.size() / 2;
    std::nth_element(state.scratch.begin(),
                     state.scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                     state.scratch.end());
    const float frameSigma = std::clamp(state.scratch[mid], 0.05f, 20.0f);
    state.sigmaDb = state.sigmaDb > 0.0f
        ? state.sigmaDb + 0.1f * (frameSigma - state.sigmaDb)
        : frameSigma;
    const float sigma = state.sigmaDb;

    // A block with too few readable points borrows its neighbour's floor.
    float last = std::numeric_limits<float>::quiet_NaN();
    for (float& f : state.blockFloor) {
        if (std::isfinite(f))
            last = f;
        else
            f = last;
    }
    last = std::numeric_limits<float>::quiet_NaN();
    for (auto it = state.blockFloor.rbegin(); it != state.blockFloor.rend(); ++it) {
        if (std::isfinite(*it))
            last = *it;
        else
            *it = last;
    }
    for (float& f : state.blockFloor)
        f += kQuartileToSigma * sigma;   // lower quartile -> the noise's middle

    const float keep = 1.0f - amount;
    const float knee = std::max(0.5f, 2.5f * sigma);         // fully flattened inside
    const float full = std::max(knee + 1.0f, 3.0f * knee);   // untouched beyond
    const float blockF = static_cast<float>(block);
    for (std::size_t i = 0; i < n; ++i) {
        // The floor here, between the two nearest block centres.
        const float pos = (static_cast<float>(i) + 0.5f) / blockF - 0.5f;
        std::size_t k0 = 0;
        float t = 0.0f;
        if (pos > 0.0f) {
            k0 = std::min(nb - 1, static_cast<std::size_t>(pos));
            t = std::clamp(pos - static_cast<float>(k0), 0.0f, 1.0f);
        }
        const std::size_t k1 = std::min(nb - 1, k0 + 1);
        const float floorDb = state.blockFloor[k0] + t * (state.blockFloor[k1] - state.blockFloor[k0]);
        const float d = points[i] - floorDb;
        if (!std::isfinite(d))
            continue;
        const float ad = std::abs(d);
        if (ad >= full)
            continue;
        const float share = keep + (1.0f - keep) * flattenSmoothStep((ad - knee) / (full - knee));
        points[i] = floorDb + d * share;
    }
}

}  // namespace AetherSDR::anan
