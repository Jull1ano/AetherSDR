#include "core/backends/anan/AnanSettings.h"

#include "core/AppSettings.h"
#include "core/backends/anan/P2Protocol.h"   // kDdc0RatesKsps

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <iterator>

namespace AetherSDR::anan {
namespace {

// Single nested-JSON key holding this backend's config (Principle V).
// Shape: {"ddc0RateKsps":int, "dither":bool,
//         "random":bool, "ddc0AdcIndex":int, "bypassAdc0Filters":bool,
//         "bypassAdc1Filters":bool, "speakerAudioEnabled":bool,
//         "bandFilters":bool, "speakerAmpMuted":bool}
const QString kRootKey = QStringLiteral("Anan");

constexpr const char* kFieldDdc0RateKsps      = "ddc0RateKsps";
constexpr const char* kFieldDither            = "dither";
constexpr const char* kFieldRandom            = "random";
constexpr const char* kFieldDdc0AdcIndex      = "ddc0AdcIndex";
constexpr const char* kFieldBypassAdc0Filters = "bypassAdc0Filters";
constexpr const char* kFieldSpeakerAudioEnabled = "speakerAudioEnabled";
constexpr const char* kFieldBypassAdc1Filters = "bypassAdc1Filters";
constexpr const char* kFieldBandFilters       = "bandFilters";
constexpr const char* kFieldSpeakerAmpMuted   = "speakerAmpMuted";
constexpr const char* kFieldRxAntenna         = "rxAntenna";   // brick3-angelia
constexpr const char* kFieldAgcThresholdAuto  = "agcThresholdAuto";   // brick3-angelia
constexpr const char* kFieldPanFftSize        = "panFftSize";         // brick3-angelia
constexpr const char* kFieldPanWindow         = "panWindow";          // brick3-angelia
constexpr int kDefaultPanWindow = 5;   // Kaiser: AnanPanAnalyzer::kDefaultWindow
constexpr const char* kFieldPanAttackMs       = "panAttackMs";        // brick3-angelia
constexpr const char* kFieldPanDecayMs        = "panDecayMs";         // brick3-angelia
constexpr int kDefaultPanAttackMs = 70;
constexpr int kDefaultPanDecayMs = 260;
constexpr int kMaxPanBallisticsMs = 5000;
constexpr const char* kFieldPanFlattenPct     = "panFlattenPct";      // brick3-angelia
constexpr int kDefaultPanFlattenPct = 50;
constexpr int kMaxPanFlattenPct = 90;

}  // namespace

QJsonObject AnanSettings::readObj()
{
    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void AnanSettings::writeObj(const QJsonObject& obj)
{
    auto& s = AppSettings::instance();
    s.setValue(kRootKey,
               QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    // COMMIT -- setValue() only marks the row dirty; save() runs the
    // transaction and there is no autosave timer. See IcomSettings::
    // writeObj(), the sibling this is modelled on, for the same note.
    s.save();
}

int AnanSettings::ddc0RateKsps()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0RateKsps)).toInt(48);
    return v > 0 ? v : 48;
}

void AnanSettings::setDdc0RateKsps(int ksps)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0RateKsps)] = ksps;
    writeObj(obj);
}

bool AnanSettings::ditherEnabled()
{
    return readObj().value(QLatin1String(kFieldDither)).toBool(true);
}

void AnanSettings::setDitherEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDither)] = on;
    writeObj(obj);
}

bool AnanSettings::randomEnabled()
{
    return readObj().value(QLatin1String(kFieldRandom)).toBool(true);
}

void AnanSettings::setRandomEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldRandom)] = on;
    writeObj(obj);
}

int AnanSettings::ddc0AdcIndex()
{
    const int v = readObj().value(QLatin1String(kFieldDdc0AdcIndex)).toInt(0);
    return v == 1 ? 1 : 0;   // anything but a deliberate 1 reads as ADC0
}

void AnanSettings::setDdc0AdcIndex(int index)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldDdc0AdcIndex)] = (index == 1 ? 1 : 0);
    writeObj(obj);
}

bool AnanSettings::bypassAdc0Filters()
{
    return readObj().value(QLatin1String(kFieldBypassAdc0Filters)).toBool(true);
}

void AnanSettings::setBypassAdc0Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc0Filters)] = on;
    writeObj(obj);
}

bool AnanSettings::speakerAudioEnabled()
{
    // false, not true: an absent key must mean "off" here. Every other option in
    // this file defaults true because absence means "the shipped hardware
    // default"; this one's absence means the operator has never asked for an
    // outbound stream, and starting one anyway is the wrong way to be wrong.
    return readObj().value(QLatin1String(kFieldSpeakerAudioEnabled)).toBool(false);
}

