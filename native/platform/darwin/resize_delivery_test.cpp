#include "desktop/windows.h"
#include "nt/providers.h"
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	try {
		auto check=[](bool ok) { if (!ok) throw std::runtime_error("resize delivery contract"); };
		check(argc==2);
		raven::ModuleSet modules;
		const std::array<std::string_view,1> names{"GetLastError"};
		auto &image=modules.add("kernel32.dll",raven::runtime_provider("kernel32.dll",0x180000000ULL,names));
		raven::LoadedImage *images[]{&image.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(10));
		raven::NativeEnvironment environment(process,image.loaded.base(),1,1);
		raven::DesktopObjects objects;
		raven::NativeWindows windows(process,environment,objects);
		const auto atom=objects.register_class({"resize",1,1});
		const auto handle=objects.add(raven::DesktopWindow{atom,1,u"resize",0,0,100,80});
		auto *window=objects.window(handle); window->visible=true;
		std::vector<raven::DesktopEvent> events;
		unsigned acknowledgements=0; uint64_t generation=0;
		raven::DesktopPresentation ui;
		ui.poll=[&](auto) { auto result=std::move(events); events.clear(); return result; };
		ui.resize_complete=[&](uint64_t target,uint64_t sequence,uint32_t width,uint32_t height) {
			check(target==handle && sequence==generation);
			check(objects.window(handle) ? width==100 && height==80 : !width && !height);
			++acknowledgements;
		};
		windows.set_presentation(ui);
		auto enqueue=[&](int x=200,int y=160) {
			events.push_back({handle,0,++generation,0,raven::DesktopEventKind::resize,x,y});
		};
		auto delivered=[&] { check(acknowledgements==generation); };
		enqueue(); windows.input_ready(); delivered(); // No sizing style: veto.
		window->style|=0x40000U;
		enqueue(0,160); windows.input_ready(); delivered(); // Invalid extent.
		window->parent=123;
		enqueue(); windows.input_ready(); delivered(); // No longer top level.
		window->parent=0;
		enqueue(100,80); windows.input_ready(); delivered(); // Already placed.
		windows.set_call([](uint64_t,std::span<const uint64_t>) -> raven::NativeResult { throw std::runtime_error("guest placement failed"); });
		enqueue();
		bool failed=false;
		try { windows.input_ready(); } catch (const std::runtime_error &) { failed=true; }
		check(failed); delivered(); // Exceptions retire the transaction too.
		windows.set_call({});
		enqueue(); windows.queue_status(1,~0U,false); window->visible=false;
		windows.input_ready(); delivered(); // Hidden between collection and delivery.
		raven::ApiRegistry registry; windows.register_api(registry);
		const auto buffer=process.map_memory(4096,PROT_READ|PROT_WRITE);
		enqueue();
		raven::ApiCall call(process,environment,{0,buffer,24});
		registry.find("GetRawInputBuffer")->second(call); delivered(); // Raw-input queue drops hidden targets.
		window->visible=true;
		enqueue(); windows.queue_status(1,~0U,false); objects.erase(handle);
		windows.input_ready(); delivered(); // Destroyed while pending.
		enqueue(); registry.find("GetRawInputBuffer")->second(call); delivered();
		enqueue(); windows.input_ready(); delivered(); // Destroyed before immediate receive.
		process.unmap_memory(buffer);
		windows.shutdown();
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
