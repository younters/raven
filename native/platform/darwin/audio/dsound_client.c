#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>

typedef HRESULT (WINAPI *create_function)(const GUID *, IDirectSound8 **, void *);

static BOOL CALLBACK enumerate(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context) {
	(void)guid; (void)module; if (description && description[0]) ++*(DWORD *)context; return TRUE;
}

int raven_entry(void)
{
	HMODULE library = LoadLibraryA("dsound.dll");
	if (!library)
		return 1;
	create_function create = (create_function)GetProcAddress(library, "DirectSoundCreate8");
	if (!create)
		return 2;
	DWORD devices = 0;
	typedef HRESULT (WINAPI *enumerate_function)(LPDSENUMCALLBACKA, LPVOID);
	enumerate_function list = (enumerate_function)GetProcAddress(library, "DirectSoundEnumerateA");
	if (!list || list(enumerate, &devices) != DS_OK || !devices) return 22;
	IDirectSound8 *sound = NULL;
	if (create(NULL, &sound, NULL) != DS_OK || !sound)
		return 3;
	if (IDirectSound8_SetCooperativeLevel(sound, GetDesktopWindow(), DSSCL_PRIORITY) != DS_OK)
		return 4;
	WAVEFORMATEX format = {0};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 2;
	format.nSamplesPerSec = 44100;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 4;
	format.nAvgBytesPerSec = 176400;
	DSBUFFERDESC description = {0};
	description.dwSize = sizeof description;
	description.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME;
	description.dwBufferBytes = 17640;
	description.lpwfxFormat = &format;
	IDirectSoundBuffer *buffer = NULL;
	if (IDirectSound8_CreateSoundBuffer(sound, &description, &buffer, NULL) != DS_OK || !buffer)
		return 5;
	void *first = NULL, *second = NULL;
	DWORD first_bytes = 0, second_bytes = 0;
	if (IDirectSoundBuffer_Lock(buffer, 100, 17640, &first, &first_bytes, &second, &second_bytes, 0) != DS_OK)
		return 6;
	if (!first || first_bytes != 17540 || !second || second_bytes != 100)
		return 7;
	for (DWORD index = 0; index < first_bytes; ++index)
		((unsigned char *)first)[index] = (unsigned char)index;
	if (IDirectSoundBuffer_Unlock(buffer, first, first_bytes, second, second_bytes) != DS_OK)
		return 8;
	DWORD status = 99;
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || status != 0)
		return 9;
	if (IDirectSoundBuffer_SetVolume(buffer, -10000) != DS_OK) return 16;
	if (IDirectSoundBuffer_SetPan(buffer, -2500) != DS_OK) return 17;
	LONG pan = 0; if (IDirectSoundBuffer_GetPan(buffer, &pan) != DS_OK || pan != -2500) return 18;
	IDirectSoundNotify *notify = NULL;
	if (IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSoundNotify, (void **)&notify) != DS_OK) return 19;
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	DSBPOSITIONNOTIFY point = {DSBPN_OFFSETSTOP, event};
	if (IDirectSoundNotify_SetNotificationPositions(notify, 1, &point) != DS_OK) return 20;
	if (IDirectSoundBuffer_Play(buffer, 0, 0, 0) != DS_OK)
		return 10;
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || !(status & DSBSTATUS_PLAYING))
		return 11;
	DWORD play = 0, write = 0;
	Sleep(80);
	if (IDirectSoundBuffer_GetCurrentPosition(buffer, &play, &write) != DS_OK || play == 0)
		return 12;
	/* One tenth of a second of audio at 176400 bytes per second: playback ends by itself. */
	Sleep(200);
	if (IDirectSoundBuffer_GetStatus(buffer, &status) != DS_OK || (status & DSBSTATUS_PLAYING))
		return 13;
	if (WaitForSingleObject(event, 1000) != WAIT_OBJECT_0) return 21;
	IDirectSoundNotify_Release(notify); CloseHandle(event);
	IDirectSoundBuffer *duplicate = NULL;
	if (IDirectSound8_DuplicateSoundBuffer(sound, buffer, &duplicate) != DS_OK || !duplicate) return 23;
	if (IDirectSoundBuffer_SetCurrentPosition(buffer, 0) != DS_OK || IDirectSoundBuffer_SetCurrentPosition(duplicate, 0) != DS_OK) return 24;
	if (IDirectSoundBuffer_SetFrequency(duplicate, 22050) != DS_OK) return 25;
	DWORD frequency = 0;
	if (IDirectSoundBuffer_GetFrequency(duplicate, &frequency) != DS_OK || frequency != 22050) return 26;
	if (IDirectSoundBuffer_Play(buffer,0,0,DSBPLAY_LOOPING) != DS_OK || IDirectSoundBuffer_Play(duplicate,0,0,DSBPLAY_LOOPING) != DS_OK) return 27;
	Sleep(140);
	if (IDirectSoundBuffer_GetStatus(duplicate,&status) != DS_OK || !(status & DSBSTATUS_LOOPING)) return 28;
	DWORD other = 0;
	if (IDirectSoundBuffer_GetCurrentPosition(buffer,&play,0) != DS_OK || IDirectSoundBuffer_GetCurrentPosition(duplicate,&other,0) != DS_OK || play == other) return 29;
	IDirectSoundBuffer_Stop(buffer); IDirectSoundBuffer_Stop(duplicate); IDirectSoundBuffer_Release(duplicate);
	DSBUFFERDESC primary_desc = {0}; primary_desc.dwSize = sizeof primary_desc; primary_desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
	IDirectSoundBuffer *primary = NULL;
	if (IDirectSound8_CreateSoundBuffer(sound,&primary_desc,&primary,NULL) != DS_OK || IDirectSoundBuffer_SetFormat(primary,&format) != DS_OK) return 30;
	IDirectSoundBuffer_Release(primary);
	IDirectSoundBuffer *same = NULL;
	if (IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSoundBuffer, (void **)&same) != DS_OK || same != buffer)
		return 14;
	if (IDirectSoundBuffer_Release(same) != 1 || IDirectSoundBuffer_Release(buffer) != 0 || IDirectSound8_Release(sound) != 0)
		return 15;
	return 0;
}
