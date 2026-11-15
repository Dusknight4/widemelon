/*
    Copyright 2026 WideMelon contributors

    This file is part of WideMelon, a fork of melonDS.

    WideMelon is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.
*/

// MultiMelon: Azahar's 3DS core in MultiMelon's window (see ThreeDS.h). Modelled on Azahar's
// libretro frontend (src/citra_libretro), which also runs the core a frame at a time on a GL
// context it doesn't own.

#include "ThreeDS.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

// MultiMelon's glad first: this file's own GL calls go through it (Azahar's copy, inside its
// libraries, has its names changed at link time; see cmake/Azahar.cmake)
#include MELONDS_GL_HEADER

#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_state.h"

// Azahar's GL loader, as renamed when its libraries are linked in
extern "C" int az_gladLoadGL(void);

namespace ThreeDS
{

namespace
{

Input g_input;          // this frame's, read by the input devices below
int g_resolution = 2;
bool g_running = false;
bool g_glLoaded = false;
std::string g_gamePath;     // what was started (a CIA's installed game, not the CIA)

class Button final : public ::Input::ButtonDevice
{
public:
    explicit Button(unsigned bit) : Bit(bit) {}
    bool GetStatus() const override { return (g_input.buttons >> Bit) & 1; }
private:
    unsigned Bit;
};

class ButtonFactory final : public ::Input::Factory<::Input::ButtonDevice>
{
public:
    std::unique_ptr<::Input::ButtonDevice> Create(const Common::ParamPackage& params) override
    {
        return std::make_unique<Button>((unsigned)params.Get("button", 0));
    }
};

class Stick final : public ::Input::AnalogDevice
{
public:
    explicit Stick(int which) : Which(which) {}
    std::tuple<float, float> GetStatus() const override
    {
        return Which == 0 ? std::make_tuple(g_input.circleX, g_input.circleY)
                          : std::make_tuple(g_input.cstickX, g_input.cstickY);
    }
private:
    int Which;
};

class StickFactory final : public ::Input::Factory<::Input::AnalogDevice>
{
public:
    std::unique_ptr<::Input::AnalogDevice> Create(const Common::ParamPackage& params) override
    {
        return std::make_unique<Stick>(params.Get("axis", 0));
    }
};

// Azahar draws into a framebuffer of ours, top screen above the touchscreen
class Window final : public Frontend::EmuWindow
{
public:
    Window()
    {
        strict_context_required = true;
        window_info.type = Frontend::WindowSystemType::Windows;
    }

    void PollEvents() override { FrameEnded = true; }   // (once a frame, from the renderer's EndFrame)
    void MakeCurrent() override {}      // the context is always current on our thread
    void DoneCurrent() override {}
    bool NeedsClearing() const override { return true; }

    void Resize(int scale)
    {
        Width = 400 * scale;
        Height = 480 * scale;
        UpdateCurrentFramebufferLayout(Width, Height);
    }

