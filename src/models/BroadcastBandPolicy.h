#pragma once

// brick3-angelia: band-aware receive for client-owned radios (ANAN, HL2).
//
// Tuning INTO a broadcast band switches the slice to AM and to that band's
// channel step (9 kHz on long/medium wave, 5 kHz on the shortwave broadcast
// bands); tuning back OUT restores the mode the slice had before. The last
// step used is remembered per place: per broadcast band type inside one, per
// modulation family (SSB, CW, DIG, AM, FM) everywhere else.
//
// Plain C++ with no Qt so the decisions are testable on their own
// (tests/broadcast_band_policy_test.cpp); RadioModel applies them.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace AetherSDR::broadcast {

enum class Kind { None, LongMediumWave, ShortWave };

struct Band {
    double lowHz;
    double highHz;
    Kind kind;
    const char* name;
};

// ITU broadcasting allocations, Region 1 (the edges a European listener's
// receiver uses). The amateur bands do not overlap them: 40 m ends at
// 7.200 MHz, where 41 m begins.
inline constexpr std::array<Band, 16> kBands{{
    {148'500.0, 283'500.0, Kind::LongMediumWave, "LW"},
    {526'500.0, 1'606'500.0, Kind::LongMediumWave, "MW"},
    {2'300'000.0, 2'495'000.0, Kind::ShortWave, "120m"},
    {3'200'000.0, 3'400'000.0, Kind::ShortWave, "90m"},
    {3'900'000.0, 4'000'000.0, Kind::ShortWave, "75m"},
    {4'750'000.0, 5'060'000.0, Kind::ShortWave, "60m"},
    {5'900'000.0, 6'200'000.0, Kind::ShortWave, "49m"},
    {7'200'000.0, 7'450'000.0, Kind::ShortWave, "41m"},
    {9'400'000.0, 9'900'000.0, Kind::ShortWave, "31m"},
    {11'600'000.0, 12'100'000.0, Kind::ShortWave, "25m"},
    {13'570'000.0, 13'870'000.0, Kind::ShortWave, "22m"},
    {15'100'000.0, 15'800'000.0, Kind::ShortWave, "19m"},
    {17'480'000.0, 17'900'000.0, Kind::ShortWave, "16m"},
    {18'900'000.0, 19'020'000.0, Kind::ShortWave, "15m"},
    {21'450'000.0, 21'850'000.0, Kind::ShortWave, "13m"},
    {25'670'000.0, 26'100'000.0, Kind::ShortWave, "11m"},
}};

inline Kind kindAt(double hz) noexcept
{
    for (const Band& b : kBands) {
        if (hz >= b.lowHz && hz < b.highHz)
            return b.kind;
    }
    return Kind::None;
}

inline int defaultStepHz(Kind kind) noexcept
{
    switch (kind) {
    case Kind::LongMediumWave: return 9'000;
    case Kind::ShortWave: return 5'000;
    case Kind::None: break;
    }
    return 0;
}

inline std::string upper(std::string_view s)
{
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

// Modes that share a step: LSB and USB are one modulation, and so on.
inline std::string modeFamily(std::string_view mode)
{
    const std::string m = upper(mode);
    if (m == "LSB" || m == "USB") return "SSB";
    if (m == "CW" || m == "CWL" || m == "CWU") return "CW";
    if (m == "DIGU" || m == "DIGL" || m == "RTTY") return "DIG";
    if (m == "AM" || m == "SAM" || m == "DSB") return "AM";
    if (m == "FM" || m == "NFM" || m == "DFM") return "FM";
    return m;
}

inline bool isAmFamily(std::string_view mode) { return modeFamily(mode) == "AM"; }

// Where a step is remembered: per broadcast band type inside one (whatever
// the mode), per modulation family elsewhere.
inline std::string contextKey(Kind kind, std::string_view mode)
{
    switch (kind) {
    case Kind::LongMediumWave: return "BC-MW";
    case Kind::ShortWave: return "BC-SW";
    case Kind::None: break;
    }
    return modeFamily(mode);
}

// "BC-MW=9000;SSB=500" <-> map. Malformed or out-of-range entries are dropped.
using StepTable = std::map<std::string, int>;

inline std::string serializeSteps(const StepTable& table)
{
    std::string out;
    for (const auto& [key, hz] : table) {
        if (!out.empty()) out += ';';
        out += key + '=' + std::to_string(hz);
    }
    return out;
}

inline StepTable parseSteps(std::string_view text)
{
    StepTable table;
    while (!text.empty()) {
        const std::size_t end = text.find(';');
        const std::string_view item = text.substr(0, end);
        const std::size_t eq = item.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            int hz = 0;
            bool digits = eq + 1 < item.size();
            for (std::size_t i = eq + 1; i < item.size() && digits; ++i) {
                const char c = item[i];
                digits = c >= '0' && c <= '9' && hz <= 100'000'000;
                if (digits) hz = hz * 10 + (c - '0');
            }
            if (digits && hz >= 1 && hz <= 1'000'000)
                table[std::string(item.substr(0, eq))] = hz;
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return table;
}

// One slice's transitions. Steps live in a StepTable shared by all slices.
class Policy {
public:
    struct Action {
        std::optional<std::string> mode;   // request this mode
        std::optional<int> stepHz;         // apply this step
    };

    [[nodiscard]] bool known() const noexcept { return m_known; }
    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] const std::string& context() const noexcept { return m_context; }

    // A frequency change (click, drag, wheel, band button). The first one
    // only learns where the slice is: a restored session is left as it was.
    // `modeChosen`: the operator picked the mode with this move (a band
    // button carries one, CB and WWV AM among them) -- the step still follows,
    // the mode is theirs.
    Action onFrequency(double hz, std::string_view reportedMode, int currentStepHz,
                       StepTable& steps, bool modeChosen = false)
    {
        Action action;
        const Kind kind = kindAt(hz);
        // A mode this policy asked for and the radio has not confirmed yet is
        // the slice's mode as far as the next decision goes: tuning back
        // across the edge before it lands must still undo it.
        if (modeChosen)
            m_pendingMode.reset();
        const std::string currentMode =
            m_pendingMode ? *m_pendingMode : std::string(reportedMode);
        if (!m_known) {
            m_known = true;
            m_kind = kind;
            m_context = contextKey(kind, currentMode);
            return action;
        }
        if (kind == m_kind)
            return action;

        std::string targetMode(currentMode);
        if (m_kind == Kind::None) {
            // Entering: remember what to come back to.
            m_modeBefore = modeChosen ? std::string() : std::string(currentMode);
            if (!modeChosen && !isAmFamily(currentMode))
                targetMode = "AM";
        } else if (kind == Kind::None) {
            // Leaving: only an AM slice is switched back; a mode the operator
            // picked inside the broadcast band (USB for a utility station,
            // say) is theirs and stays.
            if (!modeChosen && isAmFamily(currentMode)) {
                if (!m_modeBefore.empty())
                    targetMode = m_modeBefore;
                else
                    targetMode = hz < 10'000'000.0 ? "LSB" : "USB";
            }
            m_modeBefore.clear();
        }
        // MW <-> SW directly: the mode stays.
        if (upper(targetMode) != upper(currentMode)) {
            action.mode = targetMode;
            m_pendingMode = targetMode;
        }

        moveTo(contextKey(kind, targetMode), kind, currentStepHz, steps, action);
        m_kind = kind;
        return action;
    }

    // The slice's mode changed (by the operator, or one this policy asked
    // for landing). Outside a broadcast band the step follows the modulation.
    Action onMode(std::string_view mode, int currentStepHz, StepTable& steps)
    {
        Action action;
        m_pendingMode.reset();   // the slice's mode is known again
        if (!m_known)
            return action;
        const std::string ctx = contextKey(m_kind, mode);
        if (ctx != m_context)
            moveTo(ctx, m_kind, currentStepHz, steps, action);
        return action;
    }

    // The step changed: remember it for where the slice is. True when the
    // table changed (the caller persists it).
    bool onStep(int hz, StepTable& steps) const
    {
        if (!m_known || hz <= 0)
            return false;
        auto it = steps.find(m_context);
        if (it != steps.end() && it->second == hz)
            return false;
        steps[m_context] = hz;
        return true;
    }

private:
    void moveTo(const std::string& ctx, Kind kind, int currentStepHz,
                StepTable& steps, Action& action)
    {
        if (ctx == m_context)
            return;
        // The place being left keeps the step it last used, even one never
        // changed by hand (the step the session started with).
        if (currentStepHz > 0 && !m_context.empty())
            steps[m_context] = currentStepHz;
        int want = 0;
        if (auto it = steps.find(ctx); it != steps.end())
            want = it->second;
        else
            want = defaultStepHz(kind);
        if (want > 0 && want != currentStepHz)
            action.stepHz = want;
        m_context = ctx;
    }

    bool m_known = false;
    Kind m_kind = Kind::None;
    std::string m_context;
    std::string m_modeBefore;
    std::optional<std::string> m_pendingMode;   // asked for, not yet confirmed
};

}  // namespace AetherSDR::broadcast
