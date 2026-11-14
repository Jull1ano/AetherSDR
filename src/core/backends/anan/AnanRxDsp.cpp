#include "core/backends/anan/AnanRxDsp.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QMetaType>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

Q_LOGGING_CATEGORY(lcAnanRxDsp, "aether.anan.rxdsp")

namespace AetherSDR::anan {

AnanRxDsp::AnanRxDsp(QObject* parent) : QObject(parent)
{
    // Registered so audioReady/spectrumReady can cross a thread boundary
    // once this object is moved onto its own DSP thread (queued connections).
    qRegisterMetaType<std::vector<float>>("std::vector<float>");
    // Control verbs arrive here as queued invokeMethod calls from the GUI
    // thread; without this the Mode argument has no metatype and Qt drops
    // the call with only a warning.
    qRegisterMetaType<WdspChannel::Mode>("WdspChannel::Mode");
}

AnanRxDsp::~AnanRxDsp()
{
    // Stop the channel first so WdspChannel::close() skips WDSP's 100 ms
    // stop-and-flush timeout (docs/HERMES.md §13 item 9b); nothing feeds it after
    // this, so no drain runs. setRunning() refuses while a processIq() callback is in
    // flight, which cannot happen on this (the feeding) thread -- a false means that
    // assumption broke, so log it.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcAnanRxDsp)
            << "could not stop the WDSP channel before destroying it: a "
               "processIq callback was in flight. Teardown will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
}

bool AnanRxDsp::configure(const Config& config, std::string* error)
{
    // Guard the rate/block inputs before the block-size division below.
    // Reject at the boundary rather than crash or build a nonsensical WDSP
    // channel (Principle VII). buildChannel() repeats this same guard so it
    // stays safe called on its own (a rate change never routes through
    // configure() -- see the header comment on both).
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        if (error) {
            *error = "AnanRxDsp: input/audio sample rate and DSP block size "
                     "must all be positive";
        }
        return false;
    }

    m_config = config;
    RebuildResult result = buildChannel(config);
    if (!result.channel || !result.analyzer) {
        if (error)
            *error = result.error;
        return false;
    }
    installChannel(std::move(result));
    return true;
}

AnanRxDsp::RebuildResult AnanRxDsp::buildChannel(const Config& config)
{
    RebuildResult result;

    // Same guard as configure() -- see its comment. Repeated here because
    // this is the entry point a rate change actually calls, on a thread
    // that isn't this object's own.
    if (config.inputSampleRateHz <= 0 || config.audioSampleRateHz <= 0
        || config.dspBlockSize <= 0) {
        result.error = "AnanRxDsp: input/audio sample rate and DSP block size "
                       "must all be positive";
        return result;
    }

    WdspChannel::Config wc;
    wc.direction = WdspChannel::Direction::Receive;
    wc.inputBlockSize = static_cast<std::size_t>(config.dspBlockSize);
    // dsp_size describes the same span of time as in_size, but at dsp_rate:
    //     dsp_insize = dsp_size * (in_rate / dsp_rate)   [WDSP channel.c]
    // so dsp_size = in_size * dsp_rate / in_rate makes WDSP consume exactly
    // one of our input blocks per DSP pass.
    wc.dspBlockSize = static_cast<std::size_t>(config.dspBlockSize) *
                      static_cast<std::size_t>(kWdspDspSampleRateHz) /
                      static_cast<std::size_t>(config.inputSampleRateHz);
    wc.inputSampleRate = config.inputSampleRateHz;
    // The WDSP DSP rate is 48 kHz and is NOT the audio rate -- see
    // Hl2RxDsp::configure()'s comment for the reference-client precedent
    // this matches (Thetis, pihpsdr both hold RXA's internal rate at
    // 48 kHz regardless of the radio's own IQ rate).
    wc.dspSampleRate = kWdspDspSampleRateHz;
    wc.outputSampleRate = config.audioSampleRateHz;
    wc.mode = config.mode;
    wc.filterLowHz = config.filterLowHz;
    wc.filterHighHz = config.filterHighHz;
    wc.agcMode = config.agcMode;
    wc.maximumAgcGainDb = config.maximumAgcGainDb;
    wc.agcFixedGainDb = config.agcFixedGainDb;
    wc.blockForOutput = config.blockForOutput;
    wc.noiseBlanker = config.noiseBlanker;
    wc.noiseBlankerLevel = config.noiseBlankerLevel;
    wc.noiseBlankerFill = config.noiseBlankerFill;
    wc.emnr = config.emnr;
    wc.autoNotch = config.autoNotch;
    wc.autoNotchLevel = config.autoNotchLevel;
    wc.squelchEnabled = config.squelchEnabled;
    wc.squelchLevel = config.squelchLevel;
    wc.levelSquelchOffsetDb = config.squelchLevelOffsetDb;
    // Unity, as Hl2RxDsp::kRxPanelGain: WDSP's RXA panel defaults to x4
    // (RXA.c create_panel, gain1 4.0), +12 dB past the AGC's own level. Left
    // at that default, band noise alone peaked at +8 dBFS on a Brick3 and
    // every speech peak clipped (offline WDSP harness, brick3-angelia).
    wc.rxPanelGain = 1.0;
    // brick3-angelia: mono, as Thetis defaults. WDSP's binaural default put
    // the audio's Hilbert transform in the right ear: on a Brick3 recording
    // the ears correlated 0.000 in USB and LSB, heard as fake stereo.
    wc.rxBinaural = false;
    // brick3-angelia: AGC slope 0, Thetis's default (the operator's own Thetis
    // database: udDSPAGCSlope 0). WdspChannel's 35 (3.5 dB, pihpsdr) plays a
    // signal near the threshold up to 3.5 dB under a strong one; offline on a
    // Brick3 recording at AGC-T 100 it left speech 2.3 dB under Thetis (p95
    // -9.6 vs -7.3 dBFS, peaks -2.3 vs -0.1); 0 matches Thetis to 0.1 dB.
    wc.agcSlopeDb = 0;
    // filterTaps left at WdspChannel::Config's own default (2048), which
    // sets the manual notches' width floor at 200 Hz (AnanBackend's
    // kMinNotchWidthHz). Hl2RxDsp lengthens its filter for a 50 Hz floor;
    // nothing here does yet.

    auto channel = WdspChannel::create(wc, &result.error);
    if (!channel)
        return result;
    // The analyzer reuses the channel's id as its analyzer slot: both are
    // unique for as long as the channel lives, and RebuildResult/this class
    // destroy the analyzer before the channel returns the id. Built here, on
    // the build thread, because its first SetAnalyzer() plans FFTW_PATIENT.
    AnanPanAnalyzer::Settings as;
    as.sampleRateHz = config.inputSampleRateHz;
    as.numPoints = config.panPoints;
    as.framesPerSecond = config.spectrumFps;
    // No averaging inside WDSP: the time average and the attack/decay run in
    // smoothFrame(), where their state survives a re-aimed analyzer.
    as.averageTimeMs = 0;
    as.fftSize = config.panFftSize;
    as.window = config.panWindow;
    auto analyzer = AnanPanAnalyzer::create(channel->channelId(), as, &result.error);
    if (!analyzer)
        return result;   // channel is released here, analyzer never existed
    result.outputBlockSize = channel->outputBlockSize();
    result.inputSampleRateHz = config.inputSampleRateHz;
    result.channel = std::move(channel);
    result.analyzer = std::move(analyzer);
    return result;
}

