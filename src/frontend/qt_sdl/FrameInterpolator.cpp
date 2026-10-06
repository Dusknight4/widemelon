// WideMelon motion smoothing: motion-compensated frame interpolation.
// Copyright (C) 2026 WideMelon contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FrameInterpolator.h"

#include <algorithm>
#include <vector>

namespace WideMelon
{

namespace
{

// Tuning, in mean absolute colour difference per channel (0..1).
constexpr float BadBlockCost = 0.05f;    // a block this far off has no real match
constexpr float CutShare = 0.2f;         // this share of bad blocks means the screen changed
constexpr float StillCutShare = 0.05f;   // the same while the camera is still

// All passes draw one triangle covering the target and work in texel space.
const char* kVS = R"(#version 140
void main()
{
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Box-filters an image down by uScale.
const char* kDownsampleFS = R"(#version 140
uniform sampler2D Src;
uniform int uScale;
out vec4 oColor;
void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * uScale;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < uScale; y++)
        for (int x = 0; x < uScale; x++)
            sum += texelFetch(Src, base + ivec2(x, y), 0);
    oColor = sum / float(uScale * uScale);
}
)";

// The same, straight from one screen of the emulator's output.
const char* kCaptureFS = R"(#version 140
uniform sampler2DArray Src;
uniform int uLayer;
uniform int uScale;
out vec4 oColor;
void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * uScale;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < uScale; y++)
        for (int x = 0; x < uScale; x++)
            sum += texelFetch(Src, ivec3(base + ivec2(x, y), uLayer), 0);
    oColor = sum / float(uScale * uScale);
}
)";

// Largest colour change inside each 16x16 region, to tell new images from repeats.
const char* kDiffFS = R"(#version 140
uniform sampler2D A;
uniform sampler2D B;
uniform ivec2 uSize;
uniform int uRowOffset;
out vec4 oColor;
void main()
{
    ivec2 base = (ivec2(gl_FragCoord.xy) - ivec2(0, uRowOffset)) * 16;
    float m = 0.0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
        {
            ivec2 p = min(base + ivec2(x, y), uSize - 1);
            vec3 d = abs(texelFetch(A, p, 0).rgb - texelFetch(B, p, 0).rgb);
            m = max(m, max(d.r, max(d.g, d.b)));
        }
    oColor = vec4(m);
}
)";

// Motion is estimated for each 8x8 block of the in-between frame. A vector v
// means A at p - v/2 matches B at p + v/2 (symmetric matching, so every block
// of the in-between frame gets exactly one vector). Vectors are in DS pixels,
// costs are the mean absolute colour difference over the block. Vector
// textures hold (vector, cost of that vector).

// Full search at quarter resolution, preferring the previous pair's camera
// motion when there is one, otherwise the shortest motion. Also rates how
// distinctive the block is: how much better its best match is than the best
// one more than two pixels away. Repeating textures, where a look-alike one
// repetition away matches just as well, get no confidence.
const char* kCoarseFS = R"(#version 140
uniform sampler2D A;
uniform sampler2D B;
uniform sampler2D Prior;
uniform vec2 uSize;
uniform bool uHavePrior;
out vec4 oColor;
const int Range = 4;
float Cost(vec2 origin, vec2 v)
{
    float s = 0.0;
    for (int y = -1; y < 3; y++)
        for (int x = -1; x < 3; x++)
        {
            vec2 p = origin + vec2(x, y) + 0.5;
            vec3 d = abs(texture(A, (p - 0.5 * v) / uSize).rgb - texture(B, (p + 0.5 * v) / uSize).rgb);
            s += d.r + d.g + d.b;
        }
    return s / 48.0;
}
void main()
{
    vec2 origin = floor(gl_FragCoord.xy) * 2.0;
    vec2 prior = uHavePrior ? texelFetch(Prior, ivec2(0), 0).xy : vec2(0.0);
    float best = 1e30, bestRaw = 1e30, far = 1e30;
    vec2 bestV = vec2(0.0);
    for (int dy = -Range; dy <= Range; dy++)
        for (int dx = -Range; dx <= Range; dx++)
        {
            vec2 v = vec2(dx, dy);
            float raw = Cost(origin, v);
            float c = raw + 0.0005 * length(v * 4.0 - prior);
            if (c < best)
            {
                // the old best becomes the far runner-up if it is far enough away
                if (length(v - bestV) * 4.0 > 2.0) far = min(far, bestRaw);
                best = c; bestRaw = raw; bestV = v;
            }
            else if (length(v - bestV) * 4.0 > 2.0)
                far = min(far, raw);
        }
    float confidence = clamp((far - bestRaw) / 0.02, 0.0, 1.0);
    oColor = vec4(bestV * 4.0, bestRaw, confidence);
}
)";

