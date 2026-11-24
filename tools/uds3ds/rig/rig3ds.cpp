// rig3ds: runs a 3DS title headless (Azahar's core, no window shown) for a while, with MultiMelon's "Wifi as
// Local Multiplayer" link (ThreeDSNet/UdsLink), and prints what happened. Two of them on loopback test 3DS
// local wireless between consoles without opening any window. It draws with the graphics card (OpenGL 4.3, in a
// hidden window: full speed) or, with RIG_RENDERER=software or no OpenGL 4.3, Azahar's software renderer (slow).
//
// usage: rig3ds USERDIR TITLE SECONDS [KEYS]
//   USERDIR  this console's 3DS folder (its sdmc/, nand/, log/ and mac.txt)
//   KEYS     optional input script for the title: "keys*frames,..." (a b x y l r start select up down left right,
//            and the Circle Pad: cup cdown cleft cright; several at once with +, nothing pressed: "*30")
// env: RIG_LINK_PORT (default 7072), RIG_LINK_PORTS (how many from there, as MultiMelon: 4), RIG_LINK_PEERS
//      ("ip:port,..."), RIG_LINK_LOOPBACK=1, RIG_NO_LINK=1,
//      RIG_PLUGIN=file.3gx (Azahar's Luma plugin loader on, that plugin as sdmc:/luma/plugins/default.3gx),
//      RIG_CMD=file: more commands, one per line, read while it runs (appended lines are picked up):
//        a keys script as above | shot NAME (a PNG of both screens) | touch X Y FRAMES (bottom screen) | quit
//      RIG_SHOTS=folder (where shots go; default USERDIR/shots), RIG_SHOT_EVERY=seconds (shots by themselves)
//      RIG_RENDERER=opengl (default) | software, RIG_SPEED=percent of the 3DS's speed (default 100; 0 = no limit)
//      a rough link (on what this console sends; see UdsLink::Impairment): RIG_LINK_LOSS=percent,
//      RIG_LINK_BURST=datagrams a loss burst lasts on average, RIG_LINK_DELAY_MS, RIG_LINK_JITTER_MS, RIG_LINK_DUP=percent,
//      RIG_LINK_REORDER=1, RIG_LINK_SEED; while running, the command "link loss=5 delay=40 ..." (or "link off");
//      "link cut=1" takes the console out of range (both ways), "link cut=0" brings it back
// A .cia TITLE is installed first (once is enough: RIG_NO_INSTALL=1 then starts the installed game).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);   // (winmm)
#endif

#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/settings.h"
#include "audio_core/input_details.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/image_interface.h"
#include "core/frontend/input.h"
#include "core/hle/service/am/am.h"
#include "core/loader/loader.h"
#include "core/hle/service/service.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_software/renderer_software.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "lodepng.h"

#include "ThreeDSNet.h"
#include "ThreeDSRoom.h"
#include "UdsLink.h"

namespace
{
unsigned g_keys = 0;    // pressed now (bit i = button i of the order below)
float g_padX = 0, g_padY = 0;

class Button final : public Input::ButtonDevice
{
public:
    explicit Button(unsigned bit) : Bit(bit) {}
    bool GetStatus() const override { return (g_keys >> Bit) & 1; }
private:
    unsigned Bit;
};

class ButtonFactory final : public Input::Factory<Input::ButtonDevice>
{
public:
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override
    {
        return std::make_unique<Button>((unsigned)params.Get("button", 0));
    }
};

class Pad final : public Input::AnalogDevice
{
public:
    std::tuple<float, float> GetStatus() const override { return {g_padX, g_padY}; }
};

class PadFactory final : public Input::Factory<Input::AnalogDevice>
{
public:
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage&) override { return std::make_unique<Pad>(); }
};

// Azahar draws into this. With OpenGL its context is always current on the one thread (as in MultiMelon).
class Window final : public Frontend::EmuWindow
{
public:
    explicit Window(bool gl)
    {
        strict_context_required = gl;
        window_info.type = gl ? Frontend::WindowSystemType::Windows : Frontend::WindowSystemType::Headless;
        UpdateCurrentFramebufferLayout(400, 480);   // top screen above, bottom screen centred below
    }
    void PollEvents() override { Frames++; }    // (once a frame, from the renderer's EndFrame)
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    unsigned long Frames = 0;
};

