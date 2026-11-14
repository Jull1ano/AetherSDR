#pragma once

#include "core/PcmFrame.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "core/backends/anan/AnanDroopCorrection.h"
#include "core/backends/anan/AnanPanView.h"
#include "core/backends/anan/P2Protocol.h"   // kDdc0RatesKsps
#include "core/backends/anan/AnanPanAnalyzer.h"
#include "core/dsp/WdspChannel.h"
#include "core/dsp/WdspProcessTally.h"

#include <QMap>

namespace AetherSDR::anan {

// The ANAN-G2 receive DSP stage: raw IQ blocks of RX1's DDC (P2Client::ddcIqReady) in;
// demodulated audio (WdspChannel), panadapter spectrum (AnanPanAnalyzer) and an
// uncalibrated raw signal-peak reading (no dBFS-to-dBm calibration yet) out.
// Runs on AnanBackend's I/O thread; same shape as Hl2RxDsp, scoped to the
// RX-only path. The WDSP impulse blanker runs ahead of demodulation; manual
// notches are WDSP's notched bandpass (addNotch() and friends).
// READ docs/HERMES.md §16 (receive handedness) BEFORE TOUCHING processIqBlock().
// P2 polarity is confirmed by `radiocert rx` (WWV: USB/DIGU recover, LSB/DIGL
// don't) and independently by SDR++ on an RSP1B. Both demodulator and analyzer
// take the raw wire IQ (the analyzer's Spectrum0() swaps I/Q itself); nothing
// here conjugates, and the handedness test pins a tone above centre landing
// above centre.
class AnanRxDsp : public QObject {
    Q_OBJECT

public:
    explicit AnanRxDsp(QObject* parent = nullptr);
    ~AnanRxDsp() override;

    // WDSP's internal DSP rate. Constant regardless of the IQ rate or
    // the audio rate -- see the note in configure() (mirrors Hl2RxDsp's
    // kWdspDspSampleRateHz exactly; this is a WDSP fact, not a radio fact).
    static constexpr int kWdspDspSampleRateHz = 48000;

