#pragma once

#include "core/backends/IRadioBackend.h"
#include "core/backends/anan/AnanRxDsp.h"
#include "core/backends/anan/AnanSliceAudio.h"
#include "core/Resampler.h"
#include "core/backends/anan/AnanDroopCalibrator.h"
#include "core/backends/anan/P2Client.h"
#include "core/dsp/WdspSMeter.h"

#include <QElapsedTimer>
#include <QMap>
#include <QString>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR::anan {

// IRadioBackend for the ANAN-G2 (openHPSDR Protocol 2). Owns one P2Client and
// one AnanRxDsp on a dedicated I/O thread, like Hl2Backend with MetisClient +
// Hl2RxDsp. Scope: one DDC, RX only.
//   - NCO/WDSP-shift split (brick3-angelia, as Hl2Backend): the slice tunes
//     inside a fixed DDC span by WDSP shift (slice - NCO - cwBfoHz); the DDC
//     recentres only when the slice leaves the usable span, and setPanCenter()
//     moves the DDC, pulling the slice along only at that edge.
//   - capabilities()' identity strings are fixed; gateware, DDC count and board
//     id come from this session's Discovery reply once it lands.
//   - setKeying() is a no-op: canTransmit is false and P2Client has no PTT
//     (TX is RFC §2.11 Phase 3).
// Receive handedness lives entirely in AnanRxDsp (see docs/HERMES.md §16).
class AnanBackend : public IRadioBackend {
    Q_OBJECT

public:
    // Public for testing. The seeded defaults are reported here alongside a
    // sweep's measured tables, so the one invariant worth pinning is that a
    // rate claims NO correction until connectRadio() has actually seeded one
    // -- reporting a default as live on a disconnected radio would be the
    // same lie in the other direction as the "nothing measured" this replaced.
    // See anan_backend_test.
    [[nodiscard]] QVariantMap droopStatus() const;

    explicit AnanBackend(QObject* parent = nullptr);
    ~AnanBackend() override;

    RadioCapabilities capabilities() const override;
    bool ownsRxAudio() const override { return true; }

    void applyRestoredState(const RestoredRadioState& state) override;
    RestoredRadioState currentOperatingState() const override;
    void connectRadio(const RadioConnectRequest& request) override;
    void disconnectRadio() override;
    bool isConnected() const override { return m_connected; }

    void setSliceFrequency(int sliceId, double hz) override;
    void setSliceMode(int sliceId, const QString& mode) override;
    void setSliceFilter(int sliceId, int lowHz, int highHz) override;
    void setSliceAgc(int sliceId, const QString& mode, int thresholdDb) override;
    // The AGC-off level, which the base class drops. Mode and threshold edits
    // go to setSliceAgc() as before.
    void requestSliceAgc(int sliceId, const SliceAgcRequest& request) override;
    void setSliceNoiseBlanker(int sliceId, AetherSDR::NoiseBlankerKind kind,
                              int level, AetherSDR::NoiseBlankerFill fill) override;
    ReceiveDispatch requestSliceDsp(int sliceId, const SliceDspRequest& request) override;
    ReceiveDispatch requestSliceAudio(int sliceId, const SliceAudioRequest& request) override;
    // brick3-angelia: receive squelch, host-side in WDSP as Hl2Backend's
    // (WdspChannel::setSquelch()): FM's noise squelch, a level squelch in
    // AM/SAM/DSB/LSB/USB, nothing in CW and the data modes. The level
    // squelch opens where the S-meter reads -140 + 0.7 * level dBm. Manual
    // only: the gate reads the whole passband and the panadapter one point's
    // bandwidth, so a margin above the pan's floor is not a level the gate
    // can hold (capabilities().squelchLevelScale).
    ReceiveDispatch requestSliceSquelch(int sliceId, const SliceSquelchRequest& request) override;
    void setSliceSquelch(int sliceId, bool on, int level) override;
    // brick3-angelia: ANT1..ANT3 on ANGELIA/ORION boards (Alex0 bits 24-26).
    ReceiveDispatch requestSliceRxAntenna(int sliceId, const QString& antenna) override;
    // brick3-angelia: host-side Auto AGC-T (the radio has none). The knob
    // follows the noise floor WDSP's S-meter reads; see autoAgcKnobFor().
    ReceiveDispatch requestSliceAgcThresholdMode(int sliceId, AgcThresholdMode mode) override;

    // The receiver's own audio stage. Without these three the operator's mute,
    // AF fader and balance moved and nothing happened: IRadioBackend's defaults
    // are no-ops, and a backend that demodulates on this host is the only thing
    // in the chain that can apply them (see AnanSliceAudio.h).
    void setSliceAudioMute(int sliceId, bool mute) override;
    void setSliceAudioGain(int sliceId, int gainPercent) override;
    void setSliceAudioPan(int sliceId, int panPercent) override;

