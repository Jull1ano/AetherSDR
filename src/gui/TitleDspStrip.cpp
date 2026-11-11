#include "TitleDspStrip.h"

#include <QGridLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>

namespace AetherSDR {

namespace {

// The VFO flag's DSP toggle look (VfoWidget kDspToggle), sized for the bar.
const QString kButtonStyle = QStringLiteral(
    "QPushButton { background: #1a2a3a; border: 1px solid #304050; border-radius: 2px; "
    "color: #c8d8e8; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
    "QPushButton:checked { background: #1a6030; color: #ffffff; border: 1px solid #20a040; }"
    "QPushButton:hover { border: 1px solid #0090e0; }"
    "QPushButton:disabled { color: #5e6e7c; background: #141e28; border: 1px solid #243040; }");

constexpr int kButtonWidth = 40;
constexpr int kButtonHeight = 20;
constexpr int kColumns = 5;

struct SlotDef {
    const char* id;
    const char* tip;
};

// Two rows of five: the blanker and the notch lead their rows, the noise
// reductions follow in the AetherRX window's own order. The last cell is
// free (a manual notch belongs there once this radio has one).
constexpr SlotDef kSlots[] = {
    {"NB", "Noise blanker on or off, for the active receiver."},
    {"NR2", "NR2 noise reduction on or off. Right-click opens its settings."},
    {"NR4", "NR4 noise reduction on or off. Right-click opens its settings."},
    {"MNR", "MNR noise reduction on or off. Right-click opens its settings."},
    {"DFNR", "DFNR noise reduction on or off. Right-click opens its settings."},
    {"ANF", "Automatic notch filter on or off, for the active receiver: "
            "removes steady carriers and heterodynes from the audio."},
    {"RN2", "RN2 noise reduction on or off. Right-click opens its settings."},
    {"BNR", "BNR noise reduction on or off. Right-click opens its settings."},
    {"NNR", "NNR noise reduction on or off. Right-click opens its settings."},
};

}  // namespace

QStringList TitleDspStrip::slotIds()
{
    QStringList ids;
    for (const SlotDef& def : kSlots)
        ids.append(QString::fromLatin1(def.id));
    return ids;
}

QStringList TitleDspStrip::clientNrIds()
{
    return {QStringLiteral("NR2"), QStringLiteral("NR4"), QStringLiteral("MNR"),
            QStringLiteral("DFNR"), QStringLiteral("RN2"), QStringLiteral("BNR"),
            QStringLiteral("NNR")};
}

TitleDspStrip::TitleDspStrip(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("titleDspStrip"));
    setAccessibleName(tr("Receive DSP switches"));

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);

    int index = 0;
    for (const SlotDef& def : kSlots) {
        const QString id = QString::fromLatin1(def.id);
        const QString tip = tr(def.tip);
        auto* b = new QPushButton(id, this);
        b->setCheckable(true);
        b->setFixedSize(kButtonWidth, kButtonHeight);
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(kButtonStyle);
        b->setObjectName(QStringLiteral("titleDsp") + id);
        b->setAccessibleName(id);
        b->setAccessibleDescription(tip);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, [this, id](bool on) {
            emit toggled(id, on);
        });
        // Right-click: this switch's settings, where it has any.
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, this, [this, id](const QPoint&) {
            emit settingsRequested(id);
        });
        grid->addWidget(b, index / kColumns, index % kColumns);
        m_slots.append({id, b, tip});
        ++index;
    }
    setFixedSize(kColumns * kButtonWidth + (kColumns - 1) * 2, 2 * kButtonHeight + 2);
}

QPushButton* TitleDspStrip::button(const QString& id) const
{
    for (const Slot& slot : m_slots) {
        if (slot.id == id)
            return slot.button;
    }
    return nullptr;
}

void TitleDspStrip::setOn(const QString& id, bool on)
{
    if (QPushButton* b = button(id)) {
        if (b->isChecked() != on)
            b->setChecked(on);
    }
}

void TitleDspStrip::setClientNrActive(const QString& method)
{
    for (const QString& id : clientNrIds())
        setOn(id, !method.isEmpty() && id == method);
}

void TitleDspStrip::setAvailable(const QString& id, bool available, const QString& toolTip)
{
    for (const Slot& slot : m_slots) {
        if (slot.id != id)
            continue;
        slot.button->setEnabled(available);
        const QString tip = toolTip.isEmpty() ? slot.defaultTip : toolTip;
        slot.button->setToolTip(tip);
        slot.button->setAccessibleDescription(tip);
        // A switch that cannot be used is not shown as running.
        if (!available && slot.button->isChecked())
            slot.button->setChecked(false);
    }
}

}  // namespace AetherSDR