    struct Config {
        int inputSampleRateHz = 48000;   // the receiver's IQ sample rate
        // Demodulated-audio rate. 24 kHz because that is AudioEngine's
        // native RX rate; every valid DDC0 rate (48/96/192/384/768/1536 kHz)
        // divides evenly into it.
        int audioSampleRateHz = 24000;
        int dspBlockSize = 1024;         // WdspChannel input/processing block
        // Panadapter output points per frame. The FFT behind them is larger
        // (AnanPanAnalyzer: at least 16384) and averaged down to this count.
        // Follows the panel's width (setPanPoints()); the droop tables stay
        // at kDroopCorrectionFftSize and are read onto this count
        // (applyDroopCorrectionDbResampled()).
        int panPoints = 1024;
        // Display frame rate the analyzer is sized for; kept current by
        // setSpectrumRateFps() so a rebuild comes up at the operator's rate.
        int spectrumFps = 25;
        // Panadapter time-average, ms; 0 = none. Kept current by
        // setSpectrumAverageMs() for the same reason.
        int spectrumAverageMs = 0;
        // Log-recursive (true) or linear-recursive (false) averaging; kept
        // current by setSpectrumLogAverage().
        bool spectrumLogAverage = true;
        // brick3-angelia: the operator's FFT size (0 = automatic) and window
        // for the panadapter analyzer -- AnanPanAnalyzer::Settings. Kept
        // current by setPanFftSize() and setPanWindow(); both apply live.
        int panFftSize = 0;
        int panWindow = AnanPanAnalyzer::kDefaultWindow;
        // brick3-angelia: the part of the sampled band on screen, as
        // fractions of it (AnanPanView.h); 0..1 is all of it. Kept current by
        // setPanView(). panPoints is the SCREEN's point count: the analyzer's
        // own follows from it and from the view.
        double panViewLow = 0.0;
        double panViewHigh = 1.0;
        // brick3-angelia: the DDC frequency panViewLow/High are fractions of
        // the band around, Hz (0 = not known). With it the view is a place in
        // absolute frequency, read from each frame at the frequency that
        // frame was computed for (see onDdcRetuned()).
        double panViewLoHz = 0.0;
        // brick3-angelia: attack and decay of the trace, ms; 0 = none. A
        // point rises with the one time constant and falls with the other,
        // after the time average above. Kept current by
        // setSpectrumBallistics().
        int spectrumAttackMs = 0;
        int spectrumDecayMs = 0;
        // brick3-angelia: Flatten, percent of the noise floor's grass drawn
        // away at publish (anan::flattenNoiseFloor()); 0 = none. Kept
        // current by setSpectrumFlatten().
        int spectrumFlattenPct = 0;
        // brick3-angelia: WDSP's automatic notch filter, kept current by
        // setAutoNotch(). In Config for the same reason as the blanker.
        bool autoNotch = false;
        int autoNotchLevel = 50;
        // brick3-angelia: receive squelch, kept current by setSquelch(). In
        // Config for the blanker's reason: a rebuilt channel starts with
        // every squelch stage off.
        bool squelchEnabled = false;
        int squelchLevel = 20;
        double squelchLevelOffsetDb = 0.0;
        WdspChannel::Mode mode = WdspChannel::Mode::Usb;
        double filterLowHz = 150.0;
        double filterHighHz = 3000.0;
        // RX AGC. WdspChannel's own defaults (mode 3 / 120 dB ceiling) run
        // the radio wide open -- 120 dB is the TOP of WDSP's AGC range, not
        // a sane operating ceiling. Matches Hl2RxDsp::Config's measured-safe
        // default; AnanBackend (commit 4) owns the operator-facing mapping.
        int agcMode = 3;
        double maximumAgcGainDb = 39.0;
        // The gain with AGC off (WDSP's fixed gain). WdspChannel's default;
        // AnanBackend supplies the operator's.
        double agcFixedGainDb = 10.0;
        // false (live): processIq is non-blocking. true: waits for each
        // output block (deterministic for an offline/burst feed -- what the
        // handedness test uses).
        bool blockForOutput = false;
        // WDSP's impulse noise blankers (ANB and NOB), run on the raw IQ ahead
        // of the channel -- see WdspChannel::setNoiseBlanker(). In Config so a
        // rate-change rebuild opens the new channel with the operator's
        // blanker, the same way it carries mode, filter and AGC. Level is
        // 0..100 in the slice model's units; the fill applies to Advanced.
        WdspChannel::NoiseBlanker noiseBlanker = WdspChannel::NoiseBlanker::Off;
        int noiseBlankerLevel = 50;
        WdspChannel::NoiseBlankerFill noiseBlankerFill =
            WdspChannel::NoiseBlankerFill::Zero;
        // brick3-angelia: WDSP's own NR2 (EMNR), Thetis's -- engaged = on.
        // In Config for the same reason as the blanker.
        std::optional<WdspChannel::Emnr> emnr;
    };

    // Synchronous convenience used by deterministic tests. Production first
    // connect and rate changes both use the split build/install path below so
    // FFTW planning never blocks this object's I/O thread.
    Q_INVOKABLE bool configure(const Config& config, std::string* error = nullptr);

    // Result of building a WdspChannel + AnanPanAnalyzer pair OFF this
    // object's own thread -- reads and writes nothing on `this`, so it is safe
    // to call from any thread. Move-only (owns two unique_ptrs).
    //
    // `analyzer` is declared AFTER `channel` so it is destroyed FIRST: it uses
    // the channel's id as its analyzer slot, and must be gone before the
    // channel's destructor returns that id to the pool.
    struct RebuildResult {
        std::unique_ptr<WdspChannel> channel;
        std::unique_ptr<AnanPanAnalyzer> analyzer;
        std::size_t outputBlockSize = 0;
        // The DDC0 rate this channel was built for. installChannel() copies it into
        // m_config.inputSampleRateHz -- the only update path for a live rate change,
        // since buildChannel() cannot touch m_config -- so droopTableForRate() selects
        // the right rate's table.
        int inputSampleRateHz = 0;
        std::string error;   // set iff channel == nullptr
    };

    // The slow half of configure() (WdspChannel::create()'s FFTW planning, up to ~a
    // minute cold), static and `this`-free so a rate change runs it on a background
    // thread while the installed channel keeps processing.
    [[nodiscard]] static RebuildResult buildChannel(const Config& config);

    // Seeds this object's current state before the FIRST asynchronous build.
    // installRebuiltChannel() deliberately reapplies m_config rather than the
    // background snapshot, so omitting this step would replace the requested
    // startup mode/filter/AGC with Config's member defaults. Subsequent edits
    // made while the build is running still update m_config and win at install.
    Q_INVOKABLE void beginInitialBuild(const Config& config);

