#include "api.h"
#include "output.h"
#include "nt/kernel_context.h"
#include "nt/text.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>

namespace raven_darwin {
using namespace raven;
namespace {
struct Wave {
	AudioFormat format{};
	std::vector<uint8_t> samples;
};
uint16_t u16(const uint8_t *at) { uint16_t value; std::memcpy(&value,at,2); return value; }
uint32_t u32(const uint8_t *at) { uint32_t value; std::memcpy(&value,at,4); return value; }
std::optional<Wave> parse_wave(std::span<const uint8_t> bytes) {
	if (bytes.size()<44 || std::memcmp(bytes.data(),"RIFF",4) || std::memcmp(bytes.data()+8,"WAVE",4)) return {};
	Wave wave; bool have_format=false;
	for (size_t at=12;at+8<=bytes.size();) {
		const auto size=u32(bytes.data()+at+4);
		if (size>bytes.size()-at-8) return {};
		const auto *data=bytes.data()+at+8;
		if (!std::memcmp(bytes.data()+at,"fmt ",4) && size>=16) {
			const auto tag=u16(data), channels=u16(data+2), block=u16(data+12), bits=u16(data+14);
			const auto rate=u32(data+4);
			if ((tag!=1 && tag!=3) || !channels || channels>8 || rate<8000 || rate>384000 ||
				block!=channels*(bits/8) || (tag==3?bits!=32:bits!=8 && bits!=16 && bits!=24 && bits!=32)) return {};
			wave.format={rate,channels,bits,block,tag==3}; have_format=true;
		} else if (!std::memcmp(bytes.data()+at,"data",4)) {
			wave.samples.assign(data,data+size);
		}
		at+=8+size+(size&1);
	}
	if (!have_format || wave.samples.empty() || wave.samples.size()%wave.format.block) return {};
	return wave;
}
Wave alias_wave() {
	Wave wave; wave.format={48000,2,32,8,true}; wave.samples.resize(4800*8);
	for (size_t frame=0;frame<4800;++frame) {
		const float envelope=std::min({1.0f,float(frame)/480,float(4800-frame)/960});
		const float sample=0.06f*envelope*std::sin(float(frame)*2*3.14159265f*660/48000);
		std::memcpy(wave.samples.data()+frame*8,&sample,4);
		std::memcpy(wave.samples.data()+frame*8+4,&sample,4);
	}
	return wave;
}
struct State {
	std::shared_ptr<AudioOutput> current;
	std::vector<uint8_t> samples;
	bool looping=false;
	void service() {
		if (!current || current->position()<samples.size()) return;
		if (looping) { current->reset(); current->enqueue(0,samples); }
		else { current.reset(); samples.clear(); }
	}
};
}
void register_playsound_api(ApiRegistry &registry, KernelContext &context) {
	auto state=std::make_shared<State>();
	context.threads.add_service([state] { state->service(); });
	for (const auto *name : {"PlaySound","PlaySoundA","PlaySoundW","sndPlaySoundA","sndPlaySoundW"}) {
		const auto entry=std::string_view(name);
		registry.add(name,entry.starts_with("snd")?2:3,[state,&context,entry](ApiCall &call) {
			const auto flags=call.u32(entry.starts_with("snd")?1:2);
			if (!call[0] || (flags&0x40)) { state->current.reset(); state->samples.clear(); call.ret(1); return; }
			if ((flags&0x10) && state->current) { call.ret(0); return; }
			if ((flags&8) && !(flags&1)) { call.ret(0); return; }
			const bool wide=entry.ends_with('W');
			const auto source=flags&0x00070004;
			std::optional<Wave> generated;
			if (source==0x10000 || (source==0 && call[0]<0x10000)) generated=alias_wave();
			std::vector<uint8_t> bytes;
			if (!generated && source==4) {
				const auto header=call.read(call[0],12);
				if (std::memcmp(header.data(),"RIFF",4)) { call.ret(0); return; }
				const auto length=u32(header.data()+4);
				if (length>32U<<20 || length<36) { call.ret(0); return; }
				bytes=call.read(call[0],size_t(length)+8);
			} else if (!generated && source==0x40004) {
				// Ask the existing PE resource provider for a WAVE resource in the caller's module.
				const auto type=context.allocator.allocate(10);
				const char16_t wave_type[]{u'W',u'A',u'V',u'E',0};
				context.process.write_memory(type,{reinterpret_cast<const uint8_t *>(wave_type),sizeof(wave_type)});
				const auto find=context.modules.resolve({"kernel32.dll",wide?"FindResourceW":"FindResourceA",0,false,0});
				const std::array<uint64_t,3> find_args{call[1],call[0],type};
				const auto resource=context.call(find,find_args).value; context.allocator.release(type);
				if (!resource) { call.ret(0); return; }
				const auto size_fn=context.modules.resolve({"kernel32.dll","SizeofResource",0,false,0});
				const std::array<uint64_t,2> resource_args{call[1],resource};
				const auto length=context.call(size_fn,resource_args).value;
				if (!length || length>32U<<20) { call.ret(0); return; }
				const auto load=context.modules.resolve({"kernel32.dll","LoadResource",0,false,0});
				const auto handle=context.call(load,resource_args).value;
				const auto lock=context.modules.resolve({"kernel32.dll","LockResource",0,false,0});
				const std::array<uint64_t,1> lock_args{handle};
				const auto pointer=context.call(lock,lock_args).value;
				if (!pointer) { call.ret(0); return; }
				bytes=call.read(pointer,length);
			} else if (!generated) {
				const auto path=wide?call.wide(call[0]):from_ansi(call.narrow(call[0]),1252);
				if (source==0x10000 || (source==0 && path.find(u'.')==path.npos)) {
					generated=alias_wave();
				} else {
					const auto resolved=context.paths.resolve(path);
					if (!resolved) { call.ret(0); return; }
					std::ifstream stream(resolved->host,std::ios::binary|std::ios::ate);
					if (!stream || stream.tellg()<44 || stream.tellg()>std::streamoff(32U<<20)) { call.ret(0); return; }
					bytes.resize(size_t(stream.tellg())); stream.seekg(0); stream.read(reinterpret_cast<char *>(bytes.data()),bytes.size());
					if (!stream) { call.ret(0); return; }
				}
			}
			const auto wave=generated?generated:parse_wave(bytes);
			if (!wave) { call.ret(0); return; }
			auto output=std::make_shared<AudioOutput>(wave->format);
			if (!output->ready()) { call.ret(0); return; }
			state->current=output; state->samples=wave->samples; state->looping=flags&8;
			output->enqueue(0,wave->samples);
			if (!(flags&1)) {
				while (state->current==output && output->position()<wave->samples.size())
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				state->service();
			}
			call.ret(1);
		});
	}
}
}