    // The RADIO's own output level and mute, applied to the speaker stream only.
    //
    // Separate from the per-slice stage above, and both apply: the slice stage is
    // how loud this receiver is wherever it is heard, this is how loud the RADIO
    // plays. Without it the radio gets the samples at whatever the slice stage
    // left them -- full scale by default -- and the G2 has no speaker volume
    // register to turn that down with.
    void setLineoutGain(int percent) override;
    void setLineoutMute(bool mute) override;
    void setPanCenter(const QString& panId, double hz, PanCenterIntent intent) override;
    void setPanBandwidth(const QString& panId, double hz) override;
    void setPanFrameRate(const QString& panId, int fps) override;
    // brick3-angelia: manual notches (Thetis's MNF; the app's TNF surfaces),
    // host-side in WDSP's notched bandpass, as Hl2Backend's. Ids are minted
    // here and learned from notchChanged().
    void createNotch(double centerHz, double widthHz) override;
    void setNotch(int notchId, const AetherSDR::NotchDelta& delta) override;
    void removeNotch(int notchId) override;
    void setNotchesEnabled(bool on) override;
    // The G2's receive step attenuator, presented as RF gain: -31..0 dB,
    // where -12 means 12 dB of attenuation. See the definition.
    void setPanRfGain(const QString& panId, int gainDb) override;
    void setPanAverage(const QString& panId, int average) override;
    void setPanWeightedAverage(const QString& panId, bool on) override;
    void setPanPixelWidth(const QString& panId, int pixels) override;
    void setCwPitch(int hz) override;
    void setKeying(bool key, const AetherSDR::TxCoordinator::Operation& operation, const AetherSDR::TxCoordinator::Completion& completion = {}) override;
    void invokeExtension(const QString& ns, const QString& verb,
                         quint64 requestId, const QVariant& arg = {}) override;
    // Transport snapshot for the status bar's Network field and Network
    // Diagnostics. Synchronous, and reads only the cache onLinkCounters() keeps
    // on this thread -- never the client's own counters, which belong to the
    // I/O thread. The push half is onLinkCounters(), on the client's tick.
    [[nodiscard]] LinkStats linkStats() const override;

    // ---- pure-function pieces, exposed static for testability ----
    // (matches the WdspChannel/Hl2RxDsp precedent of exposing normally-
    // internal helpers specifically so a test can pin them directly, rather
    // than only indirectly through the class's live behaviour.)

    // Mode string -> WdspChannel::Mode. Unknown strings fall back to USB
    // (matches Hl2Backend's own modeFromString fallback) -- HERMES.md §16.7's
    // own regression: "CW" (not just "CWU") and "NFM" both need entries or a
    // radio reporting either falls through silently and demodulates as SSB.
    [[nodiscard]] static WdspChannel::Mode modeFromString(const QString& mode) noexcept;

    // Per-mode default passband, operator-facing (marker-relative) Hz.
    // Vocabulary matches modeFromString's.
    [[nodiscard]] static std::pair<int, int> defaultPassbandForMode(const QString& mode) noexcept;
    // brick3-angelia: the operator's filter for this mode if one was set
    // (LSB/USB and DIGL/DIGU share one width, mirrored), else the default.
    [[nodiscard]] std::pair<int, int> passbandForMode(const QString& mode) const;

    // Every spelling modeFromString() maps, case-insensitive. The restore needs
    // it because modeFromString() falls back to USB instead of failing.
    [[nodiscard]] static bool isKnownModeString(const QString& mode) noexcept;

    // Where a session starts when this radio has no remembered state.
    static constexpr double kFirstConnectFrequencyHz = 14'175'000.0;
    static constexpr const char* kFirstConnectMode = "USB";

    // Bounded by the DDS encoding, not a band edge: tuningMinHz/MaxHz are not
    // reported. Refused, never clamped, at or above Nyquist.
    [[nodiscard]] static bool isRestorableFrequencyHz(double hz) noexcept;

    // The CW BFO offset (HERMES.md §5: "CW has no BFO unless you build one").
    // +pitchHz for CWU/CW, -pitchHz for CWL, 0 for every other mode -- zero
    // for non-CW is why every mode routes through this rather than only the
    // CW ones (entering/leaving CW must re-push the shift either way).
    [[nodiscard]] static double cwBfoOffsetHz(const QString& mode, int pitchHz) noexcept;

    // Snaps an operator-requested span to the nearest rate this radio
    // actually offers (capabilities().sampleRatesHz), by RATIO (log
    // distance), not linear -- see the definition comment for why linear
    // distance is provably wrong for these octave-spaced rates
    // (HERMES.md §15.1; mirrors Hl2Backend::nearestIqSampleRateHz()).
    // There is no continuous zoom here -- the DDC runs at exactly one of six
    // fixed rates.
    [[nodiscard]] static int nearestDdc0RateKsps(int requestedKsps) noexcept;

    // brick3-angelia: a requested panadapter FFT size as the analyzer takes
    // it: 0 (automatic) for anything not positive, else the next power of two
    // within AnanPanAnalyzer::kSmallestFftSize..kMaxFftSize.
    [[nodiscard]] static int sanitizedPanFftSize(int requested) noexcept;

