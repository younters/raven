#include "output.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace raven_darwin {
namespace {
AudioStreamBasicDescription description(uint32_t rate, uint32_t channels)
{
	return {double(rate), kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
		channels * 4, 1, channels * 4, channels, 32, 0};
}
}
float audio_sample(const uint8_t *at, const AudioFormat &format)
{
	if (format.floating) {
		float value;
		std::memcpy(&value, at, 4);
		return std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
	}
	if (format.bits == 8)
		return (int(*at) - 128) / 128.0f;
	if (format.bits == 16) {
		int16_t value;
		std::memcpy(&value, at, 2);
		return value / 32768.0f;
	}
	if (format.bits == 24) {
		const int32_t value = int32_t(uint32_t(at[0]) << 8 | uint32_t(at[1]) << 16 | uint32_t(at[2]) << 24);
		return value / 2147483648.0f;
	}
	int32_t value;
	std::memcpy(&value, at, 4);
	return value / 2147483648.0f;
}

struct AudioOutput::State {
	struct Slot {
		AudioQueueBufferRef buffer = nullptr;
		std::atomic<bool> free{true};
	};
	struct Pending { std::vector<uint8_t> data; size_t offset = 0; };
	struct Marker { uint64_t header, frame; };
	struct Segment { double start; uint64_t count, guest; };
	AudioFormat format;
	AudioQueueRef queue = nullptr;
	AudioQueueBufferRef rendered = nullptr;
	std::array<Slot, 4> slots;
	std::mutex mutex;
	std::condition_variable changed;
	std::deque<Pending> pending;
	std::deque<Marker> markers;
	std::deque<Segment> segments;
	std::vector<uint64_t> finished;
	std::atomic<uint32_t> volume{0xffffffffU};
	std::atomic<uint64_t> callback_count{0};
	uint64_t submitted = 0, dispatched = 0, played = 0, queue_frames = 0, offline_frames = 0;
	uint64_t total_submitted = 0, total_played = 0;
	uint32_t chunk, render_rate, render_chunk;
	bool offline, stopping = false, paused = false, offline_started = false;
	std::atomic<bool> failed{false};
	std::thread worker;

