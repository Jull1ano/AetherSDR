#include "TitleFrequencyReadout.h"

#include "FreqLineEdit.h"
#include "FrequencyEntryParser.h"
#include "core/ThemeManager.h"

#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kHeight = 30;
constexpr int kFontPixelSize = 22;
constexpr int kPadX = 8;
// Digits the readout always has room for: four of MHz (a transverter band),
// three of kHz and three of Hz.
constexpr int kMaxDigits = 10;
// Below this the wheel stops: nothing tunes to zero or a negative frequency.
constexpr qint64 kMinHz = 1'000;
// The HF shorthand's ceiling, the same as the VFO flag's entry: a bare number
// above 54 is kHz, above 54000 is Hz.
constexpr double kHfMaxMhz = 54.0;

}  // namespace

TitleFrequencyReadout::TitleFrequencyReadout(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("titleFrequencyReadout"));
    setAccessibleName(tr("Frequency"));
    setAccessibleDescription(
        tr("The active receiver's frequency. Scroll over a digit to tune by "
           "that digit, or click and type a frequency in kilohertz or megahertz."));
    setToolTip(tr("Scroll over a digit to tune by that digit.\n"
                  "Click to type a frequency, then Enter: 14240 or 14.24 is "
                  "14.240 MHz, 603 is 603 kHz."));
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(kHeight);

    auto* edit = new FreqLineEdit(this);
    m_edit = edit;
    edit->setHintText(tr("kHz or MHz"));
    edit->setObjectName(QStringLiteral("titleFrequencyEdit"));
    edit->setAccessibleName(tr("Frequency entry"));
    edit->setAccessibleDescription(
        tr("Type a frequency in kilohertz or megahertz and press Enter."));
    edit->setAlignment(Qt::AlignCenter);
    ThemeManager::instance().applyStyleSheet(
        edit,
        QStringLiteral(
            "QLineEdit { background: {{color.background.0}};"
            " border: 1px solid {{color.accent}}; border-radius: 3px;"
            " color: {{color.text.primary}}; font-size: 20px; font-weight: bold;"
            " font-family: \"{{font.family.freq}}\"; padding: 0 4px; }"));
    edit->hide();
    edit->installEventFilter(this);
    connect(edit, &QLineEdit::returnPressed, this, [this] { commitEdit(); });
    connect(edit, &QLineEdit::editingFinished, this, [this] { endEdit(); });

    // Custom-painted, so not repainted on a theme change unless asked.
    auto& tm = ThemeManager::instance();
    tm.declareWidgetTokens(this, {QStringLiteral("font.family.freq"),
                                  QStringLiteral("color.text.primary"),
                                  QStringLiteral("color.accent")});
    connect(&tm, &ThemeManager::themeChanged, this, [this] {
        applyFont();
        update();
    });
    applyFont();
}

void TitleFrequencyReadout::applyFont()
{
    m_font = ThemeManager::instance().font(this, QStringLiteral("font.family.freq"));
    m_font.setPixelSize(kFontPixelSize);
    m_font.setBold(true);
    const QFontMetricsF fm(m_font);
    m_digitWidth = 0.0;
    for (char c = '0'; c <= '9'; ++c)
        m_digitWidth = std::max(m_digitWidth, fm.horizontalAdvance(QLatin1Char(c)));
    m_digitWidth = std::max<qreal>(m_digitWidth, 8.0);
    m_dotWidth = std::max<qreal>(fm.horizontalAdvance(QLatin1Char('.')), 6.0);
    setFixedWidth(static_cast<int>(std::ceil(kMaxDigits * m_digitWidth + 2 * m_dotWidth))
                  + 2 * kPadX);
    rebuildCells();
}

QString TitleFrequencyReadout::formatHz(qint64 hz)
{
    if (hz <= 0)
        return QStringLiteral("--.---.---");
    const qint64 mhz = hz / 1'000'000;
    const qint64 khz = (hz / 1'000) % 1'000;
    const qint64 rest = hz % 1'000;
    return QStringLiteral("%1.%2.%3")
        .arg(mhz, 2, 10, QLatin1Char('0'))
        .arg(khz, 3, 10, QLatin1Char('0'))
        .arg(rest, 3, 10, QLatin1Char('0'));
}

double TitleFrequencyReadout::parseEntryMhz(const QString& text)
{
    QString raw = text.trimmed();
    raw.replace(QLatin1Char(','), QLatin1Char('.'));
    if (raw.isEmpty())
        return 0.0;
    const QString clean = FrequencyEntryParser::normalizedMhzText(raw);
    bool ok = false;
    double value = clean.toDouble(&ok);
    if (!ok || !std::isfinite(value) || value <= 0.0)
        return 0.0;
    // 144.200 typed with its dot is MHz as it stands; anything else above the
    // HF ceiling is the kHz or Hz shorthand.
    const bool explicitHighMhz =
        FrequencyEntryParser::isExplicitMhzEntry(raw, clean) && value > kHfMaxMhz;
    if (!explicitHighMhz) {
        if (value > kHfMaxMhz * 1000.0)
            value /= 1e6;
        else if (value > kHfMaxMhz)
            value /= 1e3;
    }
    return value >= 0.001 ? value : 0.0;
}

void TitleFrequencyReadout::setFrequencyMhz(double mhz)
{
    const qint64 hz = (std::isfinite(mhz) && mhz > 0.0) ? std::llround(mhz * 1e6) : 0;
    if (hz == m_hz)
        return;
    m_hz = hz;
    if (m_hz <= 0 && m_edit->isVisible())
        endEdit();
    rebuildCells();
    update();
}

void TitleFrequencyReadout::rebuildCells()
{
    m_cells.clear();
    const QString text = formatHz(m_hz);
    qreal total = 0.0;
    for (const QChar ch : text)
        total += ch == QLatin1Char('.') ? m_dotWidth : m_digitWidth;

    // Place values run right to left; the separators carry none.
    QVector<qint64> steps(text.size(), 0);
    qint64 place = 1;
    for (int i = text.size() - 1; i >= 0; --i) {
        if (text.at(i) == QLatin1Char('.'))
            continue;
        steps[i] = place;
        place *= 10;
    }

    qreal x = (width() - total) / 2.0;
    bool leading = m_hz > 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const bool dot = ch == QLatin1Char('.');
        const qreal w = dot ? m_dotWidth : m_digitWidth;
        Cell cell;
        cell.rect = QRectF(x, 0.0, w, height());
        cell.ch = ch;
        cell.stepHz = (m_hz > 0 && !dot) ? steps.at(i) : 0;
        // Only the padding zero ahead of the MHz figure is "leading".
        if (dot || ch != QLatin1Char('0') || steps.at(i) < 10'000'000)
            leading = false;
        cell.leadingZero = leading;
        m_cells.append(cell);
        x += w;
    }
    if (m_hoverCell >= m_cells.size())
        m_hoverCell = -1;
}