    // The AGC-T knob (0..100) as WDSP gain in dB: the AGC's maximum gain, or
    // the fixed gain with AGC off. -20..120 dB is the range of deskHPSDR's AGC
    // gain slider and of Thetis's AGC-T, which sets both gains the same way.
    // With AGC on, the AGC reaches its maximum only on weak signals, so a high
    // setting cannot clip a strong one. With AGC off, the knob is the gain.
    static constexpr double kAgcKnobMinDb = -20.0;
    static constexpr double kAgcKnobDbPerStep = 1.4;
    [[nodiscard]] static constexpr double agcKnobDb(int level) noexcept
    {
        return kAgcKnobMinDb + kAgcKnobDbPerStep * std::clamp(level, 0, 100);
    }
    // 71 = 79.4 dB, the step nearest deskHPSDR's 80 dB AGC gain default.
    static constexpr int kDefaultAgcThreshold = 71;
    // 29 = 20.6 dB, the step nearest Thetis's 20 dB fixed gain default
    // (deskHPSDR has no AGC-off gain control).
    static constexpr int kDefaultAgcOffLevel = 29;

    // Live operator AGC state as setSliceAgc()/requestSliceAgc() last stored
    // it, which connectRadio() and beginRateChange() build the DSP config
    // from. Exposed read-only for testing without a live radio (matches
    // WdspChannel's own *ForTest accessor convention), not part of the
    // operator-facing seam.
    [[nodiscard]] int agcModeForTest() const noexcept { return m_agcMode; }
    [[nodiscard]] double agcMaxGainDbForTest() const noexcept { return agcKnobDb(m_agcThreshold); }
    [[nodiscard]] double agcFixedGainDbForTest() const noexcept { return agcKnobDb(m_agcOffLevel); }

    // Auto AGC-T (brick3-angelia): the knob that puts the band noise at
    // kAutoAgcNoiseOutDbfs in the audio. WDSP's S-meter and its AGC read the
    // same samples (RXA.c xrxa: smeter after nbp0, agc after bp1, unity in
    // between; a -60 dBFS tone meters -60.0), and below the AGC's knee the
    // output is input plus the maximum gain, so with the noise metering N dB
    // the gain is kAutoAgcNoiseOutDbfs + 3 - N; the 3 dB is the real audio
    // against the complex rms. Offline WDSP harness, noise metering -101.4:
    // 62 dB leaves it at -42 dBFS and S9+ speech at -10..-6 dBFS, while the
    // AGC's own knee (90 dB there) would hold the noise at -14 dBFS, the
    // flat, fatiguing sound of an AGC-T set too high.
    static constexpr double kAutoAgcNoiseOutDbfs = -42.0;
    static constexpr double kComplexToRealRmsDb = 3.0;
    [[nodiscard]] static constexpr int autoAgcKnobFor(double noiseFloorDbfs) noexcept
    {
        const double maxGainDb = kAutoAgcNoiseOutDbfs + kComplexToRealRmsDb - noiseFloorDbfs;
        const double knob = (maxGainDb - kAgcKnobMinDb) / kAgcKnobDbPerStep;
        return std::clamp(static_cast<int>(knob + 0.5), 0, 100);
    }
    // The live receive state as applyLiveRxState() writes it into the DSP config.
    [[nodiscard]] AnanRxDsp::Config liveRxConfigForTest() const
    {
        AnanRxDsp::Config config;
        applyLiveRxState(config);
        return config;
    }
    [[nodiscard]] int attenuationDbForTest() const noexcept { return m_attenuationDb; }
    [[nodiscard]] bool noiseBlankerOnForTest() const noexcept
    {
        return m_nbKind != AetherSDR::NoiseBlankerKind::Off;
    }
    [[nodiscard]] AetherSDR::NoiseBlankerKind noiseBlankerKindForTest() const noexcept
    {
        return m_nbKind;
    }
    [[nodiscard]] int noiseBlankerLevelForTest() const noexcept { return m_nbLevel; }
    [[nodiscard]] bool squelchOnForTest() const noexcept { return m_squelchOn; }
    [[nodiscard]] int squelchLevelForTest() const noexcept { return m_squelchLevel; }
    [[nodiscard]] AetherSDR::NoiseBlankerFill noiseBlankerFillForTest() const noexcept
    {
        return m_nbFill;
    }
    // The radio's own output level/mute as the lineout seam last set them. Same
    // *ForTest convention as the accessors above: read-only, not part of the seam.
    [[nodiscard]] int lineoutGainPercentForTest() const noexcept { return m_lineoutGainPercent; }
    [[nodiscard]] bool lineoutMutedForTest() const noexcept { return m_lineoutMuted; }
    // What applyRestoredState() accepted; connectRadio() consumes it behind a socket.
    [[nodiscard]] double restoredFrequencyHzForTest() const noexcept { return m_restoredFreqHz; }
    [[nodiscard]] QString restoredModeForTest() const { return m_restoredMode; }
    // Drives the S-meter path as AnanRxDsp::meterUpdate would, so the
    // smoothing and publish tick can be tested without a live radio.
    void feedMeterForTest(float dbfs) { onDspMeter(dbfs); }
    // The smoothed value itself, whether or not the publish tick has come
    // round. Without this the ballistics can only be observed through the
    // tick, which makes the assertion depend on wall-clock timing and lets a
    // test pass with the smoothing replaced by a plain assignment.
    [[nodiscard]] double sMeterDbmForTest() const noexcept { return m_sMeter.value(); }

private:
    friend class AnanNoiseBlankerTestAccess;
    friend class AnanLinkStatsTestAccess;
    friend class AnanSupplyRailTestAccess;
    void beginDspSetup();
    void finishDspSetup(quint64 generation, bool ok, const QString& error);
    // The "restart P2Client with m_pendingParams, then retune" half of what
    // finishDspSetup() used to do inline -- extracted so beginRateChange()'s
    // background-rebuild path (finishRateChange()) can reuse it too, instead
    // of a second copy. Reads m_rateChanging to decide which connect timeout
    // to pass (see P2Client::start()'s own comment).
    void startP2ClientSession(quint64 generation);
    // Set by connectRadio() just before beginDspSetup(); read back by
    // finishDspSetup() once AnanRxDsp::configure() completes, so the P2Client
    // session isn't started until the DSP chain that will consume its IQ
    // actually exists.
    P2Client::Params m_pendingParams;
    AnanRxDsp::Config m_pendingDspConfig;
    // Panadapter points from the panel width (setPanPixelWidth()). Kept here
    // as well as in AnanRxDsp so a connect that happens after the width
    // arrives still builds the analyzer at that count.
    int m_panPoints = static_cast<int>(kDroopCorrectionFftSize);
    void emitSliceState();
    void emitPanState();

