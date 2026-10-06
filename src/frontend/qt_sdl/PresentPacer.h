// WideMelon motion smoothing: how many images to present per emulated frame.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <deque>
#include <vector>

namespace WideMelon
{

// Chooses how many images motion smoothing presents per emulated frame on a
// display faster than the game (up to two at 120 Hz).
//
// With vsync, every present waits for its own display refresh, and the GPU
// work of the next emulated frame queues up behind the last of them. With two
// presents per frame, the next frame's emulation therefore only gets the one
// refresh after the second present to run in. When it takes longer, frames come
// in late and the game drops below full speed. Then this falls back to one
// present per frame, shown for two refreshes, which leaves the whole frame time
// for emulation; 30 fps scenes are still smoothed to 60 fps. Two presents per
// frame come back once emulation is fast enough (for example with the JIT
// recompiler), measured while presenting once per frame.
class PresentPacer
{
public:
    // ratio: display refreshes per emulated frame (1 to 4).
    // emuMs: how long emulating the last frame took.
    // periodMs: time since the previous frame started.
    // Returns the number of presents for this frame: 1 or ratio.
    int Update(int ratio, double emuMs, double periodMs, double frameMs, double refreshMs)
    {
        if (ratio != Ratio)
        {
            Ratio = ratio;
            Reset();
        }
        if (ratio <= 1) return 1;

        SinceChange++;
        if (Count > 1)
        {
            Late.push_back(periodMs > frameMs * 1.3);
            if (Late.size() > 60) Late.pop_front();
            if (std::count(Late.begin(), Late.end(), true) >= 2)
            {
                // it didn't hold up: wait longer before the next try if the last one was recent
                Holdoff = (SinceChange < 600) ? std::min(Holdoff * 2, 7200) : 120;
                Change(1);
            }
        }
        else
        {
            EmuTimes.push_back(emuMs);
            if (EmuTimes.size() > 120) EmuTimes.pop_front();
            if (SinceChange >= Holdoff && EmuTimes.size() == 120)
            {
                std::vector<double> sorted(EmuTimes.begin(), EmuTimes.end());
                std::nth_element(sorted.begin(), sorted.begin() + 108, sorted.end());
                if (sorted[108] < refreshMs * 0.6)
                    Change(ratio);
            }
        }
        return Count;
    }

    void Reset()
    {
        Count = 1;
        SinceChange = 0;
        Holdoff = 120;
        EmuTimes.clear();
        Late.clear();
    }

private:
    void Change(int count)
    {
        Count = count;
        SinceChange = 0;
        EmuTimes.clear();
        Late.clear();
    }

    int Ratio = 1;
    int Count = 1;
    int SinceChange = 0;
    int Holdoff = 120;
    std::deque<double> EmuTimes;
    std::deque<bool> Late;
};

}
