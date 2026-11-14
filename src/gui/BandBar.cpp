#include "gui/BandBar.h"

#include "core/AppSettings.h"
#include "gui/BandButtonCatalog.h"
#include "gui/RxApplet.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>

#include <algorithm>
#include <array>
#include <functional>
#include <limits>

namespace AetherSDR {

namespace {

// Matches the Band menu's buttons (SpectrumOverlayMenu kBandBtnStyle);
// broadcast labels in amber, modes in green.
const QString kAmateurStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }"
    "QPushButton:pressed { background: rgba(0, 112, 192, 230); }"
    // The band (or setting) in force: the hover look, held.
    "QPushButton:checked { background: rgba(0, 112, 192, 210); border: 1px solid #30b0ff; "
    "color: #ffffff; }"
    "QPushButton:disabled { color: #56687a; border: 1px solid #263442; }");
const QString kBroadcastStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #504030; "
    "border-radius: 2px; color: #ffcc66; font-size: 11px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(160, 110, 0, 180); border: 1px solid #e0a000; }"
    "QPushButton:pressed { background: rgba(160, 110, 0, 230); }"
    "QPushButton:checked { background: rgba(160, 110, 0, 210); border: 1px solid #ffc030; "
    "color: #ffffff; }");
const QString kModeStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #2c5048; "
    "border-radius: 2px; color: #a8e8d0; font-size: 11px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(0, 140, 110, 180); border: 1px solid #20c090; }"
    "QPushButton:pressed { background: rgba(0, 140, 110, 230); }"
    "QPushButton:checked { background: rgba(0, 140, 110, 210); border: 1px solid #40e0b0; "
    "color: #ffffff; }"
    "QPushButton:disabled { color: #4c6a62; border: 1px solid #243a36; }");
const QString kSliderStyle = QStringLiteral(
    "QSlider::groove:horizontal { height: 4px; background: #24384c; border-radius: 2px; }"
    "QSlider::sub-page:horizontal { background: #1e8fd0; border-radius: 2px; }"
    "QSlider::handle:horizontal { background: #c8d8e8; width: 10px; margin: -4px 0px; "
    "border-radius: 5px; }"
    "QSlider::handle:horizontal:hover { background: #ffffff; }"
    "QSlider::sub-page:horizontal:disabled { background: #3a4a5a; }"
    "QSlider::handle:horizontal:disabled { background: #56687a; }");
const QString kComboStyle = QStringLiteral(
    "QComboBox { background: rgba(30, 40, 55, 220); border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 0px 4px; }"
    "QComboBox:hover { border: 1px solid #0090e0; }"
    "QComboBox:disabled { color: #56687a; }"
    "QComboBox::drop-down { width: 12px; border: none; }"
    "QComboBox QAbstractItemView { background: #1a2433; color: #c8d8e8; "
    "selection-background-color: #0070c0; }");
const QString kValueStyle = QStringLiteral(
    "QLabel { background: rgba(20, 28, 40, 220); border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; }"
    "QLabel:disabled { color: #56687a; }");

constexpr int kLabelPixelSize = 11;   // the styles' font-size
constexpr int kButtonHeight = 20;
constexpr int kButtonPadding = 14;    // beside the widest label
constexpr int kMinButtonWidth = 34;
constexpr int kSpacing = 2;           // between two buttons
constexpr int kRowSpacing = 2;        // between two rows of a block
constexpr int kBlockRows = 2;         // a block of buttons is this many rows tall
constexpr int kGroupGap = 11;         // between two blocks, a separator in it
constexpr int kSeparatorWidth = 3;
constexpr int kLineSpacing = 4;       // between two lines of blocks
constexpr int kMarginV = 2;
constexpr int kGripWidth = 9;
constexpr int kGripGap = 4;           // between a grip and the blocks