    // Convert one demodulated block to the radio's speaker stream and hand it to
    // P2Client. Takes the SAME buffer the speakers get, after the receiver's
    // audio stage, so mute and the fader reach the radio as well.
    //
    // Does nothing unless the session was started with speaker audio enabled.
    void sendSpeakerAudioToRadio(const QByteArray& stereoFloat);
    // Build or discard the two resamplers for the current audio rate. Called
    // whenever the DSP is configured or rebuilt, because the rate is theirs to
    // follow and a rate change invalidates their filter state.
    void resetSpeakerResamplers();
    // Declares SLC:LEVEL and RAD:+13.8A to the meter seam; on every connect,
    // before the first reading can arrive. See its definition.
    void defineMeters();
    // One client snapshot -> one LinkStats push. The FIXED cadence is the
    // client's timer, not ours: this runs on whatever the client published, and
    // it must keep arriving after the radio goes quiet, because "nothing came
    // this second" is the observation the heartbeat's alarm path waits for
    // (IRadioBackend::LinkStats).
    void onLinkCounters(const P2Client::LinkCounters& counters);
    // The last snapshot the client published, cached on THIS thread so the
    // synchronous linkStats() getter never reads the I/O thread's counters --
    // see P2Client::LinkCounters for why that read would be a race.
    P2Client::LinkCounters m_linkCounters;
    bool m_linkCountersSeen = false;
    // rxBytes at the previous snapshot, for LinkStats::alive -- a link that is
    // bound and counting but has gone quiet must read as dead, which a
    // cumulative total alone cannot say.
    quint64 m_lastSnapshotRxBytes = 0;
    // One WDSP S-meter reading (dBFS) -> dBm, smoothed, published on a tick.
    void onDspMeter(float dbfs);
    // brick3-angelia: Auto AGC-T. setAgcThresholdAuto() switches the mode
    // and persists it; applyAutoAgcThreshold() runs on m_autoAgcTimer.
    void setAgcThresholdAuto(bool on);
    void applyAutoAgcThreshold();
    // The same smoother Hl2Backend publishes through, so the two receivers'
    // needles move alike by construction -- see WdspSMeter.h.
    SMeterSmoother m_sMeter;
    // One status packet's supply-rail count -> RAD:+13.8A, in volts, as the MEAN
    // of every count since the last publication, once a second. A plain mean
    // and not the S-meter's attack/decay: the readout wants the rail's level,
    // and one packet alone wanders by about +/-11 counts (+/-0.3 V) on the bench.
    void onSupplyRailCounts(int counts);
    // The clocked half of the above, publication included, so the window and
    // the meter it feeds are testable against an injected time.
    void onSupplyRailCountsAt(int counts, qint64 nowMs);
    // Every 1000 ms: under MeterModel::kVitalsFreshMs (1500), so a live rail
    // never reads as stale between publications, and about five packets at
    // p2app's 200 ms receive cadence.
    static constexpr qint64 kSupplyRailWindowMs = 1000;
    // Empties the window. Called on disconnect and at linkUp, so a new session
    // never averages in the last one's counts.
    void resetSupplyRailWindow();
    QElapsedTimer m_supplyRailClock;
    // -1 = no window open.
    qint64 m_supplyRailWindowStartMs = -1;
    qint64 m_supplyRailCountSum = 0;
    int m_supplyRailSamples = 0;
    // Leading+trailing throttle around applyTuneToRadioAndPan() -- see
    // setSliceFrequency()'s comment for why an unthrottled click/drag-tune
    // gesture is a problem for this backend specifically.
    void scheduleTuneApply();
    void applyTuneToRadioAndPan();
    // Live rate change for setPanBandwidth() -- see its own definition
    // comment for why this is a full stop+reconfigure+restart of the
    // P2Client session rather than an in-place resend while streaming.
    // Kicks off the background DSP rebuild (AnanRxDsp::buildChannel(), on
    // m_dspBuildThread) and returns immediately -- the old channel and the
    // live P2Client session both keep running, undisturbed, for the whole
    // build. finishRateChange() is what actually stops/restarts P2Client,
    // once the new channel already exists.
    void beginRateChange(int newRateKsps);
    // brick3-angelia: ask for a DDC rate (snapped to one the radio runs). The
    // Rate control and the calibration sweep come through here; zooming does
    // not -- see m_viewSpanHz.
    void requestSampleRateKsps(int requestedKsps);
    // Runs once the background build (started by beginRateChange()) has
    // finished and been installed. Same body as the old beginRateChange()'s
    // second half, just triggered after the rebuild instead of before it --
    // see its own definition comment for the retry-window fix folded in
    // here too.
    void finishRateChange(quint64 generation, bool ok, const QString& error);

