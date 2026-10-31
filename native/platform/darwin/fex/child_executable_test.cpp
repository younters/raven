// SPDX-License-Identifier: MIT
#include "nt/runtime_executable.h"
#include <iostream>

int main()
{
	using raven::child_runtime_executable;
	if (child_runtime_executable("/runtime/raven-fex-host") != "/runtime/raven" ||
		child_runtime_executable("/runtime/raven") != "/runtime/raven" ||
		child_runtime_executable("/runtime/custom-raven") != "/runtime/custom-raven") {
		std::cerr << "child runtime path selection failed\n";
		return 1;
	}
}
