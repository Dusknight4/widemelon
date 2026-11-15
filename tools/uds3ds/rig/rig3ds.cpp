// rig3ds: runs a 3DS title headless (Azahar's core, software renderer, no window) for a while, with
// MultiMelon's "Wifi as Local Multiplayer" link (ThreeDSNet/UdsLink), and prints what happened. Two of
// them on loopback test 3DS local wireless between consoles without opening any window.
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
// A .cia TITLE is installed first (once is enough: RIG_NO_INSTALL=1 then starts the installed game).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/settings.h"
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
#include "video_core/renderer_software/renderer_software.h"

#include "lodepng.h"

#include "ThreeDSNet.h"
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

class Window final : public Frontend::EmuWindow
{
public:
    Window() { window_info.type = Frontend::WindowSystemType::Headless; UpdateCurrentFramebufferLayout(400, 480); }
    void PollEvents() override { Frames++; }
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    unsigned long Frames = 0;
};

const char* kNames[] = {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select",
                        "cup", "cdown", "cleft", "cright"};
const int kOrder[] = {Settings::NativeButton::A, Settings::NativeButton::B, Settings::NativeButton::X,
                      Settings::NativeButton::Y, Settings::NativeButton::Up, Settings::NativeButton::Down,
                      Settings::NativeButton::Left, Settings::NativeButton::Right, Settings::NativeButton::L,
                      Settings::NativeButton::R, Settings::NativeButton::Start, Settings::NativeButton::Select};
constexpr unsigned kPadUp = 1u << 12, kPadDown = 1u << 13, kPadLeft = 1u << 14, kPadRight = 1u << 15;

struct Step
{
    enum Kind { Keys, Shot, Touch, Quit } kind = Keys;
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
    else if (line == "quit")
    {
        s.kind = Step::Quit;
        out.push_back(s);
    }
    else
        ParseKeys(line, out);
}

// both screens, the top one above the bottom one (centred), as a PNG
bool SaveShot(const std::string& path)
{
    auto* sw = dynamic_cast<SwRenderer::RendererSoftware*>(&Core::System::GetInstance().GPU().Renderer());
    if (!sw) return false;
    const auto& top = sw->Screen(VideoCore::ScreenId::TopLeft);
    const auto& bottom = sw->Screen(VideoCore::ScreenId::Bottom);
    const unsigned W = 400, H = 480;
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
    if (shotDir.back() != '/' && shotDir.back() != '\\') shotDir += '/';
    const double shotEvery = std::getenv("RIG_SHOT_EVERY") ? std::atof(std::getenv("RIG_SHOT_EVERY")) : 0.0;
    FileUtil::CreateFullPath(shotDir);

    FileUtil::SetUserPath(dir);
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

    Settings::values.graphics_api = Settings::GraphicsAPI::Software;
    Settings::values.frame_limit = 100;
    Settings::values.output_type = AudioCore::SinkType::Null;

    if (!std::getenv("RIG_NO_LINK"))
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

    Window window;
    system.ApplySettings();
    const auto result = system.Load(window, game);
    std::printf("load: %d\n", (int)result);
    std::fflush(stdout);
    if (result != Core::System::ResultStatus::Success) return 1;

    const auto start = std::chrono::steady_clock::now();
    unsigned long stepFrames = 0, lastFrames = 0;
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
            while (!steps.empty() && steps.front().kind == Step::Shot)
            {
                const bool ok = SaveShot(shotDir + steps.front().name + ".png");
                std::printf("[%5.1f s] shot %s%s\n", t, steps.front().name.c_str(), ok ? "" : " FAILED");
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
            const auto s = UdsLink::GetStats();
            std::printf("[%5.1f s] frames %lu, link: %d nearby, frames out %u in %u\n", t, window.Frames, s.peers, s.framesOut, s.framesIn);
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
    std::printf("done after %lu frames\n", window.Frames);
    std::fflush(stdout);
    Common::Log::Stop();
    if (std::getenv("RIG_FAST_EXIT")) std::_Exit(0);
    return 0;
}
