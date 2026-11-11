#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QPushButton;

namespace AetherSDR {

// brick3-angelia: one slim line of band buttons above the spectrum -- the
// amateur bands low to high, the HF broadcast bands, then WWV and GEN
// (BandButtonCatalog.h, the same targets as the Band menu). A click emits
// bandSelected() with the same arguments as SpectrumOverlayMenu's, and
// MainWindow routes both to selectBand(). View -> Band Bar shows or hides it.
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

signals:
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);

private:
    struct Button {
        QString bandName;
        QPushButton* button{nullptr};
    };
    void syncChecked();

    QVector<Button> m_buttons;
    QString m_currentBand;
};

}  // namespace AetherSDR
