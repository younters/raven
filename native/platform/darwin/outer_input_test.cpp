// The parallel execution wait may poll the desktop before the guest creates its first window. Inject only after presentation;
// the unchanged upstream serial fixture checks the stricter serial ordering.
#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv)
{
    try {
        if (argc != 3) throw std::runtime_error("bootstrap and input fixture required");
        std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0) throw std::runtime_error("cannot read input fixture");
        std::vector<uint8_t> bytes(size_t(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())))
            throw std::runtime_error("short input fixture");
        raven::ModuleSet modules;
        auto &entry = modules.add("outer-surface.exe", bytes);
        uint64_t window = 0;
        unsigned frames = 0, removed = 0;
        bool injected = false;
        raven::DesktopPresentation presentation;
        presentation.present = [&](uint64_t handle, auto, auto, auto, auto) { window = handle; ++frames; };
        presentation.remove = [&](uint64_t handle) { if (handle == window) ++removed; };
        presentation.poll = [&](auto) {
            std::vector<raven::DesktopEvent> events;
            if (window && frames && !injected) {
                injected = true;
                events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,3,4});
                events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,19,24});
            }
            return events;
        };
        const auto result = raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(10), {}, {}, presentation);
        if (result.stop != raven::NativeStop::returned || result.value || !injected || !frames || removed != 1)
            throw std::runtime_error("parallel surface input workflow failed");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
