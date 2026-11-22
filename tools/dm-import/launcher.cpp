// brutalmorrowind-import: runs the Dark Messiah importer with the Python runtime
// installed next to it (dm-import/python), passing its arguments, standard
// streams and exit code through. Without a bundled runtime it uses
// BRUTAL_IMPORT_PYTHON or python3 from PATH, which is how development builds run.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef DM_IMPORT_RELATIVE
#define DM_IMPORT_RELATIVE "dm-import"
#endif

namespace
{
    std::filesystem::path executableDirectory()
    {
#ifdef _WIN32
        std::wstring buffer(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        buffer.resize(size);
        return std::filesystem::path(buffer).parent_path();
#else
        return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
    }

#ifdef _WIN32
    // Quote one argument for CommandLineToArgvW / the C runtime.
    std::wstring quote(const std::wstring& argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
            return argument;
        std::wstring quoted = L"\"";
        for (auto it = argument.begin();; ++it)
        {
            std::size_t backslashes = 0;
            while (it != argument.end() && *it == L'\\')
            {
                ++it;
                ++backslashes;
            }
            if (it == argument.end())
            {
                quoted.append(backslashes * 2, L'\\');
                break;
            }
            if (*it == L'"')
                quoted.append(backslashes * 2 + 1, L'\\');
            else
                quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
        quoted.push_back(L'"');
        return quoted;
    }
#endif
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    // Installed layout first, then dm-import next to the program (build tree,
    // packages).
    std::filesystem::path base = executableDirectory() / DM_IMPORT_RELATIVE;
    if (!std::filesystem::is_regular_file(base / "dm_import.py"))
        base = executableDirectory() / "dm-import";
    const std::filesystem::path script = base / "dm_import.py";
#ifdef _WIN32
    std::filesystem::path python = base / "python" / "python.exe";
#else
    std::filesystem::path python = base / "python" / "bin" / "python3";
#endif
    const bool bundled = std::filesystem::is_regular_file(python);
    if (!bundled)
    {
        const char* configured = std::getenv("BRUTAL_IMPORT_PYTHON");
#ifdef _WIN32
        python = configured != nullptr ? configured : "python";
#else
        python = configured != nullptr ? configured : "python3";
#endif
    }
    if (!std::filesystem::is_regular_file(script))
    {
        std::cerr << "brutalmorrowind-import: missing " << script.string() << std::endl;
        return 1;
    }

    // Only the importer's own modules: no user site, no PYTHONPATH from the
    // environment. Children started with sys.executable inherit this.
#ifdef _WIN32
    SetEnvironmentVariableW(L"PYTHONPATH", nullptr);
    SetEnvironmentVariableW(L"PYTHONHOME", nullptr);
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");

    std::wstring commandLine = quote(python.wstring()) + L" " + quote(script.wstring());
    for (int i = 1; i < argc; ++i)
        commandLine += L" " + quote(argv[i]);

    // Steps run as children of Python; a job takes them down with this process.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    // Hand Python this process's standard handles explicitly: the launcher
    // reads progress from stdout and cancels by closing stdin.
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    for (HANDLE handle : { startup.hStdInput, startup.hStdOutput, startup.hStdError })
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(bundled ? python.wstring().c_str() : nullptr, commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
    {
        std::wcerr << L"brutalmorrowind-import: cannot start " << python.wstring() << L" (error " << GetLastError()
                   << L")" << std::endl;
        return 1;
    }
    if (job != nullptr)
        AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    if (bundled)
    {
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("QT_PLUGIN_PATH");
        unsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
    }
    unsetenv("PYTHONPATH");
    unsetenv("PYTHONHOME");
    setenv("PYTHONNOUSERSITE", "1", 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    setenv("PYTHONUTF8", "1", 1);

    const std::string pythonPath = python.string();
    const std::string scriptPath = script.string();
    std::vector<char*> arguments{ const_cast<char*>(pythonPath.c_str()), const_cast<char*>(scriptPath.c_str()) };
    for (int i = 1; i < argc; ++i)
        arguments.push_back(argv[i]);
    arguments.push_back(nullptr);
    execvp(arguments[0], arguments.data());
    std::cerr << "brutalmorrowind-import: cannot start " << pythonPath << std::endl;
    return 1;
#endif
}
