#include "windows.h"
#include "api.h"
#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==3,"bootstrap and nonclient fixture required");
		std::ifstream file(argv[2],std::ios::binary | std::ios::ate);
		check(bool(file) && file.tellg()>0,"cannot read nonclient fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg())); file.seekg(0);
		check(bool(file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))),"short nonclient fixture");
		raven::ModuleSet modules; auto &entry=modules.add("client.exe",bytes);
		auto &user=modules.add("user32.dll",raven::runtime_provider("user32.dll",0x180000000ULL,raven::NativeDesktop::user_exports()));
		raven::LoadedImage *images[]={&entry.loaded,&user.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(3));
		for (const auto &symbol:user.exports) process.add_api_trap(raven::api_trap_address(user,"user32.dll",symbol.names.at(0),symbol.address));
		raven::NativeEnvironment environment(process,entry.loaded.base(),1,1);
		raven::DesktopMetrics metrics; metrics.caption=30; metrics.frame_x=9;
		raven::DesktopObjects objects(256*1024*1024,{48,16,32},metrics),defaults;
		raven::NativeWindows windows(process,environment,objects),other(process,environment,defaults);
		const auto call=[&](raven::NativeWindows &owner,const char *name,std::initializer_list<uint64_t> args) {
			const auto function=modules.resolve({"user32.dll",name,0,false,0});
			check(process.call(function,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api,"nonclient API did not trap");
			check(owner.dispatch(name),"nonclient API not dispatched");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"nonclient API did not return"); return result.value;
		};
		check(call(windows,"GetSystemMetrics",{4})==30 && call(other,"GetSystemMetrics",{4})==26 &&
			call(windows,"GetSystemMetrics",{32})==9,"metric owners shared global state");
		check(call(windows,"GetSystemMetrics",{11})==48 && call(windows,"GetSystemMetrics",{12})==16 &&
			call(other,"GetSystemMetrics",{11})==32 && call(other,"GetSystemMetrics",{12})==32,"icon metrics ignored the display owner");
		raven::DesktopPresentation presentation;
		presentation.screen_size=[] { return std::optional(std::pair<int32_t,int32_t>{1280,720}); };
		windows.set_presentation(presentation);
		check(call(windows,"GetSystemMetrics",{0})==1280 && call(windows,"GetSystemMetrics",{1})==720 &&
			call(other,"GetSystemMetrics",{0})==1920 && call(other,"GetSystemMetrics",{1})==1080,"host screen dimensions escaped their owner");
		for (const auto [width,height] : {std::pair{0,720}, {-1,720}, {1280,0}, {1280,-1}, {8193,720}, {1280,8193}}) {
			auto invalid=presentation;
			invalid.screen_size=[=] { return std::optional(std::pair{width,height}); };
			bool rejected=false;
			try { windows.set_presentation(invalid); } catch (const std::runtime_error &) { rejected=true; }
			check(rejected && call(windows,"GetSystemMetrics",{0})==1280 && call(windows,"GetSystemMetrics",{1})==720,
				"invalid host dimensions changed screen metrics");
		}
		windows.set_presentation(presentation);
		{
			const auto owner=objects.add(raven::DesktopWindow{0,0,u"owner",0,0,4,4});
			objects.window(owner)->visible=true;
			const auto popup=objects.add(raven::DesktopWindow{0,0,u"popup",0,0,4,4});
			auto *window=objects.window(popup);
			window->owner=owner;
			window->visible=window->painted=true;
			window->style=0x90000000U;
			window->glass=std::array<int32_t,4>{-1,-1,-1,-1};
			std::vector<uint8_t> pixels(4*4*4,255);
			pixels[0]=pixels[1]=pixels[2]=pixels[3]=0;
			pixels[4]=16; pixels[5]=32; pixels[6]=48; pixels[7]=64;
			pixels[8]=pixels[9]=pixels[10]=0; pixels[11]=255;
			const auto bitmap=objects.add(raven::DesktopBitmap{4,4,32,pixels});
			window=objects.window(popup);
			window->bitmap=bitmap;
			window->dc=objects.add(raven::DesktopDC{bitmap,bitmap,popup});
			bool alpha=false;
			unsigned frames=0;
			auto glass=presentation;
			glass.present=[](auto,auto,auto,auto,auto) {};
			glass.alpha=[&](uint64_t handle,bool enabled) { check(handle==popup,"wrong alpha surface"); alpha=enabled; };
			glass.owned_popup=[&](uint64_t handle,uint64_t parent,auto,auto,auto,auto,std::span<const uint8_t> shown) {
				check(handle==popup && parent==owner && alpha==(frames==0),"glass popup alpha mode differs");
				check(std::equal(shown.begin(),shown.end(),pixels.begin(),pixels.end()),"glass popup lost transparent or opaque black pixels");
				++frames;
			};
			windows.set_presentation(glass);
			windows.drawing(popup); windows.flush_dirty(true);
			objects.window(popup)->glass.reset();
			windows.drawing(popup); windows.flush_dirty(true);
			check(frames==2,"glass popup was not presented");
			windows.set_presentation(presentation);
		}
		{
			const auto handle=objects.add(raven::DesktopWindow{0,0,u"client caption",0,0,240,80});
			auto *window=objects.window(handle);
			window->visible=window->painted=true;
			window->style=0x10cf0000U;
			window->glass=std::array<int32_t,4>{1,1,1,1};
			std::vector<uint8_t> pixels(240*80*4,64),shown;
			window->bitmap=objects.add(raven::DesktopBitmap{240,80,32,pixels});
			window->dc=objects.add(raven::DesktopDC{window->bitmap,window->bitmap,handle});
			auto glass=presentation;
			glass.present=[&](auto,auto,auto,auto,std::span<const uint8_t> image) { shown.assign(image.begin(),image.end()); };
			windows.set_presentation(glass);
			windows.drawing(handle); windows.flush_dirty(true);
			check(shown==pixels,"narrow glass border drew over the client's caption buttons");
			objects.window(handle)->glass=std::array<int32_t,4>{-1,-1,-1,-1};
			windows.drawing(handle); windows.flush_dirty(true);
			check(shown!=pixels,"fully extended caption lost compositor buttons");
			objects.set_dpi(192);
			windows.drawing(handle); windows.flush_dirty(true);
			int32_t first=240,last=-1;
			for (int32_t y=0;y<80;++y)
				for (int32_t x=140;x<232;++x)
					if (shown[(size_t(y)*240+x)*4]!=64) { first=std::min(first,x); last=std::max(last,x); }
			check(last-first>=19 && last-first<=23,"192-DPI close glyph did not scale with its button");
			objects.set_dpi(96);
			objects.window(handle)->visible=false;
			windows.set_presentation(presentation);
		}
		const auto page=process.map_memory(8192,PROT_READ | PROT_WRITE); process.protect_memory(page+4096,4096,PROT_NONE);
		const auto rect=page+4096-16;
		const std::array<uint8_t,16> input{0,0,0,0,0,0,0,0,64,1,0,0,240,0,0,0}; process.write_memory(rect,input);
		environment.set_last_error(71);
		check(call(windows,"AdjustWindowRectEx",{rect,0xcf0000,0,0})==1 && environment.last_error()==71,"configured rectangle adjustment failed");
		const auto output=process.read_memory(rect,16);
		const int32_t expected[]={-9,-34,329,244};
		for (unsigned i=0;i<4;++i) {
			uint32_t value=0; for (unsigned j=0;j<4;++j) value|=uint32_t(output[i*4+j])<<(j*8);
			check(int32_t(value)==expected[i],"adjustment ignored its owning metrics");
		}
		process.protect_memory(page,4096,PROT_READ);
		check(!call(windows,"AdjustWindowRect",{rect,0xcf0000,0}) && environment.last_error()==998 && process.read_memory(rect,16)==output,
			"readonly rectangle was modified");
		check(!call(windows,"AdjustWindowRectEx",{rect+4,0xcf0000,0,0}) && environment.last_error()==998 && process.read_memory(rect,16)==output,
			"guard-page rectangle was partially modified");
		metrics.frame_x=2; bool rejected=false;
		try { raven::DesktopObjects invalid(256*1024*1024,{},metrics); } catch (const std::runtime_error &) { rejected=true; }
		check(rejected,"inconsistent frame metrics accepted");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
