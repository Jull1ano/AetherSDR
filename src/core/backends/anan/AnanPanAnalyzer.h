#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AetherSDR::anan {

// ANAN-G2 panadapter spectrum via WDSP's display analyzer
// (third_party/wdsp/upstream/analyzer.c), configured to deskHPSDR's defaults:
// FFT = next power of two >= output points, min 16384; Kaiser window (PiAlpha
// 14); per-point AVERAGE detector (the main de-graining); overlap sized for one
// full FFT per display frame; operator time average in ms (0 = none, else WDSP
// mode 3 log-recursive, weight exp(-1 / (F * t)) at FFT rate F, or linear);
// levels normalised to one output point's bandwidth.
// Our addition: the running average is SEEDED from the first frame (after
// create and when averaging is re-enabled), because the analyzer is rebuilt on
// every rate change and an unseeded average fades in from -160 dB.
// Input is RAW wire IQ: Spectrum0() swaps I/Q, which is what orients the HPSDR
// convention correctly. Output runs lowest to highest frequency.
// Threading: create() plans FFTs (FFTW_PATIENT) off any real-time thread and
// takes WdspChannel::fftwSetupLock() itself; everything else, destructor
// included, runs on the one feeding thread (SetAnalyzer() resets the input ring
// and cannot race Spectrum0()).
class AnanPanAnalyzer {
public:
    struct Settings {
        int sampleRateHz = 48000;
        int numPoints = 1024;          // output points per frame
        int framesPerSecond = 25;      // display frame rate
        int averageTimeMs = 0;         // time average, ms; 0 = none
        bool logAverage = true;        // log-recursive (true) or linear-recursive
        // brick3-angelia: the operator's FFT size. 0 = automatic (kMinFftSize,
        // or the point count when that is larger); otherwise rounded up to a
        // power of two in kSmallestFftSize..kMaxFftSize. A smaller FFT follows
        // signals faster and looks grainier; a larger one averages more bins
        // into each point, so the trace is calmer and slower. Read by create()
        // only: a new size re-plans FFTW_PATIENT, which no setter may do on
        // the feeding thread -- the owner rebuilds instead.
        int fftSize = 0;
        // brick3-angelia: WDSP's analyzer window, 0..kWindowCount-1:
        // 0 rectangular, 1 Blackman-Harris (4-term), 2 Hann, 3 flat-top,
        // 4 Hamming, 5 Kaiser (PiAlpha 14, deskHPSDR's and the default),
        // 6 Blackman-Harris (7-term).
        int window = 5;
        // brick3-angelia: the part of the sampled band the frames cover, as
        // fractions of it (0 = its lowest frequency, 1 = its highest); 0..1 is
        // the whole band. WDSP drops the FFT bins outside it and spreads the
        // rest over numPoints, which is how Thetis zooms: the sample rate stays
        // put and the display shows a slice of it, at the FFT's own
        // resolution. Rounded outward to whole bins (windowLow()/windowHigh()
        // report what is really returned).
        double viewLow = 0.0;
        double viewHigh = 1.0;
        // The point count the levels are normalised to: a level is the power
        // in one point's bandwidth when the WHOLE band is spread over this
        // many points. 0 = numPoints. Kept at the screen's width while
        // numPoints follows the zoom, so the noise floor does not move when
        // the view is zoomed (Thetis's "1 Hz BW" behaviour).
        int normPoints = 0;
    };

    static constexpr int kDefaultWindow = 5;
    static constexpr int kWindowCount = 7;

    // Complex samples handed to the analyzer per Spectrum0() call. A power of
    // two, so it divides the analyzer's input ring (2 * kMaxFftSize).
    static constexpr int kBlockSize = 1024;
    // Largest FFT any setting can ask for, and the analyzer's buffer size:
    // Thetis's own largest (brick3-angelia). XCreateAnalyzer sizes its buffers
    // for this and for WDSP's own maxima (dMAX_PIXOUTS x dMAX_AVERAGE x
    // dMAX_PIXELS window-average frames among them): about 60 MB per analyzer,
    // and two exist briefly during a rebuild.
    static constexpr int kMaxFftSize = 262144;
    static constexpr int kMinFftSize = 16384;
    // The smallest FFT the operator may pick (Settings::fftSize).
    static constexpr int kSmallestFftSize = 4096;
    // Most output points the analyzer can return (WDSP's dMAX_PIXELS).
    static constexpr int kMaxPoints = 16384;