    bool FrameEnded = false;
    int Width = 400, Height = 480;
    bool Touching = false;
};

std::unique_ptr<Window> g_window;

// our framebuffer (what TryPresent draws into) and the 2-layer texture the panel shows
GLuint g_fbo = 0, g_fboTex = 0, g_copyFbo = 0, g_screens = 0;
int g_fboW = 0, g_fboH = 0, g_texW = 0, g_texH = 0;

void MakeTargets()
{
    if (g_fbo && g_fboW == g_window->Width && g_fboH == g_window->Height) return;
    if (!g_fbo) { glGenFramebuffers(1, &g_fbo); glGenFramebuffers(1, &g_copyFbo); glGenTextures(1, &g_fboTex); glGenTextures(1, &g_screens); }
    g_fboW = g_window->Width;
    g_fboH = g_window->Height;
    glBindTexture(GL_TEXTURE_2D, g_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_fboW, g_fboH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_fboTex, 0);

    g_texW = g_fboW;
    g_texH = g_fboH / 2;
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_screens);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, g_texW, g_texH, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// GL state as Azahar left it, put back before each frame. Azahar keeps its own copy of the GL
// state (OpenGLState::cur_state) and only sends what differs from it, so the real state must
// match that copy whenever Azahar runs: DefaultGLState() makes both the defaults (the copy's
// and, call by call, the context's, whatever the panel changed), and the saved state is
// then applied over them. The panel draws in between, starting from those defaults.
OpenGL::OpenGLState g_azaharState;

void DefaultGLState()
{
    OpenGL::OpenGLState{}.Apply();
    // (OpenGLState's defaults, set on the context whatever it had)
    glDisable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 0, 0xFF);
    glStencilMask(0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
    glBlendColor(0, 0, 0, 0);
    glDisable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_COPY);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int unit = 7; unit >= 0; unit--)
    {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

void SetupSettings()
{
    auto& v = Settings::values;
    v.graphics_api = Settings::GraphicsAPI::OpenGL;
    v.use_gles = false;
    v.resolution_factor = (u32)g_resolution;
    v.layout_option = Settings::LayoutOption::Default;
    v.swap_screen = false;
    v.use_vsync = false;        // the panel's swap waits for the display
    v.frame_limit = 100;        // Azahar keeps the 3DS's pace

    v.current_input_profile.touch_device = "engine:emu_window";
    static const int order[] = {
        Settings::NativeButton::A, Settings::NativeButton::B, Settings::NativeButton::Select,
        Settings::NativeButton::Start, Settings::NativeButton::Right, Settings::NativeButton::Left,
        Settings::NativeButton::Up, Settings::NativeButton::Down, Settings::NativeButton::R,
        Settings::NativeButton::L, Settings::NativeButton::X, Settings::NativeButton::Y,
        Settings::NativeButton::ZL, Settings::NativeButton::ZR, Settings::NativeButton::Home};
    for (int i = 0; i < Settings::NativeButton::NumButtons; i++)
        v.current_input_profile.buttons[i] = "";
    for (int bit = 0; bit < (int)std::size(order); bit++)
        v.current_input_profile.buttons[order[bit]] = "engine:multimelon,button:" + std::to_string(bit);
    v.current_input_profile.analogs[Settings::NativeAnalog::CirclePad] = "engine:multimelon,axis:0";
    v.current_input_profile.analogs[Settings::NativeAnalog::CStick] = "engine:multimelon,axis:1";
    v.current_input_profile.motion_device = "engine:motion_emu";
}

// A CIA into the virtual 3DS (its SD card or NAND), as the 3DS's installer does; then the
// path of the game it holds. "" when done, else what went wrong.
std::string InstallCIA(const std::string& path, std::string& gamePath)
{
    u64 titleId = 0;
    {
        FileUtil::IOFile file(path, "rb");
        FileSys::CIAContainer cia;
        if (!file.IsOpen() || cia.Load(&file) != Loader::ResultStatus::Success)
            return "This CIA file couldn't be read.";
        titleId = cia.GetTitleMetadata().GetTitleID();
    }
    using S = Service::AM::InstallStatus;
    switch (Service::AM::InstallCIA(path))
    {
    case S::Success: break;
    case S::ErrorEncrypted:
        return "This CIA is encrypted. Make a decrypted one with GodMode9 on your 3DS (its title manager: "
               "\"Build CIA (decrypted)\").";
    case S::ErrorInvalid: return "This CIA file is damaged or not a 3DS title.";
    default: return "This CIA couldn't be installed.";
    }
    // (an application, or a demo; an update, DLC or system title has no game to start)
    const u32 kind = (u32)(titleId >> 32);
    if (kind != 0x00040000 && kind != 0x00040002)
        return "Installed. (It's an update or DLC: open the game itself to play.)";
    gamePath = Service::AM::GetTitleContentPath(Service::AM::GetTitleMediaType(titleId), titleId);
    if (gamePath.empty() || !FileUtil::Exists(gamePath))
        return "The CIA was installed, but its game wasn't found.";
    return "";
}

// RunLoop until the kernel's background work (file access...) is done: a savestate can't be made in
// the middle of it (what Azahar's own frontends wait for, see citra_libretro's DrainAsyncOperations)
bool WaitAsync(Core::System& system)
{
    const auto start = std::chrono::steady_clock::now();
    while (system.KernelRunning() && system.Kernel().AreAsyncOperationsPending())
    {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) return false;
        if (system.RunLoop() != Core::System::ResultStatus::Success) return false;
    }
    return true;
}

}

