#pragma once

// brick3-angelia: the band buttons shared by the Band menu
// (SpectrumOverlayMenu) and the one-line band bar above the spectrum
// (BandBar), so the two always offer the same targets. bandName is the key
// MainWindow::selectBand() receives; broadcast bands use "BC<metres>" so they
// never collide with an amateur band of the same metre number (60 m, 15 m).

#include "models/BandSettings.h"
#include "models/BroadcastBandPolicy.h"

#include <QString>

#include <array>

namespace AetherSDR::bandbuttons {

struct Entry {
    const char* label;
    const char* bandName;
    double freqMhz;
    const char* mode;
};

// HF broadcast bands, ITU Region 1 edges (models/BroadcastBandPolicy.h),
// each opened mid-band in AM on the 5 kHz grid.
inline constexpr std::array<Entry, 14> kBroadcast{{
    {"120m", "BC120", 2.400, "AM"},
    {"90m", "BC90", 3.300, "AM"},
    {"75m", "BC75", 3.955, "AM"},
    {"60m", "BC60", 4.950, "AM"},
    {"49m", "BC49", 6.000, "AM"},
    {"41m", "BC41", 7.350, "AM"},
    {"31m", "BC31", 9.650, "AM"},
    {"25m", "BC25", 11.800, "AM"},
    {"22m", "BC22", 13.700, "AM"},
    {"19m", "BC19", 15.400, "AM"},
    {"16m", "BC16", 17.700, "AM"},
    {"15m", "BC15", 18.950, "AM"},
    {"13m", "BC13", 21.600, "AM"},
    {"11m", "BC11", 25.800, "AM"},
}};

// The one-line bar: the amateur bands, then the broadcast bands
// (kBroadcast), then the utility targets. The bar fills two rows of eight
// row by row, so the first eight make the top row: the LF/MF bands, then
// 160 and the classic 80 / 40 / 20 / 10 side by side (asked for on
// brick3-angelia); 60, 30, 17, 15, 12, CB and 6 make the second row.
inline constexpr std::array<Entry, 15> kBarAmateur{{
    {"2200", "2200m", 0.1375, "CW"},
    {"630", "630m", 0.475, "CW"},
    {"MW", "LMW", 0.999, "AM"},
    {"160", "160m", 1.900, "LSB"},
    {"80", "80m", 3.800, "LSB"},
    {"40", "40m", 7.150, "LSB"},
    {"20", "20m", 14.225, "USB"},
    {"10", "10m", 28.400, "USB"},
    {"60", "60m", 5.357, "USB"},
    {"30", "30m", 10.125, "DIGU"},
    {"17", "17m", 18.130, "USB"},
    {"15", "15m", 21.300, "USB"},
    {"12", "12m", 24.950, "USB"},
    {"CB", "CB", 27.185, "AM"},
    {"6", "6m", 50.150, "USB"},
}};

inline constexpr std::array<Entry, 2> kBarUtility{{
    {"WWV", "WWV", 10.000, "AM"},
    {"GEN", "GEN", 0.500, "AM"},
}};

// CB with its extended channels: between the 11 m broadcast band and 10 m.
inline constexpr double kCbLowMhz = 26.5;
inline constexpr double kCbHighMhz = 28.0;

inline bool isCbMhz(double mhz) noexcept
{
    return mhz >= kCbLowMhz && mhz < kCbHighMhz;
}

// The bandName of the button whose band holds `mhz` -- the one the band bar
// keeps lit. Broadcast bands first: in ITU Region 1 7.2-7.3 MHz is 41 m and
// 3.9-4.0 MHz is 75 m, although the amateur table (US edges) also claims
// them. Then the amateur band, then CB; anything else is GEN. Empty for no
// frequency. MainWindow::bandZoomKeyForMhz() keeps the same order.
inline QString buttonBandForMhz(double mhz)
{
    if (!(mhz > 0.0))
        return {};
    const double hz = mhz * 1e6;
    for (const broadcast::Band& b : broadcast::kBands) {
        if (hz >= b.lowHz && hz < b.highHz) {
            if (b.kind == broadcast::Kind::LongMediumWave)
                return QStringLiteral("LMW");
            QString metres = QString::fromLatin1(b.name);   // "41m"
            metres.chop(1);
            return QStringLiteral("BC") + metres;
        }
    }
    const QString amateur = BandSettings::bandForFrequency(mhz);
    if (amateur != QLatin1String("GEN"))
        return amateur;
    if (isCbMhz(mhz))
        return QStringLiteral("CB");
    return QStringLiteral("GEN");
}

// Bands with no Flex band-stack slot: every radio tunes them directly.
inline bool isTuneOnlyBand(const QString& bandName)
{
    return bandName == QLatin1String("LMW") || bandName == QLatin1String("CB")
           || bandName.startsWith(QLatin1String("BC"));
}

}  // namespace AetherSDR::bandbuttons
