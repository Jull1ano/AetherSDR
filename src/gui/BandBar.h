#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QFrame;
class QPushButton;
class QResizeEvent;

namespace AetherSDR {

// brick3-angelia: the band buttons above the spectrum -- the amateur bands
// low to high, the HF broadcast bands, then WWV and GEN (BandButtonCatalog.h,
// the same targets as the Band menu). A click emits bandSelected() with the
// same arguments as SpectrumOverlayMenu's, and MainWindow routes both to
// selectBand(). View -> Band Bar shows or hides it.
//
// Every button has the same compact width and the buttons are centred,
// flowing into as many rows as the bar's width needs -- by default two, the
// amateur bands over the broadcast ones. A group of buttons moves to the next
// row whole when it does not fit beside the one before. A grip on either side
// drags the width narrower or wider (a double-click goes back to two rows),
// and the width is kept ("BandBarWidth").
//
// The button of the band the receiver is in stays lit (checked), as Thetis
// keeps its band button down: setCurrentFrequencyMhz() follows the active
// slice, so the bar is right however the frequency got there -- a button, the
// wheel, a typed frequency or a click in the spectrum.
class BandBar : public QWidget {
    Q_OBJECT

public:
    explicit BandBar(QWidget* parent = nullptr);

    // Light the button whose band holds `mhz` (bandbuttons::buttonBandForMhz);
    // 0 or less lights none.
    void setCurrentFrequencyMhz(double mhz);
    // The bandName of the lit button; empty when none is.
    QString currentBand() const { return m_currentBand; }

    // The width the buttons flow in, in logical pixels; 0 is the default
    // (two rows). Not saved: the grips save what the operator drags.
    int contentWidth() const { return m_contentWidth; }
    void setContentWidth(int px);

signals:
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Button {
        QString bandName;
        QPushButton* button{nullptr};
        int group{0};   // 0 amateur, 1 broadcast, 2 utility
    };
    void syncChecked();
    // Places the buttons, separators and grips for the current width, and
    // sets the bar's height to the rows that takes.
    void relayout();
    [[nodiscard]] int groupWidth(int group) const;
    [[nodiscard]] int defaultContentWidth() const;
    [[nodiscard]] int availableContentWidth() const;
    [[nodiscard]] int effectiveContentWidth() const;
    void beginWidthDrag();
    void dragWidth(int dx, int direction);
    void endWidthDrag();
    void resetWidth();
    void saveWidth() const;

    QVector<Button> m_buttons;
    QVector<QFrame*> m_separators;   // m_separators[g - 1] stands before group g
    QWidget* m_leftGrip{nullptr};
    QWidget* m_rightGrip{nullptr};
    QString m_currentBand;
    int m_groupCount{0};
    int m_buttonWidth{40};
    int m_contentWidth{0};
    int m_dragStartWidth{0};
};

}  // namespace AetherSDR
