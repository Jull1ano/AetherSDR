#include "QuickVfoMemory.h"

namespace AetherSDR {

std::optional<QuickVfoSetting> QuickVfoMemory::select(Vfo vfo, const QuickVfoSetting& live)
{
    if (vfo == m_live || live.empty())
        return std::nullopt;
    const QuickVfoSetting target = m_parked;
    m_parked = live;
    m_live = vfo;
    if (target.empty())
        return std::nullopt;   // a VFO never set starts where the other one is
    return target;
}

void QuickVfoMemory::copyLiveToParked(const QuickVfoSetting& live)
{
    if (!live.empty())
        m_parked = live;
}

void QuickVfoMemory::save(const QuickVfoSetting& live)
{
    if (!live.empty())
        m_saved = live;
}

int QuickVfoMemory::indexOf(long long hz) const
{
    for (int i = 0; i < static_cast<int>(m_history.size()); ++i) {
        if (m_history[static_cast<size_t>(i)].hz == hz)
            return i;
    }
    return -1;
}

QuickVfoMemory::DwellResult QuickVfoMemory::dwell(const QuickVfoSetting& live)
{
    if (live.empty())
        return DwellResult::None;
    const int index = indexOf(live.hz);
    if (index < 0) {
        m_history.insert(m_history.begin(), live);
        if (static_cast<int>(m_history.size()) > kHistoryCapacity)
            m_history.resize(kHistoryCapacity);
        m_selected = 0;
        return DwellResult::Added;
    }
    m_selected = index;
    QuickVfoSetting& entry = m_history[static_cast<size_t>(index)];
    if (entry == live)
        return DwellResult::Selected;
    entry = live;
    return DwellResult::Updated;
}

std::optional<QuickVfoSetting> QuickVfoMemory::step(int direction)
{
    const int count = static_cast<int>(m_history.size());
    if (m_selected < 0 || count < 2 || direction == 0)
        return std::nullopt;
    m_selected = ((m_selected + (direction < 0 ? -1 : 1)) % count + count) % count;
    return m_history[static_cast<size_t>(m_selected)];
}

std::optional<QuickVfoSetting> QuickVfoMemory::pick(int index)
{
    if (index < 0 || index >= static_cast<int>(m_history.size()))
        return std::nullopt;
    m_selected = index;
    return m_history[static_cast<size_t>(index)];
}

void QuickVfoMemory::clearHistory()
{
    m_history.clear();
    m_selected = -1;
}

void QuickVfoMemory::restore(Vfo live, const QuickVfoSetting& parked,
                             const QuickVfoSetting& saved,
                             const std::vector<QuickVfoSetting>& history, int selected)
{
    reset();
    m_live = live;
    if (!parked.empty())
        m_parked = parked;
    if (!saved.empty())
        m_saved = saved;
    for (const QuickVfoSetting& entry : history) {
        if (entry.empty() || indexOf(entry.hz) >= 0)
            continue;
        m_history.push_back(entry);
        if (static_cast<int>(m_history.size()) == kHistoryCapacity)
            break;
    }
    if (!m_history.empty())
        m_selected = (selected >= 0 && selected < static_cast<int>(m_history.size())) ? selected : 0;
}

void QuickVfoMemory::reset()
{
    m_live = Vfo::A;
    m_parked = {};
    m_saved = {};
    m_history.clear();
    m_selected = -1;
}

}  // namespace AetherSDR