// OpenGL 4.3 core in a window that's never shown (nothing on screen, no focus taken); false if there's none
SDL_Window* g_sdlWindow = nullptr;
GLuint g_shotFbo = 0, g_shotTex = 0;

bool StartGL()
{
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    g_sdlWindow = SDL_CreateWindow("rig3ds", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 400, 480,
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = g_sdlWindow ? SDL_GL_CreateContext(g_sdlWindow) : nullptr;
    if (!context || SDL_GL_MakeCurrent(g_sdlWindow, context) != 0 || !gladLoadGL())
    {
        if (g_sdlWindow) SDL_DestroyWindow(g_sdlWindow);
        g_sdlWindow = nullptr;
        SDL_Quit();
        return false;
    }
    SDL_GL_SetSwapInterval(0);
    std::printf("renderer: OpenGL, %s\n", (const char*)glGetString(GL_RENDERER));
    return true;
}

// the newest frame, read back from the card: 400x480 RGBA, row 0 at the top
bool ReadFrameGL(std::vector<unsigned char>& img)
{
    const unsigned W = 400, H = 480;
    // Azahar keeps its own copy of the GL state and sends only what differs from it: put the context in its
    // default state (through that copy), do ours, put back what we changed, then Azahar's state over it
    const OpenGL::OpenGLState saved = OpenGL::OpenGLState::GetCurState();
    OpenGL::OpenGLState{}.Apply();
    if (!g_shotFbo)
    {
        glGenFramebuffers(1, &g_shotFbo);
        glGenTextures(1, &g_shotTex);
        glBindTexture(GL_TEXTURE_2D, g_shotTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_shotFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_shotTex, 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_shotFbo);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    Core::System::GetInstance().GPU().Renderer().TryPresent(0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_shotFbo);
    GLint pack = 0;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    std::vector<unsigned char> raw(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 0, 0);
    saved.Apply();
    img.assign(W * H * 4, 0);
    for (unsigned y = 0; y < H; y++)    // (GL's rows go bottom up)
        std::copy_n(&raw[(size_t)(H - 1 - y) * W * 4], W * 4, &img[(size_t)y * W * 4]);
    for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
    return true;
}

const char* kNames[] = {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select",
                        "cup", "cdown", "cleft", "cright"};
const int kOrder[] = {Settings::NativeButton::A, Settings::NativeButton::B, Settings::NativeButton::X,
                      Settings::NativeButton::Y, Settings::NativeButton::Up, Settings::NativeButton::Down,
                      Settings::NativeButton::Left, Settings::NativeButton::Right, Settings::NativeButton::L,
                      Settings::NativeButton::R, Settings::NativeButton::Start, Settings::NativeButton::Select};
constexpr unsigned kPadUp = 1u << 12, kPadDown = 1u << 13, kPadLeft = 1u << 14, kPadRight = 1u << 15;

struct Step
{
    enum Kind { Keys, Shot, Touch, Quit, Link } kind = Keys;
    unsigned keys = 0;
    unsigned long frames = 1;
    std::string name;
    unsigned x = 0, y = 0;
};

void ParseKeys(const std::string& script, std::deque<Step>& out)
{
    size_t at = 0;
    while (at < script.size())
    {
        size_t end = script.find(',', at);
        if (end == std::string::npos) end = script.size();
        const std::string item = script.substr(at, end - at);
        at = end + 1;
        const size_t star = item.find('*');
        const std::string keys = item.substr(0, star);
        Step s;
        s.frames = star == std::string::npos ? 1ul : std::strtoul(item.c_str() + star + 1, nullptr, 10);
        size_t k = 0;
        while (k <= keys.size())
        {
            size_t plus = keys.find('+', k);
            if (plus == std::string::npos) plus = keys.size();
            const std::string key = keys.substr(k, plus - k);
            for (int i = 0; i < 16; i++) if (key == kNames[i]) s.keys |= 1u << i;
            k = plus + 1;
        }
        out.push_back(s);
    }
}

void ParseCommand(std::string line, std::deque<Step>& out)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') return;
    Step s;
    if (line.rfind("shot ", 0) == 0)
    {
        s.kind = Step::Shot;
        s.name = line.substr(5);
        out.push_back(s);
    }
    else if (line.rfind("touch ", 0) == 0)
    {
        s.kind = Step::Touch;
        unsigned long f = 10;
        if (std::sscanf(line.c_str() + 6, "%u %u %lu", &s.x, &s.y, &f) >= 2)
        {
            s.frames = f;
            out.push_back(s);
        }
    }
    else if (line == "link" || line.rfind("link ", 0) == 0)
    {
        s.kind = Step::Link;
        s.name = line.size() > 5 ? line.substr(5) : "";
        out.push_back(s);
    }
    else if (line == "quit")
    {
        s.kind = Step::Quit;
        out.push_back(s);
    }
    else
        ParseKeys(line, out);
}

// a rough link: "loss=5 burst=3 delay=40 jitter=20 dup=1 reorder=1 seed=7 cut=1" changes those (others keep their
// value); "off" turns it all off
UdsLink::Impairment ParseLink(const std::string& text, UdsLink::Impairment imp)
{
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find(' ', at);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        if (item == "off") { imp = {}; continue; }
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq);
        const double v = std::atof(item.c_str() + eq + 1);
        if (key == "loss") imp.lossPct = v;
        else if (key == "burst") imp.burst = v;
        else if (key == "delay") imp.delayMs = v;
        else if (key == "jitter") imp.jitterMs = v;
        else if (key == "dup") imp.dupPct = v;
        else if (key == "reorder") imp.reorder = v != 0.0;
        else if (key == "seed") imp.seed = (uint32_t)v;
        else if (key == "cut") imp.cut = v != 0.0;
    }
    return imp;
}

