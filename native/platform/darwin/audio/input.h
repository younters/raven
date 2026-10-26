#pragma once
#include "output.h"
namespace raven_darwin {
class AudioInput {
public:
	explicit AudioInput(AudioFormat);
	~AudioInput();
	bool ready() const;
	void start();
	void stop();
	void reset();
	std::vector<uint8_t> take(uint32_t frames);
	uint32_t padding();
	uint64_t position();
	static bool has_default_device();
	static uint32_t device_rate();
	// Does not request access. Automated tests must call this before constructing an input.
	static bool authorized();
private:
	struct State;
	std::unique_ptr<State> state;
};
}
