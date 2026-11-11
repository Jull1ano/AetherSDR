#include "gui/BandBar.h"

#include "gui/BandButtonCatalog.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSizePolicy>

namespace AetherSDR {

namespace {

// Matches the Band menu's buttons (SpectrumOverlayMenu kBandBtnStyle), one
// size smaller so a full line fits; broadcast labels in amber.
const QString kAmateurStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 10px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }"
    "QPushButton:pressed { background: rgba(0, 112, 192, 230); }");
const QString kBroadcastStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #504030; "
    "border-radius: 2px; color: #ffcc66; font-size: 10px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(160, 110, 0, 180); border: 1px solid #e0a000; }"
    "QPushButton:pressed { background: rgba(160, 110, 0, 230); }");

constexpr int kButtonHeight = 18;
constexpr int kButtonMinWidth = 24;

}  // namespace

BandBar::BandBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("bandBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#bandBar { background: rgba(10, 14, 22, 235); }"));
    setAccessibleName(tr("Band bar"));

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(2, 1, 2, 1);
    row->setSpacing(1);

    auto addButton = [this, row](const bandbuttons::Entry& e, const QString& style,
                                 const QString& tip) {
        auto* btn = new QPushButton(QString::fromLatin1(e.label), this);
        btn->setFixedHeight(kButtonHeight);
        btn->setMinimumWidth(kButtonMinWidth);
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setStyleSheet(style);
        btn->setToolTip(tip);
        btn->setAccessibleName(tip);
        const QString bandName = QString::fromLatin1(e.bandName);
        const double freq = e.freqMhz;
        const QString mode = QString::fromLatin1(e.mode);
        connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode] {
            emit bandSelected(bandName, freq, mode);
        });
        row->addWidget(btn);
    };
    auto addGap = [this, row] {
        auto* gap = new QFrame(this);
        gap->setFrameShape(QFrame::VLine);
        gap->setFixedWidth(5);
        gap->setStyleSheet(QStringLiteral("color: #304050;"));
        row->addWidget(gap);
    };

    for (const auto& e : bandbuttons::kBarAmateur) {
        addButton(e, kAmateurStyle,
                  tr("%1 band (%2 MHz)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3));
    }
    addGap();
    for (const auto& e : bandbuttons::kBroadcast) {
        addButton(e, kBroadcastStyle,
                  tr("%1 broadcast band (%2 MHz, AM)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3));
    }
    addGap();
    for (const auto& e : bandbuttons::kBarUtility) {
        addButton(e, kAmateurStyle, QString::fromLatin1(e.label));
    }
    setFixedHeight(kButtonHeight + 2);
}

}  // namespace AetherSDR