    // Identity guard for the droop-calibration write, in front of
    // AnanDroopCalibrator::saveTables() (which owns the merge, the schema
    // guard and the codec). Returns an empty string on success, or the
    // operator-facing reason it did not persist -- never void: the caller
    // reports the outcome to the dialog and the bridge, which both used to
    // claim success regardless.
    [[nodiscard]] QString persistDroopTables(
        const QMap<int, anan::DroopCorrectionTable>& tables);
    // If a zoom request arrived while a previous one was still in flight
    // (m_pendingBandwidthKsps != 0), starts it now. Called from every path
    // that clears m_rateChanging -- linkUp success and both finishDspSetup()
    // failure branches -- so a fast zoom sweep converges on the operator's
    // LATEST request instead of stalling on whichever one happened to be
    // running when they stopped clicking.
    void retryPendingRateChange();
    [[nodiscard]] double cwBfoHz() const noexcept { return cwBfoOffsetHz(m_mode, m_cwPitchHz); }
    // The operator's mode, passband, AGC and noise blanker, written into a DSP
    // config. connectRadio() and beginRateChange() both build theirs with it.
    void applyLiveRxState(AnanRxDsp::Config& config) const;
    // Re-push mode + filter (with the CW BFO folded in) + shift, in that
    // order, to m_dsp. HERMES.md §16.7: mode changes must re-push the
    // passband, every time, not only when its value changed.
    void pushModeFilterShift();
    // brick3-angelia: the slice moves inside a fixed DDC span (Thetis CTUN,
    // Hl2Backend). WDSP's shift is slice - NCO, then the CW BFO (HERMES.md
    // §16.2, §5); pushShift() queues it to the DSP.
    [[nodiscard]] double usableHalfSpanHz() const noexcept
    {
        return m_pendingParams.rx1RateKsps * 1000.0 / 2.0 * kUsablePassbandFraction;
    }
    [[nodiscard]] double rxShiftHz() const noexcept
    {
        return (m_sliceFreqHz - m_ncoHz) - cwBfoHz();
    }
    void pushShift();

    // ---- brick3-angelia: the view, separate from the sample rate ----
    // The DDC rate says how much spectrum is captured. What the panadapter
    // SHOWS is a view inside it -- a centre and a span no wider than the rate
    // -- and zooming and panning move the view, not the rate (Thetis). The
    // DDC is retuned only when the view would leave the captured band.
    [[nodiscard]] double rateHz() const noexcept
    {
        return m_pendingParams.rx1RateKsps * 1000.0;
    }
    // The span shown: the whole band while m_viewSpanHz is 0 or no narrower
    // than it, and never below kMinViewFraction of it.
    [[nodiscard]] double viewSpanHz() const noexcept;
    [[nodiscard]] double viewCenterHz() const noexcept
    {
        return m_viewCenterHz > 0.0 ? m_viewCenterHz : m_ncoHz;
    }
    // How far the view's centre may sit from the DDC's. The screen shows the
    // middle (1 - 2 * kPanEdgeCropFraction) of the reported span, and that
    // part stays inside the band's flat middle -- clear of the outer
    // kPanEdgeZoneFraction each side, where the DDC rolls off and the trace
    // is a correction, not a measurement -- so a zoomed view never shows the
    // band's edge. Zero for the whole-band view, and for any view too wide to
    // fit in that middle: those sit centred on the DDC and show the edges as
    // the whole band always has.
    [[nodiscard]] double viewSlackHz() const noexcept;
    // setPanCenter() and setPanBandwidth() only note what was asked; one
    // queued commitViewRequest() applies both, so a zoom about the cursor --
    // a centre and a span in one request -- is a single move of the view.
    void queueViewCommit();
    void commitViewRequest();
    double m_pendingViewCenterHz = 0.0;   // 0 = none asked
    double m_pendingViewSpanHz = -1.0;    // below 0 = none asked
    bool m_viewCommitQueued = false;
    // The view was moved: retune the DDC just far enough to keep it inside.
    void moveDdcToHoldView();
    // The DDC was moved: pull the view back inside the captured band.
    void pullViewIntoBand();
    // After either: a slice left outside the usable band comes along, as it
    // does when the pan is dragged at the whole-band view.
    void keepSliceInUsableBand();
    // The view as fractions of the captured band (AnanPanView.h): 0..1 for
    // the whole of it.
    [[nodiscard]] std::pair<double, double> viewFractions() const noexcept;
    // ...and sent to the DSP.
    void pushView();
    // The zoom limits for the rate in force: kMinViewFraction of it to all of it.
    void emitPanLimits();

