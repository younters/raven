#include "output.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void require(bool value, const char *what)
{
	if (!value) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++failures;
	}
}
}
int main(int argc, char **)
{
	using raven_darwin::AudioOutput;
	if (argc > 1) {
		// Real-device verification stays entirely silent. Callbacks and the presentation clock prove hardware delivery.
		if (!AudioOutput::has_default_device())
			return 77;
		AudioOutput output({44100, 2, 16, 4, false});
		require(output.ready(), "real device opens");
		if (!output.ready())
			return 1;
		output.enqueue(1, std::vector<uint8_t>(44100 / 2 * 4));
		for (int attempt = 0; attempt < 200 && output.busy(); ++attempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		require(!output.busy(), "hardware presents half a second of silence");
		require(output.position() == 44100 / 2 * 4, "hardware presentation clock reaches end");
		require(output.callbacks() > 0, "CoreAudio returned buffers");
		std::printf("CoreAudio real device: callbacks=%llu bytes=%llu padding=%llu\n",
			(unsigned long long)output.callbacks(), (unsigned long long)output.position(), (unsigned long long)output.padding());
		return failures ? 1 : 0;
	}
	AudioOutput unsupported({1000, 2, 16, 4, false}, true);
	require(!unsupported.ready(), "unsupported sample rate is rejected before playback");
	for (uint32_t rate : {8000U, 44100U, 48000U, 96000U, 192000U, 384000U})
		for (uint32_t channels : {1U, 2U, 6U, 8U})
			for (uint32_t bits : {8U, 16U, 24U, 32U}) {
				const uint32_t block = channels * (bits / 8), frames = rate / 100;
				AudioOutput output({rate, channels, bits, block, false}, true);
				require(output.ready(), "offline integer format opens");
				if (!output.ready())
					continue;
				std::vector<uint8_t> samples(frames * block, 0);
				// Both signs of 0.25 full scale (unsigned PCM8 has its zero at 128).
				for (size_t offset = 0; offset < samples.size(); offset += bits / 8)
					samples[offset + bits / 8 - 1] = offset / block < frames / 2 ? (bits == 8 ? 160 : 32) : (bits == 8 ? 96 : 224);
				output.enqueue(11, samples);
				require(output.padding() == frames, "padding includes pending guest frames");
				const auto rendered = output.render(frames);
				require(rendered.size() == frames * channels, "offline renderer supplies requested frames");
				if (!rendered.empty())
					require(std::abs(rendered[channels * (frames / 4)] - 0.25f) < 0.001f &&
						std::abs(rendered[channels * (frames * 3 / 4)] + 0.25f) < 0.001f, "signed integer PCM conversion");
				require(output.position() == samples.size(), "clock advances by rendered guest bytes");
				require(output.padding() == 0 && !output.busy(), "padding drains");
				require(output.take_finished() == std::vector<uint64_t>{11}, "header completes exactly once");
				output.render(frames);
				require(output.position() == samples.size(), "underrun silence does not advance guest position");
				output.enqueue(12, samples);
				output.pause(true);
				require(output.render(frames).empty(), "pause freezes rendering");
				output.reset();
				require(output.position() == 0 && !output.busy(), "reset discards pending frames and resets clock");
				require(output.take_finished() == std::vector<uint64_t>{12}, "reset returns pending header once");
				output.enqueue(13, samples);
				output.render(frames);
				require(output.position() == samples.size(), "new stream advances after reset");
				require(output.take_finished() == std::vector<uint64_t>{13}, "new header completes after reset");
			}
	AudioOutput floating({48000, 2, 32, 8, true}, true);
	require(floating.ready(), "float opens");
	std::vector<float> values(960, 0.5f);
	std::vector<uint8_t> bytes(values.size() * 4);
	std::memcpy(bytes.data(), values.data(), bytes.size());
	floating.set_volume(0x80000000U);
	floating.enqueue(1, bytes);
	const auto scaled = floating.render(480);
	require(scaled.size() == 960, "float render count");
	if (scaled.size() == 960)
		require(scaled[100] == 0 && std::abs(scaled[101] - 0.25f) < 0.001f, "left mute and right volume");
	floating.reset();
	floating.set_volume(0xffffffffU);
	values[0] = std::numeric_limits<float>::quiet_NaN();
	values[1] = std::numeric_limits<float>::infinity();
	values[2] = 1.5f;
	values[3] = -2.0f;
	std::memcpy(bytes.data(), values.data(), bytes.size());
	floating.enqueue(2, bytes);
	const auto clipped = floating.render(480);
	require(clipped.size() == 960 && clipped[0] == 0 && clipped[1] == 0 && clipped[2] == 1 && clipped[3] == -1,
		"non-finite floats are silent and out-of-range floats are clipped");
	AudioOutput resampled({44100, 2, 16, 4, false}, true, 48000);
	require(resampled.ready(), "44.1 to 48 kHz converter opens");
	std::vector<int16_t> constant(44100 / 5 * 2, 8192);
	std::vector<uint8_t> input(constant.size() * 2);
	std::memcpy(input.data(), constant.data(), input.size());
	resampled.enqueue(21, std::move(input));
	for (int index = 0; index < 30; ++index) {
		const auto result = resampled.render(480);
		if (index == 5)
			require(result.size() == 960 && std::abs(result[480] - 0.25f) < 0.001f, "resampling preserves PCM amplitude");
	}
	require(resampled.position() == constant.size() * 2 && !resampled.busy(), "resampling clock uses guest frames");
	require(resampled.callbacks() > 4, "buffers recycle across a long stream");
	require(resampled.take_finished() == std::vector<uint64_t>{21}, "resampled header completes");
	std::printf("CoreAudio offline contract: %d failures\n", failures);
	return failures ? 1 : 0;
}
