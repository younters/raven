#include "nt/providers.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and warp-cursor fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0 && file.tellg()<1024*1024,"invalid warp-cursor fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short warp-cursor fixture");
		raven::ModuleSet modules;
		auto &entry=modules.add("warp-cursor.exe",bytes);
		uint64_t window=0;
		std::string title;
		bool focused=false,first=false,second=false,held=false;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t handle,std::string_view name,uint32_t,uint32_t,std::span<const uint8_t>) {
			window=handle; title=name;
		};
		// A one-point constraint is what a host holds the pointer still for.
		presentation.confine=[&](uint64_t,std::optional<std::array<int32_t,4>> rectangle) {
			held=rectangle && (*rectangle)[2]-(*rectangle)[0]==1 && (*rectangle)[3]-(*rectangle)[1]==1;
		};
		presentation.poll=[&](std::chrono::milliseconds) {
			std::vector<raven::DesktopEvent> events;
			if (title=="warp: ready" && !focused) {
				focused=true;
				events.push_back({window,0,0,0,raven::DesktopEventKind::keyboard_focus});
			}
			if (title=="warp: held" && !first) {
				first=true;
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_relative,5,-3});
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,20,20});
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_relative,7,9});
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,20,20});
			}
			if (title=="warp: again" && !second) {
				second=true;
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_relative,-4,6});
				events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,20,20});
			}
			return events;
		};
		const auto result=entry.image.is_64 ?
			raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(5),{},{},presentation) :
			raven::run_native_process_x86(modules,entry,argv[1],std::chrono::seconds(5),{},presentation);
		std::cout<<"Warp-cursor workflow returned "<<result.value<<"\n";
		check(result.stop==raven::NativeStop::returned && !result.value && second && held,"warp-cursor workflow failed");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