void AnanRxDsp::beginInitialBuild(const Config& config)
{
    m_config = config;
    m_rebuildInFlight = true;
}

void AnanRxDsp::beginRebuild()
{
    m_rebuildInFlight = true;
}

bool AnanRxDsp::installRebuiltChannel(RebuildResult result)
{
    m_rebuildInFlight = false;
    // buildChannel() never returns one without the other; refuse a result
    // that does rather than install a channel with no spectrum stage.
    if (!result.channel || !result.analyzer) {
        // The build failed and the old channel carries on. Notch and tune
        // changes made while it ran only reached the list (canPushToChannel()
        // was false): give them to the channel that is still playing.
        if (m_channel) {
            pushAllNotches(*m_channel);
            // Likewise a squelch change: processIqBlock() pushes it.
            m_squelchPending = true;
        }
        return false;
    }
    installChannel(std::move(result));
    return true;
}

void AnanRxDsp::installChannel(RebuildResult result)
{
    // The only update site for this field outside configure()'s own
    // synchronous m_config = config -- see RebuildResult::inputSampleRateHz's
    // comment. Must land before droopTableForRate() is ever consulted again,
    // which processIqBlock() does on every block once m_channel is swapped
    // below.
    m_config.inputSampleRateHz = result.inputSampleRateHz;
    // brick3-angelia: the view that was sent ahead for this rate, if one was.
    if (m_nextViewRateHz == m_config.inputSampleRateHz) {
        m_config.panViewLow = m_nextView.low;
        m_config.panViewHigh = m_nextView.high;
    }
    m_nextViewRateHz = 0;

    m_iqBuffer.clear();
    m_exchangeHeadWait.invalidate();   // a fresh channel starts primed
    m_i.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_q.assign(static_cast<std::size_t>(m_config.dspBlockSize), 0.0f);
    m_left.assign(result.outputBlockSize, 0.0f);
    m_right.assign(result.outputBlockSize, 0.0f);
    m_stereo.assign(result.outputBlockSize * 2, 0.0f);
    // DC blocker pole for the AUDIO rate -- the blocker runs on
    // WdspChannel's output, not its 48 kHz internal rate. Recomputed here so
    // a rate change keeps the same corner frequency instead of moving it.
    const float pole = dcBlockerPole(kDcBlockerCornerHz,
                                     static_cast<double>(m_config.audioSampleRateHz));
    m_dcBlockL.r = pole;
    m_dcBlockR.r = pole;
    // PcmFormat accepts 24000/48000 only. AnanBackend hardcodes 24000 today, so
    // this cannot fail in production — but if that rate ever moves, a silent
    // refusal here stops ANAN audio dead while the spectrum keeps updating,
    // which reads as a dead radio rather than a configuration error.
    if (!m_pcmProducer.start(PcmPurpose::Speaker, -1,
                             {m_config.audioSampleRateHz, PcmLayout::Stereo})) {
        qWarning() << "AnanRxDsp: no PCM producer for audio rate"
                   << m_config.audioSampleRateHz
                   << "Hz - RX audio will be silent on this channel";
    }
    m_dcBlockL.reset();
    m_dcBlockR.reset();

    // Same idea for the S-meter, one stage further back: the new channel's
    // own average starts at zero (create_meter() -> flush_meter()), so its
    // first readings are low for the length of the tap's time constant
    // whether or not a mute is involved. A rate change arms this twice --
    // here, and again when beginRateChange()'s settle window unmutes -- which
    // costs nothing: the window is re-armed, not accumulated.
    armMeterSettle();

    // Re-apply m_config's CURRENT values -- for configure() this is exactly
    // what buildChannel() was just given (m_config == config already); for
    // a rate-change swap it may include mode/filter/AGC changes the operator
    // made WHILE the build was in flight, which only ever reached m_config
    // (setMode() et al.'s in-flight gating) and never reached the snapshot
    // buildChannel() actually built from.
    result.channel->setMode(m_config.mode);
    result.channel->setFilter(m_config.filterLowHz, m_config.filterHighHz);
    result.channel->setAgcFixedGain(m_config.agcFixedGainDb);
    result.channel->setAgc(m_config.agcMode, m_config.maximumAgcGainDb);
    // The squelch, after setMode() above so the channel routes it to the
    // stage that mode uses. The operator may have moved it while the build
    // ran; a refusal is retried from processIqBlock().
    m_squelchPending = !result.channel->setSquelch(m_config.squelchEnabled,
                                                   m_config.squelchLevel,
                                                   m_config.squelchLevelOffsetDb);
    // A rebuild creates a fresh channel; restore the operator's current
    // slice offset rather than silently snapping the slice to centre.
    if (m_shiftHz != 0.0)
        result.channel->setShift(m_shiftHz);
    // Sent even when it matches what the channel was built with: the operator
    // may have moved the NB button while the background build ran.
    if (!result.channel->setNoiseBlanker(m_config.noiseBlanker,
                                         m_config.noiseBlankerLevel,
                                         m_config.noiseBlankerFill)) {
        qCWarning(lcAnanRxDsp) << "noise blanker refused by the rebuilt channel";
    }
    if (result.channel->config().emnr != m_config.emnr
        && !result.channel->setEmnr(m_config.emnr)) {
        qCWarning(lcAnanRxDsp) << "NR2 (EMNR) refused by the rebuilt channel";
    }
    // The manual notches: a new channel's list is empty.
    pushAllNotches(*result.channel);
    if ((result.channel->config().autoNotch != m_config.autoNotch
         || result.channel->config().autoNotchLevel != m_config.autoNotchLevel)
        && !result.channel->setAutoNotch(m_config.autoNotch, m_config.autoNotchLevel)) {
        qCWarning(lcAnanRxDsp) << "ANF refused by the rebuilt channel";
    }
    // The hold flag belongs to the channel, so a rebuild loses it. A rate
    // change swaps the channel while audio is muted for its settle window.
    result.channel->setNoiseBlankerHold(m_audioMuted);

    // Stop the OUTGOING channel before it is destroyed so close() skips the 100 ms
    // stop-and-flush timeout; this is the processIq() thread, so no block reaches it
    // in between. Not in beginRebuild(): the old channel must keep producing audio
    // through the background build. Checked as in the destructor.
    if (m_channel && !m_channel->setRunning(false)) {
        qCWarning(lcAnanRxDsp)
            << "could not stop the outgoing WDSP channel before the swap: a "
               "processIq callback was in flight. The rebuild will pay WDSP's "
               "100 ms stop-and-flush timeout.";
    }
    // The analyzer was built for m_config.spectrumFps as it stood when the
    // build began; bring it up to the operator's current rate.
    result.analyzer->setFramesPerSecond(m_config.spectrumFps);
    // The view and the screen's width as they stand now: the analyzer was
    // built for the whole band at the width of when the build began.
    retargetAnalyzer(*result.analyzer);
    m_viewDirty = false;
    // The trace belongs to the outgoing analyzer, and after a rate change to
    // another band: start it again from the first frame.
    m_traceValid = false;
    m_frameClock.invalidate();
    result.analyzer->setWindow(m_config.panWindow);
    // The operator may have picked another FFT size while the build ran.
    result.analyzer->setFftSize(m_config.panFftSize);
    // Analyzer before channel: the outgoing analyzer uses the outgoing
    // channel's id as its slot and must be destroyed before that channel
    // releases the id -- see RebuildResult.
    m_analyzer = std::move(result.analyzer);
    m_channel = std::move(result.channel);
    // The new analyzer starts at the DDC frequency now (no history to rotate).
    if (m_ddcLoHz > 0.0)
        m_analyzer->setLo(m_ddcLoHz);
    m_traceLoHz = 0.0;
    publishNoiseBlankerState();
}