    QThread* m_ioThread = nullptr;
    P2Client* m_client = nullptr;    // lives on m_ioThread; nullptr parent (moveToThread requires it)
    AnanRxDsp* m_dsp = nullptr;      // lives on m_ioThread; nullptr parent
    // Build-only thread for a rate change's background AnanRxDsp::buildChannel()
    // call -- never touches P2Client or the real-time IQ path, so it can never
    // starve the keepalive the way blocking m_ioThread with the same work
    // used to (see beginRateChange()'s comment). m_dspBuildContext is a bare
    // QObject living there, used purely as an invokeMethod thread-affinity
    // target -- it owns no state of its own.
    QThread* m_dspBuildThread = nullptr;
    QObject* m_dspBuildContext = nullptr;
    bool m_connected = false;
    // Bumped on every connectRadio()/disconnectRadio(); a finishDspSetup()
    // callback checks the generation it captured against the current one and
    // bails silently if they differ -- the guard against a slow first
    // AnanRxDsp::configure() (FFTW PATIENT planning, ~19s cold per
    // HERMES.md §22.3) completing after a newer connect or a disconnect.
    quint64 m_connectGeneration = 0;

    // Filled in from P2Client::discoveryInfoReceived() -- see capabilities()'s
    // own comment. Reset at the start of connectRadio() so a fresh connect
    // (possibly to a DIFFERENT host) never reports a prior radio's identity
    // before its own reply lands.
    bool m_discoveryInfoReceived = false;
    quint8 m_discoveredBoardId = 0;
    quint8 m_discoveredFirmwareVer = 0;
    quint8 m_discoveredNumDdc = 0;

    // scheduleTuneApply()'s leading+trailing throttle state. Lives on this
    // object's own thread (the GUI thread), not m_ioThread.
    static constexpr int kTuneThrottleMs = 33;   // ~30 Hz ceiling on real DDC retunes
    QTimer* m_tuneThrottleTimer = nullptr;
    // Active ADC attenuation, mirrored in m_pendingParams for session restarts.
    // Captured/restored only through RadioStateMemory's rfGain extension.
    int m_attenuationDb = 0;
    bool m_tunePendingApply = false;

    // setPanBandwidth() serialization: one rate change at a time, gated on
    // m_rateChanging clearing (linkUp, or a finishDspSetup() failure), not a fixed
    // cooldown -- cold FFTW planning can take seconds, and overlapping restarts trip
    // P2Client's 2 s connect watchdog. At most one request is remembered while busy;
    // a newer one supersedes it.
    int m_pendingBandwidthKsps = 0;   // 0 = none pending
    // Set from beginRateChange() through the next linkUp (or a failure path). Also
    // suppresses connected()/disconnected() churn: a zoom is not a disconnect.
    bool m_rateChanging = false;
    // Audio mute after a rate-change linkUp so a fresh WdspChannel settles before
    // live RF (see the linkUp handler). Display is unaffected by the mute.
    static constexpr int kRateChangeAudioSettleMs = 300;
    // Connect timeout for the rate-change restart path only: the just-stopped radio
    // needs settle time to re-arm its DDC pipeline, and the 2 s first-connect
    // default fires spurious connection errors on it.
    static constexpr int kRateChangeConnectTimeoutMs = 6000;
    // Settle window after a LIVE rate change: the new WdspChannel is installed
    // before the radio switches, so it briefly sees old-rate samples until p2app's
    // register write lands; audio stays muted across it (finishRateChange()). The
    // session never stops, so no restart settle is needed. 250 ms is a starting
    // value, bench-clean at 48 and 1536 ksps. It also replaces
    // kRateChangeAudioSettleMs here, but measured from the swap, so WDSP gets 250 ms
    // minus the handoff latency; if a rate change thumps on unmute, split the two
    // settles before raising this (#5547).
    static constexpr int kRateChangeLiveSettleMs = 250;

    // DDC-Specific resend offsets (ms) inside the settle window, on top of the
    // immediate send: a lost datagram costs nothing audible (see finishRateChange()).
    static constexpr int kRateChangeResendMs[] = {60, 140};

