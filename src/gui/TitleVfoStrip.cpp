#include "TitleVfoStrip.h"

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The DSP strip's look (TitleDspStrip), so the two groups read as one panel.
const QString kButtonStyle = QStringLiteral(
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px; "
    "color: #c8d8e8; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
    "QPushButton:checked { background: #1a6030; color: #ffffff; border: 1px solid #20a040; }"
    "QPushButton:hover { border: 1px solid #0090e0; }"
    "QPushButton:disabled { color: #5e6e7c; background: #141e28; border: 1px solid #243040; }");

const QString kParkedStyle = QStringLiteral(
    "QLabel { color: #8fa8c0; font-size: 10px; font-weight: bold; background: transparent; }"
    "QLabel:disabled { color: #4a5a68; }");

constexpr int kHeight = 20;
constexpr int kGap = 2;
constexpr int kVfoWidth = 22;
constexpr int kCopyWidth = 30;
constexpr int kSaveWidth = 36;
constexpr int kRestoreWidth = 50;
constexpr int kArrowWidth = 20;
constexpr int kStripWidth = kSaveWidth + kRestoreWidth + 3 * kArrowWidth + 4 * kGap;

enum class Arrow { Left, Down, Right };

// Drawn, not a text glyph: Windows draws some of the arrow characters as
// colour emoji, and a drawn triangle is the same size on every font.
QPixmap arrowPixmap(Arrow arrow, const QColor& color, qreal dpr)
{
    const int side = 8;
    QPixmap pm(QSize(side, side) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    QPolygonF tri;
    switch (arrow) {
    case Arrow::Left:  tri << QPointF(6.5, 0.5) << QPointF(6.5, 7.5) << QPointF(1.0, 4.0); break;
    case Arrow::Down:  tri << QPointF(0.5, 1.5) << QPointF(7.5, 1.5) << QPointF(4.0, 7.0); break;
    case Arrow::Right: tri << QPointF(1.5, 0.5) << QPointF(1.5, 7.5) << QPointF(7.0, 4.0); break;
    }
    p.drawPolygon(tri);
    return pm;
}

QIcon arrowIcon(Arrow arrow, qreal dpr)
{
    QIcon icon;
    icon.addPixmap(arrowPixmap(arrow, QColor(0xc8, 0xd8, 0xe8), dpr), QIcon::Normal);
    icon.addPixmap(arrowPixmap(arrow, QColor(0x5e, 0x6e, 0x7c), dpr), QIcon::Disabled);
    return icon;
}

QPushButton* makeButton(QWidget* parent, const QString& text, int width,
                        const QString& objectName, const QString& accessibleName,
                        const QString& tip)
{
    auto* b = new QPushButton(text, parent);
    b->setFixedSize(width, kHeight);
    b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(kButtonStyle);
    b->setObjectName(objectName);
    b->setAccessibleName(accessibleName);
    b->setAccessibleDescription(tip);
    b->setToolTip(tip);
    return b;
}

}  // namespace