void AnanRxDsp::onDdcRetuned(double hz)
{
    if (!(hz > 0.0))
        return;
    m_ddcLoHz = hz;
    // The running analyzer, also mid-rebuild: it is still the one being fed.
    if (m_analyzer)
        m_analyzer->setLo(hz);
}

void AnanRxDsp::setMode(WdspChannel::Mode mode)
{
    m_config.mode = mode;
    // See beginRebuild()'s own comment: pushing through mid-build would
    // block this thread on WDSP's process-wide setup mutex for however long
    // the background build has left. m_config still updates, so the value
    // is not lost -- installRebuiltChannel() re-applies it at the swap.
    if (m_channel && !m_rebuildInFlight)
        m_channel->setMode(mode);
}

void AnanRxDsp::setFilter(double lowHz, double highHz)
{
    m_config.filterLowHz = lowHz;
    m_config.filterHighHz = highHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setFilter(lowHz, highHz);
}

void AnanRxDsp::setAgc(int agcMode, double maximumGainDb)
{
    m_config.agcMode = agcMode;
    m_config.maximumAgcGainDb = maximumGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgc(agcMode, maximumGainDb);
}

void AnanRxDsp::setAgcFixedGain(double fixedGainDb)
{
    m_config.agcFixedGainDb = fixedGainDb;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setAgcFixedGain(fixedGainDb);
}

void AnanRxDsp::setAudioMuted(bool muted)
{
    // The mute LIFTING is the edge that matters to the S-meter: WDSP's
    // average has been integrating the zeros this class fed it for the whole
    // mute and nothing flushes it, so the first blocks after the unmute still
    // read that silence. Swallow them instead of publishing them -- see
    // WdspSMeter.h for why the guard in processIqBlock() is not enough on
    // its own.
    if (m_audioMuted && !muted)
        armMeterSettle();
    m_audioMuted = muted;
    // Muted, the channel is clocked with ZEROS. The noise blanker triggers on
    // magnitude against a running average magnitude, so silence would drag
    // that average toward zero and the first real block afterwards would look
    // like one long impulse and be blanked. Holding the stage freezes the
    // average at the pre-mute level; see WdspChannel::setNoiseBlankerHold().
    if (m_channel)
        m_channel->setNoiseBlankerHold(muted);
}

void AnanRxDsp::setNoiseBlanker(WdspChannel::NoiseBlanker kind, int level,
                                WdspChannel::NoiseBlankerFill fill)
{
    m_config.noiseBlanker = kind;
    m_config.noiseBlankerLevel = std::clamp(level, 0, 100);
    m_config.noiseBlankerFill = fill;
    if (!m_channel || m_rebuildInFlight)
        return;
    // WdspChannel refuses, rather than blocks on, a control call that races
    // another one. Logged so a refused toggle is not silent; m_config keeps
    // the request and the next rebuild applies it.
    if (!m_channel->setNoiseBlanker(m_config.noiseBlanker,
                                    m_config.noiseBlankerLevel,
                                    m_config.noiseBlankerFill)) {
        qCWarning(lcAnanRxDsp) << "noise blanker kind" << static_cast<int>(kind)
                               << "level" << m_config.noiseBlankerLevel
                               << "fill" << static_cast<int>(fill)
                               << "refused by the channel";
    }
    publishNoiseBlankerState();
}

