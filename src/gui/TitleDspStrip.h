#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QPushButton;

namespace AetherSDR {

// brick3-angelia: the receive DSP switches beside the title bar's frequency
// readout, as Thetis keeps its NR / NB / ANF grid beside the VFO: the noise
// blanker, the automatic notch filter, and the client noise reductions
// (NR2 NR4 MNR DFNR RN2 BNR NNR), two rows of small toggle buttons.
//
// It owns nothing. A click only emits toggled(id, on); MainWindow switches
// the real thing -- the active slice's NB and ANF, the audio engine's noise
// reduction -- and tells the strip what is running (setOn, setClientNrActive)
// and what this radio and build can run (setAvailable). So a refused switch
// never leaves a button lit.
class TitleDspStrip : public QWidget {
    Q_OBJECT

public:
    explicit TitleDspStrip(QWidget* parent = nullptr);

    // The ids, also the button labels.
    static QStringList slotIds();          // every button, in layout order
    static QStringList clientNrIds();      // the noise reductions among them

    // Light or dim one button.
    void setOn(const QString& id, bool on);
    // The one running noise reduction (the engine keeps them exclusive), or
    // empty for none: lights it and dims the other six.
    void setClientNrActive(const QString& method);
    // Whether the button can be used here, and why not when it cannot. An
    // empty tip keeps the button's own.
    void setAvailable(const QString& id, bool available, const QString& toolTip = {});

signals:
    // A button was clicked; `on` is what the click asks for.
    void toggled(const QString& id, bool on);
    // A button was right-clicked: show that switch's settings.
    void settingsRequested(const QString& id);

private:
    QPushButton* button(const QString& id) const;

    struct Slot {
        QString id;
        QPushButton* button{nullptr};
        QString defaultTip;
    };
    QVector<Slot> m_slots;
};

}  // namespace AetherSDR
