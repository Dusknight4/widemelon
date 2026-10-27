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

#ifndef SCREEN_H
#define SCREEN_H

#include <atomic>
#include <chrono>
#include <optional>
#include <deque>
#include <map>

#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QScreen>
#include <QCloseEvent>
#include <QTimer>

#include "glad/glad.h"
#include "ScreenLayout.h"
#include "PhoneFramePacer.h"
#include "duckstation/gl/context.h"


class MainWindow;
class EmuInstance;
namespace melonDS { class NDS; }
namespace WideMelon { class FrameInterpolator; }


const struct { int id; float ratio; const char* label; } aspectRatios[] =
{
    { 0, 1,                       "4:3 (native)" },
    { 4, (5.f  / 3) / (4.f / 3), "5:3 (3DS)"},
    { 1, (16.f / 9) / (4.f / 3),  "16:9" },
    { 2, (21.f / 9) / (4.f / 3),  "21:9" },
    { 3, 0,                       "window" }
};
constexpr int AspectRatiosNum = sizeof(aspectRatios) / sizeof(aspectRatios[0]);


class ScreenPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ScreenPanel(QWidget* parent);
    virtual ~ScreenPanel();

    void setFilter(bool filter);

    void setMouseHide(bool enable, int delay);

    QTimer* setupMouseTimer();
    void updateMouseTimer();
    QTimer* mouseTimer;
    QSize screenGetMinSize(int factor);

    void osdSetEnabled(bool enabled);
    void osdAddMessage(unsigned int color, const char* msg);

    virtual void drawScreen() {}// = 0;

private slots:
    void onScreenLayoutChanged();
    void onAutoScreenSizingChanged(int sizing);

protected:
    MainWindow* mainWindow;
    EmuInstance* emuInstance;

    bool filter;

    int screenRotation;
    int screenGap;
    int screenLayout;
    bool screenSwap;
    int screenSizing;
    bool integerScaling;
    int screenAspectTop, screenAspectBot;

    int autoScreenSizing;

    ScreenLayout layout;
    float screenMatrix[kMaxScreenTransforms][6];
    int screenKind[kMaxScreenTransforms];
    int numScreens;

    // WideMelon: the Overlay layout in parts of a Pokémon game (battles, the PC's boxes,
    // the bag: ScreenLayout::OverlayMode), which the screens slide to and from as the
    // emulator signals them (emuInstance->screenMode). modeLayout[0] is unused (layout).
    // The slide runs on the thread that draws: from blendFrom (the matrices on screen when
    // the mode last changed) to blendMode's, blend going from 0 to 1.
    ScreenLayout modeLayout[ScreenLayout::Overlay_Modes];
    float modeMatrix[ScreenLayout::Overlay_Modes][kMaxScreenTransforms][6];
    bool hasMode[ScreenLayout::Overlay_Modes] = {};
    int blendMode = 0;
    float blendFrom[kMaxScreenTransforms][6];
    float battleBlend = 0.f;    // how much of the battle layout is on screen (for its backdrop)
    float blend = 1.f;
    std::chrono::steady_clock::time_point battleBlendTime;
    std::atomic<int> touchMode {0};
    ScreenLayout& touchLayout() { const int m = touchMode; return (m > 0 && hasMode[m]) ? modeLayout[m] : layout; }
    const float (*modeMatrices(int mode))[6] { return (mode > 0 && hasMode[mode]) ? modeMatrix[mode] : screenMatrix; }
    void blendedScreenMatrices(float out[kMaxScreenTransforms][6]);

    bool touching = false;

    bool mouseHide;
    int mouseHideDelay;

    struct OSDItem
    {
        unsigned int id;
        qint64 timestamp;

        char text[256];
        unsigned int color;

        bool rendered;
        QImage bitmap;

        int rainbowstart;
        int rainbowend;
    };

    QMutex osdMutex;
    bool osdEnabled;
    unsigned int osdID;
    std::deque<OSDItem> osdItems;

    QPixmap splashLogo;
    OSDItem splashText[3];
    QPoint splashPos[4];

    void loadConfig();

    virtual void setupScreenLayout();
    int effectiveScreenSizing() const;

    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

    void tabletEvent(QTabletEvent* event) override;
    void touchEvent(QTouchEvent* event);
    bool event(QEvent* event) override;

    void showCursor();

    int osdFindBreakPoint(const char* text, int i);
    void osdLayoutText(const char* text, int* width, int* height, int* breaks);
    unsigned int osdRainbowColor(int inc);

    virtual void osdRenderItem(OSDItem* item);
    virtual void osdDeleteItem(OSDItem* item);

    void osdUpdate();

    void calcSplashLayout();
};