TitleVfoStrip::TitleVfoStrip(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("titleVfoStrip"));
    setAccessibleName(tr("Quick VFO controls"));

    auto* rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(kGap);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(kGap);
    auto* bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 0, 0, 0);
    bottom->setSpacing(kGap);
    rows->addLayout(top);
    rows->addLayout(bottom);

    // ── VFO A / B ────────────────────────────────────────────────────────
    m_vfoDefaultTipA = tr("VFO A. Lit while the receiver is on VFO A. Click to tune "
                          "the receiver to VFO A; VFO B keeps where it was.");
    m_vfoDefaultTipB = tr("VFO B. Lit while the receiver is on VFO B. Click to tune "
                          "the receiver to VFO B; VFO A keeps where it was. The first "
                          "time, B starts where A is.");
    m_copyDefaultTip = tr("Copy the VFO you are listening to into the other one: "
                          "frequency, mode and filter (A>B while A is lit, B>A while "
                          "B is lit).");
    m_a = makeButton(this, QStringLiteral("A"), kVfoWidth, QStringLiteral("titleVfoA"),
                     tr("VFO A"), m_vfoDefaultTipA);
    m_b = makeButton(this, QStringLiteral("B"), kVfoWidth, QStringLiteral("titleVfoB"),
                     tr("VFO B"), m_vfoDefaultTipB);
    m_a->setCheckable(true);
    m_b->setCheckable(true);
    m_a->setChecked(true);
    m_copy = makeButton(this, QStringLiteral("A=B"), kCopyWidth, QStringLiteral("titleVfoCopy"),
                        tr("Copy VFO"), m_copyDefaultTip);
    m_parked = new QLabel(this);
    m_parked->setObjectName(QStringLiteral("titleVfoParked"));
    m_parked->setStyleSheet(kParkedStyle);
    m_parked->setFixedHeight(kHeight);
    m_parked->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_parked->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_parked->setAccessibleName(tr("Other VFO"));
    top->addWidget(m_a);
    top->addWidget(m_b);
    top->addWidget(m_copy);
    top->addWidget(m_parked, 1);

    // A click does not light a button by itself: MainWindow lights the VFO
    // the receiver really ends up on.
    connect(m_a, &QPushButton::clicked, this, [this](bool) {
        m_a->setChecked(m_liveA);
        m_b->setChecked(!m_liveA);
        emit vfoRequested(true);
    });
    connect(m_b, &QPushButton::clicked, this, [this](bool) {
        m_a->setChecked(m_liveA);
        m_b->setChecked(!m_liveA);
        emit vfoRequested(false);
    });
    connect(m_copy, &QPushButton::clicked, this, [this] { emit copyRequested(); });

    // ── Save / Restore and the history ───────────────────────────────────
    m_save = makeButton(this, tr("Save"), kSaveWidth, QStringLiteral("titleVfoSave"),
                        tr("Quick memory save"),
                        tr("Save the frequency, mode and filter to the quick memory. "
                           "Restore tunes back to it."));
    m_restore = makeButton(this, tr("Restore"), kRestoreWidth, QStringLiteral("titleVfoRestore"),
                           tr("Quick memory restore"), QString());
    const QString historyTip = tr("The last %1 frequencies you stayed on for %2 seconds, "
                                  "newest first.").arg(16).arg(4);
    m_prev = makeButton(this, QString(), kArrowWidth, QStringLiteral("titleVfoHistoryPrevious"),
                        tr("History previous"),
                        tr("Step through the frequency history towards the newest. ")
                            + historyTip);
    m_list = makeButton(this, QString(), kArrowWidth, QStringLiteral("titleVfoHistoryList"),
                        tr("History list"),
                        tr("Frequency history: pick one to tune to it. ") + historyTip
                            + tr(" Flashes green when a frequency is added."));
    m_next = makeButton(this, QString(), kArrowWidth, QStringLiteral("titleVfoHistoryNext"),
                        tr("History next"),
                        tr("Step through the frequency history towards the oldest. ")
                            + historyTip);
    const qreal dpr = devicePixelRatioF() > 1.0 ? devicePixelRatioF() : 2.0;
    m_prev->setIcon(arrowIcon(Arrow::Left, dpr));
    m_list->setIcon(arrowIcon(Arrow::Down, dpr));
    m_next->setIcon(arrowIcon(Arrow::Right, dpr));
    for (QPushButton* b : {m_prev, m_list, m_next})
        b->setIconSize(QSize(8, 8));
    bottom->addWidget(m_save);
    bottom->addWidget(m_restore);
    bottom->addWidget(m_prev);
    bottom->addWidget(m_list);
    bottom->addWidget(m_next);

    connect(m_save, &QPushButton::clicked, this, [this] { emit saveRequested(); });
    connect(m_restore, &QPushButton::clicked, this, [this] { emit restoreRequested(); });
    connect(m_prev, &QPushButton::clicked, this, [this] { emit stepRequested(-1); });
    connect(m_next, &QPushButton::clicked, this, [this] { emit stepRequested(1); });
    connect(m_list, &QPushButton::clicked, this, [this] { showHistoryMenu(); });

    m_flashTimer = new QTimer(this);
    m_flashTimer->setSingleShot(true);
    m_flashTimer->setInterval(300);
    connect(m_flashTimer, &QTimer::timeout, this, [this] { m_list->setStyleSheet(kButtonStyle); });

    setFixedSize(kStripWidth, 2 * kHeight + kGap);
    setSaved(QString());
    refreshEnabled();
}