// One step of recursive search at DS resolution, as in TV motion estimators.
// Each block tries the global (camera) motion, its vector from the previous
// pair, the median and the vectors of its neighbours, no motion, and small
// updates (uStep pixels) of the best one. Candidates pay for straying from
// the neighbourhood and, scaled by how many blocks agree on it, from the
// global motion: in flat areas the motion found at nearby edges spreads
// inward, and repeating textures (grass, rows of trees), where a look-alike
// one repetition away matches just as well, keep the camera motion. Moving
// objects still get their own vectors because they match clearly better.
const char* kRefineFS = R"(#version 140
uniform sampler2D A;
uniform sampler2D B;
uniform sampler2D Prev;
uniform sampler2D Hist;
uniform sampler2D Global;
uniform vec2 uSize;
uniform ivec2 uGrid;
uniform float uStep;
uniform bool uUseHistory;
uniform bool uHaveGlobal;
out vec4 oColor;
float Cost(vec2 origin, vec2 v)
{
    float s = 0.0;
    for (int y = -1; y < 9; y++)
        for (int x = -1; x < 9; x++)
        {
            vec2 p = origin + vec2(x, y) + 0.5;
            vec3 d = abs(texture(A, (p - 0.5 * v) / uSize).rgb - texture(B, (p + 0.5 * v) / uSize).rgb);
            s += d.r + d.g + d.b;
        }
    return s / 300.0;
}
vec2 pred, hist;
vec3 glob;
float Penalty(vec2 v)
{
    float p = 0.002 * min(length(v - pred), length(v - hist));
    if (uHaveGlobal)
        p += 0.004 * glob.z * min(length(v - glob.xy), 4.0);
    return p;
}
void main()
{
    ivec2 blk = ivec2(gl_FragCoord.xy);
    vec2 origin = vec2(blk) * 8.0;

    vec2 nb[9];
    int k = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            nb[k++] = texelFetch(Prev, clamp(blk + ivec2(dx, dy), ivec2(0), uGrid - 1), 0).xy;

    // vector median of the neighbourhood
    pred = nb[4];
    float spread = 1e30;
    for (int i = 0; i < 9; i++)
    {
        float s = 0.0;
        for (int j = 0; j < 9; j++)
            s += length(nb[i] - nb[j]);
        if (s < spread) { spread = s; pred = nb[i]; }
    }
    hist = uUseHistory ? texelFetch(Hist, blk, 0).xy : pred;
    glob = uHaveGlobal ? texelFetch(Global, ivec2(0), 0).xyz : vec3(0.0);
    glob.z = clamp((glob.z - 0.15) / 0.25, 0.0, 1.0);   // trust it once enough blocks agree

    // the global motion is tried first, so it wins ties
    vec2 cand[9];
    cand[0] = uHaveGlobal ? glob.xy : pred;
    cand[1] = hist; cand[2] = pred; cand[3] = nb[4];
    cand[4] = nb[1]; cand[5] = nb[3]; cand[6] = nb[5]; cand[7] = nb[7];
    cand[8] = vec2(0.0);
    float best = 1e30, bestRaw = 0.0;
    vec2 bestV = cand[0];
    for (int i = 0; i < 9; i++)
    {
        float raw = Cost(origin, cand[i]);
        float c = raw + Penalty(cand[i]);
        if (c < best) { best = c; bestRaw = raw; bestV = cand[i]; }
    }

    vec2 center = bestV;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            if (dx == 0 && dy == 0) continue;
            vec2 v = center + vec2(dx, dy) * uStep;
            float raw = Cost(origin, v);
            float c = raw + Penalty(v) + 0.001;
            if (c < best) { best = c; bestRaw = raw; bestV = v; }
        }
    oColor = vec4(bestV, bestRaw, 0.0);
}
)";

