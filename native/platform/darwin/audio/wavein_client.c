#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#define CHECK(x,n) if (!(x)) return n
int raven_entry(void) {
	UINT count=waveInGetNumDevs();
	CHECK(count<=1,1);
	if (!count) return 0;
	WAVEINCAPSW caps={0};
	CHECK(waveInGetDevCapsW(0,&caps,sizeof(caps))==MMSYSERR_NOERROR && caps.szPname[0],2);
	WAVEFORMATEX format={WAVE_FORMAT_PCM,1,16000,32000,2,16,0};
	CHECK(waveInOpen(0,WAVE_MAPPER,&format,0,0,WAVE_FORMAT_QUERY)==MMSYSERR_NOERROR,3);
	return 0;
}
