#include "api.h"
#include "nt/providers.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sstream>
#endif

static void check(bool value,const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc,char **argv)
{
	try {
		check(argc==2,"bootstrap required");
		raven::ModuleSet modules;
		auto &module=modules.add("gdi32.dll",raven::runtime_provider("gdi32.dll",0x180000000,raven::NativeDesktop::gdi_exports()));
		raven::LoadedImage *images[]{&module.loaded};
		raven::NativeProcess process(images,argv[1],std::chrono::seconds(2));
		for (const auto &symbol : module.exports)
			if (!symbol.names.empty() && symbol.forwarder.empty())
				process.add_api_trap(raven::api_trap_address(module,"gdi32.dll",symbol.names.at(0),symbol.address));
		raven::NativeEnvironment environment(process,module.loaded.base(),1,1);
		raven::NativeResources resources(process,environment,modules,module);
		raven::DesktopObjects objects(4096+sizeof(raven::DesktopDC));
		raven::NativeDesktop desktop(process,environment,objects,resources);
		const auto call=[&](const char *name,std::initializer_list<uint64_t> args) {
			const auto address=modules.resolve({"gdi32.dll",name,0,false,0});
			check(process.call(address,std::span(args.begin(),args.size()),std::chrono::seconds(1)).stop==raven::NativeStop::api,"bitmap API did not trap");
			check(desktop.dispatch(name),"bitmap API not dispatched");
			const auto result=process.resume(std::chrono::seconds(1));
			check(result.stop==raven::NativeStop::returned,"bitmap API lost caller"); return result.value;
		};
		const auto dc=call("CreateCompatibleDC",{0});
		const auto address=process.map_memory(4096,PROT_READ | PROT_WRITE);
		std::vector<uint8_t> input(24,0x53); process.write_memory(address,input);
		const auto source=objects.add(raven::DesktopBitmap{3,2,32,{},address,true,false,true});
		check(dc && source && call("SelectObject",{dc,source})==0x10000,"cannot establish budget source");
#ifdef __APPLE__
		pid_t children[2]{};
		proc_listchildpids(getpid(), children, sizeof children);
		const pid_t child = children[0];
		check(child > 0 && !children[1], "owned bootstrap child is ambiguous");
		const auto mappings = [&]() {
			std::ostringstream result;
			uint64_t cursor = 0;
			proc_regioninfo region{};
			while (proc_pidinfo(child, PROC_PIDREGIONINFO, cursor, &region, sizeof region) == sizeof region) {
				result << region.pri_address << ' ' << region.pri_size << ' ' << region.pri_protection << '\n';
				if (!region.pri_size || region.pri_address > UINT64_MAX - region.pri_size)
					break;
				cursor = region.pri_address + region.pri_size;
			}
			check(!result.str().empty(), "cannot inspect owned guest mappings");
			return result.str();
		};
#else
		// The guest is started by the thread that traces it, which need not be the main one, and kernels without the per-task
		// children lists still give every process its parent.
		std::vector<long> owned;
		for (const auto &entry:std::filesystem::directory_iterator("/proc")) {
			const auto name=entry.path().filename().string();
			if (name.empty() || name.find_first_not_of("0123456789")!=name.npos) continue;
			std::ifstream stat(entry.path()/"stat"); std::string line; std::getline(stat,line);
			const auto close=line.rfind(')'); if (close==line.npos) continue;
			std::istringstream fields(line.substr(close+2)); char state=0; long parent=0;
			if (fields>>state>>parent && parent==getpid()) owned.push_back(std::stol(name));
		}
		check(owned.size()==1,"owned bootstrap child is ambiguous");
		const long child=owned.front();
		const auto mappings=[&]() {
			std::ifstream file("/proc/"+std::to_string(child)+"/maps"); check(bool(file),"cannot inspect owned guest mappings");
			return std::string(std::istreambuf_iterator<char>(file),{});
		};
#endif
#ifdef __APPLE__
		// Rosetta can map translation storage the first time this entry point runs; that belongs to the translator, not the bitmap.
		check(!call("CreateCompatibleBitmap",{dc,3,2}) && environment.last_error()==8,"bitmap budget did not reject allocation");
#endif
		const auto before=mappings(); const auto budget=objects.bytes();
		for (unsigned attempt=0;attempt<16;++attempt) {
			environment.set_last_error(0x7654);
			check(!call("CreateCompatibleBitmap",{dc,3,2}) && environment.last_error()==8,"bitmap budget did not reject allocation");
			check(objects.count()==2 && objects.bytes()==budget && mappings()==before && process.read_memory(address,input.size())==input,"failed bitmap allocation leaked mapping, resource or source data");
		}
		check(!call("DeleteObject",{source}) && objects.bitmap(source),"selected bitmap deleted prematurely");
		check(call("SelectObject",{dc,0x10000})==source && call("DeleteObject",{source}) && !objects.bitmap(source),"DIB lifetime did not release resource");
		check(process.memory_available(address,4096),"DIB deletion retained guest allocation");
		const auto ddb=objects.add(raven::DesktopBitmap{1,1,32,std::vector<uint8_t>(4)});
		check(ddb && call("SelectObject",{dc,ddb})==0x10000,"cannot select DDB boundary source");
		check(!call("CreateCompatibleBitmap",{dc,0x80000000,0x80000000}) && environment.last_error()==8,"bitmap allocation size overflow accepted");
		check(call("SelectObject",{dc,0x10000})==ddb && call("DeleteObject",{ddb}) && call("DeleteObject",{0x10000}) &&
			call("DeleteDC",{dc}) && !objects.count() && !objects.bytes(),"compatible bitmap resources leaked or stock identity failed");
		check(objects.bitmap(0x10000) && objects.bitmap(0x10000)->stock,"stock bitmap identity was destroyed");
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
	return 0;
}
