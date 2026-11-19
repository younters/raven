#include "fex_process.h"
#include <stdexcept>
namespace raven {
void FexProcess::watch_write(uint64_t address)
{
	(void)address;
	throw std::runtime_error("FEX watch_write is not implemented");
}
void FexProcess::trace_instructions(size_t count, std::function<void(uint64_t)> sink)
{
	(void)count;
	(void)sink;
	throw std::runtime_error("FEX trace_instructions is not implemented");
}
void FexProcess::decline_served()
{
	throw std::runtime_error("FEX decline_served is not implemented");
}
}
