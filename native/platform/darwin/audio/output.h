#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace raven_darwin {

struct AudioFormat {
	uint32_t rate, channels, bits, block;
	bool floating;
};

// The queue-level WaveOut contract. Only this object owns CoreAudio resources; no guest pointer crosses the audio thread.
class AudioOutput {
public:
	explicit AudioOutput(AudioFormat format, bool offline = false, uint32_t render_rate = 0);
	~AudioOutput();
	AudioOutput(const AudioOutput &) = delete;
	AudioOutput &operator=(const AudioOutput &) = delete;
	bool ready() const;
	void enqueue(uint64_t header, std::vector<uint8_t> data);
	void reset();
	void pause(bool value);
	bool busy();
	std::vector<uint64_t> take_finished();
	uint64_t position(); // bytes presented, excluding underrun silence
	uint64_t padding(); // guest frames not yet presented (including pending headers)
	void set_volume(uint32_t value);
	uint32_t get_volume() const;
	uint64_t callbacks() const;
	// Deterministic, inaudible contract tests use the very same AudioQueue converter and buffering path.
	std::vector<float> render(uint32_t frames);
	static bool has_default_device();

private:
	struct State;
	std::unique_ptr<State> state;
};

}
