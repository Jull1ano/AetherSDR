#pragma once

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QFrame;
class QHideEvent;
class QPushButton;
class QResizeEvent;
class QShowEvent;

namespace AetherSDR {

class RxApplet;
class QuickRxStrip;   // BandBar.cpp

// brick3-angelia: the bar above the spectrum, in blocks side by side:
//
//   modes | amateur bands | HF broadcast bands | WWV, GEN | quick RX controls
//
// The band targets are BandButtonCatalog.h's, the same as the Band menu: a
// click emits bandSelected() with the same arguments as SpectrumOverlayMenu's,
// and MainWindow routes both to selectBand(). The mode buttons and the RX
// controls (filter presets, step, AGC, volume, squelch) MIRROR the RX Controls
// panel (setQuickRxSource()): they show its state and click or set its own
// widgets, so every rule those carry stays in one place, and they keep working
// with the side panel hidden. View -> Band Bar shows or hides the bar.
//
// Every button has the same compact width; a block of buttons is two rows
// tall. The blocks are centred and move to a new line when the bar's width
// runs out (a block wider than the bar takes more rows). A grip on either
// side drags the width narrower or wider (a double-click goes back to one
// line), and the width is kept ("BandBarBlocksWidth").
//
// The button of the band the receiver is in stays lit (checked), as Thetis
// keeps its band button down: setCurrentFrequencyMhz() follows the active
// slice, so the bar is right however the frequency got there.
class BandBar : public QWidget {
    Q_OBJECT

public:
    explicit BandBar(QWidget* parent = nullptr);
    ~BandBar() override;

    // Light the button whose band holds `mhz` (bandbuttons::buttonBandForMhz);
    // 0 or less lights none.
    void setCurrentFrequencyMhz(double mhz);
    // The bandName of the lit button; empty when none is.
    QString currentBand() const { return m_currentBand; }

    // The RX Controls panel the mode buttons and the quick RX block mirror;
    // nullptr leaves them greyed (and no mode buttons).
    void setQuickRxSource(RxApplet* source);

    // The width the blocks flow in, in logical pixels; 0 is the default (all
    // on one line). Not saved: the grips save what the operator drags.
    int contentWidth() const { return m_contentWidth; }
    void setContentWidth(int px);

signals:
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    enum Group { Amateur = 0, Broadcast = 1, Utility = 2 };
    struct Button {
        QString bandName;
        QPushButton* button{nullptr};
        int group{0};
    };
    struct ModeButton {
        QString mode;
        QPushButton* button{nullptr};
    };
    struct Block {
        QVector<QPushButton*> buttons;   // a grid of equal buttons, or
        QWidget* fixed{nullptr};         // one widget of its own size
        int rows{0};
        int cols{0};
        int width{0};
        int height{0};
    };

    void syncChecked();
    // Copies the RX Controls panel's state into the mode buttons and the
    // quick RX block; runs on a timer while the bar is shown.
    void syncFromRx();
    void rebuildModeButtons(const QStringList& modes);
    void selectMode(const QString& mode);
    [[nodiscard]] QVector<Block> blocks(int contentWidth) const;
    [[nodiscard]] int naturalWidth() const;
    [[nodiscard]] int minimumContentWidth() const;
    [[nodiscard]] int availableContentWidth() const;
    [[nodiscard]] int effectiveContentWidth() const;
    // Places every block, separator and grip for the current width, and sets
    // the bar's height to the lines that takes.
    void relayout();
    void beginWidthDrag();
    void dragWidth(int dx, int direction);
    void endWidthDrag();
    void resetWidth();
    void saveWidth() const;

    QVector<Button> m_buttons;
    QVector<ModeButton> m_modeButtons;
    QStringList m_modeNames;
    QVector<QFrame*> m_separators;   // a pool; one per gap between two blocks
    QWidget* m_leftGrip{nullptr};
    QWidget* m_rightGrip{nullptr};
    QuickRxStrip* m_quickRx{nullptr};
    QPointer<RxApplet> m_rxSource;
    QTimer m_rxPoll;
    QString m_currentBand;
    int m_buttonWidth{40};
    int m_contentWidth{0};
    int m_dragStartWidth{0};
};

}  // namespace AetherSDR