// Global motion vote: each texel takes one candidate (blocks spread over the
// screen, the previous global motion, no motion) and adds up the confidence
// of the blocks that agree with it to within a pixel. Only distinctive blocks
// (edges, paths, trees, sprites) get a say, not repeating textures.
const char* kVoteFS = R"(#version 140
uniform sampler2D Field;
uniform sampler2D Prior;
uniform sampler2D Conf;
uniform ivec2 uGrid;
uniform bool uHavePrior;
out vec4 oColor;
void main()
{
    int i = int(gl_FragCoord.x);
    vec2 c;
    if (i == 62)
        c = uHavePrior ? texelFetch(Prior, ivec2(0), 0).xy : vec2(0.0);
    else if (i == 63)
        c = vec2(0.0);
    else
        c = texelFetch(Field, ivec2((i % 8) * uGrid.x / 8 + uGrid.x / 16, (i / 8) * uGrid.y / 8 + uGrid.y / 16), 0).xy;

    float support = 0.0, total = 0.0;
    for (int y = 0; y < uGrid.y; y++)
        for (int x = 0; x < uGrid.x; x++)
        {
            float w = texelFetch(Conf, ivec2(x, y), 0).w;
            total += w;
            if (length(texelFetch(Field, ivec2(x, y), 0).xy - c) < 1.0)
                support += w;
        }
    oColor = vec4(c, support, total);
}
)";

// Picks the best-supported candidate; the previous global motion wins ties.
// Its trust is the share of the confidence that agrees, reduced when only a
// few blocks were distinctive at all.
const char* kPickFS = R"(#version 140
uniform sampler2D Votes;
uniform float uBlocks;
out vec4 oColor;
void main()
{
    vec4 best = texelFetch(Votes, ivec2(62, 0), 0);
    for (int i = 0; i < 64; i++)
    {
        vec4 v = texelFetch(Votes, ivec2(i, 0), 0);
        if (v.z > best.z) best = v;
    }
    float share = best.z / max(best.w, 1e-3);
    float enough = clamp(best.w / (0.05 * uBlocks), 0.0, 1.0);
    oColor = vec4(best.xy, share * enough, 0.0);
}
)";

// Vector median of each 3x3 group of blocks, to drop isolated wrong vectors.
const char* kSmoothFS = R"(#version 140
uniform sampler2D Fine;
uniform ivec2 uGrid;
out vec4 oColor;
void main()
{
    ivec2 blk = ivec2(gl_FragCoord.xy);
    vec2 v[9];
    int k = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            v[k++] = texelFetch(Fine, clamp(blk + ivec2(dx, dy), ivec2(0), uGrid - 1), 0).xy;
    float best = 1e30;
    vec2 bestV = v[4];
    for (int i = 0; i < 9; i++)
    {
        float s = 0.0;
        for (int j = 0; j < 9; j++)
            s += length(v[i] - v[j]);
        if (s < best) { best = s; bestV = v[i]; }
    }
    oColor = vec4(bestV, texelFetch(Fine, blk, 0).z, 0.0);
}
)";

// Decides whether this pair gets in-between frames at all, from the blocks
// without a good match. A large share of them means the screen changed rather
// than moved (a fade, a new scene, a large menu). When the camera is still
// (menus, the party screen, battles), a smaller share is enough: a submenu or
// text box popping in or out covers well over 5% of the blocks, while a
// walking character or an animated icon covers only 1-2%.
// Output: cut flag, share of bad blocks, mean cost.
const char* kStatsFS = R"(#version 140
uniform sampler2D Fine;
uniform sampler2D Global;
uniform ivec2 uGrid;
uniform float uBadCost;
uniform float uCutShare;
uniform float uStillCutShare;
out vec4 oColor;
void main()
{
    float bad = 0.0, total = 0.0;
    for (int y = 0; y < uGrid.y; y++)
        for (int x = 0; x < uGrid.x; x++)
        {
            float c = texelFetch(Fine, ivec2(x, y), 0).z;
            bad += (c > uBadCost) ? 1.0 : 0.0;
            total += c;
        }
    float n = float(uGrid.x * uGrid.y);
    bool still = length(texelFetch(Global, ivec2(0), 0).xy) < 0.5;
    bool cut = bad / n > (still ? uStillCutShare : uCutShare);
    oColor = vec4(cut ? 1.0 : 0.0, bad / n, total / n, 0.0);
}
)";

