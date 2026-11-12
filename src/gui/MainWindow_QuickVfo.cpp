// MainWindow_QuickVfo.cpp — brick3-angelia: the title bar's quick VFO strip,
// right of the DSP switches, as Thetis keeps these beside its VFO:
//
//   A / B / A=B    two VFOs on the one receiver (single-receiver radios; a
//                  radio with a receiver per slice uses slices for that)
//   Save/Restore   one quick-memory slot
//   < v >          the last 30 frequencies the receiver stayed on for 4 s
//
// QuickVfoMemory keeps the state; this file feeds it the active slice's
// frequency, mode and filter, tunes the slice to what it hands back, and
// stores it per radio as the "QuickVfo" feature document.

#include "MainWindow.h"

#include "TitleBar.h"
#include "TitleVfoStrip.h"
#include "core/RadioSettingsScope.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QStatusBar>
#include <QTimer>

#include <cmath>

namespace AetherSDR {

namespace {

const QString kFeature = QStringLiteral("QuickVfo");
constexpr int kSchemaVersion = 1;

// 14143000 -> "14.143.000", as Thetis and the title readout group it.
QString formatHz(long long hz)
{
    return QStringLiteral("%1.%2.%3")
        .arg(hz / 1000000)
        .arg(static_cast<int>((hz / 1000) % 1000), 3, 10, QLatin1Char('0'))
        .arg(static_cast<int>(hz % 1000), 3, 10, QLatin1Char('0'));
}

// "14.143.000 USB, 2.7 kHz filter"
QString describe(const QuickVfoSetting& s)
{
    QString d = formatHz(s.hz);
    if (!s.mode.empty())
        d += QLatin1Char(' ') + QString::fromStdString(s.mode);
    const int width = s.filterHigh - s.filterLow;
    if (width >= 1000)
        d += QStringLiteral(", %1 kHz filter").arg(width / 1000.0, 0, 'f', 1);
    else if (width > 0)
        d += QStringLiteral(", %1 Hz filter").arg(width);
    return d;
}

QJsonObject toJson(const QuickVfoSetting& s)
{
    return QJsonObject{{QStringLiteral("hz"), static_cast<double>(s.hz)},
                       {QStringLiteral("mode"), QString::fromStdString(s.mode)},
                       {QStringLiteral("lo"), s.filterLow},
                       {QStringLiteral("hi"), s.filterHigh}};
}

QuickVfoSetting fromJson(const QJsonValue& value)
{
    const QJsonObject o = value.toObject();
    QuickVfoSetting s;
    const double hz = o.value(QStringLiteral("hz")).toDouble();
    if (std::isfinite(hz) && hz > 0.0 && hz < 1.0e12)
        s.hz = std::llround(hz);
    s.mode = o.value(QStringLiteral("mode")).toString().left(32).toStdString();
    s.filterLow = o.value(QStringLiteral("lo")).toInt();
    s.filterHigh = o.value(QStringLiteral("hi")).toInt();
    return s;
}

QString scopeKey(const RadioSettingsScope& scope)
{
    if (!scope.isValid() || !scope.hasRadioIdentity())
        return {};
    return scope.family() + QLatin1Char('/') + scope.radioId();
}

}  // namespace

QuickVfoSetting MainWindow::liveQuickVfoSetting() const
{
    QuickVfoSetting s;
    auto* slice = activeSlice();
    if (!slice || !m_radioModel.isConnected())
        return s;
    const double mhz = slice->frequency();
    if (!std::isfinite(mhz) || mhz <= 0.0)
        return s;
    s.hz = std::llround(mhz * 1.0e6);
    s.mode = slice->mode().toStdString();
    s.filterLow = slice->filterLow();
    s.filterHigh = slice->filterHigh();
    return s;
}

void MainWindow::loadQuickVfo()
{
    const RadioSettingsScope scope = m_radioModel.settingsScope();
    const QString key = m_radioModel.isConnected() ? scopeKey(scope) : QString();
    if (key == m_quickVfoScopeKey)
        return;
    m_quickVfoScopeKey = key;
    m_quickVfo.reset();
    if (key.isEmpty())
        return;
    const QJsonObject doc = scope.featureExact(kFeature);
    std::vector<QuickVfoSetting> history;
    for (const QJsonValue& v : doc.value(QStringLiteral("history")).toArray())
        history.push_back(fromJson(v));
    const auto live = doc.value(QStringLiteral("live")).toString() == QLatin1String("B")
        ? QuickVfoMemory::Vfo::B : QuickVfoMemory::Vfo::A;
    m_quickVfo.restore(live, fromJson(doc.value(QStringLiteral("parked"))),
                       fromJson(doc.value(QStringLiteral("saved"))), history,
                       doc.value(QStringLiteral("selected")).toInt(-1));
}

void MainWindow::storeQuickVfo()
{
    if (m_quickVfoScopeKey.isEmpty())
        return;
    const RadioSettingsScope scope = m_radioModel.settingsScope();
    if (scopeKey(scope) != m_quickVfoScopeKey)
        return;   // another radio now: never write this one's state under it
    QJsonArray history;
    for (const QuickVfoSetting& s : m_quickVfo.history())
        history.append(toJson(s));
    const QJsonObject doc{
        {QStringLiteral("live"),
         m_quickVfo.liveVfo() == QuickVfoMemory::Vfo::B ? QStringLiteral("B") : QStringLiteral("A")},
        {QStringLiteral("parked"), toJson(m_quickVfo.parked())},
        {QStringLiteral("saved"), toJson(m_quickVfo.saved())},
        {QStringLiteral("history"), history},
        {QStringLiteral("selected"), m_quickVfo.selectedIndex()},
    };
    if (!scope.setFeature(kFeature, kSchemaVersion, doc))
        qWarning() << "QuickVfo: could not store the quick VFO state for" << m_quickVfoScopeKey;
}

void MainWindow::tuneToQuickVfo(const QuickVfoSetting& setting, const char* source)
{
    auto* slice = activeSlice();
    if (!slice || setting.empty())
        return;
    // Mode first (it may bring its own filter), then the frequency, then the
    // filter: the band stack recall's order (RadioModel::recallBandStack).
    // The mode was read off this radio's own slice, so it is one it runs; a
    // host-DSP backend (ANAN, HL2) publishes no mode list to check it against.
    const QString mode = QString::fromStdString(setting.mode);
    const QStringList modes = slice->modeList();
    const bool modeKnown = !mode.isEmpty() && (modes.isEmpty() || modes.contains(mode));
    if (modeKnown && mode != slice->mode())
        slice->setMode(mode);
    // The stored mode is the operator's: the broadcast-band rule must not
    // switch it to AM (or back out of AM) on this tune, as for a band button.
    if (modeKnown && std::llround(slice->frequency() * 1.0e6) != setting.hz)
        m_radioModel.noteOperatorChosenMode(slice->sliceId());
    applyTuneRequest(slice, setting.hz / 1.0e6, TuneIntent::AbsoluteJump, source);
    if (setting.filterLow != 0 || setting.filterHigh != 0)
        slice->setFilterWidth(setting.filterLow, setting.filterHigh);
}

void MainWindow::restartQuickVfoDwell()
{
    if (m_quickVfoDwellTimer)
        m_quickVfoDwellTimer->start(QuickVfoMemory::kDwellMs);
}

void MainWindow::refreshTitleVfoStrip()
{
    auto* strip = m_titleBar ? m_titleBar->vfoStrip() : nullptr;
    if (!strip)
        return;
    loadQuickVfo();
    const bool ready = m_radioModel.isConnected() && activeSlice()
        && !m_quickVfoScopeKey.isEmpty();
    const bool oneReceiver = m_radioModel.maxSlices() <= 1;
    strip->setVfoAvailable(ready && oneReceiver,
        ready && !oneReceiver
            ? tr("This radio has a receiver for each slice: open slice B for a second VFO.")
            : QString());
    strip->setMemoryAvailable(ready);

    const bool liveA = m_quickVfo.liveVfo() == QuickVfoMemory::Vfo::A;
    strip->setLiveVfoA(liveA);
    const QChar live = liveA ? QChar(u'A') : QChar(u'B');
    const QChar other = liveA ? QChar(u'B') : QChar(u'A');
    const QuickVfoSetting& parked = m_quickVfo.parked();
    if (!ready || !oneReceiver) {
        strip->setParked(QString(), QString());
    } else if (parked.empty()) {
        strip->setParked(QStringLiteral("%1 —").arg(other),
                         tr("VFO %1 is not set yet: it starts where VFO %2 is.").arg(other).arg(live));
    } else {
        strip->setParked(QStringLiteral("%1 %2").arg(other).arg(formatHz(parked.hz)),
                         tr("VFO %1: %2").arg(other).arg(describe(parked)));
    }

    const QuickVfoSetting& saved = m_quickVfo.saved();
    strip->setSaved(saved.empty() ? QString() : describe(saved));

    QStringList labels;
    for (const QuickVfoSetting& s : m_quickVfo.history()) {
        QString label = formatHz(s.hz);
        if (!s.mode.empty())
            label += QStringLiteral("   ") + QString::fromStdString(s.mode);
        labels.append(label);
    }
    strip->setHistory(labels, m_quickVfo.selectedIndex());
}

void MainWindow::wireTitleVfoStrip()
{
    auto* strip = m_titleBar ? m_titleBar->vfoStrip() : nullptr;
    if (!strip)
        return;

    m_quickVfoDwellTimer = new QTimer(this);
    m_quickVfoDwellTimer->setSingleShot(true);
    connect(m_quickVfoDwellTimer, &QTimer::timeout, this, [this] {
        if (!m_radioModel.isConnected() || profileLoadRadioStateWritesHeld())
            return;
        if (m_radioModel.transmitModel().isTransmitting())
            return;
        loadQuickVfo();
        if (m_quickVfoScopeKey.isEmpty())
            return;
        const auto result = m_quickVfo.dwell(liveQuickVfoSetting());
        if (result == QuickVfoMemory::DwellResult::None)
            return;
        storeQuickVfo();
        refreshTitleVfoStrip();
        if (auto* s = m_titleBar ? m_titleBar->vfoStrip() : nullptr) {
            if (result == QuickVfoMemory::DwellResult::Added)
                s->flashAdded();
            else if (result == QuickVfoMemory::DwellResult::Updated)
                s->flashUpdated();
        }
    });

    connect(strip, &TitleVfoStrip::vfoRequested, this, [this](bool a) {
        loadQuickVfo();
        if (m_quickVfoScopeKey.isEmpty() || m_radioModel.maxSlices() > 1)
            return;
        const auto vfo = a ? QuickVfoMemory::Vfo::A : QuickVfoMemory::Vfo::B;
        const auto target = m_quickVfo.select(vfo, liveQuickVfoSetting());
        storeQuickVfo();
        if (target)
            tuneToQuickVfo(*target, "titlebar-vfo-ab");
        refreshTitleVfoStrip();
    });
    connect(strip, &TitleVfoStrip::copyRequested, this, [this] {
        loadQuickVfo();
        const QuickVfoSetting live = liveQuickVfoSetting();
        if (m_quickVfoScopeKey.isEmpty() || live.empty())
            return;
        m_quickVfo.copyLiveToParked(live);
        storeQuickVfo();
        refreshTitleVfoStrip();
        const bool liveA = m_quickVfo.liveVfo() == QuickVfoMemory::Vfo::A;
        const QString to = liveA ? QStringLiteral("B") : QStringLiteral("A");
        const QString from = liveA ? QStringLiteral("A") : QStringLiteral("B");
        statusBar()->showMessage(tr("VFO %1 = VFO %2: %3").arg(to, from, describe(live)), 4000);
    });
    connect(strip, &TitleVfoStrip::saveRequested, this, [this] {
        loadQuickVfo();
        const QuickVfoSetting live = liveQuickVfoSetting();
        if (m_quickVfoScopeKey.isEmpty() || live.empty())
            return;
        m_quickVfo.save(live);
        storeQuickVfo();
        refreshTitleVfoStrip();
        statusBar()->showMessage(tr("Saved to the quick memory: %1").arg(describe(live)), 4000);
    });
    connect(strip, &TitleVfoStrip::restoreRequested, this, [this] {
        loadQuickVfo();
        if (!m_quickVfoScopeKey.isEmpty() && !m_quickVfo.saved().empty())
            tuneToQuickVfo(m_quickVfo.saved(), "titlebar-quick-restore");
    });
    connect(strip, &TitleVfoStrip::stepRequested, this, [this](int direction) {
        loadQuickVfo();
        if (m_quickVfoScopeKey.isEmpty())
            return;
        if (const auto target = m_quickVfo.step(direction)) {
            storeQuickVfo();
            tuneToQuickVfo(*target, "titlebar-history-step");
        }
        refreshTitleVfoStrip();
    });
    connect(strip, &TitleVfoStrip::historyPicked, this, [this](int index) {
        loadQuickVfo();
        if (m_quickVfoScopeKey.isEmpty())
            return;
        if (const auto target = m_quickVfo.pick(index)) {
            storeQuickVfo();
            tuneToQuickVfo(*target, "titlebar-history-pick");
        }
        refreshTitleVfoStrip();
    });
    connect(strip, &TitleVfoStrip::clearHistoryRequested, this, [this] {
        loadQuickVfo();
        if (m_quickVfoScopeKey.isEmpty())
            return;
        m_quickVfo.clearHistory();
        storeQuickVfo();
        refreshTitleVfoStrip();
    });

    // A connect loads that radio's state (lazily, once its identity is known);
    // a disconnect drops it and dims the strip.
    connect(&m_radioModel, &RadioModel::connectionStateChanged, this, [this](bool connected) {
        if (!connected && m_quickVfoDwellTimer)
            m_quickVfoDwellTimer->stop();
        QTimer::singleShot(0, this, [this] { refreshTitleVfoStrip(); });
    });
    connect(&m_radioModel, &RadioModel::capabilitiesChanged, this,
            [this](bool, const RadioCapabilities&) { refreshTitleVfoStrip(); });
    QTimer::singleShot(0, this, [this] { refreshTitleVfoStrip(); });
}

}  // namespace AetherSDR