void TitleVfoStrip::setLiveVfoA(bool a)
{
    m_liveA = a;
    m_a->setChecked(a);
    m_b->setChecked(!a);
}

void TitleVfoStrip::setParked(const QString& text, const QString& toolTip)
{
    m_parked->setText(text);
    m_parked->setToolTip(toolTip);
    m_parked->setAccessibleDescription(toolTip);
}

void TitleVfoStrip::setVfoAvailable(bool available, const QString& toolTip)
{
    m_vfoAvailable = available;
    m_a->setToolTip(toolTip.isEmpty() ? m_vfoDefaultTipA : toolTip);
    m_b->setToolTip(toolTip.isEmpty() ? m_vfoDefaultTipB : toolTip);
    m_copy->setToolTip(toolTip.isEmpty() ? m_copyDefaultTip : toolTip);
    m_a->setAccessibleDescription(m_a->toolTip());
    m_b->setAccessibleDescription(m_b->toolTip());
    m_copy->setAccessibleDescription(m_copy->toolTip());
    refreshEnabled();
}

void TitleVfoStrip::setMemoryAvailable(bool available)
{
    m_memoryAvailable = available;
    refreshEnabled();
}

void TitleVfoStrip::setSaved(const QString& description)
{
    m_hasSaved = !description.isEmpty();
    const QString tip = m_hasSaved
        ? tr("Tune back to the quick memory: %1.").arg(description)
        : tr("Nothing saved yet: Save stores the frequency, mode and filter.");
    m_restore->setToolTip(tip);
    m_restore->setAccessibleDescription(tip);
    refreshEnabled();
}

void TitleVfoStrip::setHistory(const QStringList& labels, int selected)
{
    m_history = labels;
    m_selected = selected;
    refreshEnabled();
}

void TitleVfoStrip::flashAdded()
{
    flash(QStringLiteral("#20a040"));
}

void TitleVfoStrip::flashUpdated()
{
    flash(QStringLiteral("#d08020"));
}

void TitleVfoStrip::flash(const QString& color)
{
    m_list->setStyleSheet(kButtonStyle
        + QStringLiteral("QPushButton, QPushButton:disabled { background: %1; border: 1px solid %1; }")
              .arg(color));
    m_flashTimer->start();
}

void TitleVfoStrip::refreshEnabled()
{
    m_a->setEnabled(m_vfoAvailable);
    m_b->setEnabled(m_vfoAvailable);
    m_copy->setEnabled(m_vfoAvailable);
    m_parked->setEnabled(m_vfoAvailable);
    m_save->setEnabled(m_memoryAvailable);
    m_restore->setEnabled(m_memoryAvailable && m_hasSaved);
    m_list->setEnabled(m_memoryAvailable && !m_history.isEmpty());
    m_prev->setEnabled(m_memoryAvailable && m_history.size() >= 2);
    m_next->setEnabled(m_memoryAvailable && m_history.size() >= 2);
}

void TitleVfoStrip::showHistoryMenu()
{
    if (m_history.isEmpty())
        return;
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("titleVfoHistoryMenu"));
    menu->setAccessibleName(tr("Frequency history"));
    for (int i = 0; i < m_history.size(); ++i) {
        QAction* act = menu->addAction(m_history.at(i));
        act->setCheckable(true);
        act->setChecked(i == m_selected);
        connect(act, &QAction::triggered, this, [this, i] { emit historyPicked(i); });
    }
    menu->addSeparator();
    connect(menu->addAction(tr("Clear history")), &QAction::triggered, this,
            [this] { emit clearHistoryRequested(); });
    menu->popup(m_list->mapToGlobal(QPoint(0, m_list->height())));
}

}  // namespace AetherSDR