void AnanRxDsp::setNr2(bool on, int gainMethod, int npeMethod, bool aeRun)
{
    m_config.emnr = on
        ? std::optional<WdspChannel::Emnr>(WdspChannel::Emnr{gainMethod, npeMethod, aeRun})
        : std::nullopt;
    if (!m_channel || m_rebuildInFlight)
        return;
    if (!m_channel->setEmnr(m_config.emnr)) {
        qCWarning(lcAnanRxDsp) << "NR2 (EMNR)" << (on ? "on" : "off")
                               << "refused by the channel";
    }
}

void AnanRxDsp::setAutoNotch(bool on, int level)
{
    m_config.autoNotch = on;
    m_config.autoNotchLevel = std::clamp(level, 0, 100);
    if (!m_channel || m_rebuildInFlight)
        return;
    if (!m_channel->setAutoNotch(m_config.autoNotch, m_config.autoNotchLevel)) {
        qCWarning(lcAnanRxDsp) << "ANF" << (on ? "on" : "off") << "refused by the channel";
    }
}

void AnanRxDsp::setSquelch(bool on, int level, double levelOffsetDb)
{
    m_config.squelchEnabled = on;
    m_config.squelchLevel = std::clamp(level, 0, 100);
    // WdspChannel refuses a non-finite offset; keep the last good one so the
    // on/off and the level still land.
    if (std::isfinite(levelOffsetDb))
        m_config.squelchLevelOffsetDb = levelOffsetDb;
    if (canPushToChannel())
        pushSquelchToChannel();
}

void AnanRxDsp::pushSquelchToChannel()
{
    const bool applied = m_channel->setSquelch(m_config.squelchEnabled,
                                               m_config.squelchLevel,
                                               m_config.squelchLevelOffsetDb);
    // Refused only while another control call is in flight. Said once, not
    // once per retried block.
    if (!applied && !m_squelchPending) {
        qCWarning(lcAnanRxDsp) << "squelch" << (m_config.squelchEnabled ? "on" : "off")
                               << "level" << m_config.squelchLevel
                               << "refused by the channel; retrying";
    }
    m_squelchPending = !applied;
}

void AnanRxDsp::pushAllNotches(WdspChannel& channel)
{
    // Tune frequency first: the centres are absolute, so a notch placed
    // against a stale one lands off its carrier.
    channel.setNotchTuneFrequency(m_notchTuneHz);
    for (int index = channel.notchCount() - 1; index >= 0; --index)
        channel.removeNotch(index);
    const double floorHz = channel.minimumNotchWidthHz();
    int index = 0;
    for (const Notch& notch : m_notches) {
        // A refused entry would put every later position out of step, so it
        // is added INACTIVE at a width WDSP takes rather than left out.
        if (!channel.addNotch(index, notch.centerHz, std::max(notch.widthHz, floorHz),
                              notch.active)) {
            qCWarning(lcAnanRxDsp) << "notch" << index << "at" << notch.centerHz
                                   << "Hz refused by the channel";
            if (!channel.addNotch(index, notch.centerHz, std::max(floorHz, 200.0), false))
                break;   // the lists differ from here on; nothing more to do safely
        }
        ++index;
    }
    channel.setNotchesEnabled(m_notchesEnabled && !m_notches.empty());
}

void AnanRxDsp::addNotch(int index, double centerHz, double widthHz, bool active)
{
    if (index < 0 || index > static_cast<int>(m_notches.size())
        || !std::isfinite(centerHz) || !std::isfinite(widthHz) || widthHz <= 0.0) {
        return;
    }
    m_notches.insert(m_notches.begin() + index, Notch{centerHz, widthHz, active});
    if (canPushToChannel())
        pushAllNotches(*m_channel);
}

void AnanRxDsp::editNotch(int index, double centerHz, double widthHz, bool active)
{
    if (index < 0 || index >= static_cast<int>(m_notches.size())
        || !std::isfinite(centerHz) || !std::isfinite(widthHz) || widthHz <= 0.0) {
        return;
    }
    m_notches[static_cast<std::size_t>(index)] = Notch{centerHz, widthHz, active};
    if (!canPushToChannel())
        return;
    // A drag sends these many times a second: one edit, not a rebuild of the
    // list -- unless WDSP refuses it, and then the list is put right.
    if (!m_channel->editNotch(index, centerHz,
                              std::max(widthHz, m_channel->minimumNotchWidthHz()), active)) {
        pushAllNotches(*m_channel);
    }
}

void AnanRxDsp::removeNotch(int index)
{
    if (index < 0 || index >= static_cast<int>(m_notches.size()))
        return;
    m_notches.erase(m_notches.begin() + index);
    if (canPushToChannel())
        pushAllNotches(*m_channel);
}

void AnanRxDsp::clearNotches()
{
    m_notches.clear();
    if (canPushToChannel())
        pushAllNotches(*m_channel);
}

void AnanRxDsp::setNotchesEnabled(bool on)
{
    m_notchesEnabled = on;
    if (canPushToChannel())
        m_channel->setNotchesEnabled(on && !m_notches.empty());
}

void AnanRxDsp::setNotchTuneFrequency(double tuneHz)
{
    if (!std::isfinite(tuneHz) || tuneHz == m_notchTuneHz)
        return;
    m_notchTuneHz = tuneHz;
    // Nothing to keep on its carrier while there are no notches; the next
    // pushAllNotches() sends the frequency with them.
    if (canPushToChannel() && !m_notches.empty())
        m_channel->setNotchTuneFrequency(tuneHz);
}

void AnanRxDsp::publishNoiseBlankerState()
{
    // Read the applied channel even after refusal, never the requested config.
    const int state = m_channel
        ? m_channel->config().noiseBlankerLevel
              + kNbLevelStride * static_cast<int>(m_channel->noiseBlankerKind())
              + kNbLevelStride * kNbKindStride
                    * static_cast<int>(m_channel->noiseBlankerFill())
        : -1;
    m_nbAppliedState.store(state, std::memory_order_relaxed);
}