    // Marks a rebuild in flight: setMode()/setFilter()/setAgc()/setAgcFixedGain()/
    // setShift() still update m_config/m_shiftHz but skip m_channel, because WDSP
    // control calls take the setup mutex (WdspChannel.cpp's g_setupMutex) the
    // background build holds for its FFTW planning -- pushing would block this thread.
    Q_INVOKABLE void beginRebuild();

    // Installs a built RebuildResult on this object's thread. Not Q_INVOKABLE: moc's
    // dispatch copy-constructs by-value arguments, which the move-only RebuildResult
    // cannot satisfy; callers invoke directly or via invokeMethod's functor overload.
    // Re-pushes the CURRENT m_config/m_shiftHz (newer than the build's snapshot),
    // resizes scratch buffers, resets the DC blocker, retires the old
    // channel/analyzer and clears the in-flight flag. Returns false, leaving the
    // current channel untouched, if result.channel is null.
    bool installRebuiltChannel(RebuildResult result);

    Q_INVOKABLE void setMode(WdspChannel::Mode mode);
    Q_INVOKABLE void setFilter(double lowHz, double highHz);
    Q_INVOKABLE void setAgc(int agcMode, double maximumGainDb);
    // The gain with AGC off, in dB. Deferred like setAgc() while a rebuild is
    // in flight; installChannel() re-applies it at the swap.
    Q_INVOKABLE void setAgcFixedGain(double fixedGainDb);
    // RX frequency shift in Hz relative to the NCO -- how a single-DDC
    // backend tunes the slice inside the passband without moving the DDC.
    Q_INVOKABLE void setShift(double shiftHz);
    // Which blanker, its level (0..100) and Advanced's fill. Deferred like
    // setMode() while a rebuild is in flight; installChannel() re-applies it at
    // the swap.
    Q_INVOKABLE void setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                     WdspChannel::NoiseBlankerFill fill);
    // brick3-angelia: WDSP's NR2 (EMNR) on or off with its methods. Deferred
    // like setNoiseBlanker() while a rebuild is in flight.
    void setNr2(bool on, int gainMethod, int npeMethod, bool aeRun);
    // brick3-angelia: WDSP's automatic notch filter (ANF), level 0..100.
    void setAutoNotch(bool on, int level);
    // brick3-angelia: receive squelch, level 0..100, as Hl2RxDsp's
    // (WdspChannel::setSquelch()): FM's noise squelch, a level squelch in
    // AM/SAM/DSB/LSB/USB, nothing in the other modes. `levelOffsetDb` refers
    // the level squelch to the antenna across the step attenuator. Deferred
    // like setNoiseBlanker() while a rebuild is in flight; a change the
    // channel refuses is retried at the top of processIqBlock().
    void setSquelch(bool on, int level, double levelOffsetDb);