// The quick RX block: two rows -- filter presets, then step / volume, then
// AGC / squelch.
constexpr int kQuickRowY1 = kButtonHeight + kRowSpacing;
constexpr int kQuickRxHeight = 2 * kButtonHeight + kRowSpacing;
// brick3-angelia: eight filter presets (two rows of four) with the step,
// volume, AGC and squelch controls trimmed so the block is no wider than
// the six-preset one was (438 px) and stays on the bands' line.
constexpr int kFilterButtonWidth = 38;
constexpr int kFilterSlots = 8;       // SSB has eight presets
constexpr int kFilterCols = 4;        // two rows of four
constexpr int kStepLabelWidth = 52;   // between the two step arrows
constexpr int kAfSliderWidth = 66;
constexpr int kAgcComboWidth = 62;
constexpr int kAgcSliderWidth = 72;
constexpr int kAgcAutoWidth = 26;
constexpr int kSquelchButtonWidth = 40;
constexpr int kQuickColAWidth = 18 + 2 + kStepLabelWidth + 2 + 18;      // 92
constexpr int kQuickColBWidth =
    kAgcComboWidth + 2 + kAgcSliderWidth + 2 + kAgcAutoWidth;           // 164
constexpr int kSquelchSliderWidth = kQuickColBWidth - kSquelchButtonWidth - 2;
constexpr int kQuickColA = kFilterCols * kFilterButtonWidth
                           + (kFilterCols - 1) * kSpacing + 8;          // 166
constexpr int kQuickColB = kQuickColA + kQuickColAWidth + 8;            // 266
constexpr int kQuickRxWidth = kQuickColB + kQuickColBWidth;             // 430
static_assert(kQuickRxWidth <= 438, "the quick RX block must fit where the old one did");
static_assert(24 + 2 + kAfSliderWidth == kQuickColAWidth, "volume row fills column A");

// Not "BandBarWidth", the key of the earlier bar of bands alone: a width
// dragged there would fold this wider bar onto three lines.
QString widthKey() { return QStringLiteral("BandBarBlocksWidth"); }

QString barText(const char* text)
{
    return QCoreApplication::translate("AetherSDR::BandBar", text);
}

// A grip beside the blocks: dragging it makes the bar narrower or wider.
// Callbacks rather than signals, so it needs no moc.
class BandBarGrip : public QWidget {
public:
    explicit BandBarGrip(QWidget* parent) : QWidget(parent)
    {
        setCursor(Qt::SizeHorCursor);
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::NoFocus);
    }

    std::function<void()> pressed;
    std::function<void(int)> dragged;   // pixels moved since the press
    std::function<void()> released;
    std::function<void()> reset;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(Qt::NoPen);
        p.setBrush((m_dragging || underMouse()) ? QColor(0x5a, 0xb0, 0xf0)
                                                : QColor(0x3a, 0x50, 0x68));
        // Two columns of dots.
        const int x0 = width() / 2 - 3;
        for (int y = 3; y + 2 <= height() - 2; y += 4) {
            p.drawRect(x0, y, 2, 2);
            p.drawRect(x0 + 4, y, 2, 2);
        }
    }
    void enterEvent(QEnterEvent* event) override
    {
        update();
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override
    {
        update();
        QWidget::leaveEvent(event);
    }
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_dragging = true;
        m_pressX = event->globalPosition().x();
        if (pressed)
            pressed();
        update();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!m_dragging) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        // Measured on the screen, not in this widget: the grip moves with
        // the width it is changing.
        if (dragged)
            dragged(qRound(event->globalPosition().x() - m_pressX));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (!m_dragging || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_dragging = false;
        if (released)
            released();
        update();
        event->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && reset) {
            reset();
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    bool m_dragging = false;
    double m_pressX = 0.0;
};

// ---- mirroring a control of the RX Controls panel -------------------------
// A source that is disabled, or hidden for this mode, greys its mirror.
// "Hidden" is asked up to the panel and no further: the panel hides a whole
// row by its container (AGC in FM), while the panel itself, or the side panel
// around it, being hidden must not grey the bar.

bool usable(const QWidget* source, const QWidget* panel)
{
    if (!source || !source->isEnabled())
        return false;
    return panel ? source->isVisibleTo(panel) : !source->isHidden();
}

void mirrorButton(QPushButton* mirror, const QPushButton* source, const QWidget* panel,
                  bool copyFace, bool copyChecked = true)
{
    const bool on = usable(source, panel);
    if (mirror->isEnabled() != on)
        mirror->setEnabled(on);
    if (!source)
        return;
    if (copyFace) {
        if (mirror->text() != source->text())
            mirror->setText(source->text());
        if (mirror->icon().cacheKey() != source->icon().cacheKey())
            mirror->setIcon(source->icon());
    }
    if (copyChecked) {
        if (mirror->isCheckable() != source->isCheckable())
            mirror->setCheckable(source->isCheckable());
        if (mirror->isCheckable() && mirror->isChecked() != source->isChecked()) {
            const QSignalBlocker block(mirror);
            mirror->setChecked(source->isChecked());
        }
    }
    const QString tip = source->toolTip();
    if (!tip.isEmpty() && mirror->toolTip() != tip)
        mirror->setToolTip(tip);
}

