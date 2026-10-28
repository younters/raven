#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <x86intrin.h>
#define CHECK(x,n) do { if (!(x)) return (n); } while (0)
static IXAudio2SourceVoice *voice;
static HANDLE completed;
static BYTE silence[4800];
static volatile LONG starts, ends, failed;
static LONG inside_callback;
static int scenario;
static HRESULT submit(ULONG_PTR id) {
	XAUDIO2_BUFFER buffer={0};
	buffer.AudioBytes=sizeof(silence); buffer.pAudioData=silence; buffer.pContext=(void *)id;
	return IXAudio2SourceVoice_SubmitSourceBuffer(voice,&buffer,0);
}
static void STDMETHODCALLTYPE pass_start(IXAudio2VoiceCallback *self,UINT32 bytes) { (void)self; (void)bytes; }
static void STDMETHODCALLTYPE pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE stream_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE buffer_start(IXAudio2VoiceCallback *self,void *context) {
	(void)self; ++starts;
	if (inside_callback) failed=1;
	++inside_callback;
	if ((ULONG_PTR)context==1 && scenario==1 && submit(2)!=S_OK) failed=1;
	if ((ULONG_PTR)context==1 && scenario==2) {
		if (IXAudio2SourceVoice_FlushSourceBuffers(voice)!=S_OK || submit(4)!=S_OK) failed=1;
	}
	Sleep(1); // A nested scheduler service must not re-enter packet delivery.
	--inside_callback;
}
static void STDMETHODCALLTYPE buffer_end(IXAudio2VoiceCallback *self,void *context) {
	(void)self; ++ends;
	if (inside_callback) failed=1;
	++inside_callback;
	if ((ULONG_PTR)context==1 && scenario==1 && submit(3)!=S_OK) failed=1;
	if ((scenario==1 && (ULONG_PTR)context==3) || (scenario==2 && (ULONG_PTR)context==4) || scenario==3)
		SetEvent(completed);
	Sleep(1);
	--inside_callback;
}
static void STDMETHODCALLTYPE loop_end(IXAudio2VoiceCallback *self,void *context) { (void)self; (void)context; }
static void STDMETHODCALLTYPE voice_error(IXAudio2VoiceCallback *self,void *context,HRESULT error) { (void)self; (void)context; (void)error; failed=1; }
static const IXAudio2VoiceCallbackVtbl table={pass_start,pass_end,stream_end,buffer_start,buffer_end,loop_end,voice_error};
static IXAudio2VoiceCallback callback={&table};
int raven_entry(void) {
	IXAudio2 *engine=0;
	IXAudio2MasteringVoice *master=0;
	CHECK(XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR)==S_OK,1);
	CHECK(IXAudio2_CreateMasteringVoice(engine,&master,0,0,0,0,0,AudioCategory_GameEffects)==S_OK,2);
	WAVEFORMATEX format={WAVE_FORMAT_PCM,2,24000,96000,4,16,0};
	completed=CreateEventW(0,TRUE,FALSE,0); CHECK(completed!=0,3);
	for (scenario=1;scenario<=3;++scenario) {
		starts=ends=failed=0; ResetEvent(completed);
		CHECK(IXAudio2_CreateSourceVoice(engine,&voice,&format,0,XAUDIO2_DEFAULT_FREQ_RATIO,&callback,0,0)==S_OK,4);
		CHECK(submit(1)==S_OK,5);
		if (scenario==2) CHECK(submit(2)==S_OK,6);
		CHECK(IXAudio2SourceVoice_Start(voice,0,0)==S_OK,7);
		if (scenario==3) {
			// Darwin's guest TSC is the 1 GHz monotonic timer. No API or trap while the buffer reaches its end.
			const unsigned long long began=__rdtsc();
			while (__rdtsc()-began<300000000ULL) {}
			CHECK(ends==0,8); // The stopped gate must defer callbacks while this guest computes.
		}
		CHECK(WaitForSingleObject(completed,INFINITE)==WAIT_OBJECT_0,9);
		CHECK(!failed,10);
		CHECK(scenario!=1 || (starts==3 && ends==3),11);
		CHECK(scenario!=2 || (starts==2 && ends==1),12);
		CHECK(scenario!=3 || (starts==1 && ends==1),13);
		IXAudio2SourceVoice_DestroyVoice(voice);
	}
	CloseHandle(completed); IXAudio2MasteringVoice_DestroyVoice(master); IXAudio2_Release(engine);
	return 0;
}