void AnanSettings::setSpeakerAudioEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldSpeakerAudioEnabled)] = on;
    writeObj(obj);
}

bool AnanSettings::bypassAdc1Filters()
{
    return readObj().value(QLatin1String(kFieldBypassAdc1Filters)).toBool(true);
}

void AnanSettings::setBypassAdc1Filters(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBypassAdc1Filters)] = on;
    writeObj(obj);
}

bool AnanSettings::bandFiltersEnabled()
{
    // false on absence: an operator who never chose it has not asked for
    // relays in the signal path.
    return readObj().value(QLatin1String(kFieldBandFilters)).toBool(false);
}

void AnanSettings::setBandFiltersEnabled(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldBandFilters)] = on;
    writeObj(obj);
}

bool AnanSettings::speakerAmpMuted()
{
    // false on absence: the radio powers up with its amplifier live.
    return readObj().value(QLatin1String(kFieldSpeakerAmpMuted)).toBool(false);
}

void AnanSettings::setSpeakerAmpMuted(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldSpeakerAmpMuted)] = on;
    writeObj(obj);
}

int AnanSettings::rxAntenna()
{
    const int v = readObj().value(QLatin1String(kFieldRxAntenna)).toInt(0);
    return (v >= 0 && v <= 2) ? v : 0;
}

void AnanSettings::setRxAntenna(int index)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldRxAntenna)] = (index >= 0 && index <= 2) ? index : 0;
    writeObj(obj);
}

bool AnanSettings::agcThresholdAuto()
{
    return readObj().value(QLatin1String(kFieldAgcThresholdAuto)).toBool(false);
}

void AnanSettings::setAgcThresholdAuto(bool on)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldAgcThresholdAuto)] = on;
    writeObj(obj);
}

int AnanSettings::panFftSize()
{
    const int v = readObj().value(QLatin1String(kFieldPanFftSize)).toInt(0);
    return v > 0 ? v : 0;
}

void AnanSettings::setPanFftSize(int size)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldPanFftSize)] = size > 0 ? size : 0;
    writeObj(obj);
}

int AnanSettings::panWindow()
{
    const int v = readObj().value(QLatin1String(kFieldPanWindow)).toInt(kDefaultPanWindow);
    return v >= 0 ? v : kDefaultPanWindow;
}

void AnanSettings::setPanWindow(int window)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldPanWindow)] = window >= 0 ? window : kDefaultPanWindow;
    writeObj(obj);
}

int AnanSettings::panAttackMs()
{
    const int v = readObj().value(QLatin1String(kFieldPanAttackMs)).toInt(kDefaultPanAttackMs);
    return (v >= 0 && v <= kMaxPanBallisticsMs) ? v : kDefaultPanAttackMs;
}

void AnanSettings::setPanAttackMs(int ms)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldPanAttackMs)] = (ms >= 0 && ms <= kMaxPanBallisticsMs) ? ms : 0;
    writeObj(obj);
}

int AnanSettings::panDecayMs()
{
    const int v = readObj().value(QLatin1String(kFieldPanDecayMs)).toInt(kDefaultPanDecayMs);
    return (v >= 0 && v <= kMaxPanBallisticsMs) ? v : kDefaultPanDecayMs;
}

void AnanSettings::setPanDecayMs(int ms)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldPanDecayMs)] = (ms >= 0 && ms <= kMaxPanBallisticsMs) ? ms : 0;
    writeObj(obj);
}

int AnanSettings::panFlattenPct()
{
    const int v = readObj().value(QLatin1String(kFieldPanFlattenPct)).toInt(kDefaultPanFlattenPct);
    return (v >= 0 && v <= kMaxPanFlattenPct) ? v : kDefaultPanFlattenPct;
}

void AnanSettings::setPanFlattenPct(int pct)
{
    QJsonObject obj = readObj();
    obj[QLatin1String(kFieldPanFlattenPct)] = std::clamp(pct, 0, kMaxPanFlattenPct);
    writeObj(obj);
}

QList<int> AnanSettings::validDdc0RatesKsps()
{
    QList<int> rates;
    rates.reserve(static_cast<int>(std::size(kDdc0RatesKsps)));
    for (const int ksps : kDdc0RatesKsps)
        rates.append(ksps);
    return rates;
}

void AnanSettings::reset()
{
    writeObj({});
}

}  // namespace AetherSDR::anan