bool IsThreeDSFile(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == "3ds" || ext == "cci" || ext == "cxi" || ext == "3dsx" || ext == "app" || ext == "elf" || ext == "axf"
        || ext == "cia";
}

void Init(const std::string& userDir)
{
    static bool done = false;
    if (done) return;
    done = true;

    std::string dir = userDir;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    FileUtil::SetUserPath(dir);
    // the log takes the game's own debug output too (svcOutputDebugString: Debug.Emulated, at debug level), for
    // modders reading their test builds; MULTIMELON_3DS_LOG_FILTER replaces the filter (Azahar's syntax)
    const char* filter = std::getenv("MULTIMELON_3DS_LOG_FILTER");
    Settings::values.log_filter.SetValue(filter ? filter : "*:Info Debug.Emulated:Debug");
    Common::Log::Initialize();   // (its log folder, in the user folder: log/azahar_log.txt)
    Common::Log::Start();

    for (const auto& module : Service::service_module_map)
        Settings::values.lle_modules.emplace(module.name, false);
    Frontend::RegisterDefaultApplets(Core::System::GetInstance());
    Core::System::GetInstance().RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    ::Input::RegisterFactory<::Input::ButtonDevice>("multimelon", std::make_shared<ButtonFactory>());
    ::Input::RegisterFactory<::Input::AnalogDevice>("multimelon", std::make_shared<StickFactory>());
}

void Finish()
{
    Unload();
    Common::Log::Stop();
}

void SetResolution(int factor) { g_resolution = std::clamp(factor, 1, 10); }

void SetSpeed(double percent)
{
    // (Azahar's frame limiter reads this each frame: its own turbo setting)
    Settings::is_temporary_frame_limit = percent != 100.0;
    Settings::temporary_frame_limit = std::max(percent, 0.0);
}

void SetAudio(float volume, bool muted)
{
    Settings::values.volume = std::clamp(volume, 0.f, 1.f);
    Settings::values.audio_muted = muted;
}

std::string Load(const std::string& path)
{
    Unload();
    std::string gamePath = path;
    {
        const size_t dot = path.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == "cia")
        {
            const std::string err = InstallCIA(path, gamePath);
            if (!err.empty()) return err;
        }
    }
    if (!g_glLoaded)
    {
        if (!az_gladLoadGL()) return "OpenGL 4.3 is needed for 3DS games.";
        g_glLoaded = true;
    }

    SetupSettings();
    if (!g_window) g_window = std::make_unique<Window>();
    g_window->Resize(g_resolution);
    g_window->FrameEnded = false;
    g_window->Touching = false;
    g_input = {};

    auto& system = Core::System::GetInstance();
    system.ApplySettings();
    g_azaharState = OpenGL::OpenGLState{};
    const auto result = system.Load(*g_window, gamePath);
    g_azaharState = OpenGL::OpenGLState::GetCurState();
    DefaultGLState();

    using R = Core::System::ResultStatus;
    switch (result)
    {
    case R::Success: break;
    case R::ErrorGetLoader:
    case R::ErrorSystemMode: return "This file isn't a 3DS game MultiMelon can open.";
    case R::ErrorLoader_ErrorEncrypted:
        return "This 3DS game is encrypted. Dump it decrypted with GodMode9 (on your 3DS: \"Dump cart (decrypted)\", "
               "or a decrypted CIA of an installed game).";
    case R::ErrorLoader_ErrorInvalidFormat: return "This 3DS file's format isn't supported.";
    case R::ErrorLoader_ErrorGbaTitle: return "This is a GBA Virtual Console title, which the 3DS core can't run.";
    case R::ErrorSystemFiles: return "This game needs 3DS system files: " + system.GetStatusDetails();
    default: return "The 3DS game couldn't be started (error " + std::to_string((int)result) + ").";
    }

    u64 programId = 0;
    system.GetAppLoader().ReadProgramId(programId);
    system.GPU().ApplyPerProgramSettings(programId);
    system.RegisterCoreLoopThreadId();
    g_running = true;
    g_gamePath = gamePath;
    return "";
}

std::string Reset()
{
    const std::string path = g_gamePath;
    return path.empty() ? "No 3DS game is running." : Load(path);
}