void AnanRxDsp::setSpectrumRateFps(int fps)
{
    m_spectrumIntervalMs = fps > 0 ? (1000 / fps) : 0;
    if (fps > 0) {
        m_config.spectrumFps = fps;
        // Mid-rebuild the incoming analyzer picks the rate up at install
        // (installChannel()); the outgoing one is about to be discarded.
        if (m_analyzer && !m_rebuildInFlight)
            m_analyzer->setFramesPerSecond(fps);
    }
    // Do NOT reset the clock or the last-emit stamp -- a rate change
    // mid-stream should take effect on the next frame that comes due, not
    // grant an immediate extra one.
}

void AnanRxDsp::setSpectrumAverageMs(int ms)
{
    if (ms < 0)
        return;
    // Read by smoothFrame() on the next frame.
    m_config.spectrumAverageMs = ms;
}

void AnanRxDsp::setPanPoints(int points)
{
    if (points < 2)
        return;
    m_config.panPoints = std::min(points, AnanPanAnalyzer::kMaxPoints);
    // Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    // up at install. A running one is re-aimed by the settle check, with the
    // view: a window being dragged wider reports every width on the way.
    if (m_analyzer && !m_rebuildInFlight) {
        m_viewDirty = true;
        m_viewChangeClock.start();
    }
}

AnanRxDsp::PanWindow AnanRxDsp::analyzerWindowForView() const noexcept
{
    const double span = m_config.panViewHigh - m_config.panViewLow;
    if (span >= 1.0 - 1e-6)
        return {};   // the whole band: nothing to keep in reserve
    return {std::max(0.0, m_config.panViewLow - kWindowMargin * span),
            std::min(1.0, m_config.panViewHigh + kWindowMargin * span)};
}

int AnanRxDsp::analyzerPointsFor(const PanWindow& window) const noexcept
{
    // One analyzer point per screen point across the view, so the window,
    // being wider, takes proportionally more.
    const double viewSpan = std::max(1e-9, m_config.panViewHigh - m_config.panViewLow);
    const double ratio = (window.high - window.low) / viewSpan;
    return std::clamp(static_cast<int>(std::lround(m_config.panPoints * ratio)), 2,
                      AnanPanAnalyzer::kMaxPoints);
}

void AnanRxDsp::retargetAnalyzer(AnanPanAnalyzer& analyzer)
{
    const PanWindow target = analyzerWindowForView();
    analyzer.setViewAndPoints(target.low, target.high, analyzerPointsFor(target),
                              m_config.panPoints);
}

void AnanRxDsp::retargetAnalyzerIfSettled()
{
    if (!m_viewDirty || !m_analyzer || m_rebuildInFlight)
        return;
    if (m_viewChangeClock.isValid() && m_viewChangeClock.elapsed() < kViewSettleMs)
        return;
    m_viewDirty = false;
    // Re-aiming empties the analyzer's input, so it is skipped when what it
    // returns already covers the view at the right scale: a pan inside the
    // window's reserve costs nothing.
    const PanWindow target = analyzerWindowForView();
    const int points = analyzerPointsFor(target);
    const double haveLow = m_analyzer->windowLow();
    const double haveHigh = m_analyzer->windowHigh();
    const double wantSpan = target.high - target.low;
    const double binFraction = 1.0 / std::max(1, m_analyzer->fftSize());
    const bool covers = m_config.panViewLow >= haveLow - 1e-9
                        && m_config.panViewHigh <= haveHigh + 1e-9;
    const bool sameScale = points == m_analyzer->numPoints()
        && std::abs((haveHigh - haveLow) - wantSpan) <= 0.02 * wantSpan + 2.0 * binFraction;
    if (covers && sameScale)
        return;
    retargetAnalyzer(*m_analyzer);
}

void AnanRxDsp::setPanView(double low, double high, int sampleRateHz, double loHz)
{
    low = std::clamp(low, 0.0, 1.0);
    high = std::clamp(high, 0.0, 1.0);
    if (!(high > low))
        return;
    if (sampleRateHz > 0 && sampleRateHz != m_config.inputSampleRateHz) {
        // For the channel being built at that rate: installChannel() takes it.
        m_nextView = {low, high};
        m_nextViewRateHz = sampleRateHz;
        if (loHz > 0.0)
            m_config.panViewLoHz = loHz;
        return;
    }
    m_nextViewRateHz = 0;   // a view for the running rate supersedes a held one
    const bool loMoved = loHz > 0.0 && loHz != m_config.panViewLoHz;
    if (loHz > 0.0)
        m_config.panViewLoHz = loHz;
    if (low == m_config.panViewLow && high == m_config.panViewHigh) {
        // Only the DDC moved under a view that kept its place in the band (a
        // drag at the whole-band view): nothing to re-aim, but show the move
        // now from the trace in hand (brick3-angelia).
        if (loMoved && m_analyzer && !m_rebuildInFlight && m_traceValid)
            publishSpectrum();
        return;
    }
    m_config.panViewLow = low;
    m_config.panViewHigh = high;
    // Mid-rebuild the incoming analyzer picks the view up at install.
    if (!m_analyzer || m_rebuildInFlight)
        return;
    m_viewDirty = true;
    m_viewChangeClock.start();
    // Show the move now, from the frame in hand: what it covers lands where
    // it belongs, and the rest follows when the analyzer has been re-aimed.
    if (m_traceValid)
        publishSpectrum();
}

