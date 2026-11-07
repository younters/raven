#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace raven {

// Whether a call of the 32-bit or the 64-bit loop should be traced. RAVEN_TRACE_API is 1 for every call or a comma-separated list of API names
// to follow, RAVEN_TRACE_PROGRAM keeps that to one program, and RAVEN_TRACE_API_ON names a file: while it exists every call is traced, checked
// at most ten times a second.
inline bool api_trace_selected(std::string_view program, std::string_view api)
{
	static const char *const selection = std::getenv("RAVEN_TRACE_API");
	static const char *const only = std::getenv("RAVEN_TRACE_PROGRAM");
	if (selection && (!only || std::equal(program.begin(), program.end(), only, only + std::char_traits<char>::length(only),
		[](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))) {
		if (std::string_view(selection) == "1")
			return true;
		const std::string wanted = std::string(",") + selection + ",";
		if (wanted.find("," + std::string(api) + ",") != wanted.npos)
			return true;
	}
	static const char *const trace_switch = std::getenv("RAVEN_TRACE_API_ON");
	if (!trace_switch)
		return false;
	static std::chrono::steady_clock::time_point checked;
	static bool switched = false;
	if (const auto now = std::chrono::steady_clock::now(); now - checked > std::chrono::milliseconds(100)) {
		checked = now;
		switched = ::access(trace_switch, F_OK) == 0;
	}
	return switched;
}

// RAVEN_PROFILE=1 prints, when the process ends, how many times each native API was called and how long the host spent serving it, plus
// the time the guest ran between calls (including the cost of stopping and restarting it). It exists to find what a program waits for.
// RAVEN_PROFILE=N with N above 1 prints the same every N seconds for the calls of that period, to see what one action costs.
class RuntimeProfile {
public:
	static RuntimeProfile &instance()
	{
		static RuntimeProfile profile;
		return profile;
	}
	bool enabled() const { return active; }
	void api(const std::string &name, std::chrono::nanoseconds spent)
	{
		const std::lock_guard lock(mutex);
		auto &entry = calls[name];
		++entry.first;
		entry.second += uint64_t(spent.count());
		if (interval.count() > 1 && std::chrono::steady_clock::now() - last_report >= interval) {
			report();
			calls.clear();
			resumes = resume_ns = 0;
			last_report = std::chrono::steady_clock::now();
		}
	}
	void guest(std::chrono::nanoseconds spent)
	{
		const std::lock_guard lock(mutex);
		++resumes;
		resume_ns += uint64_t(spent.count());
	}

private:
	// Guest threads run side by side and each reports its own calls.
	std::mutex mutex;
	bool active = std::getenv("RAVEN_PROFILE") != nullptr;
	std::map<std::string, std::pair<uint64_t, uint64_t>> calls;
	uint64_t resumes = 0, resume_ns = 0;
	const std::chrono::seconds interval{active ? std::atoi(std::getenv("RAVEN_PROFILE")) : 0};
	std::chrono::steady_clock::time_point last_report = std::chrono::steady_clock::now();
	~RuntimeProfile()
	{
		if (active)
			report();
	}
	void report() const
	{
		std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> rows(calls.begin(), calls.end());
		std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.second > b.second.second; });
		uint64_t total_calls = 0, total_ns = 0;
		for (const auto &row : rows) {
			total_calls += row.second.first;
			total_ns += row.second.second;
		}
		std::cerr << std::dec << "profile: " << total_calls << " API calls served in " << total_ns / 1000000 << " ms; guest ran " << resumes
			<< " times for " << resume_ns / 1000000 << " ms in total\n";
		for (size_t index = 0; index < rows.size() && index < 25; ++index)
			std::cerr << "profile: " << rows[index].first << " calls=" << rows[index].second.first << " ms=" << rows[index].second.second / 1000000
				<< " us/call=" << rows[index].second.second / 1000 / std::max<uint64_t>(rows[index].second.first, 1) << '\n';
		// Each call stops and restarts the guest, which costs more than most calls themselves: the most frequent come next.
		std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second.first > b.second.first; });
		for (size_t index = 0; index < rows.size() && index < 15; ++index)
			std::cerr << "profile: frequent " << rows[index].first << " calls=" << rows[index].second.first << '\n';
		size_t stops = 0;
		for (const auto &row : rows)
			if (row.first.starts_with("(stop) ") && stops++ < 15)
				std::cerr << "profile: stopping " << row.first.substr(7) << " calls=" << row.second.first << '\n';
	}
};

}