bool SaveState(const std::string& path)
{
    if (!g_running) return false;
    auto& system = Core::System::GetInstance();
    DefaultGLState();
    g_azaharState.Apply();
    bool ok = false;
    try
    {
        if (WaitAsync(system))
        {
            const std::vector<u8> state = system.SaveStateBuffer();
            FileUtil::IOFile file(path, "wb");
            ok = !state.empty() && file.IsOpen() && file.WriteBytes(state.data(), state.size()) == state.size();
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(Frontend, "savestate: {}", e.what());
    }
    g_azaharState = OpenGL::OpenGLState::GetCurState();
    DefaultGLState();
    return ok;
}

bool LoadState(const std::string& path)
{
    if (!g_running) return false;
    std::vector<u8> state;
    {
        FileUtil::IOFile file(path, "rb");
        if (!file.IsOpen()) return false;
        state.resize(file.GetSize());
        if (state.empty() || file.ReadBytes(state.data(), state.size()) != state.size()) return false;
    }
    auto& system = Core::System::GetInstance();
    DefaultGLState();
    g_azaharState.Apply();
    bool ok = false;
    try
    {
        ok = WaitAsync(system) && system.LoadStateBuffer(std::move(state));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR(Frontend, "savestate: {}", e.what());
    }
    g_azaharState = OpenGL::OpenGLState::GetCurState();
    DefaultGLState();
    return ok;
}

void Unload()
{
    if (!g_running) return;
    g_running = false;
    DefaultGLState();
    g_azaharState.Apply();
    Core::System::GetInstance().Shutdown();
    DefaultGLState();
}

bool Running() { return g_running; }

void RunFrame(const Input& input)
{
    if (!g_running) return;
    g_input = input;

    // touch: in the framebuffer's coordinates, where the touchscreen is in Azahar's layout
    const auto& bottom = g_window->GetFramebufferLayout().bottom_screen;
    if (input.touching)
    {
        const unsigned x = bottom.left + (unsigned)(std::clamp(input.touchX, 0.f, 319.f) * bottom.GetWidth() / 320.f);
        const unsigned y = bottom.top + (unsigned)(std::clamp(input.touchY, 0.f, 239.f) * bottom.GetHeight() / 240.f);
        if (g_window->Touching) g_window->TouchMoved(x, y);
        else g_window->TouchPressed(x, y);
        g_window->Touching = true;
    }
    else if (g_window->Touching)
    {
        g_window->TouchReleased();
        g_window->Touching = false;
    }

    // Azahar's GL state back as it had it, then the core until the frame ends (it paces
    // itself to the 3DS's frame rate)
    DefaultGLState();
    g_azaharState.Apply();
    auto& system = Core::System::GetInstance();
    g_window->FrameEnded = false;
    for (int slices = 0; slices < 100000 && !g_window->FrameEnded; slices++)
    {
        if (system.RunLoop() != Core::System::ResultStatus::Success)
        {
            g_running = false;
            break;
        }
    }
    g_azaharState = OpenGL::OpenGLState::GetCurState();
    DefaultGLState();
}

unsigned CopyScreens()
{
    if (!g_window) return 0;
    MakeTargets();

    // the newest frame into our framebuffer
    DefaultGLState();
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fbo);
    glViewport(0, 0, g_fboW, g_fboH);
    Core::System::GetInstance().GPU().Renderer().TryPresent(0);
    DefaultGLState();

    // each screen into its layer, the right way up (row 0 the top, as the panel's textures)
    const auto& layout = g_window->GetFramebufferLayout();
    auto copy = [&](const Common::Rectangle<u32>& r, int layer, int dstX) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copyFbo);
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, g_screens, 0, layer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
        const int w = (int)r.GetWidth(), h = (int)r.GetHeight();
        const int y0 = g_fboH - (int)r.bottom, y1 = g_fboH - (int)r.top;
        glBlitFramebuffer((int)r.left, y0, (int)r.left + w, y1, dstX, h, dstX + w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    };
    copy(layout.top_screen, 0, 0);
    copy(layout.bottom_screen, 1, (g_texW - (int)layout.bottom_screen.GetWidth()) / 2);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return g_screens;
}

int TextureWidth() { return g_texW; }
int TextureHeight() { return g_texH; }

}