void AnanRxDsp::publishSpectrum()
{
    // The view is a place in absolute frequency (brick3-angelia): its
    // fractions are of the band around panViewLoHz, and the trace sits at
    // m_traceLoHz. While a retune is on its way the two differ, and the view
    // is read where it really is in the trace -- the move shows at once and
    // the spectrum stays put under it, instead of jumping by the step until
    // the frames catch up.
    const double rate = static_cast<double>(m_config.inputSampleRateHz);
    double viewLow = m_config.panViewLow;
    double viewHigh = m_config.panViewHigh;
    if (m_config.panViewLoHz > 0.0 && m_traceLoHz > 0.0 && rate > 0.0) {
        const double d = (m_config.panViewLoHz - m_traceLoHz) / rate;
        viewLow += d;
        viewHigh += d;
    }
    // The RF bounds of what goes out, for the display (0 = not known).
    const double lo = m_config.panViewLoHz > 0.0 ? m_config.panViewLoHz : m_traceLoHz;
    const double lowHz = (lo > 0.0 && rate > 0.0) ? lo + (m_config.panViewLow - 0.5) * rate : 0.0;
    const double highHz = (lo > 0.0 && rate > 0.0) ? lo + (m_config.panViewHigh - 0.5) * rate : 0.0;

    const bool isView = m_trace.size() == static_cast<std::size_t>(m_config.panPoints)
        && std::abs(m_traceWindow.low - viewLow) < 1e-9
        && std::abs(m_traceWindow.high - viewHigh) < 1e-9;
    // Flatten (drawing only) is off while the droop sweep measures: it needs
    // the floor exactly as received.
    const bool flatten = m_config.spectrumFlattenPct > 0 && !m_droopBypassed;
    if (isView) {
        if (!flatten) {
            emit spectrumReady(m_trace, lowHz, highHz);
            return;
        }
        m_publishBins = m_trace;
        flattenNoiseFloor(m_publishBins, m_config.spectrumFlattenPct / 100.0f, m_flatten);
        emit spectrumReady(m_publishBins, lowHz, highHz);
        return;
    }
    resamplePanWindow(m_trace, m_traceWindow.low, m_traceWindow.high,
                      viewLow, viewHigh,
                      static_cast<std::size_t>(std::max(2, m_config.panPoints)), m_viewBins);
    if (flatten)
        flattenNoiseFloor(m_viewBins, m_config.spectrumFlattenPct / 100.0f, m_flatten);
    emit spectrumReady(m_viewBins, lowHz, highHz);
}

void AnanRxDsp::carryOntoFrameGrid(std::vector<float>& state)
{
    // The state as it reads on the new frame's grid. Where the old grid did
    // not reach, there is nothing to carry: the new frame's own value starts
    // those points. In absolute frequency (brick3-angelia): when the DDC has
    // moved, the state's window is where it lies in the frame's band.
    const float unknown = std::numeric_limits<float>::quiet_NaN();
    const double rate = static_cast<double>(m_config.inputSampleRateHz);
    const double shift = (m_traceLoHz > 0.0 && m_frameLoHz > 0.0 && rate > 0.0)
        ? (m_traceLoHz - m_frameLoHz) / rate : 0.0;
    resamplePanWindow(state, m_traceWindow.low + shift, m_traceWindow.high + shift,
                      m_frameWindow.low, m_frameWindow.high, m_bins.size(), m_carry, &unknown);
    for (std::size_t i = 0; i < m_carry.size(); ++i) {
        if (std::isnan(m_carry[i]))
            m_carry[i] = m_bins[i];
    }
    state.swap(m_carry);
}

void AnanRxDsp::smoothFrame()
{
    // The time since the last frame, so the time constants hold whatever the
    // frame rate does. The first frame of a run assumes the display rate.
    double dt = 1.0 / std::max(1, m_config.spectrumFps);
    if (m_frameClock.isValid())
        dt = std::clamp(static_cast<double>(m_frameClock.nsecsElapsed()) / 1.0e9, 0.001, 0.25);
    m_frameClock.start();

    maskUncoveredPoints();
    const bool loMoved = m_traceLoHz > 0.0 && m_frameLoHz > 0.0
        && std::abs(m_traceLoHz - m_frameLoHz) > 1e-3;
    if (!m_traceValid || m_bins.size() < 2) {
        m_average = m_bins;
        m_trace = m_bins;
    } else if (m_trace.size() != m_bins.size() || loMoved
               || std::abs(m_traceWindow.low - m_frameWindow.low) > 1e-12
               || std::abs(m_traceWindow.high - m_frameWindow.high) > 1e-12) {
        // The analyzer was re-aimed (a zoom, a pan past its window, a new
        // width), or the DDC moved (brick3-angelia): keep what has been built
        // up, where it is in frequency, on the new grid.
        carryOntoFrameGrid(m_average);
        carryOntoFrameGrid(m_trace);
    }
    // Points the frame's samples do not all cover (the band edge a retune
    // inside the FFT window has not reached yet) hold what was built up.
    if (m_frameInvalid.size() == m_bins.size() && m_average.size() == m_bins.size()) {
        for (std::size_t i = 0; i < m_bins.size(); ++i) {
            if (m_frameInvalid[i])
                m_bins[i] = m_average[i];
        }
    }
    m_traceWindow = m_frameWindow;
    m_traceLoHz = m_frameLoHz;
    m_traceValid = true;

    averageTowards(m_average, m_bins,
                   smoothingAlpha(dt, m_config.spectrumAverageMs / 1000.0),
                   m_config.spectrumLogAverage);
    if (m_config.spectrumAttackMs <= 0 && m_config.spectrumDecayMs <= 0) {
        m_trace = m_average;
    } else {
        followWithBallistics(m_trace, m_average,
                             smoothingAlpha(dt, m_config.spectrumAttackMs / 1000.0),
                             smoothingAlpha(dt, m_config.spectrumDecayMs / 1000.0));
    }
}

void AnanRxDsp::maskUncoveredPoints()
{
    // Which frame points lie outside what the frame's samples all cover
    // (relative to m_frameLoHz). Those carry no trustworthy level: a point
    // with nothing built up there yet takes the frame's noise floor, the rest
    // hold the running average (smoothFrame()).
    const std::size_t n = m_bins.size();
    m_frameInvalid.assign(n, 0);
    const double rate = static_cast<double>(m_config.inputSampleRateHz);
    if (n < 2 || !(rate > 0.0) || (m_frameValidLowHz <= -1.0e11 && m_frameValidHighHz >= 1.0e11))
        return;
    std::size_t bad = 0;
    std::vector<float> covered;
    covered.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double fraction = m_frameWindow.low + (m_frameWindow.high - m_frameWindow.low)
            * static_cast<double>(i) / static_cast<double>(n - 1);
        const double relHz = (fraction - 0.5) * rate;
        if (relHz < m_frameValidLowHz || relHz > m_frameValidHighHz) {
            m_frameInvalid[i] = 1;
            ++bad;
        } else {
            covered.push_back(m_bins[i]);
        }
    }
    if (bad == 0 || covered.empty())
        return;
    // The frame's floor: its 20th percentile over the covered points.
    const auto k = covered.begin() + static_cast<std::ptrdiff_t>(covered.size() / 5);
    std::nth_element(covered.begin(), k, covered.end());
    const float floorDb = *k;
    for (std::size_t i = 0; i < n; ++i) {
        if (m_frameInvalid[i])
            m_bins[i] = floorDb;
    }
}

