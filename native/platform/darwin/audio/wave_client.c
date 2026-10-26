#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

/* A Windows program, using the public WinMM API all the way through the Rosetta guest and host scheduler.
 * Silence is intentional: automated real-device checks must never surprise someone wearing headphones. */
int raven_entry(void)
{
	WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0};
	HWAVEOUT output = NULL;
	static short samples[44100];
	WAVEHDR header = {0};
	MMTIME position = {0};
	if (!waveOutGetNumDevs())
		return 77;
	if (waveOutOpen(&output, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL))
		return 1;
	if (waveOutSetVolume(output, 0))
		return 2;
	/* Quiet 441 Hz triangle PCM, explicitly muted above. Nonzero source samples exercise the guest copy and volume path. */
	for (unsigned frame = 0; frame < 22050; ++frame) {
		int phase = (int)(frame % 100);
		short value = (short)((phase < 50 ? phase : 100 - phase) * 2 - 50);
		samples[frame * 2] = samples[frame * 2 + 1] = value;
	}
	header.lpData = (char *)samples;
	header.dwBufferLength = sizeof(samples);
	if (waveOutPrepareHeader(output, &header, sizeof(header)))
		return 3;
	if (waveOutPause(output) || waveOutWrite(output, &header, sizeof(header)))
		return 4;
	Sleep(30);
	position.wType = TIME_SAMPLES;
	if (waveOutGetPosition(output, &position, sizeof(position)) || position.u.sample)
		return 5;
	if (waveOutRestart(output))
		return 6;
	for (int attempt = 0; attempt < 300 && !(header.dwFlags & WHDR_DONE); ++attempt)
		Sleep(10);
	if (!(header.dwFlags & WHDR_DONE))
		return 7;
	position.wType = TIME_SAMPLES;
	if (waveOutGetPosition(output, &position, sizeof(position)) || position.u.sample != 22050)
		return 8;
	Sleep(30);
	if (waveOutGetPosition(output, &position, sizeof(position)) || position.u.sample != 22050)
		return 9;
	/* Reset an in-flight header and prove no old completion advances the restarted clock. */
	if (waveOutWrite(output, &header, sizeof(header)))
		return 11;
	Sleep(10);
	if (waveOutReset(output))
		return 12;
	for (int attempt = 0; attempt < 50 && !(header.dwFlags & WHDR_DONE); ++attempt)
		Sleep(10);
	if (!(header.dwFlags & WHDR_DONE) || waveOutGetPosition(output, &position, sizeof(position)) || position.u.sample)
		return 13;
	if (waveOutUnprepareHeader(output, &header, sizeof(header)) || waveOutClose(output))
		return 10;
	return 0;
}