void mirrorSlider(QSlider* mirror, const QSlider* source, const QWidget* panel)
{
    const bool on = usable(source, panel);
    if (mirror->isEnabled() != on)
        mirror->setEnabled(on);
    // Not while the operator is moving the mirror: it is the one that is right.
    if (!source || mirror->isSliderDown())
        return;
    const QSignalBlocker block(mirror);
    if (mirror->minimum() != source->minimum() || mirror->maximum() != source->maximum())
        mirror->setRange(source->minimum(), source->maximum());
    if (mirror->value() != source->value())
        mirror->setValue(source->value());
}

void mirrorCombo(QComboBox* mirror, const QComboBox* source, const QWidget* panel)
{
    const bool on = usable(source, panel);
    if (mirror->isEnabled() != on)
        mirror->setEnabled(on);
    // Not while its list is open.
    if (!source || (mirror->view() && mirror->view()->isVisible()))
        return;
    const QSignalBlocker block(mirror);
    bool same = mirror->count() == source->count();
    for (int i = 0; same && i < source->count(); ++i)
        same = mirror->itemText(i) == source->itemText(i);
    if (!same) {
        mirror->clear();
        for (int i = 0; i < source->count(); ++i)
            mirror->addItem(source->itemText(i), source->itemData(i));
    }
    if (mirror->currentIndex() != source->currentIndex())
        mirror->setCurrentIndex(source->currentIndex());
}

}  // namespace

// The quick RX block: the RX Controls panel's filter presets, tuning step,
// AGC mode and threshold, volume and squelch, in two compact rows. Each
// control mirrors the panel's own (RxApplet::quickControls()): it shows that
// control's state and acts by clicking or setting it, so the panel's rules
// apply unchanged. BandBar calls sync() on its timer.
class QuickRxStrip : public QWidget {
public:
    explicit QuickRxStrip(QWidget* parent);

    void setSource(RxApplet* source)
    {
        m_source = source;
        sync();
    }
    void sync();

private:
    RxApplet::QuickControls controls() const
    {
        return m_source ? m_source->quickControls() : RxApplet::QuickControls{};
    }
    void syncSoon()
    {
        QTimer::singleShot(0, this, [this] { sync(); });
        QTimer::singleShot(200, this, [this] { sync(); });
    }
    QPushButton* makeButton(const QRect& at, const QString& text, const QString& name);
    QSlider* makeSlider(const QRect& at, const QString& name);

    QPointer<RxApplet> m_source;
    std::array<QPushButton*, kFilterSlots> m_filter{};
    std::array<QPointer<QPushButton>, kFilterSlots> m_filterSource;
    QPushButton* m_stepDown{nullptr};
    QLabel* m_step{nullptr};
    QPushButton* m_stepUp{nullptr};
    QComboBox* m_agc{nullptr};
    QSlider* m_agcThreshold{nullptr};
    QPushButton* m_agcAuto{nullptr};
    QPushButton* m_mute{nullptr};
    QSlider* m_af{nullptr};
    QPushButton* m_squelch{nullptr};
    QSlider* m_squelchLevel{nullptr};
};