// Picks the vector for each DS pixel of the frame at time uT, from its own
// block's vector, the global motion, the four surrounding blocks (and their
// blend) and no motion. Anything but the own block's vector has to match
// clearly better: the block vectors already prefer the camera motion on
// repeating textures, and a pixel inside a sprite whose pattern happens to
// line up with the camera motion still moves with the rest of its sprite.
const char* kAssignFS = R"(#version 140
uniform sampler2D A;
uniform sampler2D B;
uniform sampler2D Smooth;
uniform sampler2D Fine;
uniform sampler2D Global;
uniform vec2 uSize;
uniform ivec2 uGrid;
uniform float uT;
out vec4 oColor;
const vec2 Offsets[5] = vec2[5](vec2(0.0), vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
float Cost(vec2 p, vec2 v)
{
    float s = 0.0;
    for (int i = 0; i < 5; i++)
    {
        vec2 q = p + Offsets[i];
        vec3 d = abs(texture(A, (q - uT * v) / uSize).rgb - texture(B, (q + (1.0 - uT) * v) / uSize).rgb);
        s += d.r + d.g + d.b;
    }
    return s / 15.0;
}
vec2 Block(ivec2 g)
{
    return texelFetch(Smooth, clamp(g, ivec2(0), uGrid - 1), 0).xy;
}
void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 g = p / 8.0 - 0.5;
    ivec2 g0 = ivec2(floor(g));
    vec2 f = fract(g);
    vec2 v00 = Block(g0), v10 = Block(g0 + ivec2(1, 0)), v01 = Block(g0 + ivec2(0, 1)), v11 = Block(g0 + ivec2(1, 1));
    vec2 own = Block(ivec2(p / 8.0));

    vec2 cand[9];
    cand[0] = own;
    cand[1] = texelFetch(Global, ivec2(0), 0).xy;
    cand[2] = texelFetch(Fine, clamp(ivec2(p / 8.0), ivec2(0), uGrid - 1), 0).xy;
    cand[3] = v00; cand[4] = v10; cand[5] = v01; cand[6] = v11;
    cand[7] = mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
    cand[8] = vec2(0.0);
    float best = 1e30, bestRaw = 0.0;
    vec2 bestV = own;
    for (int i = 0; i < 9; i++)
    {
        float raw = Cost(p, cand[i]);
        float pen = 0.012 * min(length(cand[i] - own), 1.0);
        if (i == 8) pen = min(pen, 0.006);   // menus and text stand still
        if (raw + pen < best) { best = raw + pen; bestRaw = raw; bestV = cand[i]; }
    }
    oColor = vec4(bestV, bestRaw, 0.0);
}
)";

// Builds the render-scale frame at time uT from both real frames.
const char* kComposeFS = R"(#version 140
uniform sampler2D A;
uniform sampler2D B;
uniform sampler2D Assign;
uniform sampler2D Stats;
uniform vec2 uSize;
uniform float uScale;
uniform float uT;
out vec4 oColor;
void main()
{
    vec2 p = gl_FragCoord.xy;
    // the screen changed rather than moved: show the real frame instead
    if (texelFetch(Stats, ivec2(0), 0).x > 0.5)
    {
        oColor = texture(A, p / uSize);
        return;
    }
    vec4 m = texelFetch(Assign, ivec2(p / uScale), 0);
    vec2 v = m.xy * uScale;
    vec4 a = texture(A, (p - uT * v) / uSize);
    vec4 b = texture(B, (p + (1.0 - uT) * v) / uSize);
    // no reliable match (something appeared or disappeared): use the nearer real frame
    if (m.z > 0.08)
        oColor = (uT < 0.5) ? a : b;
    else
        oColor = mix(a, b, uT);
}
)";

GLuint MakeTexture(GLenum internalFormat, int width, int height, GLenum filter)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum format = (internalFormat == GL_R32F) ? GL_RED : GL_RGBA;
    const GLenum type = (internalFormat == GL_RGBA8) ? GL_UNSIGNED_BYTE : GL_FLOAT;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);
    return tex;
}

