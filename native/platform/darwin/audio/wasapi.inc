// Included inside State: transport and lifetime are shared with the host COM implementation.
struct Client {
	AudioFormat format{AudioMixer::device_rate(),2,32,8,true};
	std::shared_ptr<AudioMixer::Voice> voice;
	std::unique_ptr<AudioInput> input;
	std::vector<uint8_t> capture_pending;
	uint32_t capacity = 0, flags = 0, acquired = 0;
	uint64_t memory = 0, event = 0, clock_start = 0, clock_elapsed = 0, capture_first = 0;
	float volume = 1;
	bool muted = false, initialized = false, running = false, locked = false, capture = false;
};
std::map<uint64_t,std::shared_ptr<Client>> clients;
std::map<uint64_t,uint32_t> device_flows, collection_flows;
std::vector<uint64_t> endpoint_callbacks;
std::chrono::steady_clock::time_point last_event{};
std::chrono::steady_clock::time_point last_trace{};
AudioDeviceID default_output = 0;
float endpoint_gain = 1;
bool endpoint_mute = false;
void notification_reference(uint64_t callback, size_t index) {
	const size_t width=context.process.mode()==NativeMode::x86?4:8;
	const auto table_bytes=context.process.read_memory(callback,width);
	uint64_t table=0; std::memcpy(&table,table_bytes.data(),width);
	const auto method_bytes=context.process.read_memory(table+index*width,width);
	uint64_t method=0; std::memcpy(&method,method_bytes.data(),width);
	const std::array<uint64_t,1> args{callback}; context.call(method,args);
}
uint64_t text(ApiCall &call, std::u16string_view value) {
	const auto at = context.allocator.allocate((value.size()+1)*2);
	call.put_wide(at,value,value.size()+1); return at;
}
uint64_t mix_format(ApiCall &call,bool capture=false) {
	const auto at = context.allocator.allocate(40);
	write_format(call,at,{capture?AudioInput::device_rate():AudioMixer::device_rate(),2,32,8,true});
	// WAVEFORMATEXTENSIBLE: stereo IEEE float, explicitly describe channels and valid bits.
	call.store<uint16_t>(at,0xfffe); call.store<uint16_t>(at+16,22);
	call.store<uint16_t>(at+18,32); call.store<uint32_t>(at+20,3);
	const std::array<uint8_t,16> floating{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
	call.write(at+24,floating); return at;
}
void client_gain(Client &client) {
	if (client.voice) {
		const auto gain = client.muted || endpoint_mute ? 0 : client.volume * endpoint_gain;
		mixer().gain(client.voice,gain,gain);
	}
}
void wasapi(ApiCall &call, std::string_view type, std::string_view method) {
	if (type == "IMMDeviceEnumerator") {
		if (method == "EnumAudioEndpoints") {
			if (!call[3] || call[1] > 2) { call.ret(invalid); return; }
			const auto address = object(call,"IMMDeviceCollection");
			collection_flows[address] = call.u32(1);
			collections[address] = (call[2]&1) ? (call[1]==2 ? uint32_t(AudioOutput::has_default_device())+uint32_t(AudioInput::has_default_device()) :
				call[1]==1 ? uint32_t(AudioInput::has_default_device()) : uint32_t(AudioOutput::has_default_device())) : 0;
			call.store_pointer(call[3],address);
		} else if (method == "GetDefaultAudioEndpoint") {
			if (!call[3] || call[1]>1 || call[2]>2) { call.ret(invalid); return; }
			call.store_pointer(call[3],0);
			if (!(call[1] == 1 ? AudioInput::has_default_device() : AudioOutput::has_default_device())) { call.ret(0x80070490); return; }
			const auto at=object(call,"IMMDevice"); device_flows[at]=call.u32(1); call.store_pointer(call[3],at);
		} else if (method == "GetDevice") {
			if (!call[1] || !call[2]) { call.ret(invalid); return; }
			call.store_pointer(call[2],0);
			const auto id=call.wide(call[1]);
			if (id != u"{raven.coreaudio.default.render}" && id != u"{raven.coreaudio.default.capture}") { call.ret(0x80070490); return; }
			const auto at=object(call,"IMMDevice"); device_flows[at]=id.ends_with(u"capture}")?1:0; call.store_pointer(call[2],at);
		} else if (method == "RegisterEndpointNotificationCallback") {
			if (!call[1]) { call.ret(invalid); return; }
			if (std::find(endpoint_callbacks.begin(),endpoint_callbacks.end(),call[1]) == endpoint_callbacks.end()) {
				notification_reference(call[1],1); endpoint_callbacks.push_back(call[1]);
			}
		} else if (method == "UnregisterEndpointNotificationCallback") {
			if (std::erase(endpoint_callbacks,call[1])) notification_reference(call[1],2);
		}
		else { call.ret(not_impl); return; }
		call.ret(ok); return;
	}
	if (type == "IMMDeviceCollection") {
		if (method == "GetCount" && call[1]) call.store<uint32_t>(call[1],collections[call[0]]);
		else if (method == "Item" && call[2]) {
			call.store_pointer(call[2],0);
			if (call[1]>=collections[call[0]]) { call.ret(invalid); return; }
			const auto at=object(call,"IMMDevice");
			const auto flow=collection_flows[call[0]];
			device_flows[at]=flow==2 ? (AudioOutput::has_default_device() && call[1]==0 ? 0 : 1) : flow;
			call.store_pointer(call[2],at);
		} else { call.ret(invalid); return; }
		call.ret(ok); return;
	}
	if (type == "IMMDevice" || type == "IMMEndpoint") {
		const auto flow=device_flows[call[0]];
		if (method == "GetId" && call[1]) call.store_pointer(call[1],text(call,flow?u"{raven.coreaudio.default.capture}":u"{raven.coreaudio.default.render}"));
		else if (method == "GetState" && call[1]) call.store<uint32_t>(call[1],(flow?AudioInput::has_default_device():AudioOutput::has_default_device())?1:4);
		else if (method == "GetDataFlow" && call[1]) call.store<uint32_t>(call[1],flow);
		else if (method == "OpenPropertyStore" && call[2]) call.store_pointer(call[2],object(call,"IPropertyStore"));
		else if (method == "Activate" && call[1] && call[4]) {
			call.store_pointer(call[4],0);
			const auto requested = interface_for(call,call[1]);
			if (std::getenv("RAVEN_AUDIO_TRACE")) {
				std::fprintf(stderr,"Raven audio: Activate IID %.*s bytes",int(requested.size()),requested.data());
				for (auto byte:call.read(call[1],16)) std::fprintf(stderr," %02x",byte);
				std::fprintf(stderr,"\n");
			}
			if (requested == "IAudioClient" || requested == "IAudioClient2" || requested == "IAudioClient3") {
				const auto address = object(call,"IAudioClient3"); clients[address] = std::make_shared<Client>(); clients[address]->capture=flow;
				call.store_pointer(call[4],address);
			} else if (requested == "IAudioSessionManager" || requested == "IAudioSessionManager2") {
				const auto address=object(call,"IAudioSessionManager2"); clients[address]=std::make_shared<Client>();
				call.store_pointer(call[4],address);
			} else if (requested == "IAudioEndpointVolume") call.store_pointer(call[4],object(call,"IAudioEndpointVolume"));
			else { call.ret(no_interface); return; }
		} else { call.ret(invalid); return; }
		call.ret(ok); return;
	}
	if (type == "IPropertyStore") {
		if (method == "GetCount" && call[1]) call.store<uint32_t>(call[1],1);
		else if (method == "GetAt" && call[2] && call[1] == 0) {
			const std::array<uint8_t,20> key{0x4e,0x25,0x5c,0xa4,0x1c,0xdf,0xfd,0x4e,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0,14,0,0,0};
			call.write(call[2],key);
		} else if (method == "GetValue" && call[1] && call[2]) {
			call.write(call[2],std::vector<uint8_t>(call.pointer_width()==4?16:24));
			const auto key = call.read(call[1],20);
			const std::array<uint8_t,20> friendly{0x4e,0x25,0x5c,0xa4,0x1c,0xdf,0xfd,0x4e,0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0,14,0,0,0};
			if (std::equal(key.begin(),key.end(),friendly.begin())) {
				call.store<uint16_t>(call[2],31); call.store_pointer(call[2]+8,text(call,u"CoreAudio Default Device"));
			}
		} else { call.ret(0x80030005); return; }
		call.ret(ok); return;
	}
	if (type == "IAudioEndpointVolume") {
		if (method == "GetChannelCount" && call[1]) call.store<uint32_t>(call[1],2);
		else if (method == "SetMasterVolumeLevelScalar" || method == "SetMasterVolumeLevel") {
			float value; const uint32_t raw = call.pointer_width()==4 ? call.u32(1) : uint32_t(call.process.api_float_arguments()[1]); std::memcpy(&value,&raw,4);
			if (!std::isfinite(value) || (method.ends_with("Scalar") && (value<0 || value>1))) { call.ret(invalid); return; }
			endpoint_gain = method.ends_with("Scalar") ? value : std::pow(10.f,value/20.f);
			for (auto &[address,c] : clients) client_gain(*c);
		} else if (method == "GetMasterVolumeLevelScalar" && call[1]) call.store<float>(call[1],endpoint_gain);
		else if (method == "GetMasterVolumeLevel" && call[1]) call.store<float>(call[1],20.f*std::log10(std::max(endpoint_gain,0.00001f)));
		else if (method == "SetMute") { endpoint_mute = call[1]!=0; for (auto &[address,c] : clients) client_gain(*c); }
		else if (method == "GetMute" && call[1]) call.store<uint32_t>(call[1],endpoint_mute);
		else if (method == "QueryHardwareSupport" && call[1]) call.store<uint32_t>(call[1],0);
		else if (method == "GetVolumeRange" && call[1] && call[2] && call[3]) { call.store<float>(call[1],-100); call.store<float>(call[2],0); call.store<float>(call[3],1); }
		else { call.ret(not_impl); return; }
		call.ret(ok); return;
	}
	if (!clients.contains(call[0])) { call.ret(invalid); return; }
	auto &c = *clients.at(call[0]);
	if (type == "IAudioSessionManager2") {
		if ((method == "GetAudioSessionControl" || method == "GetSimpleAudioVolume") && call[3]) {
			const auto at=object(call,method == "GetAudioSessionControl" ? "IAudioSessionControl" : "ISimpleAudioVolume");
			clients[at]=clients.at(call[0]); call.store_pointer(call[3],at);
		} else if (method == "RegisterSessionNotification" || method == "UnregisterSessionNotification" ||
			method == "RegisterDuckNotification" || method == "UnregisterDuckNotification") {}
		else { call.ret(not_impl); return; }
		call.ret(ok); return;
	}
	if (type == "IAudioSessionControl") {
		if (method == "GetState" && call[1]) call.store<uint32_t>(call[1],c.voice && c.running ? 1 : 0);
		else if ((method == "GetDisplayName" || method == "GetIconPath") && call[1]) call.store_pointer(call[1],text(call,u""));
		else if (method == "GetGroupingParam" && call[1]) call.write(call[1],std::array<uint8_t,16>{});
		else if (method == "SetDisplayName" || method == "SetIconPath" || method == "SetGroupingParam" ||
			method == "RegisterAudioSessionNotification" || method == "UnregisterAudioSessionNotification") {}
		else { call.ret(not_impl); return; }
		call.ret(ok); return;
	}
	constexpr uint32_t uninitialized=0x88890001, already=0x88890002, too_large=0x88890006, out_of_order=0x88890007, not_stopped=0x88890005;
	if (type == "IAudioClient3") {
		if (method == "GetMixFormat" && call[1]) call.store_pointer(call[1],mix_format(call,c.capture));
		else if (method == "GetDevicePeriod") { if (call[1]) call.store<uint64_t>(call[1],100000); if (call[2]) call.store<uint64_t>(call[2],100000); }
		else if (method == "IsFormatSupported" && call[2]) {
			if (call[3]) call.store_pointer(call[3],0);
			if (call[1]) { call.ret(0x8889000e); return; }
			if (!format(call,call[2])) { call.ret(0x88890008); return; }
		} else if (method == "Initialize" || method == "InitializeSharedAudioStream") {
			if (c.initialized) { call.ret(already); return; }
			const bool standard = method == "Initialize";
			const bool x86 = call.pointer_width() == 4;
			if (standard && call[1]) { call.ret(0x8889000e); return; }
			const uint64_t fmt = call[standard ? (x86?7:5) : 3];
			auto f = format(call,fmt);
			if (!f) { call.ret(0x88890008); return; }
			c.format = *f; c.flags = call.u32(standard?2:1);
			uint64_t duration = standard ? call[3] : uint64_t(call.u32(2))*10000000/f->rate;
			if (standard && x86) duration |= call[4]<<32;
			if (duration > 20000000) { call.ret(invalid); return; }
			c.capacity = std::max<uint64_t>(f->rate/10,(duration*f->rate+9999999)/10000000);
			c.memory = context.allocator.allocate(uint64_t(c.capacity)*f->block);
			if (c.capture) {
				c.input=std::make_unique<AudioInput>(*f);
				if (!c.input->ready()) { c.input.reset(); context.allocator.release(c.memory); c.memory=0; call.ret(0x88890004); return; }
			} else { c.voice = mixer().create(*f,false); client_gain(c); }
			c.initialized=true;
		} else if (method == "SetClientProperties") { /* No hardware offload. */ }
		else if (method == "IsOffloadCapable" && call[2]) call.store<uint32_t>(call[2],0);
		else if (method == "GetSharedModeEnginePeriod" && call[2] && call[3] && call[4] && call[5]) {
			auto f = format(call,call[1]); if (!f) { call.ret(invalid); return; }
			for (int i=2;i<=5;++i) call.store<uint32_t>(call[i],f->rate/100);
		} else if (method == "GetCurrentSharedModeEnginePeriod" && call[1] && call[2]) { call.store_pointer(call[1],mix_format(call,c.capture)); call.store<uint32_t>(call[2],(c.capture?AudioInput::device_rate():AudioMixer::device_rate())/100); }
		else if (!c.initialized) { call.ret(uninitialized); return; }
		else if (method == "GetBufferSize" && call[1]) call.store<uint32_t>(call[1],c.capacity);
		else if (method == "GetStreamLatency" && call[1]) call.store<uint64_t>(call[1],400000);
		else if (method == "GetCurrentPadding" && call[1]) call.store<uint32_t>(call[1],c.capture?uint32_t(std::min<uint64_t>(c.capacity,c.input->padding()+c.capture_pending.size()/c.format.block)):uint32_t(std::min<uint64_t>(c.capacity,mixer().padding(c.voice))));
		else if (method == "SetEventHandle") { if (!call[1] || !(c.flags&0x40000)) { call.ret(invalid); return; } c.event = call[1]; }
		else if (method == "Start") {
			if (c.running) { call.ret(0x8889000a); return; }
			if ((c.flags&0x40000) && !c.event) { call.ret(0x88890014); return; }
			c.clock_start=c.capture?c.input->position():mixer().clock();
			if (c.capture && !c.input->start()) { call.ret(0x88890004); return; }
			c.running=true;
			if (!c.capture) mixer().play(c.voice);
		} else if (method == "Stop") { if (c.running) c.clock_elapsed+=(c.capture?c.input->position():mixer().clock())-c.clock_start; c.running=false; if (c.capture) c.input->stop(); else mixer().stop(c.voice); }
		else if (method == "Reset") { if (c.running) { call.ret(not_stopped); return; } if (c.locked) { call.ret(out_of_order); return; } if (c.capture) { c.input->reset(); c.capture_pending.clear(); c.capture_first=0; } else mixer().reset(c.voice); c.clock_elapsed=0; }
		else if (method == "GetService" && call[1] && call[2]) {
			call.store_pointer(call[2],0); const auto type = interface_for(call,call[1]);
			if ((type != "IAudioRenderClient" && type != "IAudioCaptureClient" && type != "IAudioClock" && type != "ISimpleAudioVolume") ||
				(type == "IAudioCaptureClient") != c.capture && (type == "IAudioCaptureClient" || type == "IAudioRenderClient")) { call.ret(no_interface); return; }
			const auto at = object(call,std::string(type)); clients[at] = clients.at(call[0]); call.store_pointer(call[2],at);
		} else { call.ret(not_impl); return; }
	} else if (type == "IAudioRenderClient") {
		if (method == "GetBuffer") {
			if (!call[2]) { call.ret(invalid); return; } call.store_pointer(call[2],0);
			if (c.locked) { call.ret(out_of_order); return; }
			if (call[1] > c.capacity-mixer().padding(c.voice)) { call.ret(too_large); return; }
			if (call[1]) { c.acquired=call.u32(1); c.locked=true; call.store_pointer(call[2],c.memory); }
		} else if (method == "ReleaseBuffer") {
			if (!c.locked && call[1]) { call.ret(out_of_order); return; }
			if (call[1]>c.acquired || (call[2]&~2)) { call.ret(invalid); return; }
			if (call[1]) {
				auto bytes = (call[2]&2) ? std::vector<uint8_t>(call[1]*c.format.block,c.format.bits==8?128:0) : call.read(c.memory,call[1]*c.format.block);
				mixer().enqueue(c.voice,std::move(bytes));
			}
			c.locked=false; c.acquired=0;
		} else { call.ret(not_impl); return; }
	} else if (type == "IAudioCaptureClient") {
		if (method == "GetNextPacketSize" && call[1]) call.store<uint32_t>(call[1],c.capture_pending.empty()?std::min(c.capacity,c.input->padding()):uint32_t(c.capture_pending.size()/c.format.block));
		else if (method == "GetBuffer") {
			if (!call[1] || !call[2] || !call[3]) { call.ret(invalid); return; }
			if (c.locked) { call.ret(out_of_order); return; }
			if (c.capture_pending.empty()) {
				c.capture_pending=c.input->take(std::min(c.capacity,c.input->padding()),&c.capture_first);
			}
			const auto frames=uint32_t(c.capture_pending.size()/c.format.block);
			call.store_pointer(call[1],frames?c.memory:0); call.store<uint32_t>(call[2],frames); call.store<uint32_t>(call[3],0);
			if (frames) {
				call.write(c.memory,c.capture_pending); c.locked=true; c.acquired=frames;
				if (call[4]) call.store<uint64_t>(call[4],c.capture_first);
				if (call[5]) call.store<uint64_t>(call[5],counter_ticks());
			}
		} else if (method == "ReleaseBuffer") {
			if (!c.locked || call[1]>c.acquired) { call.ret(out_of_order); return; }
			c.capture_pending.erase(c.capture_pending.begin(),c.capture_pending.begin()+call[1]*c.format.block);
			c.capture_first+=call[1];
			c.locked=false; c.acquired=0;
		} else { call.ret(not_impl); return; }
	} else if (type == "IAudioClock") {
		if (method == "GetFrequency" && call[1]) call.store<uint64_t>(call[1],c.format.rate);
		else if (method == "GetPosition" && call[1]) { call.store<uint64_t>(call[1],c.capture?c.input->position():(c.clock_elapsed+(c.running?mixer().clock()-c.clock_start:0))*c.format.rate/AudioMixer::device_rate()); if (call[2]) call.store<uint64_t>(call[2],counter_ticks()); }
		else if (method == "GetCharacteristics" && call[1]) call.store<uint32_t>(call[1],1);
		else { call.ret(invalid); return; }
	} else if (type == "ISimpleAudioVolume") {
		if (method == "SetMasterVolume") {
			const uint32_t raw=call.pointer_width()==4 ? call.u32(1) : uint32_t(call.process.api_float_arguments()[1]); float value; std::memcpy(&value,&raw,4);
			if (!std::isfinite(value) || value<0 || value>1) { call.ret(invalid); return; } c.volume=value; client_gain(c);
		} else if (method == "GetMasterVolume" && call[1]) call.store<float>(call[1],c.volume);
		else if (method == "SetMute") { c.muted=call[1]!=0; client_gain(c); }
		else if (method == "GetMute" && call[1]) call.store<uint32_t>(call[1],c.muted);
		else { call.ret(invalid); return; }
	} else { call.ret(not_impl); return; }
	call.ret(ok);
}
std::map<uint64_t,uint32_t> collections;
void service_clients() {
	const auto now = std::chrono::steady_clock::now();
	if (now-last_event < std::chrono::milliseconds(10)) return;
	last_event=now;
	if (std::getenv("RAVEN_AUDIO_TRACE") && mixer_ && now-last_trace>std::chrono::seconds(1)) {
		last_trace=now;
		std::fprintf(stderr,"Raven audio: clients=%zu clock=%llu CoreAudio callbacks=%llu\n",clients.size(),
			(unsigned long long)mixer().clock(),(unsigned long long)mixer().callbacks());
	}
	AudioDeviceID current=0; UInt32 size=sizeof(current);
	AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
	if (!AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,0,nullptr,&size,&current)) {
		if (default_output && default_output!=current && !endpoint_callbacks.empty()) {
			constexpr std::u16string_view id=u"{raven.coreaudio.default.render}";
			const auto address=context.allocator.allocate((id.size()+1)*2);
			std::vector<uint8_t> bytes((id.size()+1)*2);
			std::memcpy(bytes.data(),id.data(),id.size()*2); context.process.write_memory(address,bytes);
			const size_t width=context.process.mode()==NativeMode::x86?4:8;
			for (const auto callback:endpoint_callbacks) try {
				const auto table_data=context.process.read_memory(callback,width);
				uint64_t table=0; std::memcpy(&table,table_data.data(),width);
				const auto method_data=context.process.read_memory(table+4*width,width);
				uint64_t method=0; std::memcpy(&method,method_data.data(),width);
				for (uint64_t role=0;role<3;++role) {
					const std::array<uint64_t,4> args{callback,0,role,address}; context.call(method,args);
				}
			} catch (...) {}
			context.allocator.release(address);
		}
		default_output=current;
	}
	for (const auto &[address,c] : clients)
		if (c->running && c->event && (c->capture ? !c->capture_pending.empty() || c->input->padding()>0 : mixer().padding(c->voice)<c->capacity)) context.objects.set_event(c->event,true);
}