class ScreenPanelNative : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelNative(QWidget* parent);
    virtual ~ScreenPanelNative();

    void drawScreen() override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void setupScreenLayout() override;

    QMutex bufferLock;
    bool hasBuffers;
    void* topBuffer;
    void* bottomBuffer;

    QImage screen[2];
    QTransform screenTrans[kMaxScreenTransforms];
};


class ScreenPanelGL : public ScreenPanel
{
    Q_OBJECT

public:
    explicit ScreenPanelGL(QWidget* parent);
    virtual ~ScreenPanelGL();

    std::optional<WindowInfo> getWindowInfo();

    bool createContext();

    void setSwapInterval(int intv);

    void initOpenGL();
    void deinitOpenGL();
    void makeCurrentGL();
    void releaseGL();

    void drawScreen() override;

    GL::Context* getContext() { return glContext.get(); }

    void transferLayout();
protected:

    qreal devicePixelRatioFromScreen() const;
    int scaledWindowWidth() const;
    int scaledWindowHeight() const;

    QPaintEngine* paintEngine() const override;

private:
    void setupScreenLayout() override;

    std::unique_ptr<GL::Context> glContext;
    // WideMelon: OpenGL runs on its own thread (melonDS::GLThread) for the main
    // window; these stand in for MakeCurrent/DoneCurrent then
    bool glThreaded = false;
    void ctxMakeCurrent();
    void ctxDoneCurrent();
    bool glInited;

    GLuint screenVertexBuffer, screenVertexArray;
    GLuint backdropVertexBuffer = 0, backdropVertexArray = 0;   // WideMelon: battle scenery right of the DS picture
    GLuint wingShaderProgram = 0;   // WideMelon: draws that scenery, with the end of a battle info bar cut off by the edge
    GLint wingTransformULoc = -1, wingScreenSizeULoc = -1;
    GLuint screenTexture;
    GLuint screenShaderProgram;
    GLint screenShaderTransformULoc, screenShaderScreenSizeULoc;

    QMutex screenSettingsLock;
    WindowInfo windowInfo;

    int lastScreenWidth = -1, lastScreenHeight = -1;

    GLuint osdShader;
    GLint osdScreenSizeULoc, osdPosULoc, osdSizeULoc;
    GLint osdScaleFactorULoc;
    GLint osdTexScaleULoc;
    GLuint osdVertexArray;
    GLuint osdVertexBuffer;
    std::map<unsigned int, GLuint> osdTextures;

    GLuint logoTexture;

    GLuint phoneCaptureFramebuffer = 0;
    GLuint phoneSourceFramebuffer = 0;
    GLuint phoneCaptureTexture = 0;
    GLuint phoneCapturePBO[2] = {0, 0};
    int phoneCapturePBOIndex = 0;
    bool phoneCapturePrimed = false;
    bool phoneCaptureMapWarned = false;
    PhoneFramePacer phoneFramePacer;

    void capturePhoneFrame(GLuint sourceTexture, int sourceWidth, int sourceHeight);

    // WideMelon motion smoothing
    std::unique_ptr<WideMelon::FrameInterpolator> interpolator;
    bool interpolatorFailed = false;
    bool smoothingActive = false;
    GLuint smoothFrame(melonDS::NDS* nds, GLuint source);
    double shownFrame = 0.0;   // WideMelon: the frame time of the picture shown (smoothing shows the past)

    void osdRenderItem(OSDItem* item) override;
    void osdDeleteItem(OSDItem* item) override;
};

#endif // SCREEN_H
