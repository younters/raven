#include "api.h"
#include "mixer.h"
#include "input.h"
#include "nt/kernel_context.h"
#include "nt/x86_com.h"
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <sys/mman.h>

namespace raven_darwin {
using namespace raven;
namespace {
constexpr uint32_t ok = 0, invalid = 0x80070057, no_interface = 0x80004002, not_impl = 0x80004001;
constexpr ComMethod methods[] = {
#include "nt/x86_dsound_vtables.inc"
#include "dsound3d_vtables.inc"
#include "wasapi_vtables.inc"
#include "xaudio_vtables.inc"
	{"IDirectSoundNotify", "QueryInterface", 0, 3}, {"IDirectSoundNotify", "AddRef", 1, 1},
	{"IDirectSoundNotify", "Release", 2, 1}, {"IDirectSoundNotify", "SetNotificationPositions", 3, 3},
};
const std::map<std::string_view,std::array<uint8_t,16>> guids{
#include "guids.inc"
};
std::string_view interface_for(ApiCall &call, uint64_t address) {
	if (!address) return {};
	const auto bytes = call.read(address,16);
	for (const auto &[name,guid] : guids)
		if (name.starts_with("IID_") && std::equal(bytes.begin(),bytes.end(),guid.begin())) return name.substr(4);
	return {};
}
struct Buffer {
	AudioFormat format{44100, 2, 16, 4, false};
	uint64_t memory = 0, notify = 0, owner = 0;
	uint32_t size = 0, flags = 0, frequency = 44100;
	int32_t volume = 0, pan = 0;
	bool looping = false;
	std::shared_ptr<AudioMixer::Voice> voice;
};
struct State {
	KernelContext &context;
	std::unique_ptr<AudioMixer> mixer_;
	AudioMixer &mixer() { if (!mixer_) mixer_ = std::make_unique<AudioMixer>(); return *mixer_; }
	std::map<uint64_t, std::string> objects;
	std::map<uint64_t, uint32_t> refs;
	std::map<std::string, uint64_t> tables;
	std::map<uint64_t, std::shared_ptr<Buffer>> buffers;
	explicit State(KernelContext &c) : context(c) {}
	uint64_t object(ApiCall &call, const std::string &type) {
		auto &table = tables[type];
		if (!table) {
			table = context.allocator.allocate(4096);
			for (const auto &method : methods) if (method.interface == type) {
				const auto name = type + "." + std::string(method.name);
				const auto address = context.modules.resolve({"raven-audio.dll", name, 0, false, 0});
				call.store_pointer(table + method.index * call.pointer_width(), address);
			}
		}
		const auto address = context.allocator.allocate(16);
		call.store_pointer(address, table); objects[address] = type; refs[address] = 1;
		return address;
	}
	void gains(Buffer &b) {
		const float gain = b.volume <= -10000 ? 0 : std::pow(10.0f, b.volume / 2000.0f);
		mixer().gain(b.voice, gain * std::pow(10.0f, -std::max(0, b.pan) / 2000.0f),
			gain * std::pow(10.0f, std::min(0, b.pan) / 2000.0f), b.frequency);
	}
	std::optional<AudioFormat> format(ApiCall &call, uint64_t at) {
		if (!at) return std::nullopt;
		auto tag = call.load<uint16_t>(at);
		const auto channels = call.load<uint16_t>(at + 2), bits = call.load<uint16_t>(at + 14);
		if (tag == 0xfffe) {
			if (call.load<uint16_t>(at+16)<22) return std::nullopt;
			const auto sub=call.read(at+24,16);
			const std::array<uint8_t,12> tail{0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
			if (!std::equal(sub.begin()+4,sub.end(),tail.begin())) return std::nullopt;
			tag=uint16_t(call.load<uint32_t>(at+24));
		}
		const auto rate = call.load<uint32_t>(at + 4); const auto block = call.load<uint16_t>(at + 12);
		if ((tag != 1 && tag != 3) || rate < 8000 || rate > 384000 || !channels || channels > 8 ||
			block != channels * (bits / 8) || (tag == 3 ? bits != 32 : bits != 8 && bits != 16 && bits != 24 && bits != 32)) return std::nullopt;
		return AudioFormat{rate, channels, bits, block, tag == 3};
	}
	void write_format(ApiCall &call, uint64_t at, const AudioFormat &f) {
		std::array<uint8_t, 18> bytes{}; call.write(at, bytes);
		call.store<uint16_t>(at, f.floating ? 3 : 1); call.store<uint16_t>(at + 2, f.channels);
		call.store<uint32_t>(at + 4, f.rate); call.store<uint32_t>(at + 8, f.rate * f.block);
		call.store<uint16_t>(at + 12, f.block); call.store<uint16_t>(at + 14, f.bits);
	}
	void directsound(ApiCall &call, std::string_view type, std::string_view method) {
		if (type == "IDirectSound8") {
			if (method == "CreateSoundBuffer") {
				if (!call[1] || !call[2] || call[3]) { call.ret(invalid); return; }
				auto b = std::make_shared<Buffer>(); b->flags = call.load<uint32_t>(call[1] + 4);
				b->size = call.load<uint32_t>(call[1] + 8);
				if (b->flags & 1) b->size = 176400;
				else {
					auto f = format(call, call.load_pointer(call[1] + 16));
					if (!f || !b->size || b->size > 0x0fffffff || b->size % f->block) { call.ret(invalid); return; }
					b->format = *f;
				}
				b->frequency = b->format.rate; b->memory = context.allocator.allocate(b->size);
				if (!b->memory) { call.ret(0x8007000e); return; }
				if (b->format.bits == 8) call.write(b->memory, std::vector<uint8_t>(b->size, 128));
				b->voice = mixer().create(b->format, true, call.read(b->memory, b->size));
				const auto address = object(call, "IDirectSoundBuffer8"); b->owner = address;
				buffers[address] = b; call.store_pointer(call[2], address);
			} else if (method == "DuplicateSoundBuffer") {
				if (!buffers.contains(call[1]) || !call[2]) { call.ret(invalid); return; }
				auto b = std::make_shared<Buffer>(*buffers.at(call[1]));
				b->voice = mixer().create(b->format, true, call.read(b->memory, b->size));
				b->notify = 0; const auto address = object(call, "IDirectSoundBuffer8"); b->owner = address;
				buffers[address] = b; gains(*b); call.store_pointer(call[2], address);
			} else if (method == "GetCaps") {
				if (!call[1] || call.load<uint32_t>(call[1]) < 96) { call.ret(invalid); return; }
				call.write(call[1], std::vector<uint8_t>(96)); call.store<uint32_t>(call[1], 96);
				call.store<uint32_t>(call[1]+4, 0x0000000f); call.store<uint32_t>(call[1]+8, 8000); call.store<uint32_t>(call[1]+12, 384000);
			} else if (method == "GetSpeakerConfig") { if (!call[1]) { call.ret(invalid); return; } call.store<uint32_t>(call[1], 4); }
			else if (method == "VerifyCertification") { if (call[1]) call.store<uint32_t>(call[1], 1); }
			call.ret(ok); return;
		}
		const auto found = buffers.find(call[0]);
		if (found == buffers.end()) { call.ret(invalid); return; }
		auto &b = *found->second;
		if (type == "IDirectSound3DBuffer" || type == "IDirectSound3DListener") {
			if (method == "SetPosition" && type == "IDirectSound3DBuffer") {
				const auto floats = call.process.api_float_arguments();
				const uint32_t rawx = call.pointer_width()==4 ? call.u32(1) : uint32_t(floats[1]);
				const uint32_t rawz = call.pointer_width()==4 ? call.u32(3) : uint32_t(floats[3]);
				float x,z; std::memcpy(&x,&rawx,4); std::memcpy(&z,&rawz,4);
				if (!std::isfinite(x) || !std::isfinite(z)) { call.ret(invalid); return; }
				b.pan = int32_t(std::clamp(x/std::max(1.f,std::hypot(x,z)),-1.f,1.f)*10000); gains(b);
			} else if (method.starts_with("Get")) { call.ret(not_impl); return; }
			call.ret(ok); return;
		}
		if (method == "SetNotificationPositions") {
			if (call[1] > 1024 || (call[1] && !call[2])) { call.ret(invalid); return; }
			std::vector<std::pair<uint32_t,uint64_t>> points;
			const size_t stride = call.pointer_width() == 4 ? 8 : 16;
			for (uint32_t i=0; i<call[1]; ++i) {
				const auto offset = call.load<uint32_t>(call[2]+i*stride);
				if (offset != UINT32_MAX && offset >= b.size) { call.ret(invalid); return; }
				points.emplace_back(offset, call.load_pointer(call[2]+i*stride+call.pointer_width()));
			}
			mixer().notifications(b.voice, std::move(points));
		} else if (method == "Lock") {
			uint32_t offset = call.u32(1), bytes = call.u32(2);
			if (call.u32(7) & 1) offset = (mixer().position(b.voice) + b.format.rate * b.format.block / 25) % b.size;
			if (call.u32(7) & 2) { offset = 0; bytes = b.size; }
			if (offset >= b.size || bytes > b.size || !call[3] || !call[4]) { call.ret(invalid); return; }
			const auto first = std::min(bytes, b.size-offset);
			if (first != bytes && (!call[5] || !call[6])) { call.ret(invalid); return; }
			call.store_pointer(call[3], b.memory+offset); call.store<uint32_t>(call[4], first);
			if (call[5]) call.store_pointer(call[5], bytes > first ? b.memory : 0);
			if (call[6]) call.store<uint32_t>(call[6], bytes-first);
		} else if (method == "Unlock") {
			// Duplicates share the sample allocation, but each has its own transport and controls.
			for (auto &[address, other] : buffers) if (other->memory == b.memory) mixer().update(other->voice, call.read(b.memory, b.size));
		} else if (method == "Play") { b.looping = call.u32(3) & 1; if (mixer().position(b.voice)>=b.size) mixer().seek(b.voice,0); mixer().play(b.voice, b.looping); }
		else if (method == "Stop") mixer().stop(b.voice);
		else if (method == "SetCurrentPosition") { if (call[1] >= b.size) { call.ret(invalid); return; } mixer().seek(b.voice, call.u32(1)); }
		else if (method == "GetCurrentPosition") {
			const auto position = mixer().position(b.voice) % b.size;
			if (call[1]) call.store<uint32_t>(call[1], position);
			if (call[2]) call.store<uint32_t>(call[2], (position + b.format.rate*b.format.block/25) % b.size);
		} else if (method == "GetStatus") { if (!call[1]) { call.ret(invalid); return; } call.store<uint32_t>(call[1], mixer().playing(b.voice) ? 1 | (b.looping ? 4 : 0) : 0); }
		else if (method == "SetVolume" || method == "SetPan" || method == "SetFrequency") {
			const auto value = call.i32(1);
			if ((method == "SetVolume" && (value < -10000 || value > 0)) || (method == "SetPan" && (value < -10000 || value > 10000)) ||
				(method == "SetFrequency" && value && (value < 100 || value > 200000))) { call.ret(invalid); return; }
			if (method == "SetVolume") b.volume = value; else if (method == "SetPan") b.pan = value; else b.frequency = value ? value : b.format.rate;
			gains(b);
		} else if (method == "GetVolume" || method == "GetPan" || method == "GetFrequency") {
			if (!call[1]) { call.ret(invalid); return; } call.store<uint32_t>(call[1], method == "GetVolume" ? b.volume : method == "GetPan" ? b.pan : b.frequency);
		} else if (method == "SetFormat") {
			auto f = format(call, call[1]);
			if (!(b.flags & 1) || !f) { call.ret(invalid); return; }
			mixer().stop(b.voice); b.format = *f; b.frequency = f->rate;
			b.voice = mixer().create(*f, true, call.read(b.memory,b.size)); gains(b);
		} else if (method == "GetFormat") {
			if (call[3]) call.store<uint32_t>(call[3], 18);
			if (call[1]) { if (call[2] < 18) { call.ret(invalid); return; } write_format(call,call[1],b.format); }
		} else if (method == "GetCaps") {
			if (!call[1] || call.load<uint32_t>(call[1]) < 20) { call.ret(invalid); return; }
			call.store<uint32_t>(call[1]+4,b.flags); call.store<uint32_t>(call[1]+8,b.size);
		} else if (method != "Restore" && method != "Initialize") { call.ret(not_impl); return; }
		call.ret(ok);
	}

#include "xaudio.inc"
#include "wasapi.inc"
	void dispatch(ApiCall &call, std::string_view name) {
		const auto dot = name.find('.'); const auto type = name.substr(0,dot), method = name.substr(dot+1);
		if (std::getenv("RAVEN_AUDIO_TRACE") && (type.starts_with("IMM") || type.starts_with("IAudio")))
			std::fprintf(stderr,"Raven audio: %.*s\n",int(name.size()),name.data());
		if (!objects.contains(call[0])) { call.ret(invalid); return; }
		const auto identity = buffers.contains(call[0]) ? buffers.at(call[0])->owner : call[0];
		if (method == "AddRef") { call.ret(++refs[identity]); return; }
		if (method == "Release") {
			const auto n = --refs[identity];
			if (!n) {
				if (buffers.contains(call[0])) {
					auto b = buffers.at(call[0]); mixer().stop(b->voice);
					for (auto it=buffers.begin();it!=buffers.end();) {
						if (it->second->owner == identity) {
							objects.erase(it->first); refs.erase(it->first); context.allocator.release(it->first); it=buffers.erase(it);
						} else ++it;
					}
					if (std::none_of(buffers.begin(),buffers.end(),[&](const auto &item) { return item.second->memory==b->memory; })) context.allocator.release(b->memory);
				} else {
					if (auto it=clients.find(call[0]);it!=clients.end()) {
						auto client=it->second; clients.erase(it);
						if (client.use_count()==1) { if (client->voice) mixer().stop(client->voice); if (client->input) client->input->stop(); if (client->memory) context.allocator.release(client->memory); }
					}
					collections.erase(call[0]); collection_flows.erase(call[0]); device_flows.erase(call[0]); objects.erase(call[0]); refs.erase(call[0]); context.allocator.release(call[0]);
				}
			}
			call.ret(n); return;
		}
		if (method == "QueryInterface") {
			if (!call[1] || !call[2]) { call.ret(invalid); return; }
			call.store_pointer(call[2],0); const auto iid = call.load<uint32_t>(call[1]);
			const auto requested = interface_for(call,call[1]);
			if (requested.empty()) { call.ret(no_interface); return; }
			if (requested == "IUnknown" || requested == type ||
				(type == "IAudioSessionManager2" && requested == "IAudioSessionManager") ||
				(type == "IAudioClient3" && (requested == "IAudioClient" || requested == "IAudioClient2"))) {
				++refs[identity]; call.store_pointer(call[2],identity); call.ret(ok); return;
			}
			if (type == "IMMDevice" && requested == "IMMEndpoint") {
				const auto at=object(call,"IMMEndpoint"); device_flows[at]=device_flows[call[0]]; call.store_pointer(call[2],at); call.ret(ok); return;
			}
			const bool device = type == "IDirectSound8";
			const bool buffer = buffers.contains(call[0]);
			if (iid == 0 || (device && (iid == 0x279afa83 || iid == 0xc50a7e93)) ||
				(buffer && (iid == 0x279afa85 || iid == 0x6825a449))) {
				const auto address = buffer ? buffers.at(call[0])->owner : call[0];
				++refs[address]; call.store_pointer(call[2],address); call.ret(ok); return;
			}
			if (buffer && (iid == 0xb0210783 || iid == 0x279afa86 || iid == 0x279afa84)) {
				auto b = buffers.at(call[0]);
				if (iid != 0xb0210783) {
					const auto at = object(call,iid == 0x279afa86 ? "IDirectSound3DBuffer" : "IDirectSound3DListener");
					buffers[at] = b; refs.erase(at); ++refs[b->owner]; call.store_pointer(call[2],at); call.ret(ok); return;
				}
				if (!b->notify || !objects.contains(b->notify)) { b->notify = object(call,"IDirectSoundNotify"); buffers[b->notify] = b; refs.erase(b->notify); }
				++refs[b->owner];
				call.store_pointer(call[2], b->notify); call.ret(ok); return;
			}
			call.ret(no_interface); return;
		}
		if (type.starts_with("IDirectSound")) directsound(call,type,method);
		else if (type.starts_with("IXAudio2")) xaudio(call,type,method);
		else wasapi(call,type,method);
	}
};
std::map<NativeProcess *, std::weak_ptr<State>> states;
}
std::span<const std::string_view> audio_method_names() {
	static const auto names = com_names(methods); return names;
}
uint32_t audio_method_words(std::string_view name) { return com_words(methods,name); }
bool create_audio_instance(ApiCall &call) {
	if (!call[0]) return false;
	const auto bytes = call.read(call[0],16);
	const auto &clsid=guids.at("CLSID_MMDeviceEnumerator"), &xaudio=guids.at("CLSID_XAudio2_7");
	const bool legacy=std::equal(bytes.begin(),bytes.end(),xaudio.begin());
	if (!legacy && !std::equal(bytes.begin(),bytes.end(),clsid.begin())) return false;
	if (!call[4]) { call.ret(invalid); return true; }
	call.store_pointer(call[4],0);
	if (call[1]) { call.ret(0x80040110); return true; }
	const auto requested=interface_for(call,call[3]);
	if (requested!="IUnknown" && requested!=(legacy?"IXAudio2Legacy":"IMMDeviceEnumerator")) { call.ret(no_interface); return true; }
	auto state=states[&call.process].lock();
	if (!state) { call.ret(0x80040154); return true; }
	call.store_pointer(call[4],state->object(call,legacy?"IXAudio2Legacy":"IMMDeviceEnumerator")); call.ret(ok); return true;
}
void register_audio_api(ApiRegistry &registry, KernelContext &context) {
	auto state = std::make_shared<State>(context); states[&context.process] = state;
	context.threads.add_service([state] { state->service_clients(); state->service_voices(); if (!state->mixer_) return; for (auto event : state->mixer().take_events()) state->context.objects.set_event(event,true); });
	for (const auto *name : {"XAudio2Create","XAudio2CreateWithVersionInfo"}) registry.add(name,std::string_view(name)=="XAudio2Create"?3:4,[state](ApiCall &call) {
		if (!call[0]) { call.ret(invalid); return; }
		call.store_pointer(call[0],0);
		if (!state->mixer().ready()) { call.ret(0x88960001); return; }
		call.store_pointer(call[0],state->object(call,"IXAudio2")); call.ret(ok);
	});
	for (const auto name : audio_method_names()) registry.add(name,audio_method_words(name),[state,name](ApiCall &call) { state->dispatch(call,name); });
	for (const auto *name : {"DirectSoundCreate", "DirectSoundCreate8"}) registry.add(name,3,[state](ApiCall &call) {
		if (!call[1] || call[2]) { call.ret(invalid); return; }
		call.store_pointer(call[1],0);
		if (!state->mixer().ready()) { call.ret(0x88780078); return; }
		call.store_pointer(call[1],state->object(call,"IDirectSound8")); call.ret(ok);
	});
	for (const auto *name : {"DirectSoundEnumerateA", "DirectSoundEnumerateW"}) registry.add(name,2,[state,name](ApiCall &call) {
		if (!call[0]) { call.ret(invalid); return; }
		const auto text = state->context.allocator.allocate(128);
		if (std::string_view(name).ends_with('W')) call.put_wide(text,u"Primary Sound Driver",64);
		else call.put_narrow(text,"Primary Sound Driver",128);
		const std::array<uint64_t,5> args{0,text,text,call[1],0};
		state->context.call(call[0],std::span(args.data(),4)); state->context.allocator.release(text); call.ret(ok);
	});
}
}