    // brick3-angelia: manual notches (Thetis's MNF, the app's TNF): nulls
    // parked at ABSOLUTE RF frequencies, in WDSP's notched bandpass
    // (WdspChannel::addNotch()). `index` is the position in a dense list, the
    // same list AnanBackend keeps by id. This object keeps the list too and
    // replays it into every channel it installs, so a rate change keeps the
    // notches; while a rebuild is in flight a change only reaches the list.
    // setNotchTuneFrequency() takes the DDC's centre and must follow every
    // retune, or the notches drift off their carriers.
    Q_INVOKABLE void addNotch(int index, double centerHz, double widthHz, bool active);
    Q_INVOKABLE void editNotch(int index, double centerHz, double widthHz, bool active);
    Q_INVOKABLE void removeNotch(int index);
    Q_INVOKABLE void clearNotches();
    Q_INVOKABLE void setNotchesEnabled(bool on);
    Q_INVOKABLE void setNotchTuneFrequency(double tuneHz);
    struct NoiseBlankerState {
        bool hasChain = false;
        WdspChannel::NoiseBlanker kind = WdspChannel::NoiseBlanker::Off;
        int level = 0;
        WdspChannel::NoiseBlankerFill fill = WdspChannel::NoiseBlankerFill::Zero;
        [[nodiscard]] bool on() const noexcept
        {
            return kind != WdspChannel::NoiseBlanker::Off;
        }
    };
    // One atomic snapshot of the installed channel, safe to query from the
    // backend's thread. Deferred or refused requests do not change this value.
    [[nodiscard]] NoiseBlankerState noiseBlankerState() const noexcept
    {
        const int state = m_nbAppliedState.load(std::memory_order_relaxed);
        if (state < 0) {
            return {};
        }
        return {true,
                static_cast<WdspChannel::NoiseBlanker>(
                    (state / kNbLevelStride) % kNbKindStride),
                state % kNbLevelStride,
                static_cast<WdspChannel::NoiseBlankerFill>(
                    state / (kNbLevelStride * kNbKindStride))};
    }
    // Cap how often a panadapter frame is produced, in frames per second.
    // Also re-sizes the analyzer's overlap and averaging weights for the new
    // rate, so one FFT still completes per display frame.
    Q_INVOKABLE void setSpectrumRateFps(int fps);
    // Panadapter time-average, ms; 0 = none. See
    // AnanPanAnalyzer::setAverageTimeMs().
    Q_INVOKABLE void setSpectrumAverageMs(int ms);
    // Log-recursive (true) or linear-recursive (false) time averaging. See
    // AnanPanAnalyzer::setLogAverage().
    Q_INVOKABLE void setSpectrumLogAverage(bool on);
    // Panadapter output points per frame, from the panel's width. Clamped to
    // AnanPanAnalyzer::kMaxPoints; below 2 is ignored. See
    // AnanPanAnalyzer::setNumPoints().
    Q_INVOKABLE void setPanPoints(int points);
    // brick3-angelia: the analyzer's window; see AnanPanAnalyzer::setWindow().
    Q_INVOKABLE void setPanWindow(int window);
    // brick3-angelia: the analyzer's FFT size (0 = automatic), live; see
    // AnanPanAnalyzer::setFftSize().
    Q_INVOKABLE void setPanFftSize(int fftSize);
    // brick3-angelia: the trace's attack and decay, ms; 0 = none.
    Q_INVOKABLE void setSpectrumBallistics(int attackMs, int decayMs);
    // brick3-angelia: Flatten, percent (0..90); drawing only.
    Q_INVOKABLE void setSpectrumFlatten(int pct);
    // brick3-angelia: the part of the sampled band to show (Config::
    // panViewLow/High). Takes effect at once -- the frame in hand is re-read
    // onto the new view and published -- and the analyzer is re-aimed once
    // the gesture has settled (kViewSettleMs).
    // `sampleRateHz` is the rate the fractions were worked out for. A view
    // for another rate than the running analyzer's belongs to the channel
    // being built for that rate: it is held and applied at its install, so
    // the frames still coming at the old rate are not read through it.
    Q_INVOKABLE void setPanView(double low, double high, int sampleRateHz, double loHz);

    // brick3-angelia: the DDC frequency was just sent to the radio (P2Client's
    // ddcFrequencySent, on this thread, in order with the IQ). The analyzer
    // marks the change in its input and rotates older samples onto the new
    // frequency (WDSP patch 25), and the trace's running average is kept in
    // absolute frequency, so a drag that moves the DDC neither doubles nor
    // smears the spectrum.
    void onDdcRetuned(double hz);

    // Installs the per-bin dB correction for ONE DDC0 rate (AnanDroopCorrection.h).
    // Ignored if `table` is not kDroopCorrectionFftSize long or the rate is not a
    // valid DDC0 rate, so bins never misalign. std::vector<float> reuses an already
    // registered metatype.
    Q_INVOKABLE void setDroopCorrectionTable(int rateKsps, const std::vector<float>& table);

    // Suspends droop correction WITHOUT discarding tables, so the calibrator measures
    // the radio rather than its own corrected output. A flag, not zero tables: a
    // stored zero table is a copy, so processIqBlock()'s identity test against
    // kDroopCorrectionZero would keep the 12 dB edge fade on and measure it as
    // droop. Routed through droopTableForRate(), and non-destructive on abort/crash.
    Q_INVOKABLE void setDroopCorrectionBypassed(bool bypassed);
    [[nodiscard]] bool droopCorrectionBypassed() const noexcept { return m_droopBypassed; }

    // Forgets every measured table. This object outlives disconnect while tables
    // are per-RADIO, so AnanBackend calls this on disconnect and before seeding a
    // new connect; otherwise radio #1's corrections render radio #2.
    Q_INVOKABLE void clearDroopCorrectionTables();

