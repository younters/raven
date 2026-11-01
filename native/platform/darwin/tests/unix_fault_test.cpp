#include "cpu/native_cpu.h"
#include "../fex/fex_process.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

struct Request { uint64_t gate, handle, args; uint32_t code; };
struct Args { uint64_t pointer, length; uint32_t mode, observed; };
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main(int argc, char **argv) {
    if (argc != 7 && argc != 8) return 2;
    try {
        raven::select_fex_execution(std::string(argv[6]) == "fex");
        const bool x86 = std::string(argv[4]) == "32";
        const std::string backend = argv[5];
        const bool fake = backend == "fault";
        const bool metal = backend == "dxmt" || backend == "mullion";
        std::ifstream input(argv[2], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        raven::PEImage image(bytes);
        if (!x86 && std::string(argv[6]) == "fex")
            require(raven::fex_reserve_image_address(image.image_base, image.image_size),
                "cannot reserve unix-call fixture image before host allocations");
        raven::LoadedImage loaded(image, image.image_base);
        raven::LoadedImage *images[] = {&loaded};
        raven::NativeProcess process(images, argv[1], std::chrono::seconds(10), x86 ? raven::NativeMode::x86 : raven::NativeMode::x64);
        const std::string root = argv[3];
        setenv("RAVEN_MTLD3D_ARM64_ROOT", root.c_str(), 1);
        const uint32_t count = metal ? (backend == "mullion" ? 197 : 146) : 22;
        const auto path = root + (metal ? "/winemetal.so" : "/mtld3d.so");
        if (fake) {
            setenv("RAVEN_MTLD3D_ARM64_ROOT", (root + "/mismatch").c_str(), 1);
            bool rejected = false;
            try { process.load_unix_library(root + "/mismatch/mtld3d.so", count); }
            catch (const std::runtime_error &) { rejected = true; }
            require(rejected, "compiled table length did not reject the forged sidecar count");
            setenv("RAVEN_MTLD3D_ARM64_ROOT", root.c_str(), 1);
        }
        void *event_before = dlsym(RTLD_DEFAULT, "NtSetEvent");
        void *layer_before = dlsym(RTLD_DEFAULT, "raven_mtld3d_layer_create");
        const auto handle = process.load_unix_library(path, count);
        if (std::string(argv[6]) == "fex") {
            require(dlsym(RTLD_DEFAULT, "NtSetEvent") == event_before, "adapter leaked NtSetEvent globally");
            require(dlsym(RTLD_DEFAULT, "raven_mtld3d_layer_create") == layer_before, "mtld3d adapter leaked globally");
        }
        const auto memory = process.map_memory(32768, PROT_READ | PROT_WRITE);
        process.protect_memory(memory + 16384, 16384, PROT_NONE);
        const auto data = memory + 256;
        Request request{process.unix_call_gate(), handle, data, metal ? 8u : fake ? 0u : 10u};
        auto write = [&](uint64_t at, const auto &object) { process.write_memory(at, {reinterpret_cast<const uint8_t *>(&object), sizeof object}); };
        auto invoke = [&](uint64_t arguments, uint32_t code) {
            request.args = arguments; request.code = code; write(memory, request);
            const uint64_t args[] = {memory};
            auto result = process.call(loaded.base() + image.entry, args, std::chrono::seconds(5));
            require(result.stop == raven::NativeStop::returned, "unix call killed/stopped the guest instead of returning");
            return uint32_t(result.value);
        };
        const uint32_t code = request.code;
        auto nested = [&](uint64_t pointer, uint64_t length) {
            if (metal) { const uint64_t args[] = {0, pointer, length, 0}; process.write_memory(data, {reinterpret_cast<const uint8_t *>(args), sizeof args}); }
            else { Args args{pointer, length, fake ? 1u : 0u, 0}; write(data, args); }
            return invoke(data, code);
        };
        constexpr uint32_t av = 0xc0000005;
        if (backend == "mtld3d") {
            const uint64_t logger[5]{};
            process.write_memory(data, {reinterpret_cast<const uint8_t *>(logger), sizeof logger});
            require(invoke(data, 0) == 0, "mtld3d logger initialization failed");
            if (argc == 8) {
                // objc_release is outside the unix image. A declined fault there
                // must terminate rather than skip foreign runtime lock frames.
                const uint64_t object = 1;
                write(memory + 1024, object);
                const uint64_t destroy[] = {0, memory + 1024, 1};
                process.write_memory(data, {reinterpret_cast<const uint8_t *>(destroy), sizeof destroy});
                invoke(data, 9);
                throw std::runtime_error("foreign objc_release fault was incorrectly recovered");
            }
            char *child_args[9]{};
            for (unsigned i=0;i<7;++i) child_args[i]=argv[i];
            child_args[7]=const_cast<char *>("--foreign-release");
            pid_t child;
            require(!posix_spawn(&child,argv[0],nullptr,nullptr,child_args,environ), "cannot spawn foreign-fault control");
            int status=0;
            require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==139,
                "foreign runtime fault did not terminate diagnostically");
        }
        require(invoke(1, code) == av, "bad top-level pointer was accepted");
        require(invoke(memory + 16384 - 8, code) == av, "partial argument block was accepted");
        require(nested(1, 16) == av, "bad nested pointer was accepted");
        require(nested(x86 ? UINT32_MAX - 3ull : UINT64_MAX - 3ull, 8) == av, "nested range overflow was accepted");
        require(nested(memory + 16384 - 4, 8) == av, "nested range crossed PROT_NONE");
        process.protect_memory(memory + 4096, 4096, PROT_NONE);
        require(nested(memory + 4096 - 2, 4) == av, "nested range crossed a logical 4K PROT_NONE page");
        if (metal) {
            const uint64_t edr[] = {0, 1};
            process.write_memory(data, {reinterpret_cast<const uint8_t *>(edr), sizeof edr});
            require(invoke(data, 97) == av, "wrapped EDR output pointer was accepted");
        }
        if (backend == "mtld3d") {
            // CreateShaderProgram: tokens and semantics are separately bounded nested arrays.
            uint8_t shader[72]{};
            uint64_t bad = 1;
            uint32_t one = 1;
            memcpy(shader + 8, &bad, 8); memcpy(shader + 28, &one, 4);
            process.write_memory(data, {shader, sizeof shader});
            require(invoke(data, 19) == av, "invalid shader token array was accepted");
            uint64_t tail = memory + 16384 - 2;
            memcpy(shader + 8, &tail, 8);
            process.write_memory(data, {shader, sizeof shader});
            require(invoke(data, 19) == av, "shader token extent crossed PROT_NONE");
            memset(shader, 0, sizeof shader);
            memcpy(shader + 16, &bad, 8); memcpy(shader + 32, &one, 4);
            process.write_memory(data, {shader, sizeof shader});
            require(invoke(data, 19) == av, "invalid shader output array was accepted");
        }
        if (fake) {
            Args fault{1, 1, 1, 0}; write(data, fault);
            require(invoke(data, 1) == av, "native fault was not contained");
            // Repeat recovery to catch a stale sigjmp frame or masked SIGSEGV.
            require(invoke(data, 1) == av, "second native fault was not contained");
            Args valid{}; write(data, valid);
            require(invoke(data, 0) == 0, "host did not survive native fault");
        } else require(nested(memory + 1024, 0) == 0, "host did not survive rejected pointers");
        std::cout << backend << " PE" << (x86 ? 32 : 64) << ": bad top-level, partial block, nested, PROT_NONE span and survival passed\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
