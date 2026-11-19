#pragma once
#include <memory>

namespace raven {
class NativeProcess;
class NativeWindows;

class DarwinDXMTPresentation {
public:
	DarwinDXMTPresentation(NativeProcess &process, NativeWindows &windows);
	~DarwinDXMTPresentation();
	void poll();
private:
	struct State;
	std::unique_ptr<State> state;
};
}