std::string DescribeLink(const UdsLink::Impairment& i)
{
    if (i.cut) return "CUT OFF (out of range: nothing sent, nothing heard)";
    if (i.lossPct <= 0 && i.delayMs <= 0 && i.jitterMs <= 0 && i.dupPct <= 0) return "clean";
    char buf[200];
    std::snprintf(buf, sizeof(buf), "rough: loss %g%% (bursts of %g), delay %g ms + 0-%g ms jitter%s, doubled %g%%",
                  i.lossPct, std::max(1.0, i.burst), i.delayMs, i.jitterMs, i.reorder ? " (may reorder)" : " (in order)",
                  i.dupPct);
    return buf;
}

// both screens, the top one above the bottom one (centred), as a PNG
bool SaveShot(const std::string& path)
{
    const unsigned W = 400, H = 480;
    auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&Core::System::GetInstance().GPU().Renderer());
    if (!sw)
    {
        std::vector<unsigned char> img;
        return g_sdlWindow && ReadFrameGL(img) && lodepng_encode32_file(path.c_str(), img.data(), W, H) == 0;
    }
    const auto& top = sw->Screen(VideoCore::ScreenId::TopLeft);
    const auto& bottom = sw->Screen(VideoCore::ScreenId::Bottom);
    std::vector<unsigned char> img(W * H * 4, 0);
    for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;
    // (a ScreenInfo holds the picture as width = screen height: rows of info.height pixels, info.width of them)
    auto blit = [&](const SwRenderer::ScreenInfo& s, unsigned ox, unsigned oy) {
        const unsigned w = s.height, h = s.width;
        if (s.pixels.size() < (size_t)w * h * 4) return;
        for (unsigned y = 0; y < h && oy + y < H; y++)
            for (unsigned x = 0; x < w && ox + x < W; x++)
            {
                const unsigned char* p = &s.pixels[((size_t)y * w + x) * 4];
                unsigned char* d = &img[((size_t)(oy + y) * W + ox + x) * 4];
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
            }
    };
    blit(top, 0, 0);
    blit(bottom, 40, 240);
    return lodepng_encode32_file(path.c_str(), img.data(), W, H) == 0;
}
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: rig3ds USERDIR TITLE SECONDS [KEYS]\n");
        return 2;
    }
    std::string dir = argv[1];
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    const std::string title = argv[2];
    const double seconds = std::atof(argv[3]);
    std::deque<Step> steps;
    if (argc > 4) ParseKeys(argv[4], steps);
    const char* cmdFile = std::getenv("RIG_CMD");
    std::string shotDir = std::getenv("RIG_SHOTS") ? std::getenv("RIG_SHOTS") : dir + "shots";
    std::replace(shotDir.begin(), shotDir.end(), '\\', '/');    // (Azahar's CreateFullPath splits on / only)
    if (shotDir.back() != '/') shotDir += '/';
    const double shotEvery = std::getenv("RIG_SHOT_EVERY") ? std::atof(std::getenv("RIG_SHOT_EVERY")) : 0.0;
    FileUtil::CreateFullPath(shotDir);

