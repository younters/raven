#include "wave_input.h"
#include "input.h"
#include "nt/kernel_context.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace raven_darwin {
using namespace raven;
namespace {
constexpr uint32_t mmsyserr_error = 1, mmsyserr_baddeviceid = 2, mmsyserr_invalhandle = 5, mmsyserr_nomem = 7, mmsyserr_notsupported = 8,
	mmsyserr_invalflag = 10, mmsyserr_invalparam = 11, waverr_badformat = 32, waverr_stillplaying = 33, waverr_unprepared = 34;
constexpr uint32_t whdr_done = 0x1, whdr_prepared = 0x2, whdr_beginloop = 0x4, whdr_endloop = 0x8, whdr_inqueue = 0x10;
constexpr uint32_t callback_kind = 0x70000, callback_null = 0, callback_event = 0x50000, wave_format_query = 0x1;
constexpr uint64_t wave_mapper = 0xffffffffU;

class WaveIn : public KernelObject {
public:
	WaveIn(const AudioFormat &value, uint32_t kind, uint64_t event) :
		input({value.rate,value.channels,value.bits,value.block,value.floating}), format(value), callback(kind), target(event) {}
	struct Header { uint64_t address; uint32_t written = 0; };
	raven_darwin::AudioInput input;
	AudioFormat format;
	uint32_t callback;
	uint64_t target;
	std::deque<Header> queued;
	bool running = false;
};
}