	static void returned(void *context, AudioQueueRef, AudioQueueBufferRef buffer)
	{
		auto &self = *static_cast<State *>(context);
		// No locks, allocation, guest access or AudioQueue calls in the real-time callback.
		for (auto &slot : self.slots)
			if (slot.buffer == buffer) {
				slot.free.store(true, std::memory_order_release);
				self.callback_count.fetch_add(1, std::memory_order_relaxed);
				self.changed.notify_one();
				break;
			}
	}
	bool check(OSStatus status, const char *operation)
	{
		if (!status)
			return true;
		std::fprintf(stderr, "CoreAudio: %s failed (%d)\n", operation, int(status));
		failed = true;
		return false;
	}
	State(AudioFormat value, bool test, uint32_t target_rate) : format(value), chunk(std::max(1U, value.rate / 100)),
		render_rate(target_rate ? target_rate : value.rate), render_chunk(std::max(1U, render_rate / 100)), offline(test)
	{
		if (format.rate < 8000 || format.rate > 384000 || !format.channels || format.channels > 8 ||
			format.block != format.channels * (format.bits / 8) ||
			(format.floating ? format.bits != 32 : format.bits != 8 && format.bits != 16 && format.bits != 24 && format.bits != 32)) {
			failed = true;
			return;
		}
		const auto spec = description(format.rate, format.channels);
		if (!check(AudioQueueNewOutput(&spec, returned, this, nullptr, nullptr, 0, &queue), "create output"))
			return;
		// Leaving CurrentDevice unset tracks the system default, including changes while a stream is open.
		const auto render_spec = description(render_rate, format.channels);
		if (offline && !check(AudioQueueSetOfflineRenderFormat(queue, &render_spec, nullptr), "offline format"))
			return;
		for (auto &slot : slots)
			if (!check(AudioQueueAllocateBuffer(queue, chunk * format.channels * 4, &slot.buffer), "allocate"))
				return;
		if (offline) {
			check(AudioQueueAllocateBuffer(queue, render_chunk * format.channels * 4, &rendered), "allocate render");
		} else {
			fill();
			if (!check(AudioQueueStart(queue, nullptr), "start"))
				return;
			worker = std::thread([this] {
				std::unique_lock lock(mutex);
				while (!stopping && !failed) {
					if (!paused) {
						update();
						fill();
					}
					changed.wait_for(lock, std::chrono::milliseconds(5));
				}
			});
		}
	}
	~State()
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		changed.notify_one();
		if (worker.joinable())
			worker.join();
		if (queue)
			AudioQueueDispose(queue, true); // joins all callbacks before slots/context are destroyed
		if (std::getenv("RAVEN_AUDIO_TRACE"))
			std::fprintf(stderr, "CoreAudio: callbacks=%llu presented_frames=%llu submitted_frames=%llu position_frames=%llu\n",
				(unsigned long long)callback_count.load(), (unsigned long long)total_played,
				(unsigned long long)total_submitted, (unsigned long long)played);
	}
	void update()
	{
		if (!queue || failed)
			return;
		double now = double(offline_frames) * format.rate / render_rate;
		if (!offline) {
			AudioTimeStamp stamp{};
			if (AudioQueueGetCurrentTime(queue, nullptr, &stamp, nullptr) || !(stamp.mFlags & kAudioTimeStampSampleTimeValid))
				return;
			now = stamp.mSampleTime;
		}
		const uint64_t previous = played;
		while (!segments.empty()) {
			const auto &segment = segments.front();
			const uint64_t advanced = uint64_t(std::clamp(now - segment.start, 0.0, double(segment.count)));
			played = std::max(played, segment.guest + advanced);
			if (advanced < segment.count)
				break;
			segments.pop_front();
		}
		total_played += played - previous;
		while (!markers.empty() && markers.front().frame <= played) {
			finished.push_back(markers.front().header);
			markers.pop_front();
		}
	}
	void fill()
	{
		for (auto &slot : slots) {
			if (!slot.free.load(std::memory_order_acquire))
				continue;
			auto *out = static_cast<float *>(slot.buffer->mAudioData);
			std::fill_n(out, chunk * format.channels, 0.0f);
			uint32_t frames = 0;
			const uint32_t gain = volume.load(std::memory_order_relaxed);
			while (frames < chunk && !pending.empty()) {
				auto &item = pending.front();
				const size_t count = std::min<size_t>(chunk - frames, (item.data.size() - item.offset) / format.block);
				for (size_t frame = 0; frame < count; ++frame)
					for (uint32_t channel = 0; channel < format.channels; ++channel) {
						const float scale = float(channel == 1 ? gain >> 16 : gain & 0xffff) / 65535.0f;
						out[(frames + frame) * format.channels + channel] =
							audio_sample(item.data.data() + item.offset + frame * format.block + channel * (format.bits / 8), format) * scale;
					}
				frames += uint32_t(count);
				item.offset += count * format.block;
				if (item.offset == item.data.size())
					pending.pop_front();
			}
			slot.buffer->mAudioDataByteSize = chunk * format.channels * 4;
			slot.free.store(false, std::memory_order_release);
			AudioTimeStamp start{};
			if (!check(AudioQueueEnqueueBufferWithParameters(queue, slot.buffer, 0, nullptr, 0, 0, 0, nullptr, nullptr, &start), "enqueue"))
				return;
			// Query the actual scheduled time: a device change/underrun can insert silence into the queue timeline.
			const double at = start.mFlags & kAudioTimeStampSampleTimeValid ? start.mSampleTime : double(queue_frames);
			if (frames)
				segments.push_back({at, frames, dispatched});
			dispatched += frames;
			queue_frames = uint64_t(std::max(0.0, at)) + chunk;
		}
	}
};