QuickRxStrip::QuickRxStrip(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("bandBarQuickRx"));
    setFixedSize(kQuickRxWidth, kQuickRxHeight);

    // Filter presets: two rows of four.
    for (int i = 0; i < kFilterSlots; ++i) {
        const QRect at((i % kFilterCols) * (kFilterButtonWidth + kSpacing),
                       i < kFilterCols ? 0 : kQuickRowY1,
                       kFilterButtonWidth, kButtonHeight);
        m_filter[i] = makeButton(at, QString(), barText("Filter width"));
        m_filter[i]->setCheckable(true);
        connect(m_filter[i], &QPushButton::clicked, this, [this, i] {
            if (QPushButton* source = m_filterSource[i])
                source->click();
            syncSoon();
        });
    }

    // Column A: the tuning step over the volume.
    m_stepDown = makeButton(QRect(kQuickColA, 0, 18, kButtonHeight), QString(QChar(0x25C0)),
                            barText("Smaller tuning step"));
    m_step = new QLabel(this);
    m_step->setGeometry(kQuickColA + 20, 0, kStepLabelWidth, kButtonHeight);
    m_step->setAlignment(Qt::AlignCenter);
    m_step->setStyleSheet(kValueStyle);
    m_step->setToolTip(barText("Tuning step"));
    m_step->setAccessibleName(barText("Tuning step"));
    m_stepUp = makeButton(QRect(kQuickColA + 22 + kStepLabelWidth, 0, 18, kButtonHeight),
                          QString(QChar(0x25B6)),
                          barText("Larger tuning step"));
    connect(m_stepDown, &QPushButton::clicked, this, [this] {
        if (QPushButton* source = controls().stepDown)
            source->click();
        syncSoon();
    });
    connect(m_stepUp, &QPushButton::clicked, this, [this] {
        if (QPushButton* source = controls().stepUp)
            source->click();
        syncSoon();
    });
    m_mute = makeButton(QRect(kQuickColA, kQuickRowY1, 24, kButtonHeight), QString(),
                        barText("Mute"));
    m_mute->setIconSize(QSize(14, 14));
    connect(m_mute, &QPushButton::clicked, this, [this] {
        if (QPushButton* source = controls().mute)
            source->click();
        syncSoon();
    });
    m_af = makeSlider(QRect(kQuickColA + 26, kQuickRowY1, kAfSliderWidth, kButtonHeight),
                      barText("Volume (AF gain)"));
    connect(m_af, &QSlider::valueChanged, this, [this](int v) {
        if (QSlider* source = controls().afGain)
            source->setValue(v);
    });

    // Column B: AGC over squelch.
    m_agc = new QComboBox(this);
    m_agc->setGeometry(kQuickColB, 0, kAgcComboWidth, kButtonHeight);
    m_agc->setFocusPolicy(Qt::NoFocus);
    m_agc->setStyleSheet(kComboStyle);
    m_agc->setToolTip(barText("AGC mode"));
    m_agc->setAccessibleName(barText("AGC mode"));
    connect(m_agc, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        if (QComboBox* source = controls().agcMode) {
            // Not a mode the panel has disabled for this radio: the panel
            // would show it and never send it.
            const QModelIndex item = source->model()->index(index, source->modelColumn());
            if (source->model()->flags(item).testFlag(Qt::ItemIsEnabled))
                source->setCurrentIndex(index);
        }
        syncSoon();
    });
    m_agcThreshold = makeSlider(QRect(kQuickColB + kAgcComboWidth + 2, 0, kAgcSliderWidth,
                                      kButtonHeight),
                                barText("AGC threshold"));
    connect(m_agcThreshold, &QSlider::valueChanged, this, [this](int v) {
        if (QSlider* source = controls().agcThreshold)
            source->setValue(v);
    });
    m_agcAuto = makeButton(QRect(kQuickColB + kAgcComboWidth + 2 + kAgcSliderWidth + 2, 0,
                                 kAgcAutoWidth, kButtonHeight),
                           QStringLiteral("A"),
                           barText("Automatic AGC threshold"));
    connect(m_agcAuto, &QPushButton::clicked, this, [this] {
        if (QPushButton* source = controls().autoAgcThreshold)
            source->click();
        syncSoon();
    });
    m_squelch = makeButton(QRect(kQuickColB, kQuickRowY1, kSquelchButtonWidth, kButtonHeight),
                           QStringLiteral("SQL"), barText("Squelch on or off"));
    connect(m_squelch, &QPushButton::clicked, this, [this] {
        if (QPushButton* source = controls().squelch)
            source->click();
        syncSoon();
    });
    m_squelchLevel = makeSlider(QRect(kQuickColB + kSquelchButtonWidth + 2, kQuickRowY1,
                                      kSquelchSliderWidth, kButtonHeight),
                                barText("Squelch level"));
    connect(m_squelchLevel, &QSlider::valueChanged, this, [this](int v) {
        if (QSlider* source = controls().squelchLevel)
            source->setValue(v);
    });

    sync();
}

QPushButton* QuickRxStrip::makeButton(const QRect& at, const QString& text, const QString& name)
{
    auto* button = new QPushButton(text, this);
    button->setGeometry(at);
    // No focus anywhere in the bar: a focused control would take the keys
    // the mode and tuning shortcuts use.
    button->setFocusPolicy(Qt::NoFocus);
    button->setStyleSheet(kAmateurStyle);
    button->setToolTip(name);
    button->setAccessibleName(name);
    return button;
}