#ifdef _WIN32
    timeBeginPeriod(1);     // (1 ms timers: a rough link's delays and the frame pacing keep time)
#endif
    FileUtil::SetUserPath(dir);
    // as MultiMelon: the game's own debug output (svcOutputDebugString) in the log too; RIG_LOG_FILTER replaces it
    Settings::values.log_filter.SetValue(std::getenv("RIG_LOG_FILTER") ? std::getenv("RIG_LOG_FILTER")
                                                                       : "*:Info Debug.Emulated:Debug");
    Common::Log::Initialize();
    Common::Log::Start();
    for (const auto& module : Service::service_module_map)
        Settings::values.lle_modules.emplace(module.name, false);
    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
    Input::RegisterFactory<Input::ButtonDevice>("rig", std::make_shared<ButtonFactory>());
    Input::RegisterFactory<Input::AnalogDevice>("rigpad", std::make_shared<PadFactory>());
    for (int i = 0; i < 12; i++)
        Settings::values.current_input_profile.buttons[kOrder[i]] = "engine:rig,button:" + std::to_string(i);
    Settings::values.current_input_profile.analogs[Settings::NativeAnalog::CirclePad] = "engine:rigpad";
    Settings::values.current_input_profile.touch_device = "engine:emu_window";

    const char* rendererName = std::getenv("RIG_RENDERER");
    const bool wantGL = !rendererName || std::string(rendererName) != "software";
    const bool gl = wantGL && StartGL();
    if (wantGL && !gl) std::printf("renderer: no OpenGL 4.3 here, so the software renderer (slow)\n");
    else if (!gl) std::printf("renderer: software (slow)\n");
    Settings::values.graphics_api = gl ? Settings::GraphicsAPI::OpenGL : Settings::GraphicsAPI::Software;
    Settings::values.use_gles = false;
    Settings::values.use_vsync = false;
    Settings::values.resolution_factor = 1;
    Settings::values.layout_option = Settings::LayoutOption::Default;
    Settings::values.frame_limit = std::getenv("RIG_SPEED") ? std::max(0.0, std::atof(std::getenv("RIG_SPEED"))) : 100.0;
    Settings::values.output_type = AudioCore::SinkType::Null;
    // the microphone: none (silence) unless RIG_MIC=static (noise). Azahar's Auto opens the PC's real microphone
    // through cubeb, which aborts here (COM isn't initialised on the emulation thread) the moment a title samples.
    {
        const char* mic = std::getenv("RIG_MIC");
        Settings::values.input_type = (mic && std::string(mic) == "static") ? AudioCore::InputType::Static
                                                                           : AudioCore::InputType::Null;
    }

    // an Azahar room (ThreeDSRoom.h) in place of the link: RIG_ROOM_HOST=port opens one (on RIG_ROOM_BIND, default
    // 127.0.0.1) and joins it; RIG_ROOM_JOIN=address:port joins one. RIG_ROOM_NICK, RIG_ROOM_PASSWORD.
    ThreeDSRoom::Init();     // (before the game starts, as MultiMelon: its local wireless can then use a room)
    const char* roomHost = std::getenv("RIG_ROOM_HOST");
    const char* roomJoin = std::getenv("RIG_ROOM_JOIN");
    const bool useRoom = roomHost || roomJoin;
    if (useRoom)
    {
        ThreeDSRoom::SetEventHandler([](int kind, const std::string& text) {
            const auto info = ThreeDSRoom::GetInfo();
            static const char* states[] = {"idle", "joining", "joined", "failed"};
            if (kind == 0)
                std::printf("room: %s%s%s\n", states[(int)info.state], text.empty() ? "" : ": ", text.c_str());
            else
                std::printf("room: %s\n", text.c_str());
            std::fflush(stdout);
        });
        const char* nickEnv = std::getenv("RIG_ROOM_NICK");
        const char* passEnv = std::getenv("RIG_ROOM_PASSWORD");
        const std::string nick = nickEnv ? nickEnv : "Rig console", pass = passEnv ? passEnv : "";
        if (roomHost)
        {
            const char* bind = std::getenv("RIG_ROOM_BIND");
            const std::string err = ThreeDSRoom::Host("Rig room", nick, (uint16_t)std::atoi(roomHost), 8, pass,
                                                      bind ? bind : "127.0.0.1");
            std::printf("room: hosting on port %s%s%s\n", roomHost, err.empty() ? "" : ": ", err.c_str());
            if (!err.empty()) return 1;
        }
        else
        {
            const std::string at = roomJoin;
            const size_t colon = at.rfind(':');
            const std::string address = colon == std::string::npos ? at : at.substr(0, colon);
            const uint16_t port = colon == std::string::npos ? ThreeDSRoom::DefaultPort
                                                             : (uint16_t)std::atoi(at.c_str() + colon + 1);
            ThreeDSRoom::Join(address, port, nick, pass);
            std::printf("room: joining %s:%u as %s\n", address.c_str(), (unsigned)port, nick.c_str());
        }
        std::fflush(stdout);
    }

    if (!std::getenv("RIG_NO_LINK") && !useRoom)
    {
        const char* port = std::getenv("RIG_LINK_PORT");
        const char* peers = std::getenv("RIG_LINK_PEERS");
        const char* loop = std::getenv("RIG_LINK_LOOPBACK");
        const char* ports = std::getenv("RIG_LINK_PORTS");
        if (!ThreeDSNet::Start(dir, port ? (uint16_t)std::atoi(port) : 7072, peers ? peers : "", loop && loop[0] == '1',
                               ports ? std::atoi(ports) : 1))
        {
            std::printf("link: couldn't open the port\n");
            return 1;
        }
        std::printf("link: port %u\n", (unsigned)ThreeDSNet::Port());
        UdsLink::Impairment imp;
        auto env = [](const char* name, double& to) { if (const char* v = std::getenv(name)) to = std::atof(v); };
        env("RIG_LINK_LOSS", imp.lossPct);
        env("RIG_LINK_BURST", imp.burst);
        env("RIG_LINK_DELAY_MS", imp.delayMs);
        env("RIG_LINK_JITTER_MS", imp.jitterMs);
        env("RIG_LINK_DUP", imp.dupPct);
        if (const char* v = std::getenv("RIG_LINK_REORDER")) imp.reorder = v[0] == '1';
        if (const char* v = std::getenv("RIG_LINK_SEED")) imp.seed = (uint32_t)std::strtoul(v, nullptr, 10);
        UdsLink::SetImpairment(imp);
        std::printf("link: %s\n", DescribeLink(imp).c_str());
    }

    if (const char* plugin = std::getenv("RIG_PLUGIN"))
    {
        Settings::values.plugin_loader_enabled = true;
        FileUtil::CreateFullPath(dir + "sdmc/luma/plugins/");
        FileUtil::Copy(plugin, dir + "sdmc/luma/plugins/default.3gx");
    }

    std::string game = title;
    if (game.size() > 4 && game.substr(game.size() - 4) == ".cia")
    {
        u64 tid = 0;
        {
            FileUtil::IOFile f(game, "rb");
            FileSys::CIAContainer cia;
            if (!f.IsOpen() || cia.Load(&f) != Loader::ResultStatus::Success) { std::printf("bad cia\n"); return 1; }
            tid = cia.GetTitleMetadata().GetTitleID();
        }
        if (!std::getenv("RIG_NO_INSTALL"))
            std::printf("install: %d\n", (int)Service::AM::InstallCIA(game));
        game = Service::AM::GetTitleContentPath(Service::AM::GetTitleMediaType(tid), tid);
        std::printf("title %016llX: %s\n", (unsigned long long)tid, game.c_str());
    }

    Window window(gl);
    system.ApplySettings();
    const auto result = system.Load(window, game);
    std::printf("load: %d\n", (int)result);
    std::fflush(stdout);
    if (result != Core::System::ResultStatus::Success) return 1;
    {
        u64 programId = 0;
        system.GetAppLoader().ReadProgramId(programId);
        system.GPU().ApplyPerProgramSettings(programId);    // (as MultiMelon does)
    }

    const auto start = std::chrono::steady_clock::now();
    unsigned long stepFrames = 0, lastFrames = 0, reportFrames = 0;
    double nextReport = 5.0, nextCmdCheck = 0.0, nextShot = shotEvery;
    size_t cmdRead = 0;     // (bytes of RIG_CMD done)
    bool touching = false, quit = false;
    while (!quit)
    {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= seconds) break;
        if (system.RunLoop() != Core::System::ResultStatus::Success)
        {
            std::printf("core stopped at %.1f s\n", t);
            break;
        }
        if (cmdFile && t >= nextCmdCheck)
        {
            nextCmdCheck = t + 0.25;
            std::string text;
            if (FileUtil::ReadFileToString(true, cmdFile, text) && text.size() > cmdRead)
            {
                const size_t lastNewline = text.rfind('\n');
                if (lastNewline != std::string::npos && lastNewline + 1 > cmdRead)
                {
                    const std::string fresh = text.substr(cmdRead, lastNewline + 1 - cmdRead);
                    cmdRead = lastNewline + 1;
                    size_t a = 0;
                    while (a < fresh.size())
                    {
                        const size_t e = fresh.find('\n', a);
                        ParseCommand(fresh.substr(a, e - a), steps);
                        a = e + 1;
                    }
                }
            }
        }
        if (shotEvery > 0 && t >= nextShot)
        {
            nextShot += shotEvery;
            char name[32];
            std::snprintf(name, sizeof(name), "t%05.0f.png", t);
            SaveShot(shotDir + name);
        }
        if (window.Frames != lastFrames)
        {
            lastFrames = window.Frames;
            if (g_sdlWindow) SDL_PumpEvents();
            while (!steps.empty() && (steps.front().kind == Step::Shot || steps.front().kind == Step::Link))
            {
                if (steps.front().kind == Step::Link)
                {
                    const auto imp = ParseLink(steps.front().name, UdsLink::GetImpairment());
                    UdsLink::SetImpairment(imp);
                    std::printf("[%5.1f s] link: %s\n", t, DescribeLink(imp).c_str());
                }
                else
                {
                    const bool ok = SaveShot(shotDir + steps.front().name + ".png");
                    std::printf("[%5.1f s] shot %s%s\n", t, steps.front().name.c_str(), ok ? "" : " FAILED");
                }
                std::fflush(stdout);
                steps.pop_front();
            }
            if (!steps.empty() && steps.front().kind == Step::Quit)
            {
                quit = true;
                continue;
            }
            g_keys = 0;
            if (!steps.empty())
            {
                const Step& s = steps.front();
                if (s.kind == Step::Touch)
                {
                    if (!touching)
                    {
                        window.TouchPressed(40 + s.x, 240 + s.y);
                        touching = true;
                    }
                }
                else
                    g_keys = s.keys;
                if (++stepFrames >= s.frames)
                {
                    if (s.kind == Step::Touch)
                    {
                        window.TouchReleased();
                        touching = false;
                    }
                    steps.pop_front();
                    stepFrames = 0;
                }
            }
            g_padX = (g_keys & kPadRight) ? 1.0f : (g_keys & kPadLeft) ? -1.0f : 0.0f;
            g_padY = (g_keys & kPadUp) ? 1.0f : (g_keys & kPadDown) ? -1.0f : 0.0f;
        }
        if (t >= nextReport)
        {
            if (useRoom)
            {
                const auto r = ThreeDSRoom::GetInfo();
                static const char* states[] = {"idle", "joining", "joined", "failed"};
                std::printf("[%5.1f s] frames %lu (%.0f a second), room: %s, %zu members\n", t, window.Frames,
                            (window.Frames - reportFrames) / 5.0, states[(int)r.state], r.members.size());
            }
            else
            {
                const auto s = UdsLink::GetStats();
                char rough[64] = "";
                if (s.dropped || s.doubled) std::snprintf(rough, sizeof(rough), ", dropped %u, doubled %u", s.dropped, s.doubled);
                std::printf("[%5.1f s] frames %lu (%.0f a second), link: %d nearby, frames out %u in %u%s\n", t, window.Frames,
                            (window.Frames - reportFrames) / 5.0, s.peers, s.framesOut, s.framesIn, rough);
            }
            reportFrames = window.Frames;
            std::fflush(stdout);
            nextReport += 5.0;
        }
    }
    std::printf("shutting down\n");
    std::fflush(stdout);
    system.Shutdown();
    std::printf("core shut down\n");
    std::fflush(stdout);
    ThreeDSNet::Stop();
    ThreeDSRoom::Leave();
    std::printf("done after %lu frames\n", window.Frames);
    std::fflush(stdout);
    Common::Log::Stop();
    if (std::getenv("RIG_FAST_EXIT")) std::_Exit(0);
    return 0;
}
