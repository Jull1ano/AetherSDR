#include "gui/FftTraceSmoothing.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expectNear(const char* what, float actual, float expected, float tol = 1e-3f)
{
    if (!(std::fabs(actual - expected) <= tol)) {
        std::fprintf(stderr, "FAIL: %s: expected %.4f, got %.4f\n", what, expected, actual);
        ++failures;
    }
}

std::vector<float> smooth(const std::vector<float>& in, int halfWidth)
{
    std::vector<float> out(in.size());
    std::vector<double> scratch;
    AetherSDR::smoothFftTraceLinearPower(in, out, halfWidth, scratch);
    return out;
}

} // namespace

int main()
{
    // Off is bit-identical: FFT Smooth at 0 must be exactly today's trace.
    {
        const std::vector<float> in{-120.0f, -90.5f, -33.25f, -140.0f, -101.0f};
        const auto out = smooth(in, 0);
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (out[i] != in[i]) {
                std::fprintf(stderr, "FAIL: off changed point %zu\n", i);
                ++failures;
            }
        }
    }

    // A flat floor stays where it is at any width.
    {
        const std::vector<float> in(64, -117.0f);
        const auto out = smooth(in, 5);
        for (std::size_t i = 0; i < out.size(); ++i) {
            expectNear("flat floor", out[i], -117.0f);
        }
    }

    // Averaging is in linear power, not dB: two neighbors at -100 dB and one
    // at -80 dB average to 10*log10((1e-10 + 1e-10 + 1e-8) / 3), about
    // -84.6 dB. A dB mean would read -93.3.
    {
        const std::vector<float> in{-100.0f, -80.0f, -100.0f};
        const auto out = smooth(in, 1);
        const float expected =
            static_cast<float>(10.0 * std::log10((1e-10 + 1e-8 + 1e-10) / 3.0));
        expectNear("linear-power mean", out[1], expected);
    }

    // An impulse spreads across exactly 2*halfWidth+1 points.
    {
        std::vector<float> in(41, -150.0f);
        in[20] = -50.0f;
        const auto out = smooth(in, 3);
        for (int i = 0; i < 41; ++i) {
            const bool inside = i >= 17 && i <= 23;
            if (inside != (out[static_cast<std::size_t>(i)] > -140.0f)) {
                std::fprintf(stderr, "FAIL: impulse spread wrong at %d (%.2f)\n",
                             i, out[static_cast<std::size_t>(i)]);
                ++failures;
            }
        }
        expectNear("impulse level", out[20],
                   static_cast<float>(10.0 * std::log10((1e-5 + 6e-15) / 7.0)));
    }

    // Edges shrink the window instead of padding it: point 0 with halfWidth 2
    // averages points 0..2 only.
    {
        const std::vector<float> in{-60.0f, -70.0f, -80.0f, -10.0f, -10.0f};
        const auto out = smooth(in, 2);
        expectNear("left edge", out[0],
                   static_cast<float>(10.0 * std::log10((1e-6 + 1e-7 + 1e-8) / 3.0)));
    }

    // A noise point next to a +10 dBm carrier stays exact. A running prefix
    // sum loses this to cancellation once the window has left the carrier.
    {
        std::vector<float> in(32, -140.0f);
        in[0] = 10.0f;
        const auto out = smooth(in, 2);
        expectNear("floor after carrier", out[20], -140.0f);
    }

    // In place is allowed.
    {
        std::vector<float> buf{-100.0f, -80.0f, -100.0f};
        std::vector<double> scratch;
        AetherSDR::smoothFftTraceLinearPower(buf, buf, 1, scratch);
        expectNear("in place", buf[1],
                   static_cast<float>(10.0 * std::log10((1e-10 + 1e-8 + 1e-10) / 3.0)));
    }

    // ---- Gaussian FFT Smooth (the slider's unit since brick3-angelia) ----
    const auto gauss = [](const std::vector<float>& in, double sigma) {
        std::vector<float> out(in.size());
        std::vector<double> scratch;
        AetherSDR::smoothFftTraceGaussianLinearPower(in, out, sigma, scratch);
        return out;
    };
    {
        const std::vector<float> in{-120.0f, -90.5f, -33.25f, -140.0f, -101.0f};
        const auto out = gauss(in, 0.0);
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (out[i] != in[i]) {
                std::fprintf(stderr, "FAIL: gaussian off changed point %zu\n", i);
                ++failures;
            }
        }
    }
    {
        const std::vector<float> in(64, -117.0f);
        const auto out = gauss(in, 3.7);
        for (std::size_t i = 0; i < out.size(); ++i) {
            expectNear("gaussian flat floor", out[i], -117.0f);
        }
    }
    // Weights e^(-k^2 / 2 sigma^2), linear power, renormalised: sigma 1 on
    // -100/-80/-100 is (1e-8 + 2 e^-0.5 1e-10) / (1 + 2 e^-0.5).
    {
        const auto out = gauss({-100.0f, -80.0f, -100.0f}, 1.0);
        const double w = std::exp(-0.5);
        expectNear("gaussian weights", out[1],
                   static_cast<float>(10.0 * std::log10((1e-8 + 2 * w * 1e-10) / (1 + 2 * w))));
    }
    // Continuous: a tenth of a point barely touches an impulse; the width
    // grows the spread steadily instead of in whole-pixel jumps.
    {
        std::vector<float> in(41, -150.0f);
        in[20] = -50.0f;
        expectNear("sigma 0.1 keeps the peak", gauss(in, 0.1)[20], -50.0f, 1e-3f);
        const float p03 = gauss(in, 0.3)[20];
        const float p06 = gauss(in, 0.6)[20];
        const float p10 = gauss(in, 1.0)[20];
        if (!(p03 < -50.0f && p06 < p03 && p10 < p06)) {
            std::fprintf(stderr, "FAIL: gaussian peak not monotonic: %.2f %.2f %.2f\n",
                         p03, p06, p10);
            ++failures;
        }
    }
    {
        std::vector<float> buf{-100.0f, -80.0f, -100.0f};
        std::vector<double> scratch;
        AetherSDR::smoothFftTraceGaussianLinearPower(buf, buf, 1.0, scratch);
        const double w = std::exp(-0.5);
        expectNear("gaussian in place", buf[1],
                   static_cast<float>(10.0 * std::log10((1e-8 + 2 * w * 1e-10) / (1 + 2 * w))));
    }
    // A stored box half-width keeps about its look: the sigma with its spread.
    if (AetherSDR::fftSmoothingStepsFromHalfWidth(0) != 0
        || AetherSDR::fftSmoothingStepsFromHalfWidth(1) != 8
        || AetherSDR::fftSmoothingStepsFromHalfWidth(16) != AetherSDR::kFftSmoothingMaxSteps) {
        std::fprintf(stderr, "FAIL: box half-width migration\n");
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
