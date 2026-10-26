#pragma once
#include "output.h"
#include <memory>
#include <vector>

namespace raven_darwin {
// Host-owned samples only. Guest copies and event delivery belong to the scheduler.
class AudioMixer {
public:
	struct Voice;
	AudioMixer();
	~AudioMixer();
	std::shared_ptr<Voice> create(AudioFormat format, bool circular, std::vector<uint8_t> data = {});
	void update(const std::shared_ptr<Voice> &, std::vector<uint8_t> data);
	void enqueue(const std::shared_ptr<Voice> &, std::vector<uint8_t> data);
	void play(const std::shared_ptr<Voice> &, bool looping = false);
	void stop(const std::shared_ptr<Voice> &);
	void reset(const std::shared_ptr<Voice> &);
	void seek(const std::shared_ptr<Voice> &, uint32_t bytes);
	void gain(const std::shared_ptr<Voice> &, float left, float right, uint32_t frequency = 0);
	uint64_t position(const std::shared_ptr<Voice> &);
	uint64_t padding(const std::shared_ptr<Voice> &);
	bool playing(const std::shared_ptr<Voice> &);
	void notifications(const std::shared_ptr<Voice> &, std::vector<std::pair<uint32_t, uint64_t>> points);
	std::vector<uint64_t> take_events();
	bool ready() const;
	static uint32_t device_rate();
private:
	struct State;
	std::unique_ptr<State> state;
};
float audio_sample(const uint8_t *, const AudioFormat &);
}
