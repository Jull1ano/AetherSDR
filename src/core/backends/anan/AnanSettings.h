#pragma once

#include <QList>
#include <QString>

class QJsonObject;

namespace AetherSDR::anan {

// Owned configuration for the ANAN-G2 backend, per Constitution Principle V:
// one nested JSON object under a single root key ("Anan"), read and written
// atomically, with one place to default. Mirrors IcomSettings's shape
// (02-working-plan.md Step 2: "AnanSettings in params, namespaced anan.*,
// mirroring IcomSettings") rather than the ad-hoc AppSettings::instance().
// value() calls ConnectionPanel/RadioModel used before this existed.
class AnanSettings {
public:
    // RX1's sample rate in ksps -- also the panadapter span. The name and the
    // stored key say DDC0, where RX1 used to run; they stay so saved settings
    // still load (RX1's DDC is kRx1Route, AnanReceiverRoutes.h). One of the six
    // rates AnanBackend::capabilities().sampleRatesHz declares. Not
    // validated here; AnanBackend::nearestDdc0RateKsps() is the actual
    // snap-to-valid-rate authority, so an out-of-range or hand-edited value
    // just gets snapped on the next connect/zoom rather than rejected here.
    static int ddc0RateKsps();
    static void setDdc0RateKsps(int ksps);

    // ADC options -- all connect-time-only (no live setter anywhere below
    // the seam; P2Client::Params carries these once, at start()). Spec
    // citations live on P2Protocol::buildDdcSpecific()/buildHighPriority(),
    // the actual wire encoders; this class only owns persistence.
    static bool ditherEnabled();
    static void setDitherEnabled(bool on);
    static bool randomEnabled();
    static void setRandomEnabled(bool on);
    // The ADC feeding RX1 (same naming note as ddc0RateKsps()). 0 = ADC0,
    // behind the switched Ant1/2/3 relay bank; 1 = ADC1, wired directly to its
    // own RX2 jack (Appendix D block diagram, spec p.90).
    static int ddc0AdcIndex();
    static void setDdc0AdcIndex(int index);
    static bool bypassAdc0Filters();
    static void setBypassAdc0Filters(bool on);
    static bool bypassAdc1Filters();
    static void setBypassAdc1Filters(bool on);

    // Send demodulated RX audio back to the radio so its own codec and speaker
    // reproduce it. Connect-time, like the ADC options above -- but DEFAULT OFF,
    // unlike all of them, because it is the only one that makes the host
    // originate a continuous outbound stream. See P2Client::Params.
    static bool speakerAudioEnabled();
    static void setSpeakerAudioEnabled(bool on);

    // ---- Live options: the High Priority packet carries them every 100 ms ----

    // Select the receive band-pass filter for the tuned frequency (true) or
    // hold the banks to the bypass flags above (false, the default). Off by
    // default: the bank is in series ahead of the ADC, so a filter that does
    // not cover the tuned frequency is silence, and the band edges still need
    // confirming band by band on a radio.
    static bool bandFiltersEnabled();
    static void setBandFiltersEnabled(bool on);

    // Mute the radio's speaker amplifier (High Priority byte 1400 bit 1).
    static bool speakerAmpMuted();
    static void setSpeakerAmpMuted(bool on);

    // brick3-angelia: receive antenna on ANGELIA/ORION boards, 0..2 =
    // ANT1..ANT3. Default ANT1.
    static int rxAntenna();
    static void setRxAntenna(int index);

    // brick3-angelia: Auto AGC-T, the AGC threshold following the in-filter
    // noise floor the host measures (AnanBackend). Off by default.
    static bool agcThresholdAuto();
    static void setAgcThresholdAuto(bool on);

    // brick3-angelia: the panadapter analyzer's FFT size (0 = automatic) and
    // window (WDSP's analyzer window number, AnanPanAnalyzer::Settings). Read
    // at connect; AnanBackend's "pan.set" verb writes them when the operator
    // changes one. Range-checked where they are applied, not here.
    static int panFftSize();
    static void setPanFftSize(int size);
    static int panWindow();
    static void setPanWindow(int window);
    // brick3-angelia: the trace's attack and decay, ms (0 = none): a point
    // rises with the one time constant and falls with the other. Defaults 70
    // and 260, a quick rise with a gentle fall. Same life cycle as the two
    // above ("pan.set" writes them).
    static int panAttackMs();
    static void setPanAttackMs(int ms);
    static int panDecayMs();
    static void setPanDecayMs(int ms);
    // brick3-angelia: Flatten, percent of the noise floor's grass drawn away
    // (0 = none, 50 = half as tall; at most 90). Default 50. Same life cycle.
    static int panFlattenPct();
    static void setPanFlattenPct(int pct);

    // The DDC0 rates a session may start at, for a UI offering the choice.
    //
    // Here rather than read from the protocol header directly, because a setup
    // page is above the radio seam and must not include family protocol code
    // (the EB3 ratchet in tools/check_engine_boundary.py enforces exactly that).
    // A CONNECTED consumer should prefer RadioCapabilities::sampleRatesHz, which
    // AnanBackend publishes from the same list; this exists because the page
    // that offers the choice is reachable with no radio attached, where there
    // are no capabilities to read.
    [[nodiscard]] static QList<int> validDdc0RatesKsps();

    // Restore every field to its default.
    static void reset();

private:
    static QJsonObject readObj();
    static void writeObj(const QJsonObject& obj);
};

}  // namespace AetherSDR::anan