    // Exposes the active channel for testing installChannel()'s reapply
    // behaviour (mode/filter/AGC/shift surviving a rebuild swap) without a
    // live radio -- matches WdspChannel's own *ForTest accessor convention.
    // Not part of the operator-facing seam; nullptr before the first
    // configure()/installRebuiltChannel().
    [[nodiscard]] const WdspChannel* channelForTest() const noexcept { return m_channel.get(); }

    // Every m_channel->processIq() outcome since construction; identical to
    // Hl2RxDsp::processTally(). `Underrun` is counted apart from the four faults as
    // it is normal while output fills (WdspProcessTally.h). MONOTONIC ACROSS
    // REBUILDS: the off-thread swap is the likeliest moment for a `Busy` or
    // geometry fault. Relaxed atomics; safe from any thread.
    [[nodiscard]] WdspProcessTally::Counts processTally() const noexcept
    {
        return m_processTally.snapshot();
    }

    // Mute the DEMODULATOR while transmitting: muting downstream would let the
    // backlog of our own transmission drain on unmute. The spectrum still runs on
    // real IQ; audio is clocked with silence. (No TX yet, RFC §2.11 Phase 3.)
    Q_INVOKABLE void setAudioMuted(bool muted);
    [[nodiscard]] bool isConfigured() const noexcept { return m_channel != nullptr; }

    // Demodulated-audio DC blocker, one pole per channel. WDSP's AM/SAM detector is
    // an envelope detector (amd.c: sqrt(I^2+Q^2)), so the carrier is a DC pedestal
    // nothing upstream removes; see Hl2RxDsp::DcBlocker.
    struct DcBlocker {
        float r = 0.0f;    // pole radius, set by configure(); <= 0 bypasses
        float x1 = 0.0f;
        float y1 = 0.0f;

        [[nodiscard]] float process(float x) noexcept
        {
            if (!(r > 0.0f))
                return x;   // bypass when unconfigured -- r=0 would be a differentiator
            float y = x - x1 + r * y1;
            if (!(std::fabs(y) > 1e-20f))
                y = 0.0f;   // flush denormals (slow on x86) during silence
            x1 = x;
            y1 = y;
            return y;
        }

        void reset() noexcept { x1 = 0.0f; y1 = 0.0f; }
    };

    // -3 dB corner, Hz. Matches Hl2RxDsp::kDcBlockerCornerHz -- well below
    // even a wide AM passband's audio content.
    static constexpr double kDcBlockerCornerHz = 20.0;

    [[nodiscard]] static float dcBlockerPole(double cornerHz,
                                             double sampleRateHz) noexcept
    {
        return static_cast<float>(
            std::exp(-2.0 * std::numbers::pi * cornerHz / sampleRateHz));
    }

    // ── Panadapter integrity across a transport gap ───────────────────────
    //
    // Partial FFT windows discarded at IQ discontinuities, including
    // accepted rewinds and duplicates. Same lifetime/empty-window semantics
    // as Hl2RxDsp::spectrumGapDiscards(). AnanBackend does not expose health
    // rows yet; this counter is currently available only at the DSP stage.
    [[nodiscard]] quint64 spectrumGapDiscards() const noexcept
    {
        return m_spectrumGapDiscards.load(std::memory_order_relaxed);
    }

public slots:
    // Feed one IQ block (normalized complex<float>). Emits spectrumReady when
    // a display frame is due and the analyzer has a new one, and
    // audioReady/meterUpdate per completed WdspChannel block.
    void processIqBlock(const std::vector<std::complex<float>>& iq);