void AnanRxDsp::setSpectrumBallistics(int attackMs, int decayMs)
{
    m_config.spectrumAttackMs = std::max(0, attackMs);
    m_config.spectrumDecayMs = std::max(0, decayMs);
}

void AnanRxDsp::setSpectrumFlatten(int pct)
{
    // Read at the next publish.
    m_config.spectrumFlattenPct = std::clamp(pct, 0, 90);
}

void AnanRxDsp::setPanFftSize(int fftSize)
{
    m_config.panFftSize = std::max(0, fftSize);
    // Mid-rebuild the incoming analyzer takes it at install; a running one
    // re-plans now, in milliseconds (estimated plans, WDSP patch 24). The
    // trace is kept: the grid it sits on does not change with the FFT size.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setFftSize(m_config.panFftSize);
}

void AnanRxDsp::setPanWindow(int window)
{
    m_config.panWindow = std::clamp(window, 0, AnanPanAnalyzer::kWindowCount - 1);
    // Same deferral as setSpectrumRateFps(): the incoming analyzer picks it
    // up at install.
    if (m_analyzer && !m_rebuildInFlight)
        m_analyzer->setWindow(m_config.panWindow);
}

void AnanRxDsp::setSpectrumLogAverage(bool on)
{
    // Read by smoothFrame() on the next frame.
    m_config.spectrumLogAverage = on;
}

void AnanRxDsp::setDroopCorrectionTable(int rateKsps, const std::vector<float>& table)
{
    if (table.size() != kDroopCorrectionFftSize)
        return;
    bool valid = false;
    for (const int r : kDdc0RatesKsps)
        valid |= (r == rateKsps);
    if (!valid)
        return;
    DroopCorrectionTable t;
    std::copy(table.begin(), table.end(), t.begin());
    m_droopTables[rateKsps] = t;
}

void AnanRxDsp::setDroopCorrectionBypassed(bool bypassed)
{
    m_droopBypassed = bypassed;
}

void AnanRxDsp::clearDroopCorrectionTables()
{
    m_droopTables.clear();
}

const DroopCorrectionTable& AnanRxDsp::droopTableForRate(int rateKsps) const noexcept
{
    // Returning the kDroopCorrectionZero OBJECT (not a zero-valued copy) is
    // what also suppresses the edge fade in processIqBlock(), which tests
    // identity against exactly this address -- see
    // setDroopCorrectionBypassed()'s comment.
    if (m_droopBypassed)
        return kDroopCorrectionZero;
    const auto it = m_droopTables.constFind(rateKsps);
    return it != m_droopTables.constEnd() ? it.value() : kDroopCorrectionZero;
}

void AnanRxDsp::setShift(double shiftHz)
{
    m_shiftHz = shiftHz;
    if (m_channel && !m_rebuildInFlight)
        m_channel->setShift(shiftHz);
}

bool AnanRxDsp::spectrumFrameDue()
{
    if (m_spectrumIntervalMs <= 0)
        return true;                       // uncapped
    if (!m_spectrumClock.isValid()) {
        m_spectrumClock.start();
        m_lastSpectrumMs = 0;
        return true;                       // paint the first frame immediately
    }
    return (m_spectrumClock.elapsed() - m_lastSpectrumMs) >= m_spectrumIntervalMs;
}

void AnanRxDsp::onSequenceGap()
{
    if (!m_analyzer) {
        return;   // between rebuilds; the new analyzer starts empty
    }
    // Counted only when something was actually in flight -- see
    // Hl2RxDsp::onSequenceGap() for why a boundary-aligned gap must not be
    // counted, and why neither the frame-rate clock nor the audio path is
    // touched here.
    if (m_analyzer->dropStagedPartial() > 0) {
        m_spectrumGapDiscards.fetch_add(1, std::memory_order_relaxed);
    }
}

