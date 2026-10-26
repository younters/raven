#include "mixer.h"
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace raven_darwin {
struct AudioMixer::Voice {
	AudioFormat format;
	std::vector<uint8_t> data;
	bool circular, running = false, looping = false, draining = false;
	float left = 1, right = 1;
	uint32_t frequency;
	double cursor = 0;
	uint64_t presented = 0, submitted = 0, base = 0;
	struct Mark { uint64_t at, position; bool end; };
	std::deque<Mark> marks;
	std::vector<std::pair<uint32_t, uint64_t>> notifications;
};
struct AudioMixer::State {
	uint32_t rate = device_rate();
	AudioOutput output{{rate, 2, 32, 8, true}};
	std::mutex mutex;
	bool stopping = false;
	std::vector<std::weak_ptr<Voice>> voices;
	std::vector<uint64_t> events;
	uint64_t dispatched = 0;
	std::thread worker;
	State() : worker([this] {
		for (;;) {
			{
				std::lock_guard lock(mutex);
				if (stopping) return;
				refresh();
				const bool active = std::any_of(voices.begin(),voices.end(),[](const auto &weak) {
					auto voice = weak.lock(); return voice && voice->running;
				});
				if (active && output.ready()) while (output.padding() < rate / 20) mix();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}) {}
	~State() {
		{ std::lock_guard lock(mutex); stopping = true; }
		worker.join();
	}
	void stopped(Voice &v) {
		for (const auto &[offset, event] : v.notifications)
			if (offset == UINT32_MAX) events.push_back(event);
	}
	void refresh() {
		const auto now = output.position() / 8;
		for (auto it = voices.begin(); it != voices.end();) {
			auto voice = it->lock();
			if (!voice) { it = voices.erase(it); continue; }
			auto &v = *voice;
			while (!v.marks.empty() && v.marks.front().at <= now) {
				const auto mark = v.marks.front(); v.marks.pop_front();
				const uint64_t size = v.data.size() / v.format.block;
				for (const auto &[offset, event] : v.notifications) {
					if (offset == UINT32_MAX || !size) continue;
					const uint64_t frame = offset / v.format.block;
					const auto before = v.presented;
					uint64_t occurrence = before / size * size + frame;
					if (occurrence < before) occurrence += size;
					if (occurrence < mark.position && mark.position > before) events.push_back(event);
				}
				v.presented = mark.position;
				if (mark.end) { v.running = false; v.draining = false; stopped(v); }
			}
			++it;
		}
	}
	void mix() {
		const uint32_t frames = rate / 100;
		std::vector<float> mixed(frames * 2, 0);
		for (const auto &weak : voices) {
			auto ptr = weak.lock(); if (!ptr) continue;
			auto &v = *ptr;
			if (!v.running || v.draining) continue;
			const uint64_t count = v.data.size() / v.format.block + v.base;
			const double step = double(v.frequency) / rate;
			bool end = false;
			uint32_t used = 0;
			for (; used < frames; ++used) {
				uint64_t frame = uint64_t(v.cursor);
				if (!count || (frame >= count && !(v.circular && v.looping))) { end = v.circular; break; }
				const auto next = (frame + 1 < count || (v.circular && v.looping)) ? (frame + 1) % count : frame % count;
				const float fraction = float(v.cursor - std::floor(v.cursor));
				for (uint32_t channel = 0; channel < 2; ++channel) {
					const uint32_t source = v.format.channels == 1 ? 0 : channel;
					const auto at = (v.circular ? frame % count : frame - v.base) * v.format.block + source * (v.format.bits / 8);
					const auto to = (v.circular ? next : next - v.base) * v.format.block + source * (v.format.bits / 8);
					const float a = audio_sample(v.data.data() + at, v.format), b = audio_sample(v.data.data() + to, v.format);
					mixed[used * 2 + channel] += (a + (b - a) * fraction) * (channel ? v.right : v.left);
				}
				v.cursor += step;
				if (!(v.circular && v.looping)) v.cursor = std::min(v.cursor,double(count));
			}
			if (v.circular && !v.looping && v.cursor >= count) end = true;
			v.marks.push_back({dispatched + used, uint64_t(v.cursor), end});
			if (end) v.draining = true;
			if (!v.circular && v.presented > v.base + 4096) {
				const auto discard = std::min(v.presented - v.base, uint64_t(v.data.size() / v.format.block));
				v.data.erase(v.data.begin(),v.data.begin()+discard*v.format.block); v.base += discard;
			}
		}
		for (auto &value : mixed) value = std::clamp(value, -1.0f, 1.0f);
		std::vector<uint8_t> bytes(mixed.size() * 4);
		std::memcpy(bytes.data(), mixed.data(), bytes.size());
		output.take_finished();
		output.enqueue(0, std::move(bytes));
		dispatched += frames;
	}
};
AudioMixer::AudioMixer() : state(std::make_unique<State>()) {}
AudioMixer::~AudioMixer() = default;
std::shared_ptr<AudioMixer::Voice> AudioMixer::create(AudioFormat f, bool circular, std::vector<uint8_t> data) {
	std::lock_guard lock(state->mutex);
	auto v = std::make_shared<Voice>(); v->format = f; v->frequency = f.rate; v->circular = circular; v->data = std::move(data);
	state->voices.push_back(v); return v;
}
void AudioMixer::update(const std::shared_ptr<Voice> &v, std::vector<uint8_t> data) { std::lock_guard lock(state->mutex); v->data = std::move(data); }
void AudioMixer::enqueue(const std::shared_ptr<Voice> &v, std::vector<uint8_t> data) {
	std::lock_guard lock(state->mutex);
	// Absolute submitted frames stay independent of the compacted queue storage.
	v->submitted += data.size() / v->format.block;
	v->data.insert(v->data.end(), data.begin(), data.end());
}
void AudioMixer::play(const std::shared_ptr<Voice> &v, bool loop) { std::lock_guard lock(state->mutex); v->looping = loop; v->running = true; v->draining = false; }
void AudioMixer::stop(const std::shared_ptr<Voice> &v) { std::lock_guard lock(state->mutex); state->refresh(); if (v->running) state->stopped(*v); v->running = false; v->draining = false; v->marks.clear(); v->cursor = double(v->presented); }
void AudioMixer::reset(const std::shared_ptr<Voice> &v) { stop(v); std::lock_guard lock(state->mutex); v->cursor = 0; v->presented = 0; v->submitted = 0; v->base = 0; if (!v->circular) v->data.clear(); }
void AudioMixer::seek(const std::shared_ptr<Voice> &v, uint32_t bytes) { std::lock_guard lock(state->mutex); v->marks.clear(); v->cursor = double(bytes / v->format.block); v->presented = uint64_t(v->cursor); }
void AudioMixer::gain(const std::shared_ptr<Voice> &v, float left, float right, uint32_t frequency) { std::lock_guard lock(state->mutex); v->left = left; v->right = right; if (frequency) v->frequency = frequency; }
uint64_t AudioMixer::position(const std::shared_ptr<Voice> &v) { std::lock_guard lock(state->mutex); state->refresh(); return v->presented * v->format.block; }
uint64_t AudioMixer::padding(const std::shared_ptr<Voice> &v) { std::lock_guard lock(state->mutex); state->refresh(); return v->submitted > v->presented ? v->submitted - v->presented : 0; }
bool AudioMixer::playing(const std::shared_ptr<Voice> &v) { std::lock_guard lock(state->mutex); state->refresh(); return v->running; }
void AudioMixer::notifications(const std::shared_ptr<Voice> &v, std::vector<std::pair<uint32_t, uint64_t>> points) { std::lock_guard lock(state->mutex); v->notifications = std::move(points); }
std::vector<uint64_t> AudioMixer::take_events() { std::lock_guard lock(state->mutex); state->refresh(); return std::exchange(state->events, {}); }
uint64_t AudioMixer::clock() { return state->output.position()/8; }
uint64_t AudioMixer::callbacks() { return state->output.callbacks(); }
bool AudioMixer::ready() const { return state->output.ready(); }
uint32_t AudioMixer::device_rate() {
	AudioDeviceID device = 0; UInt32 size = sizeof(device);
	AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, &device)) return 48000;
	Float64 rate = 48000; size = sizeof(rate); property.mSelector = kAudioDevicePropertyNominalSampleRate;
	AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &rate);
	return uint32_t(rate);
}
}
