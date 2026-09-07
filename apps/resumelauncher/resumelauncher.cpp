// ResumeLauncher.exe
//
// Finds the most recently modified .omwsave file under the user's
// "Documents\My Games\OpenMW\saves" folder (searched recursively, since
// saves are organized in one subfolder per character) and launches
// openmw_vr.exe (expected to sit right next to this .exe) straight into that
// save via --skip-menu --load-savegame, instead of going through the main
// menu. All other settings (data paths, mods, graphics) come from the same
// openmw.cfg/settings.cfg openmw.exe always reads - this doesn't touch or
// duplicate any of that, it only picks the save file and adds two
// command-line flags. This VR launcher is intentionally independent from the
// desktop OpenMW launcher and never falls back to openmw.exe.
//
// Windows subsystem app (no console window). Shows a message box on error
// instead of writing to a console nobody would see.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <optional>

namespace fs = std::filesystem;

namespace
{
    void showError(const std::wstring& message)
    {
        MessageBoxW(nullptr, message.c_str(), L"ResumeLauncher", MB_OK | MB_ICONERROR);
    }

    std::optional<fs::path> findDocumentsFolder()
    {
        PWSTR path = nullptr;
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path);
        if (FAILED(hr) || !path)
            return std::nullopt;
        fs::path result(path);
        CoTaskMemFree(path);
        return result;
    }

    // Recursively find the most recently modified *.omwsave under savesRoot.
    std::optional<fs::path> findLatestSave(const fs::path& savesRoot)
    {
        std::optional<fs::path> latest;
        fs::file_time_type latestTime;

        std::error_code ec;
        if (!fs::exists(savesRoot, ec) || ec)
            return std::nullopt;

        for (auto it = fs::recursive_directory_iterator(savesRoot, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                continue;
            const auto& entry = *it;
            if (!entry.is_regular_file(ec) || ec)
                continue;
            if (entry.path().extension() != L".omwsave")
                continue;

            auto writeTime = entry.last_write_time(ec);
            if (ec)
                continue;

            if (!latest || writeTime > latestTime)
            {
                latest = entry.path();
                latestTime = writeTime;
            }
        }
        return latest;
    }

    // Wrap a path in quotes for the command line, escaping any embedded quotes.
    std::wstring quoteArg(const std::wstring& arg)
    {
        std::wstring out = L"\"";
        for (wchar_t c : arg)
        {
            if (c == L'"')
                out += L'\\';
            out += c;
        }
        out += L'"';
        return out;
    }
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    wchar_t exePathBuf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        showError(L"Could not determine this executable's own path.");
        return 1;
    }
    fs::path selfDir = fs::path(exePathBuf).parent_path();

    fs::path openmwExe = selfDir / L"openmw_vr.exe";
    if (!fs::exists(openmwExe))
    {
        showError(L"openmw_vr.exe was not found next to the OpenMW-VR ResumeLauncher.exe:\n" + openmwExe.wstring()
            + L"\n\nPlace this VR ResumeLauncher.exe in the same folder as openmw_vr.exe.");
        return 1;
    }

    auto documents = findDocumentsFolder();
    if (!documents)
    {
        showError(L"Could not locate the Documents folder.");
        return 1;
    }

    fs::path savesRoot = *documents / L"My Games" / L"OpenMW" / L"saves";
    auto latestSave = findLatestSave(savesRoot);
    if (!latestSave)
    {
        showError(L"No save games (.omwsave) were found under:\n" + savesRoot.wstring());
        return 1;
    }

    std::wstring commandLine = quoteArg(openmwExe.wstring()) + L" --skip-menu --load-savegame "
        + quoteArg(latestSave->wstring());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CreateProcessW requires a mutable command-line buffer.
    std::wstring mutableCommandLine = commandLine;
    BOOL ok = CreateProcessW(openmwExe.c_str(), mutableCommandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
        selfDir.c_str(), &si, &pi);

    if (!ok)
    {
        DWORD err = GetLastError();
        showError(L"Failed to launch openmw_vr.exe (error " + std::to_wstring(err) + L").");
        return 1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
