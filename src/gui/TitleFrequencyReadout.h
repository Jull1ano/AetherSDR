#pragma once

#include <QChar>
#include <QFont>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

class QLineEdit;

namespace AetherSDR {

// brick3-angelia: the active receiver's frequency in the middle of the title
// bar, as a control rather than a label:
//   - scroll the wheel over a digit to tune by that digit's place (the digit
//     under the pointer is marked);
//   - click to type a frequency and press Enter. A bare number is read the HF
//     way, as the VFO flag's entry reads it: 14240 is 14.240 MHz, 603 is
//     603 kHz, 7 is 7 MHz, 14.24 is 14.240 MHz. Esc or clicking away cancels.
// It owns no tuning: both gestures only emit, and MainWindow routes them
// through the same tune path as the VFO flag (applyTuneRequest), so every
// range check and pan-follow rule applies unchanged. setFrequencyMhz() is fed
// from the active slice.
class TitleFrequencyReadout : public QWidget {
    Q_OBJECT

public:
    explicit TitleFrequencyReadout(QWidget* parent = nullptr);

    // The frequency shown, in MHz. 0 or less means no receiver: the readout
    // shows dashes and takes no input.
    void setFrequencyMhz(double mhz);
    double frequencyMhz() const { return static_cast<double>(m_hz) / 1e6; }

    // "14.222.000": MHz (at least two digits), kHz, Hz. Exposed for tests.
    static QString formatHz(qint64 hz);
    // A typed entry as MHz, or 0 when it is not a frequency. Exposed for tests.
    static double parseEntryMhz(const QString& text);

signals:
    // The wheel moved a digit: tune to `mhz`.
    void stepTuneRequested(double mhz);
    // A frequency was typed and entered.
    void directEntryCommitted(double mhz);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    struct Cell {
        QRectF rect;        // where the character is drawn
        QChar ch;
        qint64 stepHz{0};   // the digit's place value; 0 for a separator
        bool leadingZero{false};
    };
    void applyFont();
    void rebuildCells();
    int digitCellAt(const QPointF& pos) const;
    void setHoverCell(int index);
    void beginEdit();
    void commitEdit();
    void endEdit();

    qint64 m_hz{0};
    QVector<Cell> m_cells;
    int m_hoverCell{-1};
    int m_wheelAccum{0};
    QFont m_font;
    qreal m_digitWidth{0.0};
    qreal m_dotWidth{0.0};
    QLineEdit* m_edit{nullptr};
    bool m_endingEdit{false};
};

}  // namespace AetherSDR
