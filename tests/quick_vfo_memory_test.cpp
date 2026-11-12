// brick3-angelia: QuickVfoMemory, the title bar's VFO A/B, Save/Restore and
// frequency history (Thetis's quick-recall pad). Plain C++, no Qt.

#include "core/QuickVfoMemory.h"

#include <cstdio>
#include <string>

using AetherSDR::QuickVfoMemory;
using AetherSDR::QuickVfoSetting;
using Vfo = QuickVfoMemory::Vfo;
using Dwell = QuickVfoMemory::DwellResult;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

QuickVfoSetting at(long long hz, const char* mode = "USB", int lo = 100, int hi = 2800)
{
    return QuickVfoSetting{hz, mode, lo, hi};
}

void testAB()
{
    QuickVfoMemory m;
    CHECK(m.liveVfo() == Vfo::A);
    CHECK(m.parked().empty());

    // B was never set: picking it starts B where A is, and nothing retunes.
    CHECK(!m.select(Vfo::B, at(14200000)).has_value());
    CHECK(m.liveVfo() == Vfo::B);
    CHECK(m.parked() == at(14200000));

    // Tune B elsewhere, then back to A: the receiver goes to A, B is parked.
    const auto toA = m.select(Vfo::A, at(7074000, "DIGU", 0, 3000));
    CHECK(toA.has_value() && *toA == at(14200000));
    CHECK(m.liveVfo() == Vfo::A);
    CHECK(m.parked() == at(7074000, "DIGU", 0, 3000));

    // Picking the live VFO again does nothing.
    CHECK(!m.select(Vfo::A, at(14210000)).has_value());
    CHECK(m.parked() == at(7074000, "DIGU", 0, 3000));

    // A then B swaps, mode and filter travelling with each.
    const auto toB = m.select(Vfo::B, at(14210000, "LSB", -2800, -100));
    CHECK(toB.has_value() && *toB == at(7074000, "DIGU", 0, 3000));
    CHECK(m.parked() == at(14210000, "LSB", -2800, -100));

    // A=B copies the live VFO into the parked one.
    m.copyLiveToParked(at(7075000, "DIGU", 0, 3000));
    CHECK(m.parked() == at(7075000, "DIGU", 0, 3000));
    CHECK(m.liveVfo() == Vfo::B);

    // An empty receiver setting (no slice) changes nothing.
    CHECK(!m.select(Vfo::A, QuickVfoSetting{}).has_value());
    CHECK(m.liveVfo() == Vfo::B);
    m.copyLiveToParked(QuickVfoSetting{});
    CHECK(m.parked() == at(7075000, "DIGU", 0, 3000));
}

void testSaveRestore()
{
    QuickVfoMemory m;
    CHECK(m.saved().empty());
    m.save(at(14143000, "USB"));
    CHECK(m.saved() == at(14143000, "USB"));
    m.save(QuickVfoSetting{});
    CHECK(m.saved() == at(14143000, "USB"));
    m.save(at(3710000, "LSB", -2800, -100));
    CHECK(m.saved() == at(3710000, "LSB", -2800, -100));
}

void testHistory()
{
    QuickVfoMemory m;
    CHECK(m.selectedIndex() == -1);
    CHECK(!m.step(1).has_value());
    CHECK(m.dwell(QuickVfoSetting{}) == Dwell::None);

    CHECK(m.dwell(at(3710000, "LSB")) == Dwell::Added);
    CHECK(m.selectedIndex() == 0);
    // One entry: the arrows have nowhere to go.
    CHECK(!m.step(1).has_value());
    CHECK(!m.step(-1).has_value());

    CHECK(m.dwell(at(7100000, "LSB")) == Dwell::Added);
    CHECK(m.dwell(at(14200000)) == Dwell::Added);
    CHECK(m.history().size() == 3);
    CHECK(m.history()[0].hz == 14200000);   // newest first
    CHECK(m.history()[2].hz == 3710000);

    // Staying on a listed frequency again selects it without a new entry.
    CHECK(m.dwell(at(7100000, "LSB")) == Dwell::Selected);
    CHECK(m.history().size() == 3);
    CHECK(m.selectedIndex() == 1);
    // A new mode on a listed frequency updates it in place.
    CHECK(m.dwell(at(7100000, "CW", 400, 800)) == Dwell::Updated);
    CHECK(m.history()[1] == at(7100000, "CW", 400, 800));
    CHECK(m.selectedIndex() == 1);

    // Thetis's stepping: > moves away from the newest, < towards it, wrapping.
    auto s = m.step(1);
    CHECK(s.has_value() && s->hz == 3710000 && m.selectedIndex() == 2);
    s = m.step(1);
    CHECK(s.has_value() && s->hz == 14200000 && m.selectedIndex() == 0);
    s = m.step(-1);
    CHECK(s.has_value() && s->hz == 3710000 && m.selectedIndex() == 2);
    s = m.step(-1);
    CHECK(s.has_value() && s->hz == 7100000 && m.selectedIndex() == 1);

    // Picking from the list.
    s = m.pick(0);
    CHECK(s.has_value() && s->hz == 14200000 && m.selectedIndex() == 0);
    CHECK(!m.pick(3).has_value());
    CHECK(!m.pick(-1).has_value());
    CHECK(m.selectedIndex() == 0);

    // The 17th frequency pushes the oldest out.
    QuickVfoMemory full;
    for (int i = 0; i < QuickVfoMemory::kHistoryCapacity + 1; ++i)
        full.dwell(at(7000000 + i * 1000));
    CHECK(full.history().size() == static_cast<size_t>(QuickVfoMemory::kHistoryCapacity));
    CHECK(full.history().front().hz == 7016000);
    CHECK(full.history().back().hz == 7001000);

    m.clearHistory();
    CHECK(m.history().empty());
    CHECK(m.selectedIndex() == -1);
    CHECK(!m.step(1).has_value());
}

void testRestoreStoredState()
{
    QuickVfoMemory m;
    std::vector<QuickVfoSetting> stored = {at(14200000), QuickVfoSetting{}, at(7100000),
                                           at(14200000, "CW")};
    for (int i = 0; i < 20; ++i)
        stored.push_back(at(10000000 + i * 1000));
    m.restore(Vfo::B, at(7074000, "DIGU"), at(14143000), stored, 1);
    CHECK(m.liveVfo() == Vfo::B);
    CHECK(m.parked() == at(7074000, "DIGU"));
    CHECK(m.saved() == at(14143000));
    // The empty entry and the duplicate frequency are dropped, the rest capped.
    CHECK(m.history().size() == static_cast<size_t>(QuickVfoMemory::kHistoryCapacity));
    CHECK(m.history()[0] == at(14200000));
    CHECK(m.history()[1] == at(7100000));
    CHECK(m.history()[2].hz == 10000000);
    CHECK(m.selectedIndex() == 1);

    // A selection out of range falls back to the newest.
    m.restore(Vfo::A, {}, {}, {at(3710000), at(3720000)}, 9);
    CHECK(m.selectedIndex() == 0);
    CHECK(m.parked().empty());
    CHECK(m.saved().empty());
    // Nothing stored: nothing selected.
    m.restore(Vfo::A, {}, {}, {}, 0);
    CHECK(m.selectedIndex() == -1);
}

}  // namespace

int main()
{
    testAB();
    testSaveRestore();
    testHistory();
    testRestoreStoredState();
    if (g_failures == 0)
        std::printf("quick_vfo_memory_test: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