QSlider* QuickRxStrip::makeSlider(const QRect& at, const QString& name)
{
    auto* slider = new QSlider(Qt::Horizontal, this);
    slider->setGeometry(at);
    slider->setFocusPolicy(Qt::NoFocus);
    slider->setStyleSheet(kSliderStyle);
    slider->setToolTip(name);
    slider->setAccessibleName(name);
    return slider;
}

void QuickRxStrip::sync()
{
    const RxApplet::QuickControls c = controls();
    const QWidget* panel = m_source.data();

    // The presets the panel shows, at most eight, as asked for on
    // brick3-angelia: the narrower half on the top row, narrow to wide
    // (1.8K 2.1K 2.4K 2.7K), the wider half below it, widest first
    // (4K 3.6K 3.3K 2.9K). The panel itself keeps its narrow-to-wide order.
    QVector<QPushButton*> visible;
    for (QPushButton* source : c.filterButtons) {
        if (source && source->isVisibleTo(panel))
            visible.append(source);
    }
    if (visible.size() > static_cast<int>(m_filter.size()))
        visible.resize(static_cast<int>(m_filter.size()));
    std::array<QPushButton*, kFilterSlots> slotSource{};
    const int topCount = std::min(kFilterCols, (static_cast<int>(visible.size()) + 1) / 2);
    for (int i = 0; i < topCount; ++i)
        slotSource[i] = visible[i];
    for (int i = topCount, col = 0; i < visible.size() && col < kFilterCols; ++i, ++col)
        slotSource[kFilterCols + col] = visible[visible.size() - 1 - (i - topCount)];
    for (int i = 0; i < static_cast<int>(m_filter.size()); ++i) {
        QPushButton* source = slotSource[i];
        m_filterSource[i] = source;
        if (source) {
            mirrorButton(m_filter[i], source, panel, true);
            if (m_filter[i]->isHidden())
                m_filter[i]->show();
        } else if (!m_filter[i]->isHidden()) {
            m_filter[i]->hide();
        }
    }

    mirrorButton(m_stepDown, c.stepDown, panel, false);
    mirrorButton(m_stepUp, c.stepUp, panel, false);
    const QString step = c.stepLabel ? c.stepLabel->text() : QString();
    if (m_step->text() != step)
        m_step->setText(step);
    m_step->setEnabled(usable(c.stepLabel, panel));
    mirrorCombo(m_agc, c.agcMode, panel);
    mirrorSlider(m_agcThreshold, c.agcThreshold, panel);
    mirrorButton(m_agcAuto, c.autoAgcThreshold, panel, true);
    mirrorButton(m_mute, c.mute, panel, true);
    mirrorSlider(m_af, c.afGain, panel);
    // The panel's SQL button is not checkable -- its text says Off, Manual
    // or Auto -- so the mirror is lit from the squelch mode itself.
    mirrorButton(m_squelch, c.squelch, panel, true, /*copyChecked=*/false);
    const bool squelchOn = m_source && m_source->sqlMode() != RxApplet::SqlMode::Off;
    if (!m_squelch->isCheckable())
        m_squelch->setCheckable(true);
    if (m_squelch->isChecked() != squelchOn) {
        const QSignalBlocker block(m_squelch);
        m_squelch->setChecked(squelchOn);
    }
    mirrorSlider(m_squelchLevel, c.squelchLevel, panel);
}

// ---- BandBar ----------------------------------------------------------------

