// Native bootstrap: no Qt/OCCT DLLs are loaded before selecting an immutable runtime.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

// Shell declarations require the Win32 types above.
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::string value;
    char c;
    while (input.get(c)) {
        if (value.size() >= 1024)
            throw std::runtime_error("Некорректное состояние обновления.");
        value += c;
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}
bool safeVersion(const std::string &v) {
    return v.size() >= 8 && v.size() <= 80 && v.front() >= '0' && v.front() <= '9' &&
           v.find("..") == v.npos && std::all_of(v.begin(), v.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '-';
           });
}
bool safeToken(const std::string &v) {
    return v.size() == 32 && std::all_of(v.begin(), v.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
void atomicWrite(const fs::path &path, const std::string &text) {
    const auto temporary = fs::path(path.native() + fs::path(".new").native());
    if (fs::is_symlink(path) || fs::is_symlink(temporary))
        throw std::runtime_error("Небезопасный путь служебного файла обновления.");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << text << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Не удалось записать состояние обновления.");
    }
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Не удалось сохранить состояние обновления на диск.");
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!flushed || !MoveFileExW(temporary.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Не удалось применить состояние обновления.");
#else
    const int descriptor = open(temporary.c_str(), O_RDWR | O_CLOEXEC);
    if (descriptor < 0 || fsync(descriptor) != 0) {
        if (descriptor >= 0)
            close(descriptor);
        throw std::runtime_error("Не удалось сохранить состояние обновления на диск.");
    }
    close(descriptor);
    fs::rename(temporary, path);
    const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        (void)fsync(directory);
        close(directory);
    }
#endif
}
class Lock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED range{};
#else
    int handle = -1;
#endif
  public:
    explicit Lock(const fs::path &root) {
        if (fs::is_symlink(root / ".updates"))
            throw std::runtime_error("Небезопасная папка обновления.");
        fs::create_directories(root / ".updates");
        const auto file = root / ".updates/install.lock";
        if (fs::is_symlink(file))
            throw std::runtime_error("Небезопасный файл блокировки обновления.");
#ifdef _WIN32
        handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE ||
            !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                        &range)) {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "Уже выполняется обновление. Повторите запуск после его завершения.");
        }
#else
        handle = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (handle < 0 || flock(handle, LOCK_EX | LOCK_NB) != 0) {
            if (handle >= 0)
                close(handle);
            handle = -1;
            throw std::runtime_error(
                "Уже выполняется обновление. Повторите запуск после его завершения.");
        }
#endif
    }
    ~Lock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            UnlockFileEx(handle, 0, 1, 0, &range);
            CloseHandle(handle);
        }
#else
        if (handle >= 0) {
            (void)flock(handle, LOCK_UN);
            close(handle);
        }
