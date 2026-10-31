// SPDX-License-Identifier: MIT
#include "nt/runtime_executable.h"
#include <iostream>
#define RAVEN_FEX_PROCESS 1
#include "platform/darwin/include/raven_darwin_launch.h"
namespace raven {
bool selected = false;
bool fex_execution_selected() { return selected; }
}

int main()
{
	using raven::child_runtime_executable;
	for (bool fex : {false, true}) {
		raven::selected = fex;
		for (bool wide : {false, true})
			if (std::string(raven_darwin::child_execution_argument(wide)) !=
				(fex ? "--execution=fex" : "--execution=native")) return 2;
	}
	if (child_runtime_executable("/runtime/raven-fex-host") != "/runtime/raven" ||
		child_runtime_executable("/runtime/raven") != "/runtime/raven" ||
		child_runtime_executable("/runtime/custom-raven") != "/runtime/custom-raven") {
		std::cerr << "child runtime path selection failed\n";
		return 1;
	}
}
