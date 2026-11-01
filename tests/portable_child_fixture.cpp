#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
int run(const std::vector<std::string> &args) {
    const auto root = fs::current_path();
    std::string version;
    {
        std::ifstream in(root / "current.txt");
        std::getline(in, version);
    }
    {
        std::ofstream out(root / "arguments.txt", std::ios::binary);
        for (const auto &arg : args)
            out << arg << '\n';
    }
    std::string token;
    for (std::size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "--update-token")
            token = args[i + 1];
    for (const auto &arg : args) {
        if (arg == "--version") {
            std::cout << version << '\n';
            return 0;
        }
        if (arg == "--hold") {
            while (!fs::exists(root / "end-hold"))
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 0;
        }
    }
    if (!token.empty()) {
        std::string mode;
        {
            std::ifstream in(root / "runtime" / version / "fixture-mode");
            std::getline(in, mode);
        }
        if (mode == "fail")
            return 17;
        if (mode == "delay")
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        {
            std::ofstream ack(root / ".updates" / ("ack-" + token));
            ack << "ready\n";
        }
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const std::wstring value(argv[i]);
        const auto n =
            WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr,
                                0, nullptr, nullptr);
        std::string text(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), text.data(),
                            n, nullptr, nullptr);
        args.push_back(std::move(text));
    }
    return run(args);
}
#else
int main(int argc, char **argv) {
    return run({argv, argv + argc});
}
#endif