    QString m_mode = QStringLiteral("USB");
    int m_filterLowHz = 100;
    int m_filterHighHz = 2900;
    // brick3-angelia: the operator's filter per mode family, kept in the
    // upper-sideband orientation for SSB and DIG (see passbandForMode()),
    // saved in the operating state's extension "filters".
    QMap<QString, std::pair<int, int>> m_filterByMode;
    void rememberFilter();
    int m_cwPitchHz = 600;
    double m_sliceFreqHz = 0.0;
    // brick3-angelia: RX1's DDC centre, i.e. the pan centre. It stays put
    // while the slice tunes inside kUsablePassbandFraction of the half-span
    // and recentres on the slice beyond it, as Hl2Backend's ncoHz does.
    double m_ncoHz = 0.0;
    static constexpr double kUsablePassbandFraction = 0.8;   // Hl2Backend's
    // brick3-angelia: the view (see viewSpanHz()). Centre in Hz, 0 until a
    // session sets it; span in Hz, 0 = the whole captured band.
    double m_viewCenterHz = 0.0;
    double m_viewSpanHz = 0.0;
    // The span the last session ended with (the "panView" state extension).
    double m_restoredViewSpanHz = 0.0;
    // The view as it stood before a rate change, for its failure path.
    double m_preRateChangeViewCenterHz = 0.0;
    double m_preRateChangeViewSpanHz = 0.0;
    // The share of a reported span the display drops from EACH side on this
    // radio: gui/SpectrumPreviewLogic.h's kEdgeTaperFraction, restated here
    // because nothing below the seam may include a GUI header.
    static constexpr double kPanEdgeCropFraction = 0.04;
    // The share of the captured band, at EACH edge, inside the DDC's roll-off
    // (AnanDroopDefaults: the correction reaches about 8.6 % in). Measured on
    // a Brick3 (gateware 121): the shipped correction, derived on another
    // board, overshoots by some 10 dB at 4-7 % from the edge.
    static constexpr double kPanEdgeZoneFraction = 0.09;
    // The narrowest view, as a share of the captured band.
    static constexpr double kMinViewFraction = 1.0 / 512.0;
    // What applyRestoredState() accepted, held for connectRadio(); kept apart
    // from the live m_sliceFreqHz/m_mode. Zero/empty: nothing restored.
    double m_restoredFreqHz = 0.0;
    QString m_restoredMode;
    // Live AGC state in AGC-T knob units (agcKnobDb() converts). Both
    // connectRadio() and beginRateChange() build the DSP config from it.
    // applyRestoredState() sets it before every connect from this radio's
    // stored AGC (the Agc domain), or the defaults when it has none, so
    // another ANAN does not inherit it the way it inherits the noise blanker
    // below. emitSliceState() publishes it so the knob shows what the DSP runs.
    int m_agcMode = 3;
    int m_agcThreshold = kDefaultAgcThreshold;
    int m_agcOffLevel = kDefaultAgcOffLevel;
    // brick3-angelia: Auto AGC-T state. The floor is the S-meter's in-filter
    // level at the ADC (no attenuation offset: the AGC sees these samples),
    // falling at once and rising at kNoiseFloorRiseDbPerSec, so speech
    // pauses set it and a steady carrier only pulls it up slowly. NaN until
    // the first reading of a session.
    bool m_agcThresholdAuto = false;
    double m_noiseFloorDbfs = std::numeric_limits<double>::quiet_NaN();
    QTimer* m_autoAgcTimer = nullptr;
    static constexpr int kAutoAgcIntervalMs = 500;
    static constexpr int kAutoAgcKnobDeadband = 2;        // ~2.8 dB: no chatter
    static constexpr double kNoiseFloorRiseDbPerSec = 1.0;
    static constexpr double kMeterReadingsPerSec = 47.0;  // WdspSMeter::emitEveryBlocks
    // The DDC rate the operator last zoomed to (RestoredRadioState::
    // sampleRateHz), snapped to kDdc0RatesKsps; 0 = none remembered.
    int m_restoredRateKsps = 0;
    // Noise blanker as setSliceNoiseBlanker() last stored it. Both
    // connectRadio() and beginRateChange() build the DSP config from it, as
    // they do the AGC above. emitSliceState() also publishes the pair when a
    // different radio gets a fresh slice, keeping its NB button in agreement
    // with the retained setting. Defaults match AnanRxDsp::Config's.
    AetherSDR::NoiseBlankerKind m_nbKind = AetherSDR::NoiseBlankerKind::Off;
    int m_nbLevel = 50;
    AetherSDR::NoiseBlankerFill m_nbFill = AetherSDR::kDefaultNoiseBlankerFill;
    // brick3-angelia: WDSP's NR2 (EMNR) as the client last asked for it
    // ("anan" "nr2.set"); retained like the blanker. Engaged = on.
    std::optional<WdspChannel::Emnr> m_nr2;
    // ---- brick3-angelia: manual notches ----
    // The list, in WDSP's order: position here is the index AnanRxDsp and
    // WDSP use. Cleared at every connect; the last session's come back
    // through replayRestoredNotches() once the link is up.
    struct NotchRecord {
        int id = 0;
        double centerHz = 0.0;
        double widthHz = 0.0;
        bool active = true;
    };
    std::vector<NotchRecord> m_notches;
    std::vector<NotchRecord> m_restoredNotches;
    bool m_notchReplayPending = false;
    int m_nextNotchId = 1;
    bool m_notchesEnabled = true;
    // The narrowest notch the receive filter can make at its 2048 taps
    // (WdspChannel::minimumNotchWidthHz()); narrower requests are widened to
    // it and reported at that width, so the marker shows what is heard.
    static constexpr double kMinNotchWidthHz = 200.0;
    static constexpr double kMaxNotchWidthHz = 6000.0;
    static constexpr int kMaxNotches = 1024;   // WDSP's notch database (RXA.c)
    [[nodiscard]] int notchIndexFor(int notchId) const noexcept;
    // The DDC's centre to the DSP: notch centres are absolute, and WDSP needs
    // the tuned frequency to place them. After every retune.
    void pushNotchTune();
    void replayRestoredNotches();

