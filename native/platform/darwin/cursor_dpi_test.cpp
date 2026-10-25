#include "nt/providers.h"
#include "nt/actctx.h"
#include "desktop/objects.h"
#include "pe/resources.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value,const char *message) { if (!value) throw std::runtime_error(message); }

int main(int argc,char **argv)
{
	try {
		check(argc==5,"bootstrap and aware/unaware/per-monitor fixtures required");
		const auto manifest=raven::activation_contexts().parse(
			"<assembly><dpiAware>true/pm</dpiAware><x:dpiAwareness>Unknown, PerMonitorV2, System</x:dpiAwareness></assembly>",false);
		check(manifest && manifest->dpi_context==-4,"manifest DPI preference differs");
		const auto unaware=raven::activation_contexts().parse(
			"<assembly><dpiAwareness>Unaware</dpiAwareness><dpiAware>true</dpiAware></assembly>",false);
		check(unaware && unaware->dpi_context==-1,"modern manifest did not override legacy DPI setting");
		for (int fixture=2;fixture<5;++fixture) {
			std::ifstream file(argv[fixture],std::ios::binary | std::ios::ate);
			check(bool(file) && file.tellg()>0,"fixture unavailable");
			std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
			check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short fixture");
			raven::ModuleSet modules; auto &entry=modules.add("cursor-dpi.exe",bytes);
			std::string program_manifest;
			for (const auto &resource : entry.image.resources())
				if (resource.type==raven::ResourceKey(uint32_t(24)) && resource.name==raven::ResourceKey(uint32_t(1))) {
					const auto data=entry.image.file_span(resource.data_address,resource.size);
					program_manifest.assign(reinterpret_cast<const char *>(data.data()),data.size());
				}
			raven::activation_contexts().prepare({},true,program_manifest);
			raven::DesktopPresentation backend;
			double scale=0;
			uint64_t window=0,last_cursor=0;
			unsigned injected=0,custom=0,hidden=0,ibeam=0,hand=0;
			auto next_input=std::chrono::steady_clock::time_point{};
			backend.backing_scale=[] { return 2.0; };
			backend.render_scale=[&](double value) { scale=value; };
			backend.screen_size=[&] { return std::optional(std::pair{int32_t(1000*scale),int32_t(800*scale)}); };
			backend.present=[&](uint64_t handle,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { window=handle; };
			backend.remove=[](uint64_t) {};
			backend.cursor=[&](uint64_t handle,int count,const raven::DesktopIcon *image) {
				if (image) {
					const auto width=image->image.color.width,height=image->image.color.height;
					check(image->cursor && image->hotspot_x==width/2 && image->hotspot_y==height/2 && image->image.and_mask[size_t(height/2)*width],"custom cursor data differs");
					++custom;
				}
				if (!handle || count<0) ++hidden;
				if (handle==raven::desktop_stock_cursor+1) ++ibeam;
				if (handle==raven::desktop_stock_cursor+137) ++hand;
				last_cursor=handle;
			};
			backend.poll=[&](std::chrono::milliseconds) {
				std::vector<raven::DesktopEvent> events;
				if (window && std::chrono::steady_clock::now()>=next_input && injected<3) {
					++injected;
					next_input=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
					events.push_back({window,0,0,0,raven::DesktopEventKind::pointer_motion,32,100});
				}
				return events;
			};
			const auto result=raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(15),{},{},backend);
			const bool completed=result.stop==raven::NativeStop::returned || result.stop==raven::NativeStop::exited;
			if (!completed || result.value) std::cerr<<"fixture "<<fixture<<" stop "<<int(result.stop)<<" value "<<result.value<<" input "<<injected<<" beam "<<ibeam<<" hand "<<hand<<'\n';
			check(completed && result.value==0,"cursor/DPI guest workflow failed");
			check(scale==(fixture==3 ? 1 : 2) && custom>=1 && hidden>=3 && ibeam==1 && hand==1 && last_cursor==raven::desktop_stock_cursor+137,
				"cursor presentation or aware/unaware scale differs");
		}
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