void register_wave_input(ApiRegistry &registry, KernelContext &context, WaveFormatReader read_format)
{
	auto &objects = context.objects;
	const auto flags_at = [](ApiCall &call, uint64_t header) { return header + (call.pointer_width() == 4 ? 16 : 24); };
	auto inputs = std::make_shared<std::map<uint64_t,std::weak_ptr<WaveIn>>>();
	const auto input_of = [&objects](uint64_t handle) { return objects.object_as<WaveIn>(handle); };
	context.threads.add_service([inputs,&context] {
		for (auto entry=inputs->begin();entry!=inputs->end();) {
			const auto device=entry->second.lock();
			if (!device) { entry=inputs->erase(entry); continue; }
			while (device->running && !device->queued.empty() && device->input.padding()) {
				const uint64_t width=context.process.mode()==NativeMode::x86?4:8;
				auto &header=device->queued.front();
				uint32_t total; const auto raw=context.process.read_memory(header.address+width,4); std::memcpy(&total,raw.data(),4);
				const auto available=total-header.written;
				auto samples=device->input.take(std::min<uint32_t>(available/device->format.block,device->input.padding()));
				if (samples.empty()) break;
				const auto pointer_raw=context.process.read_memory(header.address,width);
				uint64_t pointer=0; std::memcpy(&pointer,pointer_raw.data(),width);
				context.process.write_memory(pointer+header.written,samples);
				header.written+=uint32_t(samples.size());
				context.process.write_memory(header.address+width+4,std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&header.written),4));
				if (header.written<total) break;
				uint32_t flags=whdr_prepared|whdr_done;
				context.process.write_memory(header.address+(width==4?16:24),std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&flags),4));
				device->queued.pop_front();
				if (device->callback==callback_event && device->target) context.objects.set_event(device->target,true);
			}
			++entry;
		}
	});
	registry.add("waveInGetNumDevs", 0, [](ApiCall &call) {
		call.ret(raven_darwin::AudioInput::has_default_device()?1:0);
	});
	// Mixer controls are separate from wave capture and remain unavailable.
	registry.add("waveInMessage", 4, [](ApiCall &call) { call.ret(2); });
	registry.add("waveInOpen", 6, [&objects,inputs,read_format](ApiCall &call) {
		if (call[0])
			call.store_pointer(call[0], 0);
		if (call[1]!=0 && uint32_t(call[1])!=uint32_t(wave_mapper)) { call.ret(mmsyserr_baddeviceid); return; }
		if (!raven_darwin::AudioInput::has_default_device()) { call.ret(mmsyserr_baddeviceid); return; }
		const auto f=read_format(call,call[2]);
		if (!f) { call.ret(waverr_badformat); return; }
		if (call.u32(5)&wave_format_query) { call.ret(0); return; }
		if (!call[0]) { call.ret(mmsyserr_invalparam); return; }
		const auto kind=call.u32(5)&callback_kind;
		if (kind!=callback_null && kind!=callback_event) { call.ret(mmsyserr_notsupported); return; }
		auto device=std::make_shared<WaveIn>(*f,kind,kind==callback_event?call[3]:0);
		if (!device->input.ready()) { call.ret(mmsyserr_error); return; }
		const auto made=objects.add_object(device);
		if (made.status) { call.ret(mmsyserr_nomem); return; }
		(*inputs)[made.value]=device; call.store_pointer(call[0],made.value); call.ret(0);
	});
	for (const auto *name : {"waveInPrepareHeader","waveInUnprepareHeader","waveInAddBuffer"})
		registry.add(name,3,[input_of,flags_at,name](ApiCall &call) {
			const auto device=input_of(call[0]);
			if (!device) { call.ret(mmsyserr_invalhandle); return; }
			if (!call[1] || call.u32(2)<(call.pointer_width()==4?32U:48U)) { call.ret(mmsyserr_invalparam); return; }
			const auto at=flags_at(call,call[1]); auto flags=call.load<uint32_t>(at);
			if (std::string_view(name)=="waveInPrepareHeader") flags|=whdr_prepared;
			else if (std::string_view(name)=="waveInUnprepareHeader") {
				if (flags&whdr_inqueue) { call.ret(waverr_stillplaying); return; }
				flags&=~whdr_prepared;
			} else {
				if (!(flags&whdr_prepared)) { call.ret(waverr_unprepared); return; }
				if (flags&whdr_inqueue) { call.ret(waverr_stillplaying); return; }
				const auto size=call.load<uint32_t>(call[1]+call.pointer_width());
				if (!size || size%device->format.block || !call.load_pointer(call[1])) { call.ret(mmsyserr_invalparam); return; }
				call.store<uint32_t>(call[1]+call.pointer_width()+4,0);
				device->queued.push_back({call[1],0}); flags=(flags&~whdr_done)|whdr_inqueue;
			}
			call.store<uint32_t>(at,flags); call.ret(0);
		});
	const auto finish_input=[&context](const std::shared_ptr<WaveIn> &device,uint64_t header) {
		const uint64_t at=header+(context.process.mode()==NativeMode::x86?16:24);
		uint32_t flags=whdr_prepared|whdr_done;
		context.process.write_memory(at,std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&flags),4));
		if (device->callback==callback_event && device->target) context.objects.set_event(device->target,true);
	};
	for (const auto *name : {"waveInClose","waveInStart","waveInStop","waveInReset"})
		registry.add(name,1,[&objects,input_of,finish_input,name](ApiCall &call) {
			const auto device=input_of(call[0]); if (!device) { call.ret(mmsyserr_invalhandle); return; }
			const auto method=std::string_view(name);
			if (method=="waveInClose") { if (!device->queued.empty()) { call.ret(waverr_stillplaying); return; } device->input.stop(); objects.close(call[0]); }
			else if (method=="waveInStart") { if (!device->input.start()) { call.ret(mmsyserr_error); return; } device->running=true; }
			else { device->running=false; if (method=="waveInReset") device->input.reset(); else device->input.stop();
				while (!device->queued.empty()) { finish_input(device,device->queued.front().address); device->queued.pop_front(); }
			}
			call.ret(0);
		});
	registry.add("waveInGetPosition",3,[input_of](ApiCall &call) {
		const auto device=input_of(call[0]); if (!device) { call.ret(mmsyserr_invalhandle); return; }
		if (!call[1] || call.u32(2)<12) { call.ret(mmsyserr_invalparam); return; }
		const auto frames=device->input.position(); auto kind=call.load<uint32_t>(call[1]); uint32_t value;
		if (kind==1) value=uint32_t(frames*1000/device->format.rate);
		else if (kind==2) value=uint32_t(frames);
		else { kind=4; value=uint32_t(frames*device->format.block); }
		call.store<uint32_t>(call[1],kind); call.store<uint32_t>(call[1]+4,value); call.ret(0);
	});
	registry.add("waveInGetID",2,[input_of](ApiCall &call) { if (!input_of(call[0])) { call.ret(mmsyserr_invalhandle); return; } if (call[1]) call.store<uint32_t>(call[1],0); call.ret(0); });
	for (const auto *name : {"waveInGetDevCapsW", "waveInGetDevCapsA"})
		registry.add(name, 3, [name](ApiCall &call) {
			if ((call[0]!=0 && uint32_t(call[0])!=uint32_t(wave_mapper)) || !raven_darwin::AudioInput::has_default_device()) { call.ret(mmsyserr_baddeviceid); return; }
			const bool wide=std::string_view(name).ends_with('W');
			if (!call[1] || call.u32(2)<8) { call.ret(mmsyserr_invalparam); return; }
			const size_t length=wide?80:48; call.write(call[1],std::vector<uint8_t>(std::min<size_t>(length,call.u32(2))));
			if (wide) call.put_wide(call[1]+8,u"CoreAudio Input",std::min<uint32_t>((call.u32(2)-8)/2,32));
			else call.put_narrow(call[1]+8,"CoreAudio Input",std::min<uint32_t>(call.u32(2)-8,32));
			call.ret(0);
		});
}
}
