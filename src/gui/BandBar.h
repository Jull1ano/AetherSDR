#pragma once

#include <QString>
#include <QWidget>

namespace AetherSDR {

// brick3-angelia: one slim line of band buttons above the spectrum -- the
// amateur bands low to high, the HF broadcast bands, then WWV and GEN
// (BandButtonCatalog.h, the same targets as the Band menu). A click emits
// bandSelected() with the same arguments as SpectrumOverlayMenu's, and
// MainWindow routes both to selectBand(). View -> Band Bar shows or hides it.
class BandBar : public QWidget {
    Q_OBJECT

public:
    explicit BandBar(QWidget* parent = nullptr);

signals:
    void bandSelected(const QString& bandName, double freqMhz, const QString& mode);
};

}  // namespace AetherSDR