    // A sequence gap on RX1's DDC precedes the NEXT block (DirectConnection on this I/O
    // thread, same call chain). Drops and counts the staged analyzer block that would
    // straddle the gap (spectrumGapDiscards()); the analyzer history and audio path
    // are left alone, as the openHPSDR clients do (~250 ms averaging makes a gap
    // cost a frame or two; purging would blank the panadapter).
    void onSequenceGap();

signals:
    void pcmReady(const AetherSDR::PcmFrame& frame);
    void audioReady(const std::vector<float>& stereoPcm);   // interleaved L,R
    // DC-centred dBFS over the view; lowHz/highHz are the RF frequencies of the
    // first and last point when known (0 = the view as last set).
    void spectrumReady(const std::vector<float>& binsDbfs, double lowHz, double highHz);
    // WDSP's own signal-strength meter (SignalAverage), NOT the RMS of the
    // demodulated audio -- the AGC holds audio level roughly constant, so an
    // audio-RMS meter would barely move with signal strength.
    void meterUpdate(float dbfs);

private:
    void publishNoiseBlankerState();
    // The applied blanker in ONE int, so the backend's thread reads a coherent
    // snapshot from a single relaxed load rather than three that can tear:
    // level + stride * kind + stride * kinds * fill. Strides are powers of two
    // and larger than the ranges they carry (levels 0..100, three kinds, five
    // fills), so each field stays legible in a debugger.
    static constexpr int kNbLevelStride = 128; // levels occupy 0..100
    static constexpr int kNbKindStride = 8;    // three kinds today
    std::atomic<int> m_nbAppliedState{-1};     // -1: no installed channel
    PcmProducer m_pcmProducer;
    bool spectrumFrameDue();

    // Shared install step for a successful RebuildResult -- resizes scratch
    // buffers, resets the DC blocker, re-applies
    // m_config/m_shiftHz's CURRENT values to the new channel (not whatever
    // it was built with, which may be stale), and takes ownership of
    // result's channel/analyzer. Used by configure() and the asynchronous
    // first-connect/rate-change path so their final installation cannot drift.
    void installChannel(RebuildResult result);

    // See spectrumGapDiscards(). Written on the I/O thread by onSequenceGap(),
    // read by whatever polls it; relaxed for the same reasons Hl2RxDsp gives.
    std::atomic<quint64> m_spectrumGapDiscards {0};
    std::unique_ptr<WdspChannel> m_channel;
    // Declared after m_channel so it is destroyed first -- see RebuildResult.
    std::unique_ptr<AnanPanAnalyzer> m_analyzer;
    double m_shiftHz = 0.0;
    Config m_config;
    // See beginRebuild()/installRebuiltChannel()'s own comments.
    bool m_rebuildInFlight = false;
    // brick3-angelia: m_config's squelch to m_channel; marks it pending when
    // the channel refuses, for processIqBlock() to try again.
    void pushSquelchToChannel();
    bool m_squelchPending = false;

    // Per-outcome counters for m_channel->processIq(); see processTally().
    // Written on the DSP thread in processIqBlock(), read from elsewhere.
    WdspProcessTally m_processTally;

    bool m_audioMuted = false;
    // The S-meter tap's gate: the settle window after our own silence or a
    // channel install, and the read cadence -- see WdspSMeter.h, and its use
    // in processIqBlock(). DSP thread only, like m_audioMuted.
    WdspSMeterTap m_meterTap;
    // Arms it from the CURRENT config. Both call sites run after
    // m_config.inputSampleRateHz has taken the new rate.
    void armMeterSettle() noexcept
    {
        m_meterTap.arm(m_config.inputSampleRateHz, m_config.dspBlockSize,
                       kWdspDspSampleRateHz);
    }
    int m_spectrumIntervalMs = 0;   // 0 = uncapped
    QElapsedTimer m_spectrumClock;
    qint64 m_lastSpectrumMs = 0;

    std::vector<std::complex<float>> m_iqBuffer;    // IQ awaiting a full DSP block
    // brick3-angelia: how long the head block has waited for WDSP's output
    // (processIqBlock()); past kExchangeStallNs it is dropped unexchanged.
    // 20 ms: four blocks at 192 ksps, a worker turns one round in well under 1.
    QElapsedTimer m_exchangeHeadWait;
    static constexpr qint64 kExchangeStallNs = 20'000'000;
    std::uint64_t m_exchangeStallDrops = 0;
    std::vector<float> m_i, m_q;                    // deinterleaved input scratch
    std::vector<float> m_left, m_right;             // WdspChannel output scratch
    DcBlocker m_dcBlockL, m_dcBlockR;
    std::vector<float> m_stereo;                    // interleaved audio out
    std::vector<float> m_bins;                      // spectrum scratch: the analyzer's frame

