#include <windows.h>

// The daylight saving rules of America/New_York (the test sets TZ): standard time starts on the first Sunday of November at 2:00,
// daylight time on the second Sunday of March at 2:00, an hour ahead; GetTimeZoneInformationForYear gives the same rules for 2030.
// Returns 0, or the number of the check that failed.
static int rule(const SYSTEMTIME *date, WORD month, WORD occurrence)
{
	return date->wYear == 0 && date->wMonth == month && date->wDayOfWeek == 0 && date->wDay == occurrence && date->wHour == 2 && date->wMinute == 0;
}

int raven_entry(void)
{
	TIME_ZONE_INFORMATION zone;
	const DWORD result = GetTimeZoneInformation(&zone);
	if (result != TIME_ZONE_ID_STANDARD && result != TIME_ZONE_ID_DAYLIGHT)
		return 1;
	if (zone.Bias != 300 || zone.DaylightBias != -60 || zone.StandardBias != 0)
		return 2;
	if (!rule(&zone.StandardDate, 11, 1) || !rule(&zone.DaylightDate, 3, 2))
		return 3;
	TIME_ZONE_INFORMATION later;
	if (!GetTimeZoneInformationForYear(2030, NULL, &later))
		return 4;
	if (!rule(&later.StandardDate, 11, 1) || !rule(&later.DaylightDate, 3, 2))
		return 5;
	DYNAMIC_TIME_ZONE_INFORMATION dynamic;
	if (GetDynamicTimeZoneInformation(&dynamic) == TIME_ZONE_ID_INVALID) return 6;
	if (!GetTimeZoneInformationForYear(0, &dynamic, &later) || later.Bias != 300) return 7;
	if (!GetTimeZoneInformationForYear(2006, NULL, &later) || !rule(&later.DaylightDate, 4, 1) || !rule(&later.StandardDate, 10, 5)) return 8;
	dynamic.DynamicDaylightTimeDisabled = TRUE;
	if (!GetTimeZoneInformationForYear(2006, &dynamic, &later) || !rule(&later.DaylightDate, 3, 2)) return 9;
	dynamic.TimeZoneKeyName[0] = 0;
	dynamic.Bias = 123;
	if (!GetTimeZoneInformationForYear(2030, &dynamic, &later) || later.Bias != 123 || !rule(&later.DaylightDate, 3, 2)) return 10;
	for (volatile unsigned char *at = (volatile unsigned char *)&dynamic; at < (volatile unsigned char *)(&dynamic + 1); ++at) *at = 0;
	dynamic.Bias = -330;
	if (!GetTimeZoneInformationForYear(2030, &dynamic, &later) || later.Bias != -330 || later.DaylightDate.wMonth || later.StandardDate.wMonth) return 11;
	return 0;
}
