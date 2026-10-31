// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <sys/mman.h>

namespace raven {
// Own the reservation as a whole. Subrange mapping may replace only pages in it.
// Guest addresses remain offsets; zero is never a usable guest pointer.
class FexGuestWindow {
public:
	static constexpr uint64_t size = uint64_t{1} << 32;
	FexGuestWindow()
	{
		void *reservation = mmap(nullptr, size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (reservation == MAP_FAILED)
			throw std::runtime_error("FEX cannot reserve the Tier 0 WoW64 guest window");
		const auto start = reinterpret_cast<uintptr_t>(reservation);
		base_ = (start + size - 1) & ~(size - 1);
		if (base_ > start) munmap(reservation, base_ - start);
		if (start + size * 2 > base_ + size)
			munmap(reinterpret_cast<void *>(base_ + size), start + size - base_);
	}
	~FexGuestWindow() { munmap(reinterpret_cast<void *>(base_), size); }
	FexGuestWindow(const FexGuestWindow &) = delete;
	FexGuestWindow &operator=(const FexGuestWindow &) = delete;
	uint64_t base() const { return base_; }
	void *host(uint64_t guest, size_t length) const
	{
		if (guest < 65536 || guest >= size || length > size - guest)
			throw std::runtime_error("FEX WoW64 range outside the guest window");
		return reinterpret_cast<void *>(base_ + guest);
	}
private:
	uintptr_t base_ = 0;
};
} // namespace raven
