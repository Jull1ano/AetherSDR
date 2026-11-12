#pragma once

#include <optional>
#include <string>
#include <vector>

namespace AetherSDR {

// brick3-angelia: the title bar's quick VFO controls, as Thetis keeps them
// beside its VFO, for a radio with one receiver:
//
//   A / B      two VFOs on one receiver. The lit one is what the receiver is
//              tuned to (the "live" VFO); the other is parked. Picking the
//              other parks the live one and tunes the receiver to it.
//   A=B        copies the live VFO into the parked one.
//   Save       one quick-memory slot (Thetis's QMS); Restore tunes back to it.
//   history    the last kHistoryCapacity frequencies the receiver stayed on
//              for kDwellMs (Thetis's quick-recall pad, the < v > buttons).
//
// Each VFO, the slot and every history entry carry frequency, mode and filter.
// Plain C++ with no Qt, so it is tested on its own: MainWindow feeds it the
// receiver's setting, tunes to what it hands back, and stores its state.
struct QuickVfoSetting {
    long long hz = 0;          // dial frequency in Hz; 0 or less = empty
    std::string mode;
    int filterLow = 0;         // Hz, relative to the dial frequency
    int filterHigh = 0;

    bool empty() const { return hz <= 0; }
    bool operator==(const QuickVfoSetting&) const = default;
};

class QuickVfoMemory {
public:
    static constexpr int kHistoryCapacity = 30;   // Thetis keeps 16; 30 asked for here
    static constexpr int kDwellMs = 4000;         // Thetis waits 4 s

    enum class Vfo { A, B };
    enum class DwellResult {
        None,       // nothing to record (an empty setting)
        Added,      // a new frequency went to the top of the history
        Updated,    // a listed frequency got a new mode or filter
        Selected,   // a listed frequency, unchanged: it is now the selected one
    };

    // ── A / B ────────────────────────────────────────────────────────────
    Vfo liveVfo() const { return m_live; }
    const QuickVfoSetting& parked() const { return m_parked; }

    // Make `vfo` the live one. `live` is the receiver's setting now. Returns
    // what to tune the receiver to, or nothing: when `vfo` is already live,
    // or when the other VFO was never set (it starts as a copy of this one).
    std::optional<QuickVfoSetting> select(Vfo vfo, const QuickVfoSetting& live);
    // A=B: the parked VFO becomes a copy of the live one.
    void copyLiveToParked(const QuickVfoSetting& live);

    // ── Save / Restore ───────────────────────────────────────────────────
    void save(const QuickVfoSetting& live);
    const QuickVfoSetting& saved() const { return m_saved; }

    // ── History ──────────────────────────────────────────────────────────
    // The receiver has stayed on `live` for kDwellMs: list it.
    DwellResult dwell(const QuickVfoSetting& live);
    const std::vector<QuickVfoSetting>& history() const { return m_history; }
    int selectedIndex() const { return m_selected; }   // -1: none
    // The < and > buttons, as Thetis steps them: index 0 is the most recent,
    // direction -1 moves towards it and +1 away, wrapping at either end.
    // Nothing with fewer than two entries.
    std::optional<QuickVfoSetting> step(int direction);
    // A pick from the list.
    std::optional<QuickVfoSetting> pick(int index);
    void clearHistory();

    // ── Restoring stored state ───────────────────────────────────────────
    // Loads what was stored, dropping what does not hold together (empty
    // history entries, duplicates, more than kHistoryCapacity, an index out
    // of range).
    void restore(Vfo live, const QuickVfoSetting& parked, const QuickVfoSetting& saved,
                 const std::vector<QuickVfoSetting>& history, int selected);
    void reset();

private:
    int indexOf(long long hz) const;

    Vfo m_live = Vfo::A;
    QuickVfoSetting m_parked;
    QuickVfoSetting m_saved;
    std::vector<QuickVfoSetting> m_history;
    int m_selected = -1;
};

}  // namespace AetherSDR
