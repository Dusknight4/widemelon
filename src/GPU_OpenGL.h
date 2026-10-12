/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef GPU_OPENGL_H
#define GPU_OPENGL_H

#include "OpenGLSupport.h"
#include <vector>
#include "GPU.h"
#include "GPU2D_OpenGL.h"
#include "GPU3D_OpenGL.h"
#include "GPU3D_Compute.h"

namespace melonDS
{

class GLRenderer : public Renderer
{
public:
    GLRenderer(melonDS::NDS& nds, bool compute);
    ~GLRenderer() override;
    bool Init() override;
    void Reset() override;
    void Stop() override;

    void PostSavestate() override;

    void SetRenderSettings(RendererSettings& settings) override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;

    void VBlank() override;
    void VBlankEnd() override;

    void AllocCapture(u32 bank, u32 start, u32 len) override;
    void SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete) override;

    bool GetFramebuffers(void** top, void** bottom) override;

    // WideMelon: engine A's background layer continued past the DS screen's right edge
    // (see RenderWideBackdrop): a one-layer texture array, BackdropWidth x 192 DS
    // pixels, or 0 when there is none. The last few are kept, each for the frame
    // whose picture it goes with (the frame count after that frame), so a picture
    // shown slightly in the past (motion smoothing) gets the one from its own frame.
    //
    // Piece, when not 0: where a battle's info bar runs off the right edge, the
    // bar's own left end belongs past the edge, turned round. Piece (PieceWidth x
    // 192, a texture2D) is that end as it was taken, once the bar stood still:
    // column d is the bar's column PieceColumn - d, alpha marks the bar's pixels in
    // lines PieceRows[0..1]. PieceShift is how far down the bar is now, in the
    // frame nearest the one shown and in the one on its other side; WideWingFS
    // copies the bar's pixels from the picture on screen, so it bobs with the bar.
    static constexpr int BackdropWidth = 128;
    static constexpr int PieceWidth = 48;
    struct WideBackdrop { GLuint Tex = 0, Piece = 0; float PieceColumn = 0.f; int PieceShift[2] {}, PieceRows[2] {}; };
    WideBackdrop GetWideBackdrop(double frame) const;
    // whether to make it: the frontend says so while a battle is on
    void SetWideBackdropWanted(bool wanted) { WideBackdropWanted = wanted; }
    // fragment shader drawing the backdrop (ScreenTex, unit 0) with the bar's end over
    // it: ShownTex (unit 1) is the picture on screen, PieceTex (unit 2); uniforms
    // uPiece, uColumn, uShift, uRows, and uSide, uWide (the picture's width and its part beside
    // the DS screen on each side, in DS pixels); fTexcoord across the backdrop
    static const char* const WideWingFS;

    bool NeedsShaderCompile() override;
    void ShaderCompileStep(int& current, int& count) override;

private:
    friend class GLRenderer2D;
    friend class GLRenderer3D;
    friend class ComputeRenderer3D;

    bool IsCompute;

    int ScaleFactor;
    int ScreenW, ScreenH;

    GLuint RectVtxBuffer;
    GLuint RectVtxArray;

    GLuint OutputTex3D;
    GLuint OutputTex2D[2];

    struct sFinalPassConfig
    {
        u32 uScreenSwap[192];
        u32 uScaleFactor;
        u32 uAuxLayer;
        u32 uDispModeA;
        u32 uDispModeB;
        u32 uBrightModeA;
        u32 uBrightModeB;
        u32 uBrightFactorA;
        u32 uBrightFactorB;
        float uAuxColorFactor;
        u32 __pad0[3];
    } FinalPassConfig;

    GLuint FPShader;
    GLuint FPConfigUBO;

    GLuint FPVertexBufferID;
    GLuint FPVertexArrayID;

    GLuint AuxInputTex;                 // aux input (VRAM and mainmem FIFO)

    // texture/fb for display capture VRAM input
    GLuint CaptureVRAMTex;
    GLuint CaptureVRAMFB;

    GLuint FPOutputTex[2];               // final output
    GLuint FPOutputFB[2];

    static constexpr int NumBackdrops = 4;
    GLuint BackdropTex[NumBackdrops] {}, BackdropFB = 0;
    GLuint EdgeRowsShader = 0, EdgeRowsTex = 0, EdgeRowsFB = 0;   // per line: where sprites reaching the right edge start
    GLuint PieceShader = 0, PieceTex = 0, PieceFB = 0;
    u32 BackdropFrame[NumBackdrops] {};
    bool BackdropValid[NumBackdrops] {};
    bool BackdropPiece[NumBackdrops] {};
    int BackdropShift[NumBackdrops] {};   // where the bar is, lines down from where its end was taken
    int BackdropNext = 0;
    bool WideBackdropWanted = false;
    // the info bar: the sprites reaching the edge, as where each line starts, while
    // they keep that shape (only moving up and down); once they have held it for a
    // while, their end is taken (PieceRows) and shown whenever the shape is there
    std::vector<float> BarRows, PieceRows;
    int BarFrames = 0;
    float PieceColumn = 0.f;
    int PieceTop = 0, PieceBottom = 0;
    // the scenery's scroll the last time a bar stood still: where it rests in a
    // battle (while battle intros slide it, a line at a time)
    u16 RestingScrollX = 0, RestingScrollY = 0;
    void RenderWideBackdrop();

    struct sCaptureConfig
    {
        float uInvCaptureSize[2];
        u32 uSrcALayer;
        u32 uSrcBLayer;
        u32 uSrcBOffset;
        u32 uDstMode;
        u32 uBlendFactors[2];
        float uSrcAOffset[192];
        float uSrcBColorFactor;
        u32 __pad0[3];
    } CaptureConfig;

    GLuint CaptureShader;
    GLuint CaptureConfigUBO;

    GLuint CaptureVtxBuffer;
    GLuint CaptureVtxArray;

    GLuint CaptureOutput256FB[4];
    GLuint CaptureOutput256Tex;
    GLuint CaptureOutput128FB[16];
    GLuint CaptureOutput128Tex;

    GLuint CapDownShader;
    GLint CapDownInputLayerULoc;

    GLuint CaptureSyncFB;
    GLuint CaptureSyncTex;

    u16* AuxInputBuffer[2];
    u8 AuxUsageMask;

    u32 DispCntA, DispCntB;
    u16 MasterBrightnessA, MasterBrightnessB;
    u32 CaptureCnt;

    bool NeedPartialRender;
    int LastLine;
    int LastCapLine;
    int Aux0VRAMCap;

    void SetScaleFactor(int scale);

    void RenderScreen(int ystart, int yend);
    void DoCapture(int ystart, int yend);
    void DownscaleCapture(int width, int height, int layer);
};

}

#endif // GPU_OPENGL_H
