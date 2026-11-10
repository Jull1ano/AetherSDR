// Band-aware receive (brick3-angelia): broadcast bands switch to AM with the
// band's channel step, leaving restores the mode, and the step is remembered
// per broadcast band type and per modulation.
#include "models/BroadcastBandPolicy.h"

#include <cstdio>
#include <string>

using namespace AetherSDR::broadcast;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A slice as RadioModel drives it: actions apply at once, as the step does;
// the mode is applied too, then reported back through onMode().
struct Slice {
    Policy policy;
    StepTable& steps;
    double hz;
    std::string mode;
    int step;

    void tune(double newHz)
    {
        hz = newHz;
        apply(policy.onFrequency(hz, mode, step, steps));
    }
    void setMode(const std::string& m)
    {
        mode = m;
        apply(policy.onMode(mode, step, steps));
    }
    void setStep(int s)
    {
        step = s;
        policy.onStep(step, steps);
    }
    void apply(const Policy::Action& a)
    {
        if (a.stepHz) setStep(*a.stepHz);
        if (a.mode) setMode(*a.mode);
    }
};

}  // namespace

int main()
{
    check(kindAt(603'000.0) == Kind::LongMediumWave, "603 kHz is medium wave");
    check(kindAt(198'000.0) == Kind::LongMediumWave, "198 kHz is long wave");
    check(kindAt(7'199'999.0) == Kind::None, "7.199999 MHz is 40 m amateur");
    check(kindAt(7'200'000.0) == Kind::ShortWave, "7.200 MHz starts 41 m");
    check(kindAt(7'350'000.0) == Kind::ShortWave, "7.350 MHz is 41 m");
    check(kindAt(14'200'000.0) == Kind::None, "20 m is not broadcast");
    check(kindAt(3'700'000.0) == Kind::None, "80 m is not broadcast");
    check(kindAt(9'650'000.0) == Kind::ShortWave, "31 m");
    check(modeFamily("lsb") == "SSB" && modeFamily("USB") == "SSB", "LSB/USB share a step");
    check(modeFamily("SAM") == "AM" && modeFamily("CWL") == "CW", "families");

    StepTable steps;
    Slice s{{}, steps, 7'100'000.0, "LSB", 500};
    s.tune(7'100'000.0);   // first sight: learns, changes nothing
    check(s.mode == "LSB" && s.step == 500, "first frequency leaves a restored session alone");

    s.tune(7'150'000.0);
    check(s.mode == "LSB" && s.step == 500, "moving inside 40 m changes nothing");

    s.tune(7'350'000.0);   // click into 41 m
    check(s.mode == "AM", "entering 41 m switches to AM");
    check(s.step == 5'000, "shortwave broadcast step is 5 kHz");

    s.tune(7'355'000.0);
    check(s.mode == "AM" && s.step == 5'000, "moving inside 41 m changes nothing");

    s.tune(7'120'000.0);   // back into 40 m
    check(s.mode == "LSB", "leaving 41 m restores LSB");
    check(s.step == 500, "and the SSB step it had");

    s.tune(603'000.0);     // band button to MW
    check(s.mode == "AM" && s.step == 9'000, "medium wave: AM, 9 kHz");

    s.setStep(10'000);     // operator prefers 10 kHz on MW
    s.tune(7'250'000.0);   // MW -> 41 m directly
    check(s.mode == "AM" && s.step == 5'000, "MW to 41 m: still AM, shortwave step");
    s.setStep(1'000);      // operator fine-tunes on 41 m
    s.tune(1'000'000.0);
    check(s.step == 10'000, "MW remembers the operator's 10 kHz");
    s.tune(9'650'000.0);
    check(s.step == 1'000, "shortwave remembers the operator's 1 kHz");

    s.tune(14'200'000.0);  // to 20 m: back to the mode used before MW
    check(s.mode == "LSB", "leaving restores the mode from before the broadcast bands");
    check(s.step == 500, "with the SSB step");

    // Per-modulation step outside broadcast bands.
    s.setMode("CW");
    check(s.step == 500, "a modulation with no step yet keeps the current one");
    s.setStep(10);
    s.setMode("USB");
    check(s.step == 500, "SSB step comes back with USB");
    s.setMode("CW");
    check(s.step == 10, "CW step comes back with CW");
    s.setMode("USB");

    // A mode picked by hand inside a broadcast band is kept on the way out.
    s.tune(7'300'000.0);
    check(s.mode == "AM", "enter 41 m again");
    s.setMode("USB");
    s.tune(7'100'000.0);
    check(s.mode == "USB", "a hand-picked non-AM mode is not overridden on leaving");

    // Already in AM before entering: stays AM after leaving.
    s.setMode("AM");
    s.tune(7'300'000.0);
    s.tune(7'100'000.0);
    check(s.mode == "AM", "AM before entering stays AM after leaving");

    // Started inside a broadcast band (restored session): leaving picks the
    // band's usual sideband.
    StepTable steps2;
    Slice r{{}, steps2, 9'650'000.0, "AM", 5'000};
    r.tune(9'650'000.0);
    r.tune(14'250'000.0);
    check(r.mode == "USB", "no remembered mode above 10 MHz: USB");
    Slice r2{{}, steps2, 6'000'000.0, "AM", 5'000};
    r2.tune(6'000'000.0);
    r2.tune(3'700'000.0);
    check(r2.mode == "LSB", "no remembered mode below 10 MHz: LSB");

    // The radio confirms a mode later than the step: tuning back across the
    // edge before AM lands must still end in the mode from before.
    {
        StepTable steps3;
        Policy p;
        p.onFrequency(7'100'000.0, "LSB", 500, steps3);
        const auto in = p.onFrequency(7'300'000.0, "LSB", 500, steps3);
        check(in.mode && *in.mode == "AM", "race: entering asks for AM");
        const auto out = p.onFrequency(7'100'000.0, "LSB", 5'000, steps3);   // AM not landed
        check(out.mode && *out.mode == "LSB", "race: leaving before AM lands asks for LSB back");
        p.onMode("AM", 5'000, steps3);    // the late AM lands...
        p.onMode("LSB", 500, steps3);     // ...then the LSB
        check(p.context() == "SSB", "race: ends in the SSB context");
    }

    // Persistence round trip.
    StepTable t{{"BC-MW", 9'000}, {"BC-SW", 5'000}, {"SSB", 500}, {"CW", 10}};
    check(parseSteps(serializeSteps(t)) == t, "steps survive a save and load");
    check(parseSteps("SSB=500;bad;CW=;=7;AM=0;FM=99999999999;DIG=250").size() == 2,
          "malformed entries are dropped");

    if (failures == 0)
        std::printf("broadcast_band_policy_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
