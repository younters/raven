#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* MapVirtualKey on the US layout, through the ANSI, Unicode and Ex forms, for the map types that tell extended keys and the left and
   right modifiers apart. Scan codes are the ones Microsoft lists in "Keyboard Input Overview", Scan codes. */

struct Case {
	UINT code, type, expected;
};

static const struct Case cases[] = {
	/* MAPVK_VK_TO_VSC: no translation is 0, and VK 0 has none. */
	{'A', MAPVK_VK_TO_VSC, 0x1e},
	{0, MAPVK_VK_TO_VSC, 0},
	{VK_LSHIFT, MAPVK_VK_TO_VSC, 0x2a},
	{VK_RSHIFT, MAPVK_VK_TO_VSC, 0x36},
	{VK_LCONTROL, MAPVK_VK_TO_VSC, 0x1d},
	{VK_UP, MAPVK_VK_TO_VSC, 0x48},
	/* MAPVK_VK_TO_VSC_EX: the 0xe0 prefix of an extended key is in the high byte. */
	{'A', MAPVK_VK_TO_VSC_EX, 0x1e},
	{VK_SHIFT, MAPVK_VK_TO_VSC_EX, 0x2a},
	{VK_RSHIFT, MAPVK_VK_TO_VSC_EX, 0x36},
	{VK_RCONTROL, MAPVK_VK_TO_VSC_EX, 0xe01d},
	{VK_RMENU, MAPVK_VK_TO_VSC_EX, 0xe038},
	{VK_UP, MAPVK_VK_TO_VSC_EX, 0xe048},
	{VK_DELETE, MAPVK_VK_TO_VSC_EX, 0xe053},
	{VK_DIVIDE, MAPVK_VK_TO_VSC_EX, 0xe035},
	{VK_LWIN, MAPVK_VK_TO_VSC_EX, 0xe05b},
	/* MAPVK_VSC_TO_VK: left and right are not told apart; an extended scan code is understood. */
	{0x1e, MAPVK_VSC_TO_VK, 'A'},
	{0x2a, MAPVK_VSC_TO_VK, VK_SHIFT},
	{0x36, MAPVK_VSC_TO_VK, VK_SHIFT},
	{0xe01d, MAPVK_VSC_TO_VK, VK_CONTROL},
	{0xe048, MAPVK_VSC_TO_VK, VK_UP},
	{0xe035, MAPVK_VSC_TO_VK, VK_DIVIDE},
	/* MAPVK_VSC_TO_VK_EX: left and right are told apart. */
	{0x2a, MAPVK_VSC_TO_VK_EX, VK_LSHIFT},
	{0x36, MAPVK_VSC_TO_VK_EX, VK_RSHIFT},
	{0x1d, MAPVK_VSC_TO_VK_EX, VK_LCONTROL},
	{0xe01d, MAPVK_VSC_TO_VK_EX, VK_RCONTROL},
	{0x38, MAPVK_VSC_TO_VK_EX, VK_LMENU},
	{0xe038, MAPVK_VSC_TO_VK_EX, VK_RMENU},
	{0xe05b, MAPVK_VSC_TO_VK_EX, VK_LWIN},
	{0x1e, MAPVK_VSC_TO_VK_EX, 'A'},
	/* A map type outside the documented ones has no translation. */
	{'A', 5, 0},
};

int raven_entry(void)
{
	const HKL layout = GetKeyboardLayout(0);
	for (unsigned index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		const struct Case *item = &cases[index];
		if (MapVirtualKeyW(item->code, item->type) != item->expected) return 10 + index;
		if (MapVirtualKeyA(item->code, item->type) != item->expected) return 100 + index;
		if (MapVirtualKeyExW(item->code, item->type, layout) != item->expected) return 200 + index;
		if (MapVirtualKeyExA(item->code, item->type, layout) != item->expected) return 300 + index;
	}
	return 0;
}
