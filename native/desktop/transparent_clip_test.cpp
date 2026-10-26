#include "nt/providers.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc,char **argv)
{
	try {
		if (argc!=3) throw std::runtime_error("bootstrap and transparent-window fixture required");
		std::ifstream file(argv[2],std::ios::binary|std::ios::ate);
		if (!file || file.tellg()<=0) throw std::runtime_error("cannot read transparent-window fixture");
		std::vector<uint8_t> bytes(size_t(file.tellg()));file.seekg(0);
		if (!file.read(reinterpret_cast<char *>(bytes.data()),std::streamsize(bytes.size()))) throw std::runtime_error("short fixture");
		raven::ModuleSet modules;auto &entry=modules.add("transparent.exe",std::move(bytes));
		unsigned frames=0;
		raven::DesktopPresentation presentation;
		presentation.present=[&](uint64_t,std::string_view,uint32_t,uint32_t,std::span<const uint8_t>) { ++frames; };
		const auto result=entry.image.is_64 ?
			raven::run_native_process(modules,entry,argv[1],std::chrono::seconds(10),{},{},presentation) :
			raven::run_native_process_x86(modules,entry,argv[1],std::chrono::seconds(10),{},presentation);
		if (result.stop!=raven::NativeStop::returned || result.value || !frames) {
			std::cerr<<"transparent clipping stop="<<int(result.stop)<<" value="<<result.value<<" frames="<<frames<<'\n';return 1;
		}
	} catch (const std::exception &error) { std::cerr<<error.what()<<'\n';return 1; }
}
