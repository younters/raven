#include "fex_process.h"
#include <stdexcept>
namespace raven {
void FexProcess::complete_debug_print(uint64_t instruction)
{
	(void)instruction;
	throw std::runtime_error("FEX complete_debug_print is not implemented");
}
uint64_t FexProcess::load_unix_library(const std::string &path, uint32_t function_count)
{
	(void)path;
	(void)function_count;
	throw std::runtime_error("FEX load_unix_library is not implemented");
}
uint32_t FexProcess::copy_unix_port(uint32_t guest_port) const
{
	(void)guest_port;
	throw std::runtime_error("FEX copy_unix_port is not implemented");
}
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
void FexProcess::set_api_argument(size_t index, uint32_t value)
{
	(void)index;
	(void)value;
	throw std::runtime_error("FEX set_api_argument is not implemented");
}
void FexProcess::decline_served()
{
	throw std::runtime_error("FEX decline_served is not implemented");
}
void FexProcess::set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t rip)> sampler)
{
	(void)period;
	(void)sampler;
	throw std::runtime_error("FEX set_guest_sampler is not implemented");
}
}