int TitleFrequencyReadout::digitCellAt(const QPointF& pos) const
{
    for (int i = 0; i < m_cells.size(); ++i) {
        if (m_cells.at(i).stepHz > 0 && m_cells.at(i).rect.contains(pos))
            return i;
    }
    return -1;
}

void TitleFrequencyReadout::setHoverCell(int index)
{
    if (index == m_hoverCell)
        return;
    m_hoverCell = index;
    update();
}

void TitleFrequencyReadout::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto& tm = ThemeManager::instance();
    const QColor text = tm.color(this, QStringLiteral("color.text.primary"));
    const QColor accent = tm.color(this, QStringLiteral("color.accent"));
    QColor dim = text;
    dim.setAlphaF(0.30);
    QColor frame = text;
    frame.setAlphaF(underMouse() ? 0.45 : 0.22);

    p.setPen(QPen(frame, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3.0, 3.0);

    p.setFont(m_font);
    for (int i = 0; i < m_cells.size(); ++i) {
        const Cell& c = m_cells.at(i);
        const bool hover = i == m_hoverCell && c.stepHz > 0;
        p.setPen(hover ? accent : ((m_hz <= 0 || c.leadingZero) ? dim : text));
        p.drawText(c.rect, Qt::AlignCenter, QString(c.ch));
        if (hover) {
            p.fillRect(QRectF(c.rect.left() + 1.0, height() - 4.0,
                              c.rect.width() - 2.0, 2.0), accent);
        }
    }
}

void TitleFrequencyReadout::wheelEvent(QWheelEvent* ev)
{
    if (m_hz <= 0 || m_edit->isVisible()) {
        ev->ignore();
        return;
    }
    const int index = digitCellAt(ev->position());
    if (index < 0) {
        ev->ignore();
        return;
    }
    setHoverCell(index);
    ev->accept();
    // One notch is 120; a high-resolution wheel or a touchpad sends fractions.
    m_wheelAccum += ev->angleDelta().y();
    const int notches = m_wheelAccum / 120;
    if (notches == 0)
        return;
    m_wheelAccum -= notches * 120;
    const qint64 next = m_hz + notches * m_cells.at(index).stepHz;
    if (next < kMinHz)
        return;
    // Shown at once so a fast scroll steps from what was just asked for; the
    // slice's own report then confirms or corrects it.
    m_hz = next;
    rebuildCells();
    update();
    emit stepTuneRequested(static_cast<double>(next) / 1e6);
}

void TitleFrequencyReadout::mouseMoveEvent(QMouseEvent* ev)
{
    setHoverCell(digitCellAt(ev->position()));
    QWidget::mouseMoveEvent(ev);
}

void TitleFrequencyReadout::leaveEvent(QEvent* ev)
{
    m_wheelAccum = 0;
    setHoverCell(-1);
    update();   // the frame dims
    QWidget::leaveEvent(ev);
}

void TitleFrequencyReadout::mousePressEvent(QMouseEvent* ev)
{
    if (ev->button() == Qt::LeftButton && m_hz > 0) {
        beginEdit();
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void TitleFrequencyReadout::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    m_edit->setGeometry(rect());
    rebuildCells();
}

bool TitleFrequencyReadout::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_edit && ev->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(ev)->key() == Qt::Key_Escape) {
            endEdit();
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TitleFrequencyReadout::beginEdit()
{
    if (m_edit->isVisible())
        return;
    m_edit->clear();
    m_edit->setGeometry(rect());
    m_edit->show();
    m_edit->raise();
    m_edit->setFocus(Qt::MouseFocusReason);
}

void TitleFrequencyReadout::commitEdit()
{
    if (!m_edit->isVisible())
        return;
    const double mhz = parseEntryMhz(m_edit->text());
    endEdit();
    if (mhz > 0.0 && m_hz > 0)
        emit directEntryCommitted(mhz);
}

void TitleFrequencyReadout::endEdit()
{
    if (!m_edit->isVisible() || m_endingEdit)
        return;
    // clearFocus() fires editingFinished, which comes back here.
    m_endingEdit = true;
    // Drop the focus BEFORE hiding: hiding a focused widget hands the focus
    // to the next one in the chain, and a combo box or line edit holding it
    // would swallow the keyboard shortcuts (M for PC Audio).
    m_edit->clearFocus();
    m_edit->hide();
    m_edit->clear();
    m_endingEdit = false;
    update();
}

}  // namespace AetherSDR