    // ---- brick3-angelia: the view inside the sampled band (AnanPanView.h) ----
    struct PanWindow {
        double low = 0.0;
        double high = 1.0;
    };
    // How long a view must stand still before the analyzer is re-aimed at it.
    static constexpr int kViewSettleMs = 120;
    // The analyzer's window reaches this far past each side of the view, as a
    // share of the view's width, so a pan of up to that much needs no re-aim.
    static constexpr double kWindowMargin = 0.5;
    [[nodiscard]] PanWindow analyzerWindowForView() const noexcept;
    [[nodiscard]] int analyzerPointsFor(const PanWindow& window) const noexcept;
    // Aim `analyzer` at the current view and screen width, now.
    void retargetAnalyzer(AnanPanAnalyzer& analyzer);
    // processIqBlock()'s check: re-aim once the view has settled, unless the
    // analyzer already covers it at the right scale.
    void retargetAnalyzerIfSettled();
    // m_bins, read onto the view if it is not the view already, and emitted.
    void publishSpectrum();
    bool m_viewDirty = false;
    QElapsedTimer m_viewChangeClock;
    // A view sent for a rate that is not running yet (setPanView()); the rate
    // is 0 when none is held.
    PanWindow m_nextView;
    int m_nextViewRateHz = 0;
    PanWindow m_frameWindow;        // the part of the band m_bins covers
    std::vector<float> m_viewBins;  // the trace as the view shows it

    // ---- brick3-angelia: manual notches ----
    struct Notch {
        double centerHz = 0.0;
        double widthHz = 0.0;
        bool active = true;
    };
    std::vector<Notch> m_notches;        // WDSP's order
    bool m_notchesEnabled = true;
    double m_notchTuneHz = 0.0;
    // True when a change may go to the channel now (see the setters' comment).
    [[nodiscard]] bool canPushToChannel() const noexcept
    {
        return m_channel != nullptr && !m_rebuildInFlight;
    }
    // WDSP's notch list rebuilt from m_notches, with the tune frequency and
    // the enable: at every install, and whenever a single change was refused
    // (so a position can never drift between the two lists).
    void pushAllNotches(WdspChannel& channel);

    // ---- brick3-angelia: the trace's movement in time ----
    // The time average (FFT AVG, log or linear) and the attack/decay run
    // HERE, on the analyzer's frames, not inside WDSP: their state then
    // survives the analyzer being re-aimed. WDSP's own average restarts from
    // one raw frame every time, which is what made the trace turn to grass
    // for a moment after each drag.
    //
    // m_average is the averaged frame, m_trace what is published: m_average
    // followed with the attack/decay. Both sit on the grid of the frame they
    // were last fed (m_traceWindow), and are carried onto a new grid when the
    // analyzer's window or point count changes.
    void smoothFrame();
    void carryOntoFrameGrid(std::vector<float>& state);
    std::vector<float> m_average;
    std::vector<float> m_trace;
    std::vector<float> m_carry;     // scratch for carryOntoFrameGrid()
    PanWindow m_traceWindow;        // the part of the band m_trace covers
    // brick3-angelia (WDSP patch 25): the DDC frequency now, the one the frame
    // in hand was computed for and the one the running trace sits at, Hz
    // (0 = not known), and which points of the frame its samples cover.
    double m_ddcLoHz = 0.0;
    double m_frameLoHz = 0.0;
    double m_traceLoHz = 0.0;
    double m_frameValidLowHz = -1.0e12;
    double m_frameValidHighHz = 1.0e12;
    std::vector<unsigned char> m_frameInvalid;   // 1 = point outside the frame's coverage
    void maskUncoveredPoints();
    bool m_traceValid = false;      // m_trace holds something to publish
    // Flatten works on a copy of what is published, never on m_trace or
    // m_average: the smoothing state stays the measurement.
    std::vector<float> m_publishBins;
    FlattenState m_flatten;
    QElapsedTimer m_frameClock;     // time between analyzer frames

    // Live droop-correction tables, keyed by DDC0 rate in ksps -- see
    // setDroopCorrectionTable(). Survives a rate-change rebuild untouched:
    // installRebuiltChannel() swaps m_channel/m_analyzer, not this object,
    // and the correction is applied to m_bins after the analyzer, independent of
    // which WdspChannel produced the IQ that fed it.
    QMap<int, DroopCorrectionTable> m_droopTables;
    // See setDroopCorrectionBypassed(). Deliberately NOT cleared by
    // clearDroopCorrectionTables(): "am I mid-sweep" is a property of the
    // sweep, not of which tables happen to be loaded.
    bool m_droopBypassed = false;
    [[nodiscard]] const DroopCorrectionTable& droopTableForRate(int rateKsps) const noexcept;
};

}  // namespace AetherSDR::anan
