// SPDX-License-Identifier: MIT
#pragma once
#include "cpu/native_cpu.h"

namespace raven {

// Experimental stopped-context engine. NativeProcess integration is a separate gate.
// One module instance per host process, on its constructing thread.
class FexProcess {
public:
	FexProcess(std::span<LoadedImage *const> images, const std::string &module_root);
	~FexProcess();
	FexProcess(const FexProcess &) = delete;
	FexProcess &operator=(const FexProcess &) = delete;
	NativeResult call(uint64_t address, std::span<const uint64_t> arguments);
	NativeResult resume();
	NativeContext context() const;
	void set_context(const NativeContext &context);
private:
	struct State;
	std::unique_ptr<State> state;
};
NativeResult run_fex_entry(const PEImage &image, LoadedImage &loaded, const std::string &root);
}
