#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <xaudio2.h>
DEFINE_GUID(CLSID_XAudio2_7,0x5a508685,0xa254,0x4fba,0x9b,0x82,0x9a,0x24,0xb0,0x03,0x06,0xaf);
DEFINE_GUID(IID_IXAudio2_7,0x8bcf1f58,0x9fe7,0x4583,0x8a,0xc6,0xe2,0xad,0xc4,0x65,0xc8,0xbb);
#define CHECK(x,n) if (!(x)) return n
int raven_entry(void) {
	void *engine=0;
	CHECK(CoInitializeEx(0,COINIT_MULTITHREADED)==S_OK,1);
	CHECK(CoCreateInstance(&CLSID_XAudio2_7,0,CLSCTX_INPROC_SERVER,&IID_IXAudio2_7,&engine)==S_OK && engine,2);
	void **vt=*(void ***)engine;
	UINT32 count=0;
	CHECK(((HRESULT (WINAPI *)(void *,UINT32 *))vt[3])(engine,&count)==S_OK && count==1,3);
	CHECK(((HRESULT (WINAPI *)(void *,UINT32,UINT32))vt[5])(engine,0,0)==S_OK,4);
	void *master=0;
	CHECK(((HRESULT (WINAPI *)(void *,void **,UINT32,UINT32,UINT32,UINT32,void *))vt[10])
		(engine,&master,0,0,0,0,0)==S_OK && master,5);
	void *submix=0;
	CHECK(((HRESULT (WINAPI *)(void *,void **,UINT32,UINT32,UINT32,UINT32,void *,void *))vt[9])
		(engine,&submix,2,48000,0,0,0,0)==S_OK && submix,11);
	WAVEFORMATEX format={WAVE_FORMAT_PCM,1,24000,48000,2,16,0};
	void *voice=0;
	CHECK(((HRESULT (WINAPI *)(void *,void **,const WAVEFORMATEX *,UINT32,float,void *,void *,void *))vt[8])
		(engine,&voice,&format,0,2.0f,0,0,0)==S_OK && voice,6);
	void **voice_vt=*(void ***)voice;
	CHECK(((HRESULT (WINAPI *)(void *,float,UINT32))voice_vt[12])(voice,0,0)==S_OK,7);
	static BYTE silence[2400];
	XAUDIO2_BUFFER buffer={0}; buffer.AudioBytes=sizeof(silence); buffer.pAudioData=silence; buffer.Flags=XAUDIO2_END_OF_STREAM;
	CHECK(((HRESULT (WINAPI *)(void *,const XAUDIO2_BUFFER *,void *))voice_vt[21])(voice,&buffer,0)==S_OK,8);
	CHECK(((HRESULT (WINAPI *)(void *,UINT32,UINT32))voice_vt[19])(voice,0,0)==S_OK,9);
	Sleep(250);
	XAUDIO2_VOICE_STATE state={0};
	((void (WINAPI *)(void *,XAUDIO2_VOICE_STATE *))voice_vt[25])(voice,&state);
	CHECK(state.BuffersQueued==0 && state.SamplesPlayed>=1200,10);
	((void (WINAPI *)(void *))voice_vt[18])(voice);
	void **submix_vt=*(void ***)submix;
	((void (WINAPI *)(void *))submix_vt[18])(submix);
	void **master_vt=*(void ***)master;
	((void (WINAPI *)(void *))master_vt[18])(master);
	((ULONG (WINAPI *)(void *))vt[2])(engine);
	CoUninitialize();
	return 0;
}