void DeleteTexture(GLuint& tex)
{
    if (tex) glDeleteTextures(1, &tex);
    tex = 0;
}

void Bind(int unit, GLuint tex)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

GLint Uniform(GLuint program, const char* name)
{
    return glGetUniformLocation(program, name);
}

}

FrameInterpolator::~FrameInterpolator()
{
    if (!Inited) return;

    Release();
    for (GLuint program : {DownsampleProgram, CaptureProgram, DiffProgram, CoarseProgram, RefineProgram,
                           VoteProgram, PickProgram, SmoothProgram, StatsProgram, AssignProgram, ComposeProgram})
        glDeleteProgram(program);
    glDeleteFramebuffers(1, &ReadFB);
    glDeleteFramebuffers(1, &DrawFB);
    glDeleteVertexArrays(1, &EmptyVAO);
}

bool FrameInterpolator::Init()
{
    if (Inited) return true;

    // samplers are listed in texture unit order
    struct ProgramSource { GLuint& Program; const char* FS; const char* Name; std::vector<const char*> Samplers; };
    const ProgramSource programs[] = {
        {DownsampleProgram, kDownsampleFS, "SmoothDownsample", {"Src"}},
        {CaptureProgram, kCaptureFS, "SmoothCapture", {"Src"}},
        {DiffProgram, kDiffFS, "SmoothDiff", {"A", "B"}},
        {CoarseProgram, kCoarseFS, "SmoothCoarse", {"A", "B", "Prior"}},
        {RefineProgram, kRefineFS, "SmoothRefine", {"A", "B", "Prev", "Hist", "Global"}},
        {VoteProgram, kVoteFS, "SmoothVote", {"Field", "Prior", "Conf"}},
        {PickProgram, kPickFS, "SmoothPick", {"Votes"}},
        {SmoothProgram, kSmoothFS, "SmoothVectors", {"Fine"}},
        {StatsProgram, kStatsFS, "SmoothStats", {"Fine", "Global"}},
        {AssignProgram, kAssignFS, "SmoothAssign", {"A", "B", "Smooth", "Fine", "Global"}},
        {ComposeProgram, kComposeFS, "SmoothCompose", {"A", "B", "Assign", "Stats"}},
    };
    for (const ProgramSource& src : programs)
    {
        if (!melonDS::OpenGL::CompileVertexFragmentProgram(src.Program, kVS, src.FS, src.Name, {}, {{"oColor", 0}}))
            return false;

        glUseProgram(src.Program);
        for (size_t unit = 0; unit < src.Samplers.size(); unit++)
            glUniform1i(Uniform(src.Program, src.Samplers[unit]), (GLint)unit);
    }

    glGenFramebuffers(1, &ReadFB);
    glGenFramebuffers(1, &DrawFB);
    glGenVertexArrays(1, &EmptyVAO);
    Inited = true;
    return true;
}

bool FrameInterpolator::Allocate(int width, int height)
{
    Release();

    Scale = std::max(1, height / 192);
    Width = width;
    Height = height;
    NativeW = std::max(4, width / Scale);
    NativeH = std::max(4, height / Scale);
    GridW = (NativeW + 7) / 8;
    GridH = (NativeH + 7) / 8;
    DiffW = (NativeW + 15) / 16;
    DiffH = (NativeH + 15) / 16;

    for (Layer& layer : Layers)
    {
        for (Slot& slot : layer.Slots)
        {
            slot.Full = MakeTexture(GL_RGBA8, Width, Height, GL_NEAREST);
            slot.Small = MakeTexture(GL_RGBA8, NativeW, NativeH, GL_LINEAR);
            slot.Quarter = MakeTexture(GL_RGBA8, NativeW / 4, NativeH / 4, GL_LINEAR);
        }
        layer.Coarse = MakeTexture(GL_RGBA32F, GridW, GridH, GL_NEAREST);
        layer.Fine = MakeTexture(GL_RGBA32F, GridW, GridH, GL_NEAREST);
        layer.Smooth = MakeTexture(GL_RGBA32F, GridW, GridH, GL_NEAREST);
        layer.History = MakeTexture(GL_RGBA32F, GridW, GridH, GL_NEAREST);
        layer.Assign = MakeTexture(GL_RGBA32F, NativeW, NativeH, GL_NEAREST);
        layer.Votes = MakeTexture(GL_RGBA32F, NumVotes, 1, GL_NEAREST);
        layer.Global = MakeTexture(GL_RGBA32F, 1, 1, GL_NEAREST);
        layer.Stats = MakeTexture(GL_RGBA32F, 1, 1, GL_NEAREST);
    }
    DiffTex = MakeTexture(GL_R32F, DiffW, DiffH * NumLayers, GL_NEAREST);

    glGenTextures(1, &Output_);
    glBindTexture(GL_TEXTURE_2D_ARRAY, Output_);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, Width, Height, NumLayers, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    Reset();
    return glGetError() == GL_NO_ERROR;
}

