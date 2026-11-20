#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace run {
using std::wstring;
using Path = std::filesystem::path;
inline std::string utf8(const wstring &w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), int(w.size()), nullptr, 0, nullptr,
                                nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode text");
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline wstring wide(const std::string &s, bool strict = false) {
    if (s.empty())
        return {};
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int n = MultiByteToWideChar(CP_UTF8, flags, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, flags, s.data(), int(s.size()), w.data(), n);
    return w;
}
inline wstring systemMessage(DWORD code) {
    wchar_t *p = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&p), 0, nullptr);
    wstring s = p ? p : std::to_wstring(code);
    if (p)
        LocalFree(p);
    while (!s.empty() && iswspace(s.back()))
        s.pop_back();
    return s;
}
inline void check(BOOL ok, const char *action) {
    if (!ok)
        throw std::runtime_error(std::string(action) + ": " + utf8(systemMessage(GetLastError())));
}
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() { close(); }
    void close() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = nullptr;
    }
};
struct Temporary {
    wchar_t path[MAX_PATH]{};
    Handle file;
    explicit Temporary(bool inherit = false) {
        wchar_t dir[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, dir);
        check(len > 0 && len < MAX_PATH, "GetTempPath");
        check(GetTempFileNameW(dir, L"pyd", 0, path) != 0, "GetTempFileName");
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, inherit};
        file.h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file.h == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            DeleteFileW(path);
            SetLastError(error);
            check(FALSE, "CreateFile");
        }
    }
    ~Temporary() {
        file.close();
        DeleteFileW(path);
    }
    void fill(const std::string &bytes) {
        DWORD count = 0;
        check(WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr),
              "WriteFile");
        if (count != bytes.size())
            throw std::runtime_error("Incomplete temporary file");
        LARGE_INTEGER zero{};
        check(SetFilePointerEx(file.h, zero, nullptr, FILE_BEGIN), "SetFilePointerEx");
    }
};
inline wstring quote(const wstring &arg) {
    wstring out = L"\"";
    size_t backslashes = 0;
    for (auto c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L'"';
}
struct Interpreter {
    wstring path;
    bool launcher;
};
inline Interpreter python() {
    // Search PATH explicitly. Opening a script never makes its directory an executable search path.
    DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    wstring env(n, L'\0');
    if (n)
        env.resize(GetEnvironmentVariableW(L"PATH", env.data(), n));
    for (auto name : {L"python.exe", L"py.exe"}) {
        for (size_t i = 0; i <= env.size();) {
            size_t e = env.find(L';', i);
            wstring d = env.substr(i, e - i);
            if (d.size() >= 2 && d.front() == '"' && d.back() == '"')
                d = d.substr(1, d.size() - 2);
            Path candidate = Path(d) / name;
            std::error_code ec;
            if (Path(d).is_absolute() && std::filesystem::is_regular_file(candidate, ec)) {
                // Store execution aliases without an installed interpreter open the Store.
                if (!(_wcsicmp(name, L"python.exe") == 0 &&
                      _wcsicmp(Path(d).filename().c_str(), L"WindowsApps") == 0))
                    return {candidate.wstring(), _wcsicmp(name, L"py.exe") == 0};
            }
            if (e == wstring::npos)
                break;
            i = e + 1;
        }
    }
    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    Path py = Path(win) / L"py.exe";
    if (GetFileAttributesW(py.c_str()) != INVALID_FILE_ATTRIBUTES)
        return {py.wstring(), true};
    throw std::runtime_error(
        utf8(L"Python 3 が見つかりません。python.exe または py.exe を PATH に追加してください。"));
}
enum class State { success, error, stopped, timeout };
struct Result {
    State state;
    DWORD code;
    long long ms;
};
using Sink = std::function<void(const char *, size_t)>;
inline constexpr size_t outputLimit = 1024 * 1024;
inline Result execute(const wstring &code, const wstring &input, const Path &filename, int timeoutMs,
                      const std::atomic<bool> &stop, const Sink &sink) {
    auto interpreter = python();
    Temporary source, in(true);
    source.fill(utf8(code));
    source.file.close();
    in.fill(utf8(input));
    Handle reader, writer, job(CreateJobObjectW(nullptr, nullptr));
    check(job.h != nullptr, "CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
          "SetInformationJobObject");
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    check(CreatePipe(&reader.h, &writer.h, &sa, 0), "CreatePipe");
    check(SetHandleInformation(reader.h, HANDLE_FLAG_INHERIT, 0), "SetHandleInformation");
    // A linecache entry makes tracebacks display the actual editor buffer, even before saving.
    wstring bootstrap = L"import sys, os, linecache, traceback\n"
                        L"src, name = sys.argv[1:3]\n"
                        L"with open(src, encoding='utf-8') as f: code = f.read()\n"
                        L"sys.argv[:] = [name]\n"
                        L"sys.path[0] = os.getcwd()\n"
                        L"linecache.cache[name] = (len(code), None, code.splitlines(True), name)\n"
                        L"import types\n"
                        L"main = types.ModuleType('__main__')\n"
                        L"main.__file__ = name\n"
                        L"sys.modules['__main__'] = main\n"
                        L"def report(t, v, tb):\n"
                        L"    if tb and tb.tb_frame.f_code.co_filename == '<string>': tb = tb.tb_next\n"
                        L"    traceback.print_exception(t, v, tb)\n"
                        L"sys.excepthook = report\n"
                        L"exec(compile(code, name, 'exec'), main.__dict__)\n";
    Path full = std::filesystem::absolute(filename);
    wstring cmd = quote(interpreter.path) + (interpreter.launcher ? L" -3" : L"") + L" -E -X utf8 -u -c " +
                  quote(bootstrap) + L" " + quote(source.path) + L" " + quote(full.wstring());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in.file.h;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = writer.h;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> buf(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    check(InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size),
          "InitializeProcThreadAttributeList");
    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST p;
        ~Attributes() { DeleteProcThreadAttributeList(p); }
    } attrs{si.lpAttributeList};
    HANDLE pass[] = {in.file.h, writer.h};
    check(UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, pass,
                                    sizeof(pass), nullptr, nullptr),
          "UpdateProcThreadAttribute");
    PROCESS_INFORMATION pi{};
    auto begin = std::chrono::steady_clock::now();
    check(CreateProcessW(interpreter.path.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                         full.parent_path().c_str(), &si.StartupInfo, &pi),
          "CreateProcess");
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) {
        DWORD err = GetLastError();
        TerminateProcess(process.h, 1);
        SetLastError(err);
        check(FALSE, "AssignProcessToJobObject");
    }
    check(ResumeThread(thread.h) != DWORD(-1), "ResumeThread");
    writer.close();
    size_t emitted = 0;
    bool clipped = false;
    auto drain = [&]() {
        // Consume a bounded batch so a continuous producer cannot starve cancellation.
        for (int batch = 0; batch < 32; ++batch) {
            DWORD avail = 0, got = 0;
            if (!PeekNamedPipe(reader.h, nullptr, 0, nullptr, &avail, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                    return;
                check(FALSE, "PeekNamedPipe");
            }
            if (!avail)
                return;
            char bytes[16384];
            check(ReadFile(reader.h, bytes, std::min<DWORD>(sizeof(bytes), avail), &got, nullptr),
                  "ReadFile");
            size_t take = std::min<size_t>(got, outputLimit - emitted);
            if (take)
                sink(bytes, take);
            emitted += take;
            if (take < got && !clipped) {
                const auto note = utf8(L"\n［出力が 1 MiB を超えたため、以降の表示を省略しました］\n");
                sink(note.data(), note.size());
                clipped = true;
            }
        }
    };
    State state = State::success;
    auto elapsed = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin)
            .count();
    };
    for (;;) {
        drain();
        DWORD wait = WaitForSingleObject(process.h, 4);
        if (wait == WAIT_OBJECT_0)
            break;
        check(wait != WAIT_FAILED, "WaitForSingleObject");
        if (stop.load() || (timeoutMs && elapsed() >= timeoutMs)) {
            state = stop.load() ? State::stopped : State::timeout;
            check(TerminateJobObject(job.h, 1), "TerminateJobObject");
            WaitForSingleObject(process.h, INFINITE);
            break;
        }
    }
    job.close();
    drain();
    DWORD status = 0;
    check(GetExitCodeProcess(process.h, &status), "GetExitCodeProcess");
    if (state == State::success && status != 0)
        state = State::error;
    return {state, status, elapsed()};
}
} // namespace run
