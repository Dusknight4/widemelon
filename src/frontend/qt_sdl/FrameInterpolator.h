// WideMelon motion smoothing: motion-compensated frame interpolation.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <deque>

#include "OpenGLSupport.h"
#include "types.h"

namespace WideMelon
{

// Generates in-between frames for the two DS screens, like the motion
// smoothing on TVs. Each screen keeps its own history of distinct images
// (30 fps games show every image twice), motion is estimated between
// neighbouring images by block matching at DS resolution, and an output
// frame at any time between them is built by moving pixels along those
// motion vectors.
//
// The dominant (camera) motion of each pair of images is found by a vote
// among the blocks and used as a prior, so repeating textures such as tall
// grass or rows of trees keep their real motion instead of a look-alike
// shifted by one repetition. Pixels that can't be matched (objects appearing
// or disappearing) fall back to the nearer real frame, and when too much of
// the screen can't be matched (menus opening, fades, scene changes) no
// in-between frame is generated at all.
//
// All methods need the OpenGL context that owns the source textures.
class FrameInterpolator
{
public:
    static constexpr int NumLayers = 2;

    FrameInterpolator() = default;
    ~FrameInterpolator();
    FrameInterpolator(const FrameInterpolator&) = delete;
    FrameInterpolator& operator=(const FrameInterpolator&) = delete;

    bool Init();

    // Records emulated frame `frame` from a two-layer RGBA texture array of
    // width x height texels (height is 192 times the render scale).
    void Submit(GLuint source, int width, int height, melonDS::u32 frame);

    // Texture array with both screens as they would look at emulated frame
    // time `time`, which may fall between two submitted frames. Returns 0 if
    // no frame has been submitted yet.
    GLuint Output(double time);

    // Emulated frames per distinct image, for the slower of the two screens.
    int ContentPeriod() const;

    void Reset();

private:
    static constexpr int NumSlots = 4;      // three distinct images + one incoming
    static constexpr int MaxHistory = NumSlots - 1;
    static constexpr int MaxGap = 4;        // longer pauses are not interpolated
    static constexpr int NumVotes = 64;     // candidates for the global motion

    struct Slot
    {
        GLuint Full = 0;    // render-scale copy
        GLuint Small = 0;   // DS resolution
        GLuint Quarter = 0; // quarter DS resolution, for the coarse motion search
        double Time = 0.0;
    };

    struct Layer
    {
        std::array<Slot, NumSlots> Slots;
        std::deque<int> Order;              // distinct images, oldest first
        int Scratch = 0;
        melonDS::u32 LastDistinct = 0;
        std::deque<int> Intervals;
        int Period = 1;

        // motion field for the pair of images last interpolated
        double PairA = -1.0, PairB = -1.0;
        GLuint Coarse = 0, Fine = 0, Smooth = 0, Assign = 0;
        GLuint Votes = 0;                   // NumVotes x 1: candidate global vectors and support
        GLuint Global = 0;                  // 1x1: global vector, support
        GLuint Stats = 0;                   // 1x1: share of blocks without a good match

        // the field and global vector, kept as the prediction for the following pair
        GLuint History = 0;
        double HistoryB = -1.0;
    };

    bool Allocate(int width, int height);
    void Release();
    void Estimate(Layer& layer, const Slot& a, const Slot& b);
    void Compose(Layer& layer, int index, double time);
    void Draw(GLuint target, int layer, int x, int y, int width, int height);
    void Blit(GLuint source, int sourceLayer, GLuint target, int targetLayer, int width, int height);
    void Downsample(GLuint source, GLuint target, int scale, int width, int height);
    void UpdatePeriod(Layer& layer, melonDS::u32 frame);

    bool Inited = false;
    bool HasSubmitted = false;
    melonDS::u32 LastSubmitted = 0;
    int Width = 0, Height = 0;      // render scale
    int Scale = 1;
    int NativeW = 0, NativeH = 0;   // DS resolution
    int GridW = 0, GridH = 0;       // 8x8 motion blocks
    int DiffW = 0, DiffH = 0;       // 16x16 change-detection regions

    std::array<Layer, NumLayers> Layers;

    GLuint Output_ = 0;             // two-layer array shown on screen
    GLuint DiffTex = 0;
    GLuint ReadFB = 0, DrawFB = 0;
    GLuint EmptyVAO = 0;

    GLuint DownsampleProgram = 0, CaptureProgram = 0, DiffProgram = 0, CoarseProgram = 0, RefineProgram = 0;
    GLuint VoteProgram = 0, PickProgram = 0, SmoothProgram = 0, StatsProgram = 0;
    GLuint AssignProgram = 0, ComposeProgram = 0;
};

}