void FrameInterpolator::Release()
{
    for (Layer& layer : Layers)
    {
        for (Slot& slot : layer.Slots)
        {
            DeleteTexture(slot.Full);
            DeleteTexture(slot.Small);
            DeleteTexture(slot.Quarter);
        }
        for (GLuint* tex : {&layer.Coarse, &layer.Fine, &layer.Smooth, &layer.History, &layer.Assign,
                            &layer.Votes, &layer.Global, &layer.Stats})
            DeleteTexture(*tex);
    }
    DeleteTexture(DiffTex);
    if (Output_) glDeleteTextures(1, &Output_);
    Output_ = 0;
    Width = Height = 0;
}

void FrameInterpolator::Reset()
{
    for (Layer& layer : Layers)
    {
        layer.Order.clear();
        layer.Scratch = 0;
        layer.Intervals.clear();
        layer.Period = 1;
        layer.PairA = layer.PairB = -1.0;
        layer.HistoryB = -1.0;
    }
}

int FrameInterpolator::ContentPeriod() const
{
    int period = 1;
    for (const Layer& layer : Layers)
        period = std::max(period, layer.Period);
    return period;
}

void FrameInterpolator::Draw(GLuint target, int layer, int x, int y, int width, int height)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DrawFB);
    if (layer < 0)
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    else
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, layer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(x, y, width, height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void FrameInterpolator::Blit(GLuint source, int sourceLayer, GLuint target, int targetLayer, int width, int height)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ReadFB);
    if (sourceLayer < 0)
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
    else
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, source, 0, sourceLayer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, DrawFB);
    if (targetLayer < 0)
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    else
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, 0, targetLayer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);

    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void FrameInterpolator::Downsample(GLuint source, GLuint target, int scale, int width, int height)
{
    glUseProgram(DownsampleProgram);
    glUniform1i(Uniform(DownsampleProgram, "uScale"), scale);
    Bind(0, source);
    Draw(target, -1, 0, 0, width, height);
}

void FrameInterpolator::UpdatePeriod(Layer& layer, melonDS::u32 frame)
{
    if (!layer.Order.empty())
    {
        const melonDS::u32 interval = frame - layer.LastDistinct;
        if (interval >= 1 && interval <= MaxGap)
        {
            layer.Intervals.push_back((int)interval);
            if (layer.Intervals.size() > 8)
                layer.Intervals.pop_front();
        }
    }
    layer.LastDistinct = frame;

    // the most common recent interval; shorter wins ties
    int counts[MaxGap + 1] = {};
    for (int interval : layer.Intervals)
        counts[interval]++;
    int period = 1;
    for (int i = 1; i <= MaxGap; i++)
        if (counts[i] > counts[period])
            period = i;
    layer.Period = period;
}