void AnanRxDsp::processIqBlock(const std::vector<std::complex<float>>& iq)
{
    if (!m_channel)
        return;

    // A squelch change the channel refused: this is the thread that calls
    // processIq(), so nothing of ours is in flight here (as Hl2RxDsp).
    if (m_squelchPending && canPushToChannel())
        pushSquelchToChannel();

    // Handedness: read docs/HERMES.md §16 before touching this. WDSP's RXA as
    // configured here selects the opposite sign to its passband bounds, and the HPSDR
    // wire is the conjugate of the analytic convention (confirmed for P2 by
    // `radiocert rx` on WWV and by SDR++ on an RSP1B). So the demodulator takes the
    // raw wire and the spectrum is mirrored exactly once (§16.6 rule 2) -- inside
    // WDSP's analyzer, whose Spectrum0() swaps I and Q. Conjugating here too would
    // mirror twice. anan_rxdsp_handedness_test pins this.

    // Panadapter: every block goes to the analyzer, whatever the display rate
    // -- its FFTs overlap so that one completes per display frame over fresh
    // samples, and none are thrown away. Only TAKING a frame is paced here.
    m_analyzer->feed(iq);
    retargetAnalyzerIfSettled();
    AnanPanAnalyzer::FrameTag tag;
    if (spectrumFrameDue() && m_analyzer->takeFrame(m_bins, &tag) && !tag.empty()) {
        // The frequency the frame was computed for (WDSP patch 25), and what
        // its samples cover; an untagged frame is at the DDC as it is now.
        m_frameLoHz = tag.loHz > 0.0 ? tag.loHz : m_ddcLoHz;
        m_frameValidLowHz = tag.validLowHz;
        m_frameValidHighHz = tag.validHighHz;
        // Real DDC0 roll-off -- the anti-alias FIR's transition band, not CIC
        // sin(x)/x (see AnanDroopCorrection.h). Applied to each raw frame,
        // before smoothFrame() averages it: a fixed dB offset per point, so
        // the order does not matter. inputSampleRateHz is always an exact multiple of 1000 for
        // the six valid DDC0 rates.
        const DroopCorrectionTable& droopTable =
            droopTableForRate(m_config.inputSampleRateHz / 1000);
        // What this frame covers: the whole band, or the window the analyzer
        // is aimed at when the view is zoomed (brick3-angelia).
        m_frameWindow = {m_analyzer->windowLow(), m_analyzer->windowHigh()};
        if (m_frameWindow.low <= 0.0 && m_frameWindow.high >= 1.0) {
            applyDroopCorrectionDbResampled(m_bins, droopTable);
            // Cosmetic edge fade (see applyEdgeFade()). connectRadio() seeds defaults for
            // all six rates, so on a G2 this always runs; the identity test only suppresses
            // it during a calibration bypass, when droopTableForRate() returns the
            // kDroopCorrectionZero object so a sweep measures the radio, not our fade.
            if (&droopTable != &kDroopCorrectionZero)
                applyEdgeFade(m_bins);
        } else {
            // A window: the same correction, read where its points really sit.
            // No fade: a zoomed view never reaches the band's outer edge
            // (AnanBackend keeps it inside the part the full view shows).
            applyDroopCorrectionDbWindowed(m_bins, droopTable, m_frameWindow.low,
                                           m_frameWindow.high);
        }
        smoothFrame();
        publishSpectrum();
        m_lastSpectrumMs = m_spectrumClock.elapsed();
    }

    // Audio: the RAW wire (see the handedness note above).
    m_iqBuffer.insert(m_iqBuffer.end(), iq.begin(), iq.end());
    const std::size_t block = static_cast<std::size_t>(m_config.dspBlockSize);
    std::size_t consumed = 0;
    while (m_iqBuffer.size() - consumed >= block) {
        // Exchange only once the worker has produced the output this call
        // reads (WDSP patch 15, as Hl2TxDsp::exchangeDueBlocks()). Packets
        // arrive in bursts; exchanged back-to-back, the second block
        // underruns, fexchange2 advances its read index anyway, and later
        // blocks come from the wrong slot of the two-slot ring -- heard as
        // ticks (brick3-angelia). The next packet retries; a worker stalled
        // past kExchangeStallNs drops the block unexchanged.
        if (!m_channel->outputReady()) {
            if (!m_exchangeHeadWait.isValid())
                m_exchangeHeadWait.start();
            if (m_exchangeHeadWait.nsecsElapsed() < kExchangeStallNs)
                break;
            m_exchangeHeadWait.invalidate();
            ++m_exchangeStallDrops;
            if (m_exchangeStallDrops == 1 || m_exchangeStallDrops % 64 == 0) {
                qCWarning(lcAnanRxDsp) << "WDSP worker stalled; dropped an unexchanged"
                                       << "IQ block," << m_exchangeStallDrops << "so far";
            }
            consumed += block;
            continue;
        }
        m_exchangeHeadWait.invalidate();
        if (m_audioMuted) {
            std::fill(m_i.begin(), m_i.end(), 0.0f);
            std::fill(m_q.begin(), m_q.end(), 0.0f);
        } else {
            for (std::size_t n = 0; n < block; ++n) {
                m_i[n] = m_iqBuffer[consumed + n].real();
                m_q[n] = m_iqBuffer[consumed + n].imag();
            }
        }
        consumed += block;

        const auto res = m_channel->processIq(m_i, m_q, m_left, m_right);
        // Count every outcome, Ok included -- the Ok count is the denominator
        // a fault total has to be read against. Same rule, same words and the
        // same bounded log schedule as Hl2RxDsp::processIqBlock: these two
        // stages are copies of each other and the counting rule is the part
        // that must not drift, which is why it lives in WdspProcessTally.h
        // rather than twice here.
        const std::uint64_t seen = m_processTally.record(res);
        if (res != WdspChannel::ProcessResult::Ok) {
            // Underrun excluded from the log and NOT from the count: it is
            // normal while the asynchronous output side fills, and logging it
            // would drown the four outcomes that are not normal.
            //
            // After processIq() returns, never inside it -- qCWarning
            // allocates, and allocating between WdspChannel's two reads of
            // wdspPortAllocationSequence() would manufacture the very
            // AllocationViolation being reported.
            if (res != WdspChannel::ProcessResult::Underrun
                && WdspProcessTally::shouldLog(seen)) {
                qCWarning(lcAnanRxDsp)
                    << "WDSP processIq failed:" << WdspProcessTally::name(res)
                    << "- occurrence" << seen
                    << "on WDSP channel" << m_channel->channelId()
                    << "- this block produces no audio";
            }
            continue;   // underrun while the pipeline fills, etc. -- no output yet
        }

        const std::size_t outN = m_left.size();
        for (std::size_t k = 0; k < outN; ++k) {
            m_stereo[2 * k] = m_dcBlockL.process(m_left[k]);
            m_stereo[2 * k + 1] = m_dcBlockR.process(m_right[k]);
        }
        QVector<float> samples(m_stereo.begin(), m_stereo.end());
        if (const auto frame = m_pcmProducer.produce(std::move(samples))) {
            emit pcmReady(*frame);
            emit audioReady(m_stereo);
        }
        // AVERAGE, NOT PEAK: xmeter's `avg` is an EMA of I*I + Q*Q, `peak` a decaying
        // peak-hold. They agree on a carrier but peak reads band noise ~11-14 dB high,
        // and S9 (-73 dBm) is an RMS quantity. The backend applies its own ballistics.
        // Gated off while muted and for the settle window after unmute or a channel
        // install (the channel is fed zeros, and the average would carry that silence
        // past the unmute; see WdspSMeter.h). The gate also fixes cadence at one reading
        // per 48k-equivalent block (~47/s at every DDC0 rate). Blocks that underrun
        // above do not tick it.
        if (!m_audioMuted && m_meterTap.tick()) {
            emit meterUpdate(static_cast<float>(
                m_channel->meter(WdspChannel::Meter::SignalAverage)));
        }
    }

    if (consumed > 0)
        m_iqBuffer.erase(m_iqBuffer.begin(),
                         m_iqBuffer.begin() + static_cast<std::ptrdiff_t>(consumed));
}

}  // namespace AetherSDR::anan
