#include "embeddedps2.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/settings/values.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace MWKurenai
{
    namespace
    {
#ifdef _WIN32
        constexpr const char* sLibraryName = "pcsx2-kurenai.dll";
#else
        constexpr const char* sLibraryName = "pcsx2-kurenai.so";
#endif
        using RunFn = int (*)(const char* root, const char* data, int argc, char** argv);
        using StopFn = void (*)();

        void* openLibrary(const std::filesystem::path& path, std::string& error)
        {
#ifdef _WIN32
            // The altered search path finds the library's own DLLs (SDL3, ...) next to it, not next to openmw.exe.
            HMODULE h = LoadLibraryExW(path.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!h)
                error = "LoadLibrary error " + std::to_string(GetLastError());
            return reinterpret_cast<void*>(h);
#else
            void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h)
                error = dlerror();
            return h;
#endif
        }

        void* symbol(void* library, const char* name)
        {
#ifdef _WIN32
            return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
            return dlsym(library, name);
#endif
        }

        // "key = value" lines (profiles/<serial>/game.profile).
        std::map<std::string, std::string> readProfile(const std::filesystem::path& path)
        {
            std::map<std::string, std::string> values;
            std::ifstream in(path);
            std::string line;
            while (std::getline(in, line))
            {
                const auto eq = line.find(" = ");
                if (eq != std::string::npos && !line.empty() && line[0] != '#')
                    values[line.substr(0, eq)] = line.substr(eq + 3);
            }
            return values;
        }
    }

    std::unique_ptr<EmbeddedPs2> EmbeddedPs2::start(const std::string& bridgeName)
    {
        static std::atomic<bool> started{ false };
        const Settings::KurenaiCategory& k = Settings::kurenai();
        const std::string folderSetting = k.mPs2Folder;
        if (folderSetting.empty())
            return nullptr;
        if (started.exchange(true))
        {
            Log(Debug::Error) << "Kurenai: the embedded game runs once per OpenMW session; restart OpenMW";
            return nullptr;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path folder = fs::absolute(Files::pathFromUnicodeString(folderSetting), ec);
        const fs::path profile = folder / "profile";
        const fs::path iso = Files::pathFromUnicodeString(k.mPs2Iso.get());
        const fs::path bios = Files::pathFromUnicodeString(k.mPs2Bios.get());
        const fs::path data
            = k.mPs2Data.get().empty() ? folder / "data" : Files::pathFromUnicodeString(k.mPs2Data.get());
        for (const auto& [what, path] : { std::pair{ "game library", folder / sLibraryName },
                 std::pair{ "game profile", profile / "game.profile" }, std::pair{ "disc image ([Kurenai] ps2 iso)", iso },
                 std::pair{ "PS2 BIOS ([Kurenai] ps2 bios)", bios } })
        {
            if (!fs::is_regular_file(path, ec))
            {
                Log(Debug::Error) << "Kurenai: cannot start Red Ninja: no " << what << " at "
                                  << Files::pathToUnicodeString(path);
                return nullptr;
            }
        }
        const auto values = readProfile(profile / "game.profile");
        const auto value = [&](const char* key) {
            const auto it = values.find(key);
            return it == values.end() ? std::string() : it->second;
        };

        std::unique_ptr<EmbeddedPs2> ps2(new EmbeddedPs2());
        std::string error;
        ps2->mLibrary = openLibrary(folder / sLibraryName, error);
        if (!ps2->mLibrary)
        {
            Log(Debug::Error) << "Kurenai: cannot load " << sLibraryName << ": " << error;
            return nullptr;
        }
        const auto run = reinterpret_cast<RunFn>(symbol(ps2->mLibrary, "krn_ps2_run"));
        ps2->mStop = reinterpret_cast<StopFn>(symbol(ps2->mLibrary, "krn_ps2_stop"));
        if (!run || !ps2->mStop)
        {
            Log(Debug::Error) << "Kurenai: " << sLibraryName << " is not the KURENAI game library";
            return nullptr;
        }
        fs::create_directories(data / "logs", ec);
        // PCSX2 looks for the BIOS in its own folder before command-line settings apply.
        fs::create_directories(data / "bios", ec);
        const fs::path biosCopy = data / "bios" / bios.filename();
        if (!fs::exists(biosCopy, ec) || fs::file_size(biosCopy, ec) != fs::file_size(bios, ec))
            fs::copy_file(bios, biosCopy, fs::copy_options::overwrite_existing, ec);

        const auto u8 = [](const fs::path& p) { return Files::pathToUnicodeString(p); };
        const std::string prof = u8(profile);
        const std::string logs = u8(data / "logs");
        std::vector<std::string>& a = ps2->mArgs;
        a = { "pcsx2-kurenai", "-game", "-surfaceless", "-bios", u8(bios), "-logfile", logs + "/pcsx2.log" };
        if (k.mPs2Renderer.get() != "auto")
            a.insert(a.end(), { "-renderer", k.mPs2Renderer.get() });
        const std::vector<std::string> sets = {
            "EmuCore/EnableFastBoot=true",
            "MemoryCards/Slot1_Enable=false",
            "MemoryCards/Slot2_Enable=false",
            "EmuCore/EnableCheats=false",
            "Kurenai/Enabled=true",
            "Kurenai/Serial=" + value("serial"),
            "Kurenai/ElfSha256=" + value("boot_sha256"),
            "Kurenai/Bridge=" + bridgeName,
            "Kurenai/AutoStart=true",
            "Kurenai/TracePoints=" + prof + "/trace_points.tsv",
            "Kurenai/TraceOutput=" + logs + "/trace.tsv",
            "Kurenai/MaxHitsPerFrame=64",
            "Kurenai/PlayerLayout=" + prof + "/player.layout",
            "Kurenai/PlayerLog=" + logs + "/player.tsv",
            "Kurenai/CollisionLayout=" + prof + "/collision.layout",
            "Kurenai/AnswerCollision=" + std::string(k.mAnswerCollision ? "true" : "false"),
            "Kurenai/AttackTable=" + prof + "/attacks.tsv",
            "Kurenai/SkeletonMaps=" + prof + "/skeletons",
            "Kurenai/WireMotions=" + prof + "/wire.tsv",
            "Kurenai/EnemyLayout=" + prof + "/enemy.layout",
            "Kurenai/HookLayout=" + prof + "/hook.layout",
            "Kurenai/HudLayout=" + prof + "/hud.layout",
            "Kurenai/KunaiAimAtLock=true",
        };
        for (const std::string& s : sets)
            a.insert(a.end(), { "-set", s });
        // An HD texture pack unpacked into <data>/textures/<serial>/replacements (PCSX2's own layout).
        const fs::path hd = data / "textures" / value("serial") / "replacements";
        if (!value("serial").empty() && fs::is_directory(hd, ec))
        {
            a.insert(a.end(), { "-set", "EmuCore/GS/LoadTextureReplacements=true" });
            Log(Debug::Info) << "Kurenai: HD textures from " << u8(hd);
        }
        a.insert(a.end(), { "--", u8(iso) });
        for (std::string& s : a)
            ps2->mArgv.push_back(s.data());
        ps2->mArgv.push_back(nullptr);
        ps2->mRoot = u8(folder);
        ps2->mData = u8(data);

        Log(Debug::Info) << "Kurenai: starting Red Ninja inside OpenMW (" << ps2->mRoot << ", log " << logs
                         << "/pcsx2.log)";
        EmbeddedPs2* self = ps2.get();
        ps2->mThread = std::thread([self, run] {
            self->mResult = run(self->mRoot.c_str(), self->mData.c_str(), static_cast<int>(self->mArgv.size() - 1),
                self->mArgv.data());
            Log(Debug::Info) << "Kurenai: Red Ninja stopped (" << self->mResult << ")";
            self->mDone = true;
        });
        return ps2;
    }

    EmbeddedPs2::~EmbeddedPs2()
    {
        if (!mThread.joinable())
            return;
        if (!mDone)
            mStop();
        // PCSX2 saves and shuts down in well under a second; a hung emulator must not hold OpenMW's exit. The
        // library stays loaded: a detached thread may still be in it.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!mDone && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (mDone)
            mThread.join();
        else
        {
            Log(Debug::Error) << "Kurenai: Red Ninja did not stop in time; leaving it";
            mThread.detach();
        }
    }
}
