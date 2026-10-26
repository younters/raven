#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#define CHECK(x,n) if (!(x)) return n
static BYTE wave[44+9600]={
	'R','I','F','F',0xa4,0x25,0,0,'W','A','V','E',
	'f','m','t',' ',16,0,0,0,1,0,2,0,0xc0,0x5d,0,0,0x00,0x77,0x01,0,4,0,16,0,
	'd','a','t','a',0x80,0x25,0,0
};
int raven_entry(void) {
	CHECK(PlaySoundW((LPCWSTR)wave,0,SND_MEMORY|SND_ASYNC|SND_NODEFAULT),1);
	Sleep(30);
	CHECK(PlaySoundW(0,0,SND_PURGE),2);
	HANDLE file=CreateFileW(L"C:\\raven-audio-silent.wav",GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);
	CHECK(file!=INVALID_HANDLE_VALUE,3);
	DWORD written=0;
	CHECK(WriteFile(file,wave,sizeof(wave),&written,0) && written==sizeof(wave),4);
	CloseHandle(file);
	CHECK(PlaySoundW(L"C:\\raven-audio-silent.wav",0,SND_FILENAME|SND_ASYNC|SND_NODEFAULT),5);
	Sleep(30);
	CHECK(PlaySoundW(0,0,SND_PURGE),6);
	CHECK(DeleteFileW(L"C:\\raven-audio-silent.wav"),7);
	CHECK(PlaySoundW(MAKEINTRESOURCEW(101),GetModuleHandleW(0),SND_RESOURCE|SND_ASYNC|SND_NODEFAULT),8);
	Sleep(30);
	CHECK(PlaySoundW(0,0,SND_PURGE),9);
	return 0;
}
