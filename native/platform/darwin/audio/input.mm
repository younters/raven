#include "input.h"
#import <AVFoundation/AVFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstring>

namespace raven_darwin {
struct AudioInput::State {
	AudioFormat format;
	AudioQueueRef queue = nullptr;
	std::mutex mutex;
	std::deque<uint8_t> bytes;
	uint64_t captured = 0;
	bool running = false;
	static void received(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer, const AudioTimeStamp *, UInt32, const AudioStreamPacketDescription *) {
		auto &s = *static_cast<State *>(context);
		{
			std::lock_guard lock(s.mutex);
			if (s.running) {
				s.captured += buffer->mAudioDataByteSize / s.format.block;
				const auto data = static_cast<uint8_t *>(buffer->mAudioData);
				s.bytes.insert(s.bytes.end(),data,data+buffer->mAudioDataByteSize);
				// A stalled Windows consumer must not grow microphone storage without bound.
				const size_t maximum = s.format.rate * s.format.block;
				if (s.bytes.size()>maximum) s.bytes.erase(s.bytes.begin(),s.bytes.end()-maximum);
			}
		}
		AudioQueueEnqueueBuffer(queue,buffer,0,nullptr);
	}
	explicit State(AudioFormat f) : format(f) {
		@autoreleasepool {
			const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
			if (status == AVAuthorizationStatusNotDetermined) {
				// Only an actual capture open asks. Device enumeration and authorization probes never do.
				dispatch_semaphore_t done = dispatch_semaphore_create(0);
				[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) { dispatch_semaphore_signal(done); }];
				dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);
			}
			if (!authorized()) return;
		}
		AudioStreamBasicDescription spec{double(f.rate),kAudioFormatLinearPCM,
			UInt32(kAudioFormatFlagIsPacked | (f.floating ? kAudioFormatFlagIsFloat : f.bits==8 ? 0 : kAudioFormatFlagIsSignedInteger)),
			f.block,1,f.block,f.channels,f.bits,0};
		if (AudioQueueNewInput(&spec,received,this,nullptr,nullptr,0,&queue)) { queue=nullptr; return; }
		for (int i=0;i<4;++i) {
			AudioQueueBufferRef buffer=nullptr;
			if (AudioQueueAllocateBuffer(queue,f.rate/100*f.block,&buffer) || AudioQueueEnqueueBuffer(queue,buffer,0,nullptr)) {
				AudioQueueDispose(queue,true); queue=nullptr; return;
			}
		}
	}
	~State() { if (queue) AudioQueueDispose(queue,true); }
};
AudioInput::AudioInput(AudioFormat f) : state(std::make_unique<State>(f)) {}
AudioInput::~AudioInput() = default;
bool AudioInput::ready() const { return state->queue; }
bool AudioInput::start() {
	if (!state->queue) return false;
	{ std::lock_guard lock(state->mutex); state->running=true; }
	if (AudioQueueStart(state->queue,nullptr)) { std::lock_guard lock(state->mutex); state->running=false; return false; }
	return true;
}
void AudioInput::stop() {
	{ std::lock_guard lock(state->mutex); state->running=false; }
	if (state->queue) AudioQueuePause(state->queue);
}
void AudioInput::reset() { stop(); std::lock_guard lock(state->mutex); state->bytes.clear(); state->captured=0; }
std::vector<uint8_t> AudioInput::take(uint32_t frames, uint64_t *first) {
	std::lock_guard lock(state->mutex);
	if (first) *first=state->captured-state->bytes.size()/state->format.block;
	const auto count = std::min<size_t>(size_t(frames)*state->format.block,state->bytes.size());
	std::vector<uint8_t> result(state->bytes.begin(),state->bytes.begin()+count);
	state->bytes.erase(state->bytes.begin(),state->bytes.begin()+count); return result;
}
uint32_t AudioInput::padding() { std::lock_guard lock(state->mutex); return uint32_t(state->bytes.size()/state->format.block); }
uint64_t AudioInput::position() { std::lock_guard lock(state->mutex); return state->captured; }
bool AudioInput::authorized() { @autoreleasepool { return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusAuthorized; } }
bool AudioInput::has_default_device() {
	AudioDeviceID device=0; UInt32 size=sizeof(device);
	AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultInputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
	return AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,0,nullptr,&size,&device)==0 && device!=0;
}
uint32_t AudioInput::device_rate() {
	AudioDeviceID device=0; UInt32 size=sizeof(device);
	AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultInputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,0,nullptr,&size,&device) || !device) return 48000;
	Float64 rate=48000; size=sizeof(rate); property.mSelector=kAudioDevicePropertyNominalSampleRate;
	AudioObjectGetPropertyData(device,&property,0,nullptr,&size,&rate);
	return uint32_t(rate);
}
}