AudioOutput::AudioOutput(AudioFormat format, bool offline, uint32_t render_rate) : state(std::make_unique<State>(format, offline, render_rate)) {}
AudioOutput::~AudioOutput() = default;
bool AudioOutput::ready() const { return state->queue && !state->failed; }
bool AudioOutput::has_default_device()
{
	AudioDeviceID device = kAudioObjectUnknown;
	UInt32 size = sizeof(device);
	const AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	return !AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, &device) && device != kAudioObjectUnknown;
}
void AudioOutput::enqueue(uint64_t header, std::vector<uint8_t> data)
{
	std::lock_guard lock(state->mutex);
	data.resize(data.size() / state->format.block * state->format.block);
	state->submitted += data.size() / state->format.block;
	state->total_submitted += data.size() / state->format.block;
	state->markers.push_back({header, state->submitted});
	if (!data.empty())
		state->pending.push_back({std::move(data)});
	state->changed.notify_one();
}
void AudioOutput::reset()
{
	std::lock_guard lock(state->mutex);
	if (!ready())
		return;
	state->check(AudioQueueStop(state->queue, true), "stop");
	state->check(AudioQueueReset(state->queue), "reset");
	for (const auto &marker : state->markers)
		state->finished.push_back(marker.header);
	state->markers.clear();
	state->segments.clear();
	state->pending.clear();
	state->submitted = state->dispatched = state->played = state->queue_frames = state->offline_frames = 0;
	state->paused = false;
	state->offline_started = false;
	for (auto &slot : state->slots)
		slot.free.store(true, std::memory_order_release);
	if (!state->offline) {
		state->fill();
		state->check(AudioQueueStart(state->queue, nullptr), "restart");
	}
}
void AudioOutput::pause(bool value)
{
	std::lock_guard lock(state->mutex);
	if (state->paused == value || !ready())
		return;
	state->update();
	state->paused = value;
	if (!state->offline)
		state->check(value ? AudioQueuePause(state->queue) : AudioQueueStart(state->queue, nullptr), value ? "pause" : "resume");
}
bool AudioOutput::busy()
{
	std::lock_guard lock(state->mutex);
	state->update();
	return !state->markers.empty();
}
std::vector<uint64_t> AudioOutput::take_finished()
{
	std::lock_guard lock(state->mutex);
	state->update();
	return std::exchange(state->finished, {});
}
uint64_t AudioOutput::position()
{
	std::lock_guard lock(state->mutex);
	state->update();
	return state->played * state->format.block;
}
uint64_t AudioOutput::padding()
{
	std::lock_guard lock(state->mutex);
	state->update();
	return state->submitted - state->played;
}
void AudioOutput::set_volume(uint32_t value) { state->volume.store(value, std::memory_order_relaxed); }
uint32_t AudioOutput::get_volume() const { return state->volume.load(std::memory_order_relaxed); }
uint64_t AudioOutput::callbacks() const { return state->callback_count.load(std::memory_order_relaxed); }
std::vector<float> AudioOutput::render(uint32_t frames)
{
	std::lock_guard lock(state->mutex);
	if (!state->offline || !ready() || state->paused)
		return {};
	frames = std::min(frames, state->render_chunk);
	state->fill();
	if (!state->offline_started) {
		if (!state->check(AudioQueueStart(state->queue, nullptr), "start offline"))
			return {};
		state->offline_started = true;
	}
	AudioTimeStamp stamp{};
	stamp.mFlags = kAudioTimeStampSampleTimeValid;
	stamp.mSampleTime = double(state->offline_frames);
	if (!state->check(AudioQueueOfflineRender(state->queue, &stamp, state->rendered, frames), "render"))
		return {};
	const auto *data = static_cast<const float *>(state->rendered->mAudioData);
	const size_t count = state->rendered->mAudioDataByteSize / sizeof(float);
	state->offline_frames += count / state->format.channels;
	state->update();
	return {data, data + count};
}
}
