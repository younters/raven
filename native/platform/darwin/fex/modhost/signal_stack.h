// SPDX-License-Identifier: MIT
#pragma once
#include <signal.h>
#include <vector>
#include <stdexcept>
class AlternateSignalStack {
	std::vector<unsigned char> storage;
	stack_t previous{};

	public:
	AlternateSignalStack() : storage(128 * 1024) {
		stack_t stack{};
		stack.ss_sp = storage.data();
		stack.ss_size = storage.size();
		if (sigaltstack(&stack, &previous))
			throw std::runtime_error("sigaltstack failed");
	}
	~AlternateSignalStack() {
		sigaltstack(&previous, nullptr);
	}
};