BandBar::BandBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("bandBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#bandBar { background: rgba(10, 14, 22, 235); }"));
    setAccessibleName(tr("Band bar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // One width for every button, from the widest label in the styles' font
    // -- the bands', and the longest mode names.
    QFont labelFont = font();
    labelFont.setPixelSize(kLabelPixelSize);
    labelFont.setBold(true);
    const QFontMetrics metrics(labelFont);
    int widest = 0;
    auto measure = [&widest, &metrics](const auto& entries) {
        for (const auto& e : entries)
            widest = std::max(widest, metrics.horizontalAdvance(QString::fromLatin1(e.label)));
    };
    measure(bandbuttons::kBarAmateur);
    measure(bandbuttons::kBroadcast);
    measure(bandbuttons::kBarUtility);
    for (const char* mode : {"DSTR", "RADE", "DIGU", "DIGL", "RTTY", "FDVU", "FDVL", "WBFM"})
        widest = std::max(widest, metrics.horizontalAdvance(QString::fromLatin1(mode)));
    m_buttonWidth = std::max(kMinButtonWidth, widest + kButtonPadding);

    auto addButton = [this](const bandbuttons::Entry& e, const QString& style,
                            const QString& tip, int group) {
        auto* btn = new QPushButton(QString::fromLatin1(e.label), this);
        btn->setFixedSize(m_buttonWidth, kButtonHeight);
        btn->setFocusPolicy(Qt::NoFocus);
        // Checkable only to show the current band (and to report it to a
        // screen reader as "checked"): a click never leaves the state it
        // toggled, syncChecked() puts back what the frequency says.
        btn->setCheckable(true);
        btn->setStyleSheet(style);
        btn->setToolTip(tip);
        btn->setAccessibleName(tip);
        const QString bandName = QString::fromLatin1(e.bandName);
        const double freq = e.freqMhz;
        const QString mode = QString::fromLatin1(e.mode);
        connect(btn, &QPushButton::clicked, this, [this, bandName, freq, mode] {
            emit bandSelected(bandName, freq, mode);
            syncChecked();
        });
        m_buttons.append({bandName, btn, group});
    };
    for (const auto& e : bandbuttons::kBarAmateur) {
        addButton(e, kAmateurStyle,
                  tr("%1 band (%2 MHz)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3),
                  Amateur);
    }
    for (const auto& e : bandbuttons::kBroadcast) {
        addButton(e, kBroadcastStyle,
                  tr("%1 broadcast band (%2 MHz, AM)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3),
                  Broadcast);
    }
    for (const auto& e : bandbuttons::kBarUtility)
        addButton(e, kAmateurStyle, QString::fromLatin1(e.label), Utility);

    m_quickRx = new QuickRxStrip(this);

    const QString gripTip = tr("Drag to make the bar narrower or wider; the "
                               "blocks move onto more lines to fit. "
                               "Double-click to put them back on one line.");
    auto makeGrip = [this, &gripTip](int direction) {
        auto* grip = new BandBarGrip(this);
        grip->setToolTip(gripTip);
        grip->setAccessibleName(tr("Band bar width"));
        grip->pressed = [this] { beginWidthDrag(); };
        grip->dragged = [this, direction](int dx) { dragWidth(dx, direction); };
        grip->released = [this] { endWidthDrag(); };
        grip->reset = [this] { resetWidth(); };
        return grip;
    };
    m_leftGrip = makeGrip(-1);
    m_rightGrip = makeGrip(+1);

    // The RX Controls panel is read on a timer while the bar shows: its
    // widgets change from many places (mode, slice, radio), several of them
    // under signal blockers, so there is no one signal to follow.
    m_rxPoll.setInterval(250);
    connect(&m_rxPoll, &QTimer::timeout, this, [this] { syncFromRx(); });

    m_contentWidth = std::max(0, AppSettings::instance().value(widthKey(), 0).toInt());
    // A new widget is 100 px wide until its layout says otherwise, which
    // would put every block on a line of its own and make the bar briefly
    // very tall -- tall enough to stretch the window. Start at the width it
    // wants.
    const int wanted = m_contentWidth > 0 ? m_contentWidth : naturalWidth();
    resize(std::max(wanted, minimumContentWidth()) + 2 * (kGripWidth + kGripGap) + 4, height());
    relayout();
}

BandBar::~BandBar() = default;

void BandBar::setCurrentFrequencyMhz(double mhz)
{
    const QString band = bandbuttons::buttonBandForMhz(mhz);
    if (band == m_currentBand)
        return;
    m_currentBand = band;
    syncChecked();
}

void BandBar::syncChecked()
{
    for (const Button& b : m_buttons) {
        const bool current = !m_currentBand.isEmpty() && b.bandName == m_currentBand;
        if (b.button->isChecked() != current)
            b.button->setChecked(current);
    }
}

void BandBar::setQuickRxSource(RxApplet* source)
{
    m_rxSource = source;
    if (m_quickRx)
        m_quickRx->setSource(source);
    syncFromRx();
}

void BandBar::syncFromRx()
{
    const RxApplet::QuickControls c =
        m_rxSource ? m_rxSource->quickControls() : RxApplet::QuickControls{};

    // The mode buttons are the panel's mode list, lit on its current mode.
    QStringList modes;
    QString current;
    if (c.mode) {
        for (int i = 0; i < c.mode->count(); ++i)
            modes << c.mode->itemText(i);
        current = c.mode->currentText();
    }
    if (modes != m_modeNames) {
        rebuildModeButtons(modes);
        relayout();
    }
    const bool modesUsable = usable(c.mode, m_rxSource.data());
    for (const ModeButton& m : m_modeButtons) {
        const bool on = m.mode == current;
        if (m.button->isChecked() != on) {
            const QSignalBlocker block(m.button);
            m.button->setChecked(on);
        }
        if (m.button->isEnabled() != modesUsable)
            m.button->setEnabled(modesUsable);
    }

    if (m_quickRx)
        m_quickRx->sync();
}

void BandBar::rebuildModeButtons(const QStringList& modes)
{
    for (const ModeButton& m : m_modeButtons) {
        m.button->hide();
        m.button->deleteLater();
    }
    m_modeButtons.clear();
    m_modeNames = modes;
    for (const QString& mode : modes) {
        auto* btn = new QPushButton(mode, this);
        btn->setFixedSize(m_buttonWidth, kButtonHeight);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setCheckable(true);
        btn->setStyleSheet(kModeStyle);
        const QString tip = tr("%1 mode").arg(mode);
        btn->setToolTip(tip);
        btn->setAccessibleName(tip);
        connect(btn, &QPushButton::clicked, this, [this, mode] { selectMode(mode); });
        btn->show();
        m_modeButtons.append({mode, btn});
    }
}

void BandBar::selectMode(const QString& mode)
{
    // Through the panel's own mode list, so a client-side mode (RADE) is
    // started the way the panel starts it.
    if (m_rxSource) {
        if (QComboBox* combo = m_rxSource->quickControls().mode) {
            const int index = combo->findText(mode);
            if (index >= 0)
                combo->setCurrentIndex(index);
        }
    }
    QTimer::singleShot(0, this, [this] { syncFromRx(); });
}

void BandBar::setContentWidth(int px)
{
    m_contentWidth = std::max(0, px);
    relayout();
}

void BandBar::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void BandBar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    syncFromRx();
    m_rxPoll.start();
}

void BandBar::hideEvent(QHideEvent* event)
{
    m_rxPoll.stop();
    QWidget::hideEvent(event);
}

QVector<BandBar::Block> BandBar::blocks(int contentWidth) const
{
    QVector<Block> out;
    const int buttonWidth = m_buttonWidth;
    auto addGrid = [&out, contentWidth, buttonWidth](const QVector<QPushButton*>& buttons) {
        if (buttons.isEmpty())
            return;
        const int n = static_cast<int>(buttons.size());
        auto widthFor = [n, buttonWidth](int rows) {
            const int cols = (n + rows - 1) / rows;
            return cols * buttonWidth + (cols - 1) * kSpacing;
        };
        // Two rows; more only when two would be wider than the bar.
        int rows = std::min(kBlockRows, n);
        while (rows < n && widthFor(rows) > contentWidth)
            ++rows;
        Block b;
        b.buttons = buttons;
        b.rows = rows;
        b.cols = (n + rows - 1) / rows;
        b.width = widthFor(rows);
        b.height = rows * kButtonHeight + (rows - 1) * kRowSpacing;
        out.append(b);
    };

    QVector<QPushButton*> modes;
    for (const ModeButton& m : m_modeButtons)
        modes.append(m.button);
    addGrid(modes);
    for (int g = Amateur; g <= Utility; ++g) {
        QVector<QPushButton*> group;
        for (const Button& b : m_buttons) {
            if (b.group == g)
                group.append(b.button);
        }
        addGrid(group);
    }
    if (m_quickRx) {
        Block b;
        b.fixed = m_quickRx;
        b.width = m_quickRx->width();
        b.height = m_quickRx->height();
        out.append(b);
    }
    return out;
}

int BandBar::naturalWidth() const
{
    // Every block two rows tall, all on one line.
    const QVector<Block> all = blocks(std::numeric_limits<int>::max() / 4);
    int total = 0;
    for (int i = 0; i < all.size(); ++i)
        total += all[i].width + (i > 0 ? kGroupGap : 0);
    return total;
}

int BandBar::minimumContentWidth() const
{
    return std::max(m_buttonWidth, m_quickRx ? m_quickRx->width() : 0);
}

int BandBar::availableContentWidth() const
{
    return width() - 2 * (kGripWidth + kGripGap) - 4;
}

int BandBar::effectiveContentWidth() const
{
    const int wanted = m_contentWidth > 0 ? m_contentWidth : naturalWidth();
    return std::max(minimumContentWidth(), std::min(wanted, availableContentWidth()));
}

void BandBar::relayout()
{
    const int contentWidth = effectiveContentWidth();
    const QVector<Block> all = blocks(contentWidth);
    if (all.isEmpty())
        return;

    // Blocks into lines: the next block starts a new line when it does not
    // fit beside the ones before.
    struct Line {
        QVector<int> blocks;
        int width = 0;
        int height = 0;
    };
    QVector<Line> lines(1);
    for (int i = 0; i < all.size(); ++i) {
        if (!lines.last().blocks.isEmpty()
            && lines.last().width + kGroupGap + all[i].width > contentWidth) {
            lines.append(Line{});
        }
        Line& line = lines.last();
        line.width += (line.blocks.isEmpty() ? 0 : kGroupGap) + all[i].width;
        line.height = std::max(line.height, all[i].height);
        line.blocks.append(i);
    }

    // Each line centred on its own; a separator in every gap.
    int separatorsUsed = 0;
    int y = kMarginV;
    for (const Line& line : lines) {
        int x = (width() - line.width) / 2;
        for (int k = 0; k < line.blocks.size(); ++k) {
            const Block& b = all[line.blocks[k]];
            if (k > 0) {
                if (separatorsUsed >= m_separators.size()) {
                    auto* sep = new QFrame(this);
                    sep->setFrameShape(QFrame::VLine);
                    sep->setStyleSheet(QStringLiteral("color: #304050;"));
                    m_separators.append(sep);
                }
                QFrame* sep = m_separators[separatorsUsed++];
                sep->setGeometry(x + (kGroupGap - kSeparatorWidth) / 2, y, kSeparatorWidth,
                                 line.height);
                if (sep->isHidden())
                    sep->show();
                x += kGroupGap;
            }
            if (b.fixed) {
                b.fixed->move(x, y);
            } else {
                for (int j = 0; j < b.buttons.size(); ++j) {
                    const int row = j / b.cols;
                    const int col = j % b.cols;
                    b.buttons[j]->move(x + col * (m_buttonWidth + kSpacing),
                                       y + row * (kButtonHeight + kRowSpacing));
                }
            }
            x += b.width;
        }
        y += line.height + kLineSpacing;
    }
    for (int i = separatorsUsed; i < m_separators.size(); ++i) {
        if (!m_separators[i]->isHidden())
            m_separators[i]->hide();
    }
    const int barHeight = y - kLineSpacing + kMarginV;

    // The grips mark the width the blocks flow in.
    const int contentLeft = (width() - contentWidth) / 2;
    const int gripHeight = barHeight - 2 * kMarginV;
    const int maxGripX = std::max(0, width() - kGripWidth);
    m_leftGrip->setGeometry(std::clamp(contentLeft - kGripGap - kGripWidth, 0, maxGripX),
                            kMarginV, kGripWidth, gripHeight);
    m_rightGrip->setGeometry(std::clamp(contentLeft + contentWidth + kGripGap, 0, maxGripX),
                             kMarginV, kGripWidth, gripHeight);

    if (minimumHeight() != barHeight || maximumHeight() != barHeight)
        setFixedHeight(barHeight);
}

void BandBar::beginWidthDrag()
{
    m_dragStartWidth = effectiveContentWidth();
}

void BandBar::dragWidth(int dx, int direction)
{
    // Centred, so the far side moves too: a grip follows the mouse when the
    // width changes by twice the distance moved.
    const int wanted = m_dragStartWidth + 2 * dx * direction;
    m_contentWidth = std::clamp(wanted, minimumContentWidth(),
                                std::max(minimumContentWidth(), availableContentWidth()));
    relayout();
}

void BandBar::endWidthDrag()
{
    saveWidth();
}

void BandBar::resetWidth()
{
    m_contentWidth = 0;
    saveWidth();
    relayout();
}

void BandBar::saveWidth() const
{
    AppSettings::instance().setValue(widthKey(), QString::number(m_contentWidth));
    AppSettings::instance().save();
}

}  // namespace AetherSDR
