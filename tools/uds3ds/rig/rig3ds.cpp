// rig3ds: runs a 3DS title headless (Azahar's core, software renderer, no window) for a while, with
// MultiMelon's "Wifi as Local Multiplayer" link (ThreeDSNet/UdsLink), and prints what happened. Two of
// them on loopback test 3DS local wireless between consoles without opening any window.
//
// usage: rig3ds USERDIR TITLE SECONDS [KEYS]
//   USERDIR  this console's 3DS folder (its sdmc/, nand/, log/ and mac.txt)
//   KEYS     optional input script for the title: "keys*frames,..." (a b x y l r start select up down left right)
// env: RIG_LINK_PORT (default 7072), RIG_LINK_PEERS ("ip:port,..."), RIG_LINK_LOOPBACK=1, RIG_NO_LINK=1,
//      RIG_PLUGIN=file.3gx (Azahar's Luma plugin loader on, that plugin as sdmc:/luma/plugins/default.3gx)
// A .cia TITLE is installed first (once is enough: RIG_NO_INSTALL=1 then starts the installed game).

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

#include "ThreeDSNet.h"
#include "UdsLink.h"

namespace
{
unsigned g_keys = 0;    // pressed now (bit i = button i of the order below)

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

class Window final : public Frontend::EmuWindow
{
public:
    Window() { window_info.type = Frontend::WindowSystemType::Headless; UpdateCurrentFramebufferLayout(400, 480); }
    void PollEvents() override { Frames++; }
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    unsigned long Frames = 0;
};

const char* kNames[] = {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select"};
const int kOrder[] = {Settings::NativeButton::A, Settings::NativeButton::B, Settings::NativeButton::X,
                      Settings::NativeButton::Y, Settings::NativeButton::Up, Settings::NativeButton::Down,
                      Settings::NativeButton::Left, Settings::NativeButton::Right, Settings::NativeButton::L,
                      Settings::NativeButton::R, Settings::NativeButton::Start, Settings::NativeButton::Select};

struct Step { unsigned keys; unsigned long frames; };
std::vector<Step> ParseKeys(const std::string& script)
{
    std::vector<Step> out;
    size_t at = 0;
    while (at < script.size())
    {
        size_t end = script.find(',', at);
        if (end == std::string::npos) end = script.size();
        const std::string item = script.substr(at, end - at);
        at = end + 1;
        const size_t star = item.find('*');
        const std::string keys = item.substr(0, star);
        Step s {0, star == std::string::npos ? 1ul : std::strtoul(item.c_str() + star + 1, nullptr, 10)};
        size_t k = 0;
        while (k <= keys.size())
        {
            size_t plus = keys.find('+', k);
            if (plus == std::string::npos) plus = keys.size();
            const std::string key = keys.substr(k, plus - k);
            for (int i = 0; i < 12; i++) if (key == kNames[i]) s.keys |= 1u << i;
            k = plus + 1;
        }
        out.push_back(s);
    }
    return out;
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
    const std::vector<Step> steps = argc > 4 ? ParseKeys(argv[4]) : std::vector<Step>{};

    FileUtil::SetUserPath(dir);
    Common::Log::Initialize();
    Common::Log::Start();
    for (const auto& module : Service::service_module_map)
        Settings::values.lle_modules.emplace(module.name, false);
    auto& system = Core::System::GetInstance();
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
    Input::RegisterFactory<Input::ButtonDevice>("rig", std::make_shared<ButtonFactory>());
    for (int i = 0; i < 12; i++)
        Settings::values.current_input_profile.buttons[kOrder[i]] = "engine:rig,button:" + std::to_string(i);

    Settings::values.graphics_api = Settings::GraphicsAPI::Software;
    Settings::values.frame_limit = 100;
    Settings::values.output_type = AudioCore::SinkType::Null;

    if (!std::getenv("RIG_NO_LINK"))
    {
        const char* port = std::getenv("RIG_LINK_PORT");
        const char* peers = std::getenv("RIG_LINK_PEERS");
        const char* loop = std::getenv("RIG_LINK_LOOPBACK");
        if (!ThreeDSNet::Start(dir, port ? (uint16_t)std::atoi(port) : 7072, peers ? peers : "", loop && loop[0] == '1'))
        {
            std::printf("link: couldn't open the port\n");
            return 1;
        }
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
    if (result != Core::System::ResultStatus::Success) return 1;

    const auto start = std::chrono::steady_clock::now();
    size_t step = 0;
    unsigned long stepFrames = 0, lastFrames = 0;
    double nextReport = 5.0;
    while (true)
    {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (t >= seconds) break;
        if (system.RunLoop() != Core::System::ResultStatus::Success)
        {
            std::printf("core stopped at %.1f s\n", t);
            break;
        }
        if (window.Frames != lastFrames)
        {
            lastFrames = window.Frames;
            if (step < steps.size())
            {
                g_keys = steps[step].keys;
                if (++stepFrames >= steps[step].frames) { step++; stepFrames = 0; }
            }
            else g_keys = 0;
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
