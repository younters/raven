// XAudio2 voices use the same host mixer as DirectSound and WASAPI. The guest
// supplies source buffers once; no audio thread reads guest memory.
struct XVoice {
	AudioFormat format{AudioMixer::device_rate(),2,32,8,true};
	std::shared_ptr<AudioMixer::Voice> voice;
	struct Packet { uint64_t begin, end, context, id; bool stream_end, started=false; };
	std::deque<Packet> packets;
	uint64_t submitted=0, callback=0;
	float volume=1, ratio=1;
	bool running=false, mastering=false, submix=false;
};
std::map<uint64_t,XVoice> xvoices;
float xmaster=1;
uint64_t next_packet=1;
bool servicing_voices=false;
bool pending_voices() const {
	return std::any_of(xvoices.begin(),xvoices.end(),[](const auto &entry) {
		return entry.second.running && !entry.second.packets.empty();
	});
}
void voice_callback(uint64_t callback, uint32_t index, uint64_t value=0) {
	if (!callback) return;
	// The service may be running inside a parked Sleep/Windows wait. A parallel
	// callback's execution wait uses the same scheduler thread, so keep that
	// outer wait alive across context.call without changing the common
	// scheduler's wait contract.
	auto &thread=context.threads.current();
	struct RestoreWait {
		GuestThread &thread;
		std::function<bool()> ready;
		std::optional<std::chrono::steady_clock::time_point> wake;
		GuestThread::State state;
		~RestoreWait() {
			thread.ready=std::move(ready); thread.wake=wake;
			if (thread.state!=GuestThread::State::done) thread.state=state;
		}
	} restore{thread,thread.ready,thread.wake,thread.state};
	try {
		const size_t width=context.process.mode()==NativeMode::x86?4:8;
		const auto table_bytes=context.process.read_memory(callback,width);
		uint64_t table=0; std::memcpy(&table,table_bytes.data(),width);
		const auto method_bytes=context.process.read_memory(table+index*width,width);
		uint64_t method=0; std::memcpy(&method,method_bytes.data(),width);
		if (std::getenv("RAVEN_AUDIO_TRACE")) std::fprintf(stderr,"Raven audio: XAudio2 callback index=%u this=%llx value=%llx method=%llx\n",index,
			(unsigned long long)callback,(unsigned long long)value,(unsigned long long)method);
		if (index==2) { const std::array<uint64_t,1> args{callback}; context.call(method,args); }
		else { const std::array<uint64_t,2> args{callback,value}; context.call(method,args); }
	} catch (const std::exception &error) { if (std::getenv("RAVEN_AUDIO_TRACE")) std::fprintf(stderr,"Raven audio: callback failed: %s\n",error.what()); }
}
void service_voices() {
	if (servicing_voices || xvoices.empty()) return;
	// Running guest code has no safe API callback frame; retain notifications until one is available.
	if (context.process.parallel() && !context.process.stopped()) return;
	servicing_voices=true;
	struct Leave { bool &active; ~Leave() { active=false; } } leave{servicing_voices};
	std::vector<uint64_t> voices;
	for (const auto &[address,v]:xvoices) voices.push_back(address);
	for (const auto address:voices) {
		auto found=xvoices.find(address);
		if (found==xvoices.end() || !found->second.voice) continue;
		std::vector<uint64_t> starts;
		const auto presented=mixer().position(found->second.voice)/found->second.format.block;
		for (const auto &packet:found->second.packets)
			if (found->second.running && !packet.started && presented>=packet.begin) starts.push_back(packet.id);
		for (const auto id:starts) {
			found=xvoices.find(address);
			if (found==xvoices.end()) break;
			auto &v=found->second;
			const auto packet=std::find_if(v.packets.begin(),v.packets.end(),[id](const auto &p) { return p.id==id; });
			if (packet==v.packets.end() || packet->started) continue;
			packet->started=true;
			const auto callback=v.callback, value=packet->context;
			// No packet iterator/reference survives guest code: it can flush, submit or destroy this voice.
			voice_callback(callback,3,value);
		}
		for (;;) {
			found=xvoices.find(address);
			if (found==xvoices.end()) break;
			auto &v=found->second;
			if (v.packets.empty() || mixer().position(v.voice)/v.format.block<v.packets.front().end) break;
			const auto packet=v.packets.front();
			const auto callback=v.callback;
			v.packets.pop_front();
			voice_callback(callback,4,packet.context);
			if (packet.stream_end && xvoices.contains(address)) voice_callback(callback,2);
		}
	}
}
void xaudio(ApiCall &call,std::string_view type,std::string_view method) {
	const bool legacy=type.starts_with("IXAudio2Legacy");
	if (type=="IXAudio2" || type=="IXAudio2Legacy") {
		if (method=="GetDeviceCount" && call[1]) call.store<uint32_t>(call[1],mixer().ready()?1:0);
		else if (method=="GetDeviceDetails" && call[2]) { call.write(call[2],std::vector<uint8_t>(512)); }
		else if (method=="Initialize" || method=="RegisterForCallbacks" || method=="UnregisterForCallbacks" ||
			method=="StartEngine" || method=="StopEngine" || method=="CommitChanges") {}
		else if (method=="CreateSourceVoice") {
			if (!call[1] || !call[2]) { call.ret(invalid); return; }
			call.store_pointer(call[1],0);
			const auto f=format(call,call[2]); if (!f || !mixer().ready()) { call.ret(0x88960001); return; }
			const auto at=object(call,legacy?"IXAudio2LegacySourceVoice":"IXAudio2SourceVoice");
			auto &v=xvoices[at]; v.format=*f; v.voice=mixer().create(*f,false); v.callback=call[5];
			mixer().gain(v.voice,xmaster,xmaster);
			call.store_pointer(call[1],at);
		} else if (method=="CreateSubmixVoice") {
			if (!call[1] || !call[2] || !call[3]) { call.ret(invalid); return; }
			const auto at=object(call,legacy?"IXAudio2LegacySubmixVoice":"IXAudio2SubmixVoice");
			auto &v=xvoices[at]; v.format={call.u32(3),uint32_t(call[2]),32,uint32_t(call[2])*4,true};
			v.mastering=true; v.submix=true; call.store_pointer(call[1],at);
		} else if (method=="CreateMasteringVoice") {
			if (!call[1] || !mixer().ready()) { call.ret(invalid); return; }
			const auto at=object(call,legacy?"IXAudio2LegacyMasteringVoice":"IXAudio2MasteringVoice");
			auto &v=xvoices[at]; v.mastering=true;
			v.format={call[3]?call.u32(3):AudioMixer::device_rate(),call[2]?uint32_t(call[2]):2,32,8,true};
			call.store_pointer(call[1],at);
		} else if (method=="GetPerformanceData" && call[1]) call.write(call[1],std::vector<uint8_t>(64));
		else if (method!="SetDebugConfiguration") { call.ret(not_impl); return; }
		call.ret(ok); return;
	}
	const auto found=xvoices.find(call[0]);
	if (found==xvoices.end()) { call.ret(invalid); return; }
	auto &v=found->second;
	if (method=="DestroyVoice") {
		const bool reset_master=v.mastering && !v.submix;
		if (v.voice) mixer().stop(v.voice);
		xvoices.erase(found); objects.erase(call[0]); refs.erase(call[0]); context.allocator.release(call[0]);
		if (reset_master) {
			xmaster=1;
			for (auto &[address,source]:xvoices) if (source.voice)
				mixer().gain(source.voice,source.volume,source.volume,uint32_t(source.format.rate*source.ratio));
		}
		call.ret(ok); return;
	}
	if (method=="GetVoiceDetails" && call[1]) {
		call.write(call[1],std::vector<uint8_t>(16));
		call.store<uint32_t>(call[1]+8,v.format.channels); call.store<uint32_t>(call[1]+12,v.format.rate);
	} else if (method=="SetVolume") {
		const auto raw=call.pointer_width()==4?call.u32(1):uint32_t(call.process.api_float_arguments()[1]);
		float volume; std::memcpy(&volume,&raw,4);
		if (!std::isfinite(volume) || volume<0 || volume>16) { call.ret(invalid); return; }
		v.volume=volume;
		if (v.voice) mixer().gain(v.voice,volume*xmaster,volume*xmaster,uint32_t(v.format.rate*v.ratio));
		if (v.mastering && !v.submix) {
			xmaster=volume;
			for (auto &[address,source]:xvoices) if (source.voice)
				mixer().gain(source.voice,source.volume*xmaster,source.volume*xmaster,uint32_t(source.format.rate*source.ratio));
		}
	} else if (method=="GetVolume" && call[1]) call.store<float>(call[1],v.volume);
	else if (method=="Start" && v.voice) { v.running=true; mixer().play(v.voice); }
	else if (method=="Stop" && v.voice) { v.running=false; mixer().stop(v.voice); }
	else if (method=="SubmitSourceBuffer" && v.voice) {
		if (!call[1]) { call.ret(invalid); return; }
		const auto at=call[1]; const auto bytes=call.load<uint32_t>(at+4);
		const auto data=call.load_pointer(at+8);
		const auto base=call.pointer_width()==4?12:16;
		const auto begin=call.load<uint32_t>(at+base), length=call.load<uint32_t>(at+base+4);
		const auto context_at=call.pointer_width()==4?32:36;
		if (!bytes || bytes>32U<<20 || bytes%v.format.block || !data || begin>bytes/v.format.block ||
			(length && length>bytes/v.format.block-begin)) { call.ret(invalid); return; }
		const auto frames=length?length:bytes/v.format.block-begin;
		if (v.packets.size()>=64) { call.ret(0x88960008); return; }
		auto samples=call.read(data+uint64_t(begin)*v.format.block,uint64_t(frames)*v.format.block);
		const auto begin_at=v.submitted;
		mixer().enqueue(v.voice,std::move(samples)); v.submitted+=frames;
		v.packets.push_back({begin_at,v.submitted,call.load_pointer(at+context_at),next_packet++,bool(call.load<uint32_t>(at)&0x40)});
	} else if (method=="FlushSourceBuffers" && v.voice) {
		mixer().reset(v.voice); v.submitted=0; v.packets.clear(); if (v.running) mixer().play(v.voice);
	} else if (method=="GetState" && v.voice && call[1]) {
		const auto width=call.pointer_width(); call.write(call[1],std::vector<uint8_t>(width==4?16:20));
		if (!v.packets.empty()) call.store_pointer(call[1],v.packets.front().context);
		call.store<uint32_t>(call[1]+width,uint32_t(v.packets.size()));
		call.store<uint64_t>(call[1]+(width==4?8:12),mixer().position(v.voice)/v.format.block);
	} else if (method=="SetFrequencyRatio" && v.voice) {
		const auto raw=call.pointer_width()==4?call.u32(1):uint32_t(call.process.api_float_arguments()[1]);
		float ratio; std::memcpy(&ratio,&raw,4); if (!std::isfinite(ratio) || ratio<0.0001f || ratio>1024) { call.ret(invalid); return; }
		v.ratio=ratio; mixer().gain(v.voice,v.volume*xmaster,v.volume*xmaster,uint32_t(v.format.rate*ratio));
	} else if (method=="GetFrequencyRatio" && call[1]) call.store<float>(call[1],v.ratio);
	else if (method=="GetChannelMask" && call[1]) call.store<uint32_t>(call[1],3);
	else if (method=="SetOutputVoices" || method=="Discontinuity" || method=="ExitLoop" || method=="SetOutputMatrix") {}
	else { call.ret(not_impl); return; }
	call.ret(ok);
}