    // brick3-angelia: WDSP's automatic notch filter (ANF) as last asked for
    // through requestSliceDsp(); retained like the blanker.
    bool m_anfOn = false;
    int m_anfLevel = 50;
    // brick3-angelia: the squelch as last asked for (setSliceSquelch());
    // emitSliceState() publishes it and the operating state's "squelch"
    // extension keeps it per radio. 20 is SliceModel's default level.
    static constexpr int kDefaultSquelchLevel = 20;
    bool m_squelchOn = false;
    int m_squelchLevel = kDefaultSquelchLevel;
    // The level squelch gates raw dBFS at the ADC, after the step
    // attenuator; the S-meter and the panadapter add the attenuation back.
    // Taking it off the threshold keeps the gate where the operator set it
    // on those scales when the attenuator moves.
    [[nodiscard]] double squelchLevelOffsetDb() const noexcept
    {
        return -static_cast<double>(m_attenuationDb);
    }
    void pushSquelch();
    // brick3-angelia: the panadapter analyzer's FFT size (0 = automatic) and
    // window, as AnanSettings holds them: loaded at connect, moved by the
    // "pan.set" verb. A new size rebuilds the DSP; a window applies live.
    int m_panFftSize = 0;
    int m_panWindow = AnanPanAnalyzer::kDefaultWindow;
    // brick3-angelia: the trace's attack and decay, ms, as AnanSettings holds
    // them: loaded at connect, moved by "pan.set", applied live.
    int m_panAttackMs = 0;
    int m_panDecayMs = 0;
    int m_panFlattenPct = 0;   // brick3-angelia: Flatten, % (AnanPanView.h)
    // brick3-angelia: the receive-audio cushion this radio declares, ms
    // (AnanSettings::rxAudioCushionMs()); 100 until a connect says otherwise.
    int m_rxAudioCushionMs = 100;

    // The receiver audio stage as last set; emitSliceState() publishes these.
    // Retained across rate changes and reconnects (like the blanker and AGC).
    // 100/50 = unity, centred.
    bool m_sliceAudioMuted = false;
    int m_sliceAudioGainPercent = 100;
    int m_sliceAudioPanPercent = 50;

    // ---- speaker stream (DDC Audio, PC -> radio) ----
    //
    // Live copy of the connect-time parameter, so the audio path tests one bool
    // rather than reaching into m_pendingParams on every block.
    bool m_speakerAudioEnabled = false;
    // The radio's own output level/mute. 50 matches RadioModel's default, which it
    // resets to on every radio change; the two halves must agree. Also the
    // level applyRestoredState() returns to for a radio with none stored.
    static constexpr int kDefaultLineoutGainPercent = 50;
    int m_lineoutGainPercent = kDefaultLineoutGainPercent;
    bool m_lineoutMuted = false;
    // One resampler PER CHANNEL, never processStereoToStereo(), which averages to
    // mono and would undo the balance (as the engine's own output resampler,
    // docs/architecture/audio-pipeline.md).
    std::unique_ptr<Resampler> m_speakerResampleL;
    std::unique_ptr<Resampler> m_speakerResampleR;
    // Deinterleave/convert scratch, retained so a steady stream does not
    // allocate once per block in the audio path.
    std::vector<float> m_speakerSrcL;
    std::vector<float> m_speakerSrcR;
    QByteArray m_speakerOutL;
    QByteArray m_speakerOutR;
    std::vector<qint16> m_speakerInterleaved;

    // Fixed identifiers -- Phase 1b is exactly one slice, one pan.
    static constexpr int kSliceId = 0;
    static const QString kPanId;

    // Per-radio settings identity ("anan" family): RadioConnectRequest::serial
    // (AnanDiscovery::macToSerial()), set at the top of connectRadio(); empty before
    // the first connect. isValid() needs only the family, so an empty serial would
    // silently target the family-wide row -- droopcal's `start` guards on an empty
    // radioId explicitly.
    QString m_radioSerial;
    AnanDroopCalibrator m_droopCalibrator;
    // brick3-angelia: this radio's own calibration as read from its settings
    // at connect (and as last applied). The calibrator only holds what THIS
    // session measured, so without these the status page called a saved
    // calibration "shipped default" after every restart, with the default's
    // numbers, while the saved tables were the ones on the display.
    QMap<int, DroopCorrectionTable> m_savedDroopTables;
    QString m_droopMessage;
    int m_droopPercent = 0;
    void publishDroopStatus();
    QString applyDroopTables(const QMap<int, DroopCorrectionTable>& tables);


    // RX1's rate actually running, captured at the top of
    // beginRateChange() before the pending fields are overwritten, so
    // finishRateChange()'s failure path can put them back. 0 until the first
    // rate change. See beginRateChange()'s own comment.
    int m_preRateChangeKsps = 0;
};

}  // namespace AetherSDR::anan
