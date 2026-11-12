#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;

namespace AetherSDR {

// brick3-angelia: the quick VFO controls right of the title bar's DSP
// switches, as Thetis keeps them beside its VFO. Two rows:
//
//   [ A ][ B ][A=B]  B 7.074.000      VFO A / B on one receiver, A=B copies
//   [Save][Restore][<][v][>]          quick memory, then the frequency history
//
// It owns nothing: a click only emits a request. MainWindow keeps the state
// (QuickVfoMemory), tunes the receiver, and tells the strip what to show.
class TitleVfoStrip : public QWidget {
    Q_OBJECT

public:
    explicit TitleVfoStrip(QWidget* parent = nullptr);

    // Which VFO is lit (true: A).
    void setLiveVfoA(bool a);
    // The parked VFO's frequency, as shown beside the buttons, and its full
    // description for the tooltip. Empty text: not set yet.
    void setParked(const QString& text, const QString& toolTip);
    // A / B / A=B usable here, and why not when not.
    void setVfoAvailable(bool available, const QString& toolTip = {});
    // Save, Restore and the history usable at all (a receiver to act on).
    void setMemoryAvailable(bool available);
    // What Restore would tune to; empty: nothing saved yet.
    void setSaved(const QString& description);
    // The history's entries, newest first, and the selected one (-1: none).
    void setHistory(const QStringList& labels, int selected);
    // Thetis's flash on the list button: a frequency was added (green) or a
    // listed one changed mode or filter (orange).
    void flashAdded();
    void flashUpdated();

signals:
    void vfoRequested(bool a);
    void copyRequested();
    void saveRequested();
    void restoreRequested();
    void stepRequested(int direction);   // -1: <, +1: >
    void historyPicked(int index);
    void clearHistoryRequested();

private:
    void showHistoryMenu();
    void flash(const QString& color);
    void refreshEnabled();

    QPushButton* m_a{nullptr};
    QPushButton* m_b{nullptr};
    QPushButton* m_copy{nullptr};
    QLabel* m_parked{nullptr};
    QPushButton* m_save{nullptr};
    QPushButton* m_restore{nullptr};
    QPushButton* m_prev{nullptr};
    QPushButton* m_list{nullptr};
    QPushButton* m_next{nullptr};
    QTimer* m_flashTimer{nullptr};

    bool m_liveA{true};
    bool m_vfoAvailable{false};
    bool m_memoryAvailable{false};
    bool m_hasSaved{false};
    QString m_vfoDefaultTipA;
    QString m_vfoDefaultTipB;
    QString m_copyDefaultTip;
    QStringList m_history;
    int m_selected{-1};
};

}  // namespace AetherSDR
