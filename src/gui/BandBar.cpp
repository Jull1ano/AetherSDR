#include "gui/BandBar.h"

#include "core/AppSettings.h"
#include "gui/BandButtonCatalog.h"

#include <QColor>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>

#include <algorithm>
#include <functional>

namespace AetherSDR {

namespace {

// Matches the Band menu's buttons (SpectrumOverlayMenu kBandBtnStyle);
// broadcast labels in amber.
const QString kAmateurStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #304050; "
    "border-radius: 2px; color: #c8d8e8; font-size: 11px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(0, 112, 192, 180); border: 1px solid #0090e0; }"
    "QPushButton:pressed { background: rgba(0, 112, 192, 230); }"
    // The band the receiver is in: the hover look, held.
    "QPushButton:checked { background: rgba(0, 112, 192, 210); border: 1px solid #30b0ff; "
    "color: #ffffff; }");
const QString kBroadcastStyle = QStringLiteral(
    "QPushButton { background: rgba(30, 40, 55, 220); border: 1px solid #504030; "
    "border-radius: 2px; color: #ffcc66; font-size: 11px; font-weight: bold; "
    "padding: 0px 2px; }"
    "QPushButton:hover { background: rgba(160, 110, 0, 180); border: 1px solid #e0a000; }"
    "QPushButton:pressed { background: rgba(160, 110, 0, 230); }"
    "QPushButton:checked { background: rgba(160, 110, 0, 210); border: 1px solid #ffc030; "
    "color: #ffffff; }");

constexpr int kLabelPixelSize = 11;   // the styles' font-size
constexpr int kButtonHeight = 20;
constexpr int kButtonPadding = 14;    // beside the widest label
constexpr int kMinButtonWidth = 34;
constexpr int kSpacing = 2;           // between two buttons
constexpr int kGroupGap = 11;         // between two groups, a separator in it
constexpr int kSeparatorWidth = 3;
constexpr int kRowSpacing = 2;
constexpr int kMarginV = 2;
constexpr int kGripWidth = 9;
constexpr int kGripGap = 4;           // between a grip and the buttons

QString widthKey() { return QStringLiteral("BandBarWidth"); }

// A grip beside the buttons: dragging it makes the bar narrower or wider.
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

}  // namespace

BandBar::BandBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("bandBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("#bandBar { background: rgba(10, 14, 22, 235); }"));
    setAccessibleName(tr("Band bar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // One width for every button, from the widest label in the styles' font.
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
    auto addSeparator = [this] {
        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::VLine);
        sep->setFixedSize(kSeparatorWidth, kButtonHeight);
        sep->setStyleSheet(QStringLiteral("color: #304050;"));
        m_separators.append(sep);
    };

    for (const auto& e : bandbuttons::kBarAmateur) {
        addButton(e, kAmateurStyle,
                  tr("%1 band (%2 MHz)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3),
                  0);
    }
    addSeparator();
    for (const auto& e : bandbuttons::kBroadcast) {
        addButton(e, kBroadcastStyle,
                  tr("%1 broadcast band (%2 MHz, AM)").arg(QString::fromLatin1(e.label))
                      .arg(e.freqMhz, 0, 'f', 3),
                  1);
    }
    addSeparator();
    for (const auto& e : bandbuttons::kBarUtility)
        addButton(e, kAmateurStyle, QString::fromLatin1(e.label), 2);
    m_groupCount = 3;

    const QString gripTip = tr("Drag to make the band bar narrower or wider; "
                               "the buttons move into rows to fit. "
                               "Double-click for two rows.");
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

    m_contentWidth = std::max(0, AppSettings::instance().value(widthKey(), 0).toInt());
    // A new widget is 100 px wide until its layout says otherwise, which
    // would put one button on each row and make the bar briefly very tall --
    // tall enough to stretch the window. Start at the width it wants.
    const int wanted = m_contentWidth > 0 ? m_contentWidth : defaultContentWidth();
    resize(std::max(wanted, m_buttonWidth) + 2 * (kGripWidth + kGripGap) + 4, height());
    relayout();
}

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

int BandBar::groupWidth(int group) const
{
    int count = 0;
    for (const Button& b : m_buttons)
        count += b.group == group ? 1 : 0;
    return count > 0 ? count * m_buttonWidth + (count - 1) * kSpacing : 0;
}

int BandBar::defaultContentWidth() const
{
    // Two rows: the amateur bands on the first, everything after them on the
    // second.
    int rest = 0;
    for (int g = 1; g < m_groupCount; ++g)
        rest += groupWidth(g) + (g > 1 ? kGroupGap : 0);
    return std::max(groupWidth(0), rest);
}

int BandBar::availableContentWidth() const
{
    return width() - 2 * (kGripWidth + kGripGap) - 4;
}

int BandBar::effectiveContentWidth() const
{
    const int wanted = m_contentWidth > 0 ? m_contentWidth : defaultContentWidth();
    return std::max(m_buttonWidth, std::min(wanted, availableContentWidth()));
}

void BandBar::relayout()
{
    if (m_buttons.isEmpty())
        return;
    const int contentWidth = effectiveContentWidth();

    struct Placed {
        QWidget* widget;
        int row;
        int x;
    };
    QVector<Placed> placed;
    QVector<int> rowWidth{0};
    int row = 0;
    int x = 0;
    auto newRow = [&row, &x, &rowWidth] {
        ++row;
        x = 0;
        rowWidth.append(0);
    };
    for (int g = 0; g < m_groupCount; ++g) {
        if (g > 0) {
            QFrame* sep = m_separators.value(g - 1);
            // A group moves to the next row whole when it does not fit
            // beside the one before; a group wider than a row wraps inside.
            if (x > 0 && x + kGroupGap + groupWidth(g) > contentWidth) {
                newRow();
                if (sep)
                    sep->hide();
            } else if (x > 0) {
                if (sep) {
                    placed.append({sep, row, x + (kGroupGap - kSeparatorWidth) / 2});
                    sep->show();
                }
                x += kGroupGap;
            } else if (sep) {
                sep->hide();
            }
        }
        bool firstInGroup = true;
        for (const Button& b : m_buttons) {
            if (b.group != g)
                continue;
            int lead = (x == 0 || firstInGroup) ? 0 : kSpacing;
            if (x > 0 && x + lead + m_buttonWidth > contentWidth) {
                newRow();
                lead = 0;
            }
            x += lead;
            placed.append({b.button, row, x});
            x += m_buttonWidth;
            rowWidth[row] = x;
            firstInGroup = false;
        }
    }

    const int rows = static_cast<int>(rowWidth.size());
    const int barHeight = 2 * kMarginV + rows * kButtonHeight + (rows - 1) * kRowSpacing;
    // Each row centred on its own.
    for (const Placed& p : placed) {
        const int left = (width() - rowWidth[p.row]) / 2;
        const int y = kMarginV + p.row * (kButtonHeight + kRowSpacing);
        p.widget->move(left + p.x, y);
    }

    // The grips mark the width the buttons flow in.
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
    m_contentWidth = std::clamp(wanted, m_buttonWidth,
                                std::max(m_buttonWidth, availableContentWidth()));
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