    // Builds and configures the analyzer in slot `disp` (0..71). Returns
    // nullptr and sets `error` if the slot cannot be created.
    [[nodiscard]] static std::unique_ptr<AnanPanAnalyzer> create(int disp, const Settings& settings,
                                                                std::string* error = nullptr);
    ~AnanPanAnalyzer();
    AnanPanAnalyzer(const AnanPanAnalyzer&) = delete;
    AnanPanAnalyzer& operator=(const AnanPanAnalyzer&) = delete;

    [[nodiscard]] int disp() const noexcept { return m_disp; }
    [[nodiscard]] int fftSize() const noexcept { return m_fftSize; }
    [[nodiscard]] int numPoints() const noexcept { return m_settings.numPoints; }
    [[nodiscard]] int framesPerSecond() const noexcept { return m_settings.framesPerSecond; }
    // The part of the sampled band the frames really cover, after rounding to
    // whole FFT bins (Settings::viewLow/viewHigh are the request).
    [[nodiscard]] double windowLow() const noexcept { return m_windowLow; }
    [[nodiscard]] double windowHigh() const noexcept { return m_windowHigh; }

    // The view (Settings::viewLow/viewHigh), the point count and the
    // normalisation count together, in one reconfiguration. No re-plan -- the
    // FFT size does not depend on any of them -- but it resets the input
    // ring, so the next frame needs a full FFT of fresh samples, and the
    // running average, kept per point, re-seeds from that frame.
    void setViewAndPoints(double viewLow, double viewHigh, int points, int normPoints);

    // The frame-rate-dependent settings -- overlap and the averaging weights --
    // re-applied for a new display rate. The FFT size does not change, so
    // nothing is re-planned; the running average is kept.
    void setFramesPerSecond(int fps);

    // The output point count, for a new panel width. Clamped to
    // 2..kMaxPoints. The FFT size cannot change -- every count up to
    // kMaxPoints fits in kMinFftSize -- so nothing is re-planned, but the
    // running average is per point, so the next frame re-seeds it.
    void setNumPoints(int points);

    // The time average, in ms: 0 = none, t > 0 = log-recursive with time
    // constant t. Cheap -- no re-plan, no ring reset. Switching averaging back
    // on re-seeds from the next frame.
    void setAverageTimeMs(int ms);

    // Log-recursive (true, deskHPSDR's default) or linear-recursive (false)
    // averaging. Cheap; re-seeds, since entering either mode resets WDSP's
    // history.
    void setLogAverage(bool on);

    // The window (Settings::window), clamped to its range. The FFT size is
    // unchanged, so nothing is re-planned: WDSP only recomputes the window
    // coefficients. Resets the input ring, as setFramesPerSecond() does.
    void setWindow(int window);
    [[nodiscard]] int window() const noexcept { return m_settings.window; }

    // Stage raw wire IQ and hand the analyzer every whole kBlockSize block.
    // A partial block is carried to the next call.
    void feed(std::span<const std::complex<float>> iq);

    // Drop a partially staged block, returning how many samples went with it
    // (0 = nothing was in flight). The analyzer's own history is left alone.
    std::size_t dropStagedPartial() noexcept;

    // If the analyzer has produced a frame since the last call, copy it into
    // `pointsDb` (resized to numPoints(), dB, lowest frequency first) and
    // return true. Returns false, leaving `pointsDb` untouched, otherwise.
    bool takeFrame(std::vector<float>& pointsDb);

    // The derived analyzer parameters for a given setting -- exposed so tests
    // can pin the formulas without a running analyzer.
    struct Derived {
        int fftSize = 0;
        int overlap = 0;
        int maxWriteahead = 0;
        double avBackmult = 0.0;
        int numAverage = 0;
    };
    [[nodiscard]] static Derived derive(const Settings& settings) noexcept;

private:
    AnanPanAnalyzer(int disp, const Settings& settings);
    void applySettings();

    int m_disp;
    Settings m_settings;
    int m_fftSize = 0;
    double m_windowLow = 0.0;
    double m_windowHigh = 1.0;
    double m_avBackmult = 0.0;
    void applyAveraging();
    bool m_seeded = false;
    std::vector<double> m_staged;          // interleaved I,Q -- 2 * kBlockSize
    std::size_t m_stagedCount = 0;         // complex samples in m_staged
    std::vector<float> m_scratch;          // GetPixels target, numPoints
};

}  // namespace AetherSDR::anan