#endif
    }
};
bool ready(const fs::path &root, const std::string &version) {
    if (!safeVersion(version) || fs::is_symlink(root / "runtime") ||
        fs::is_symlink(root / "runtime" / version))
        return false;
    const auto directory = root / "runtime" / version;
    return read(directory / ".ready") == version && read(directory / "VERSION") == version &&
           fs::is_regular_file(directory / "bin/3d-bending-simulator.exe") &&
           !fs::is_symlink(directory / "bin") &&
           !fs::is_symlink(directory / "bin/3d-bending-simulator.exe");
}
#ifdef _WIN32
std::wstring wide(const std::string &s) {
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    if (count <= 0 && !s.empty())
        throw std::runtime_error("Не удалось прочитать аргумент запуска.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        result.data(), count);
    return result;
}
std::string utf8(const std::wstring &s) {
    const auto count = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}
std::wstring quote(const std::wstring &s) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
using Process = HANDLE;
Process launch(const fs::path &exe, const std::vector<std::string> &arguments,
               const fs::path &root) {
    std::wstring command = quote(exe.native());
    for (const auto &arg : arguments)
        command += L" " + quote(wide(arg));
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    if (GetStdHandle(STD_OUTPUT_HANDLE) &&
        GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE && GetStdHandle(STD_ERROR_HANDLE) &&
        GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        root.c_str(), &startup, &process))
        throw std::runtime_error("Не удалось запустить программу. Предыдущая версия сохранена.");
    CloseHandle(process.hThread);
    return process.hProcess;
}
bool exited(Process p) {
    return WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
}
void release(Process p) {
    CloseHandle(p);
}
int wait(Process p) {
    WaitForSingleObject(p, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p, &code);
    release(p);
    return static_cast<int>(code);
}
void waitForParent(unsigned long pid) {
    if (!pid)
        return;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return;
        throw std::runtime_error(
            "Не удалось дождаться закрытия программы. Повторите запуск через главный ярлык.");
    }
    const auto result = WaitForSingleObject(process, 120000);
    CloseHandle(process);
    if (result != WAIT_OBJECT_0)
        throw std::runtime_error("Программа ещё работает. Обновление отменено.");
}
void message(const std::string &text) {
    const auto output = GetStdHandle(STD_ERROR_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE && GetFileType(output) != FILE_TYPE_UNKNOWN) {
        DWORD written = 0;
        const auto line = text + '\n';
        if (GetFileType(output) == FILE_TYPE_CHAR) {
            const auto value = wide(line);
            WriteConsoleW(output, value.data(), static_cast<DWORD>(value.size()), &written,
                          nullptr);
        } else
            WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    MessageBoxW(nullptr, wide(text).c_str(), L"3D Bending Simulator", MB_OK | MB_ICONINFORMATION);
}
fs::path executablePath() {
    std::wstring buffer(32768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        throw std::runtime_error("Не удалось определить папку программы.");
    buffer.resize(length);
    return fs::path(buffer);
}
#else
using Process = pid_t;
Process launch(const fs::path &exe, const std::vector<std::string> &arguments,
               const fs::path &root) {
    const auto process = fork();
    if (process < 0)
        throw std::runtime_error("Не удалось запустить программу.");
    if (!process) {
        if (chdir(root.c_str()) != 0)
            _exit(127);
        std::vector<std::string> strings{exe.string()};
        strings.insert(strings.end(), arguments.begin(), arguments.end());
        std::vector<char *> argv;
        for (auto &s : strings)
            argv.push_back(s.data());
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
        _exit(127);
    }
    return process;
}
bool exited(Process p) {
    int status = 0;
    return waitpid(p, &status, WNOHANG) == p;
}
void release(Process) {}
int wait(Process p) {
    int status = 0;
    waitpid(p, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
void waitForParent(unsigned long pid) {
    const auto end = std::chrono::steady_clock::now() + 120s;
    while (pid && kill(static_cast<pid_t>(pid), 0) == 0) {
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error("Программа ещё работает. Обновление отменено.");
        std::this_thread::sleep_for(100ms);
    }
}
void message(const std::string &text) {
    std::cerr << text << '\n';
}
fs::path executablePath() {
    return fs::read_symlink("/proc/self/exe");
}
#endif
void recover(const fs::path &root) {
    const auto journal = root / ".updates/pending.txt";
    if (!fs::exists(journal))
        return;
    const auto text = read(journal);
    const auto first = text.find('\n'),
               second = text.find('\n', first == text.npos ? first : first + 1);
    const auto old = text.substr(0, first);
    const auto next = first == text.npos ? "" : text.substr(first + 1, second - first - 1);
    const auto token = second == text.npos ? "" : text.substr(second + 1);
    if (!ready(root, old) || !safeVersion(next) || !safeToken(token))
        throw std::runtime_error(
            "Не удалось восстановить состояние обновления. Предыдущая версия и данные сохранены.");
    const auto current = read(root / "current.txt");
    if (current != old && current != next)
        throw std::runtime_error("Состояние обновления изменилось. Файлы программы не заменены.");
    const auto ack = root / ".updates" / ("ack-" + token);
    const bool confirmed = current == next && ready(root, next) && read(ack) == "ready";
    atomicWrite(root / "current.txt", confirmed ? next : old);
    fs::remove(journal);
    fs::remove(ack);
}
int run(std::vector<std::string> args) {
    fs::path root = executablePath().parent_path();
    std::string next, from, token, reopen;
    unsigned long parent = 0;
    std::vector<std::string> forward;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto &arg = args[i];
        const auto value = [&]() -> std::string {
            if (++i >= args.size())
                throw std::runtime_error("Не указан параметр запуска.");
            return args[i];
        };
        if (arg == "--root") {
            const auto text = value();
            root = fs::path(
                std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
        } else if (arg == "--activate")
            next = value();
        else if (arg == "--from")
            from = value();
        else if (arg == "--token")
            token = value();
        else if (arg == "--reopen")
            reopen = value();
        else if (arg == "--wait-pid")
            parent = std::stoul(value());
        else
            forward.push_back(arg);
    }
    root = fs::canonical(root);
    if (read(root / "portable-format.txt") != "1")
        throw std::runtime_error("Эта папка не содержит поддерживаемый portable-комплект.");
    if (!next.empty())
        waitForParent(parent);
    Lock lock(root);
    recover(root);
    const auto current = read(root / "current.txt");
    if (!ready(root, current))
        throw std::runtime_error("Текущая версия неполна. Распакуйте portable ZIP в новую папку.");
    if (next.empty()) {
        const auto child =
            launch(root / "runtime" / current / "bin/3d-bending-simulator.exe", forward, root);
        const bool synchronous = std::any_of(forward.begin(), forward.end(), [](const auto &s) {
            return s == "--smoke-test" || s == "--version" || s == "--update-runtime-probe";
        });
        if (synchronous)
            return wait(child);
        release(child);
        return 0;
    }
    if (!safeToken(token) || !ready(root, next) || current != from || next == current)
        throw std::runtime_error("Комплект изменился или новая версия не готова. Запустите "
                                 "программу через главный ярлык и повторите проверку.");
    atomicWrite(root / ".updates/pending.txt", current + '\n' + next + '\n' + token);
    atomicWrite(root / "current.txt", next);
    std::vector<std::string> startup{
        "--update-token", token, "--portable-root",
        root.u8string().empty()
            ? ""
            : std::string(reinterpret_cast<const char *>(root.u8string().c_str()))};
    if (!reopen.empty()) {
        startup.push_back("--");
        startup.push_back(reopen);
    }
    try {
        const auto child =
            launch(root / "runtime" / next / "bin/3d-bending-simulator.exe", startup, root);
        const auto end = std::chrono::steady_clock::now() + 30s;
        bool ended = false;
        const auto ack = root / ".updates" / ("ack-" + token);
        while (std::chrono::steady_clock::now() < end && read(ack) != "ready") {
            if (exited(child)) {
                ended = true;
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
        if (read(ack) == "ready") {
            fs::remove(root / ".updates/pending.txt");
            fs::remove(ack);
            release(child);
            return 0;
        }
        recover(root);
        release(child);
        if (ended) {
            std::vector<std::string> oldArgs;
            if (!reopen.empty()) {
                oldArgs.push_back("--");
                oldArgs.push_back(reopen);
            }
            release(
                launch(root / "runtime" / current / "bin/3d-bending-simulator.exe", oldArgs, root));
        }
        message("Новая версия не подтвердила успешный запуск. Для следующего запуска восстановлена "
                "предыдущая версия. Проекты и настройки сохранены.");
        return 1;
    } catch (...) {
        recover(root);
        std::vector<std::string> oldArgs;
        if (!reopen.empty()) {
            oldArgs.push_back("--");
            oldArgs.push_back(reopen);
        }
        release(launch(root / "runtime" / current / "bin/3d-bending-simulator.exe", oldArgs, root));
        throw;
    }
}
} // namespace
#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto **wideArgs = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::string> args;
    for (int i = 0; i < count; ++i)
        args.push_back(utf8(wideArgs[i]));
    LocalFree(wideArgs);
#else
int main(int argc, char **argv) {
    std::vector<std::string> args(argv, argv + argc);
#endif
    try {
        return run(std::move(args));
    } catch (const std::exception &e) {
        message(e.what());
        return 1;
    }
}