void FrameInterpolator::Submit(GLuint source, int width, int height, melonDS::u32 frame)
{
    if (!Inited || width <= 0 || height <= 0) return;

    if (width != Width || height != Height)
    {
        if (!Allocate(width, height)) { Release(); return; }
    }

    if (HasSubmitted && frame == LastSubmitted) return;
    if (HasSubmitted && frame < LastSubmitted) Reset();   // savestate or reset
    HasSubmitted = true;
    LastSubmitted = frame;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(EmptyVAO);

    // shrink each screen into its incoming slot and measure the change; the
    // full-size copy is only made for images that turn out to be new
    bool compare = false;
    for (int l = 0; l < NumLayers; l++)
    {
        Layer& layer = Layers[l];
        Slot& slot = layer.Slots[layer.Scratch];

        glUseProgram(CaptureProgram);
        glUniform1i(Uniform(CaptureProgram, "uLayer"), l);
        glUniform1i(Uniform(CaptureProgram, "uScale"), Scale);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, source);
        Draw(slot.Small, -1, 0, 0, NativeW, NativeH);

        if (layer.Order.empty()) continue;
        glUseProgram(DiffProgram);
        glUniform2i(Uniform(DiffProgram, "uSize"), NativeW, NativeH);
        glUniform1i(Uniform(DiffProgram, "uRowOffset"), l * DiffH);
        Bind(0, slot.Small);
        Bind(1, layer.Slots[layer.Order.back()].Small);
        Draw(DiffTex, -1, 0, l * DiffH, DiffW, DiffH);
        compare = true;
    }

    std::vector<float> diff;
    if (compare)
    {
        diff.resize((size_t)DiffW * DiffH * NumLayers);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, ReadFB);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, DiffTex, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, DiffW, DiffH * NumLayers, GL_RED, GL_FLOAT, diff.data());
    }

    for (int l = 0; l < NumLayers; l++)
    {
        Layer& layer = Layers[l];
        bool changed = layer.Order.empty();
        if (!changed)
        {
            const float* rows = diff.data() + (size_t)l * DiffW * DiffH;
            changed = std::any_of(rows, rows + DiffW * DiffH, [](float d) { return d > 0.5f / 255.f; });
        }
        if (!changed) continue;   // a repeat of the newest image

        Slot& slot = layer.Slots[layer.Scratch];
        Blit(source, l, slot.Full, -1, Width, Height);
        Downsample(slot.Small, slot.Quarter, 4, NativeW / 4, NativeH / 4);
        UpdatePeriod(layer, frame);
        slot.Time = frame;
        layer.Order.push_back(layer.Scratch);

        if ((int)layer.Order.size() > MaxHistory)
        {
            layer.Scratch = layer.Order.front();
            layer.Order.pop_front();
        }
        else
        {
            for (int i = 0; i < NumSlots; i++)
                if (std::find(layer.Order.begin(), layer.Order.end(), i) == layer.Order.end())
                {
                    layer.Scratch = i;
                    break;
                }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void FrameInterpolator::Estimate(Layer& layer, const Slot& a, const Slot& b)
{
    if (layer.PairA == a.Time && layer.PairB == b.Time) return;

    // the previous pair ended where this one starts: its motion is a good prediction
    const bool chained = layer.HistoryB == a.Time;

    glUseProgram(CoarseProgram);
    glUniform2f(Uniform(CoarseProgram, "uSize"), NativeW / 4, NativeH / 4);
    glUniform1i(Uniform(CoarseProgram, "uHavePrior"), chained);
    Bind(0, a.Quarter);
    Bind(1, b.Quarter);
    Bind(2, layer.Global);
    Draw(layer.Coarse, -1, 0, 0, GridW, GridH);

    // recursive search down to a quarter pixel, ping-ponging between textures
    glUseProgram(RefineProgram);
    glUniform2f(Uniform(RefineProgram, "uSize"), NativeW, NativeH);
    glUniform2i(Uniform(RefineProgram, "uGrid"), GridW, GridH);
    glUniform1i(Uniform(RefineProgram, "uUseHistory"), chained);
    auto refine = [&](GLuint from, GLuint to, float step, bool haveGlobal)
    {
        glUseProgram(RefineProgram);
        glUniform1f(Uniform(RefineProgram, "uStep"), step);
        glUniform1i(Uniform(RefineProgram, "uHaveGlobal"), haveGlobal);
        Bind(0, a.Small);
        Bind(1, b.Small);
        Bind(2, from);
        Bind(3, layer.History);
        Bind(4, layer.Global);
        Draw(to, -1, 0, 0, GridW, GridH);
    };
    refine(layer.Coarse, layer.Smooth, 2.f, chained);

    // vote on the camera motion of this pair
    glUseProgram(VoteProgram);
    glUniform2i(Uniform(VoteProgram, "uGrid"), GridW, GridH);
    glUniform1i(Uniform(VoteProgram, "uHavePrior"), chained);
    Bind(0, layer.Smooth);
    Bind(1, layer.Global);
    Bind(2, layer.Coarse);
    Draw(layer.Votes, -1, 0, 0, NumVotes, 1);

    glUseProgram(PickProgram);
    glUniform1f(Uniform(PickProgram, "uBlocks"), (float)(GridW * GridH));
    Bind(0, layer.Votes);
    Draw(layer.Global, -1, 0, 0, 1, 1);

    refine(layer.Smooth, layer.Fine, 1.f, true);
    refine(layer.Fine, layer.Smooth, 0.5f, true);
    refine(layer.Smooth, layer.Fine, 0.25f, true);

    glUseProgram(SmoothProgram);
    glUniform2i(Uniform(SmoothProgram, "uGrid"), GridW, GridH);
    Bind(0, layer.Fine);
    Draw(layer.Smooth, -1, 0, 0, GridW, GridH);

    glUseProgram(StatsProgram);
    glUniform2i(Uniform(StatsProgram, "uGrid"), GridW, GridH);
    glUniform1f(Uniform(StatsProgram, "uBadCost"), BadBlockCost);
    glUniform1f(Uniform(StatsProgram, "uCutShare"), CutShare);
    glUniform1f(Uniform(StatsProgram, "uStillCutShare"), StillCutShare);
    Bind(0, layer.Fine);
    Bind(1, layer.Global);
    Draw(layer.Stats, -1, 0, 0, 1, 1);

    Blit(layer.Smooth, -1, layer.History, -1, GridW, GridH);
    layer.HistoryB = b.Time;
    layer.PairA = a.Time;
    layer.PairB = b.Time;
}

void FrameInterpolator::Compose(Layer& layer, int index, double time)
{
    const std::deque<int>& order = layer.Order;
    const Slot& oldest = layer.Slots[order.front()];
    const Slot& newest = layer.Slots[order.back()];

    if (time <= oldest.Time) { Blit(oldest.Full, -1, Output_, index, Width, Height); return; }
    if (time >= newest.Time) { Blit(newest.Full, -1, Output_, index, Width, Height); return; }

    size_t i = 0;
    while (i + 2 < order.size() && layer.Slots[order[i + 1]].Time <= time)
        i++;
    const Slot& a = layer.Slots[order[i]];
    const Slot& b = layer.Slots[order[i + 1]];

    // after a pause (the player standing still), the first image of the new
    // motion is approached over one normal image period rather than shown late
    double start = a.Time;
    if (b.Time - a.Time > MaxGap)
        start = b.Time - layer.Period;
    const float t = (float)((time - start) / (b.Time - start));
    if (t < 0.001f) { Blit(a.Full, -1, Output_, index, Width, Height); return; }

    Estimate(layer, a, b);

    glUseProgram(AssignProgram);
    glUniform2f(Uniform(AssignProgram, "uSize"), NativeW, NativeH);
    glUniform2i(Uniform(AssignProgram, "uGrid"), GridW, GridH);
    glUniform1f(Uniform(AssignProgram, "uT"), t);
    Bind(0, a.Small);
    Bind(1, b.Small);
    Bind(2, layer.Smooth);
    Bind(3, layer.Fine);
    Bind(4, layer.Global);
    Draw(layer.Assign, -1, 0, 0, NativeW, NativeH);

    glUseProgram(ComposeProgram);
    glUniform2f(Uniform(ComposeProgram, "uSize"), Width, Height);
    glUniform1f(Uniform(ComposeProgram, "uScale"), Scale);
    glUniform1f(Uniform(ComposeProgram, "uT"), t);
    Bind(0, a.Full);
    Bind(1, b.Full);
    Bind(2, layer.Assign);
    Bind(3, layer.Stats);
    Draw(Output_, index, 0, 0, Width, Height);
}

GLuint FrameInterpolator::Output(double time)
{
    if (!Inited || !Output_) return 0;
    for (const Layer& layer : Layers)
        if (layer.Order.empty()) return 0;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(EmptyVAO);

    for (int l = 0; l < NumLayers; l++)
        Compose(Layers[l], l, time);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    return Output_;
}

}
