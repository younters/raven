#include "input.h"
#include <chrono>
#include <thread>
int main(int argc, char **) {
	if (!raven_darwin::AudioInput::authorized() || !raven_darwin::AudioInput::has_default_device()) return 77;
	if (argc>1) return 0;
	raven_darwin::AudioInput input({48000,1,32,4,true});
	if (!input.ready()) return 1;
	input.start(); std::this_thread::sleep_for(std::chrono::milliseconds(200)); input.stop();
	if (!input.position() || !input.padding()) return 2;
	input.reset(); return input.position() ? 3 : 0;
}
