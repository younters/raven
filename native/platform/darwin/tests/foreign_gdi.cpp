#include "desktop/foreign_surface.h"
#include <cerrno>
#include <csignal>
#include <iostream>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <chrono>

extern char **environ;
static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
    pid_t child = 0;
    const uint64_t window = raven::window_handle_base() + 1;
    try {
        check(argc == 4, "runtime, fixture and engine required");
        raven::publish_window(window, 64, 48);
        std::vector<std::string> words{argv[1], "--run-process", std::string("--execution=") + argv[3],
            "--timeout-ms=15000", argv[2], "--", std::to_string(window)};
        std::vector<char *> arguments;
        for (auto &word : words) arguments.push_back(word.data());
        arguments.push_back(nullptr);
        check(posix_spawn(&child, argv[1], nullptr, nullptr, arguments.data(), environ) == 0, "cannot start GDI producer");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        uint32_t seen = 0;
        for (unsigned frame = 0; frame < 2; ++frame) {
            bool received = false;
            while (!received && std::chrono::steady_clock::now() < deadline) {
                received = raven::take_foreign_frame(window, seen, [&](const uint8_t *pixels, uint32_t width, uint32_t height) {
                    check(width == (frame ? 80U : 64U) && height == (frame ? 40U : 48U), "GDI frame size differs");
                    for (size_t i = 0; i < size_t(width) * height; ++i)
                        check(pixels[i*4] == (frame ? 0 : 191) && pixels[i*4+1] == (frame ? 32 : 128) &&
                            pixels[i*4+2] == (frame ? 255 : 64), "foreign BitBlt pixels differ");
                });
                if (!received) {
                    int status = 0;
                    if (waitpid(child, &status, WNOHANG) == child) {
                        child = 0;
                        throw std::runtime_error("GDI producer ended before publishing its frame");
                    }
                    usleep(1000);
                }
            }
            check(received, "foreign GDI frame timed out");
            if (!frame) raven::publish_window(window, 80, 40);
            else raven::withdraw_window(window);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0) check(errno == EINTR, "GDI producer wait failed");
        child = 0;
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "GDI producer failed");
        std::cout << "PASS: foreign GetDC/BitBlt/ReleaseDC pixels and resize (" << argv[3] << ")\n";
    } catch (const std::exception &error) {
        if (child > 0) { kill(child, SIGKILL); while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {} }
        raven::withdraw_window(window);
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
