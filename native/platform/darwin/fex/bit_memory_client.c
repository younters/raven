// SPDX-License-Identifier: MIT
// A memory bit operation with a register offset reaches past its operand, in either direction. Every form must touch the bit the whole
// offset names; Delphi's allocator and snmalloc both keep bitmaps wider than the one byte a lost offset leaves reachable.
#include <stdint.h>

static uint32_t words[64];

#define BIT_OP(ins, offset, base, carry) \
	__asm__ volatile(ins " %[o],(%[p])\n setc %[c]\n" : [c] "=q"(carry) : [o] "r"(offset), [p] "r"(base) : "cc", "memory")

static int check(int32_t offset)
{
	uint32_t *const middle = words + 32;
	const int index = offset >> 5;
	const uint32_t bit = (uint32_t)1 << (offset & 31);
	unsigned char carry;
	for (int i = 0; i < 64; ++i)
		words[i] = 0;
	BIT_OP("bts", offset, middle, carry);
	for (int i = 0; i < 64; ++i)
		if (words[i] != (i == 32 + index ? bit : 0))
			return 1;
	if (carry)
		return 2;
	BIT_OP("bt", offset, middle, carry);
	if (!carry)
		return 3;
	BIT_OP("btc", offset, middle, carry);
	if (!carry || middle[index])
		return 4;
	BIT_OP("btc", offset, middle, carry);
	if (carry || middle[index] != bit)
		return 5;
	BIT_OP("btr", offset, middle, carry);
	if (!carry || middle[index])
		return 6;
	BIT_OP("lock bts", offset, middle, carry);
	if (carry || middle[index] != bit)
		return 7;
	return 0;
}

int raven_entry(void)
{
	static const int32_t offsets[] = {0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 64, 100, 255, 256, 268, 1000, -1, -2, -8, -9, -31, -32, -33, -100, -1000};
	for (unsigned i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
		const int result = check(offsets[i]);
		if (result)
			return 10 * (int)(i + 1) + result;
	}
	return 0;
}
