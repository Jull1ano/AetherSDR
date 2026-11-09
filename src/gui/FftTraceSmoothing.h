#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace AetherSDR {

// The box's ceiling, ±16 logical pixels: the FFT Smooth setting's unit before
// brick3-angelia, kept for reading a stored value.
constexpr int kFftSmoothingMaxHalfWidthPx = 16;

// FFT Smooth slider: a Gaussian sigma in tenths of a logical pixel. The box
// below went from nothing (0) to a three-pixel mean (1) in one step, too
// grassy or too soft; tenths fill the range between, and a Gaussian keeps a
// peak's shape where a box flattens it. 80 = 8.0 px.
constexpr int kFftSmoothingStepsPerPx = 10;
constexpr int kFftSmoothingMaxSteps = 80;

// A stored ±h box as the sigma with its spread, sqrt(h(h+1)/3), so an old
// setting keeps about its look (1 -> 0.8 px).
[[nodiscard]] inline int fftSmoothingStepsFromHalfWidth(int halfWidthPx) noexcept
{
    if (halfWidthPx <= 0) {
        return 0;
    }
    const double sigmaPx = std::sqrt(halfWidthPx * (halfWidthPx + 1.0) / 3.0);
    return std::clamp(static_cast<int>(std::lround(sigmaPx * kFftSmoothingStepsPerPx)),
                      1, kFftSmoothingMaxSteps);
}

// FFT Smooth (Display panel): display-only smoothing across frequency, the
// panadapter counterpart of the AetherRX/AetherTX EQ analyzer's Smoothing
// setting (ClientEqCurveWidget::applyFractionalOctaveSmoothing). Same
// convention: each point becomes the mean of its neighbors in LINEAR power,
// then back to dB, so the noise does not drag the trace down the way a dB
// mean would. The window is a fixed number of points rather than a fraction
// of an octave because the panadapter's frequency axis is linear.
//
// The window is ±halfWidth points, clamped (shrinking) at the edges, so a
// halfWidth of 0 returns the input unchanged. `out` may alias `in`.
//
// Summed directly rather than through a running prefix sum: a trace spans
// more than 150 dB, which is beyond double precision, so P[hi] - P[lo] taken
// after a strong carrier cancels to garbage in the noise beside it.
// `linearScratch` is caller-owned so the per-frame render path does not
// allocate.
inline void smoothFftTraceLinearPower(std::span<const float> in, std::span<float> out,
                                      int halfWidth, std::vector<double>& linearScratch)
{
    const int n = static_cast<int>(std::min(in.size(), out.size()));
    if (n <= 0) {
        return;
    }
    if (halfWidth <= 0 || n < 2) {
        std::copy_n(in.begin(), n, out.begin());
        return;
    }

    // dB -> linear power: exp(x * ln(10) / 10) == pow(10, x / 10).
    constexpr double kLn10Over10 = 0.23025850929940457;
    linearScratch.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float db = in[static_cast<std::size_t>(i)];
        linearScratch[static_cast<std::size_t>(i)] =
            std::isfinite(db) ? std::exp(static_cast<double>(db) * kLn10Over10) : 0.0;
    }

    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - halfWidth);
        const int hi = std::min(n - 1, i + halfWidth);
        double sum = 0.0;
        for (int j = lo; j <= hi; ++j) {
            sum += linearScratch[static_cast<std::size_t>(j)];
        }
        const double mean = sum / static_cast<double>(hi - lo + 1);
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(10.0 * std::log10(std::max(mean, 1e-30)));
    }
}

// The FFT Smooth the panadapter draws with: the same linear-power average,
// weighted by a Gaussian of `sigmaPoints` (truncated at 3 sigma, renormalised
// at the edges), so the width is continuous. sigma <= 0 returns the input
// unchanged. `out` may alias `in`; `scratch` holds the linear powers and the
// kernel.
inline void smoothFftTraceGaussianLinearPower(std::span<const float> in, std::span<float> out,
                                              double sigmaPoints, std::vector<double>& scratch)
{
    const int n = static_cast<int>(std::min(in.size(), out.size()));
    if (n <= 0) {
        return;
    }
    if (!(sigmaPoints > 0.0) || n < 2) {
        std::copy_n(in.begin(), n, out.begin());
        return;
    }
    const int half = std::min(n - 1, static_cast<int>(std::ceil(3.0 * sigmaPoints)));

    constexpr double kLn10Over10 = 0.23025850929940457;
    scratch.resize(static_cast<std::size_t>(n + half + 1));
    double* lin = scratch.data();
    double* w = scratch.data() + n;
    for (int i = 0; i < n; ++i) {
        const float db = in[static_cast<std::size_t>(i)];
        lin[i] = std::isfinite(db) ? std::exp(static_cast<double>(db) * kLn10Over10) : 0.0;
    }
    const double inv2s2 = 1.0 / (2.0 * sigmaPoints * sigmaPoints);
    for (int k = 0; k <= half; ++k) {
        w[k] = std::exp(-static_cast<double>(k) * k * inv2s2);
    }

    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - half);
        const int hi = std::min(n - 1, i + half);
        double sum = 0.0;
        double weight = 0.0;
        for (int j = lo; j <= hi; ++j) {
            const double wj = w[j > i ? j - i : i - j];
            sum += wj * lin[j];
            weight += wj;
        }
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(10.0 * std::log10(std::max(sum / weight, 1e-30)));
    }
}

} // namespace AetherSDR
