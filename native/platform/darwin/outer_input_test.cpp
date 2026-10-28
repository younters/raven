// The parallel execution wait may poll the desktop before the guest creates its first window. Inject only after presentation;
// the unchanged upstream serial fixture checks the stricter serial ordering.
#include "nt/providers.h"

#include <chrono>
#include <vector>
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
		std::vector<uint8_t> final;
		raven::DesktopPresentation presentation;
		presentation.present = [&](uint64_t handle, std::string_view title, uint32_t width, uint32_t height, std::span<const uint8_t> pixels) {
			if (!handle || (window && window!=handle) || title!="outer surface" || width!=320 || height!=240 || pixels.size()!=320*240*4)
				throw std::runtime_error("outer surface identity differs");
			window=handle; final.assign(pixels.begin(),pixels.end()); ++frames;
		};
		presentation.remove = [&](uint64_t handle) {
			if (handle!=window) throw std::runtime_error("removed wrong outer surface");
			++removed;
		};
		presentation.poll = [&](auto) {
			std::vector<raven::DesktopEvent> events;
			if (window && frames && !injected) {
				injected = true;
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,3,4});
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,19,24});
			}
			return events;
		};
		const auto result = entry.image.is_64 ?
			raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(10), {}, {}, presentation) :
			raven::run_native_process_x86(modules, entry, argv[1], std::chrono::seconds(10), {}, presentation);
		if (result.stop != raven::NativeStop::returned || result.value || !injected || !frames || removed != 1)
			throw std::runtime_error("parallel surface input workflow failed");
		unsigned ink=0;
		for (unsigned y=0;y<240;++y) for (unsigned x=0;x<320;++x) {
			const auto at=(y*320+x)*4;
			if ((x<7 || x>=307 || y<11 || y>=223) && !(final[at]==255 && !final[at+1] && !final[at+2]))
				throw std::runtime_error("client drawing damaged the outer border");
			if (final[at]==255 && final[at+1]==255 && final[at+2]==255) ++ink;
		}
		if (!ink) throw std::runtime_error("client text was not presented");
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
