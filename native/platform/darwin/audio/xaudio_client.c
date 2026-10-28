#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#define CHECK(x,n) if (!(x)) return n
static volatile UINT32 started=0, ended=0, streams=0;
static void STDMETHODCALLTYPE pass_start(IXAudio2VoiceCallback *self,UINT32 bytes) { (void)self; (void)bytes; }
static void STDMETHODCALLTYPE pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE stream_end(IXAudio2VoiceCallback *self) { (void)self; ++streams; }
static void STDMETHODCALLTYPE buffer_start(IXAudio2VoiceCallback *self,void *context) { (void)self; if (context==(void*)0x1234) ++started; }
static void STDMETHODCALLTYPE buffer_end(IXAudio2VoiceCallback *self,void *context) { (void)self; if (context==(void*)0x1234) ++ended; }
static void STDMETHODCALLTYPE loop_end(IXAudio2VoiceCallback *self,void *context) { (void)self; (void)context; }
static void STDMETHODCALLTYPE voice_error(IXAudio2VoiceCallback *self,void *context,HRESULT error) { (void)self; (void)context; (void)error; }
static const IXAudio2VoiceCallbackVtbl callback_vtable={pass_start,pass_end,stream_end,buffer_start,buffer_end,loop_end,voice_error};
static IXAudio2VoiceCallback callback={&callback_vtable};
#ifdef AUDIO_LIFECYCLE
static volatile LONG progress;
static DWORD WINAPI worker(void *unused) { (void)unused; for (;;) InterlockedIncrement(&progress); }
#endif
int raven_entry(void) {
	IXAudio2 *engine=0;
	CHECK(XAudio2Create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR)==S_OK && engine,1);
	IXAudio2MasteringVoice *master=0;
	CHECK(IXAudio2_CreateMasteringVoice(engine,&master,0,0,0,0,0,AudioCategory_GameEffects)==S_OK && master,2);
	DWORD mask=0;
	CHECK(IXAudio2MasteringVoice_GetChannelMask(master,&mask)==S_OK && mask==3,3);
	IXAudio2SubmixVoice *submix=0;
	CHECK(IXAudio2_CreateSubmixVoice(engine,&submix,2,48000,0,0,0,0)==S_OK && submix,11);
	WAVEFORMATEX format={WAVE_FORMAT_PCM,2,24000,96000,4,16,0};
	IXAudio2SourceVoice *voice=0;
	CHECK(IXAudio2_CreateSourceVoice(engine,&voice,&format,0,XAUDIO2_DEFAULT_FREQ_RATIO,&callback,0,0)==S_OK && voice,4);
	CHECK(IXAudio2SourceVoice_SetVolume(voice,0,0)==S_OK,5);
	static BYTE silence[4800];
	XAUDIO2_BUFFER buffer={0}; buffer.AudioBytes=sizeof(silence); buffer.pAudioData=silence; buffer.Flags=XAUDIO2_END_OF_STREAM; buffer.pContext=(void*)0x1234;
	CHECK(IXAudio2SourceVoice_SubmitSourceBuffer(voice,&buffer,0)==S_OK,6);
	XAUDIO2_VOICE_STATE state={0}; IXAudio2SourceVoice_GetState(voice,&state,0);
	CHECK(state.BuffersQueued==1,7);
	CHECK(IXAudio2SourceVoice_Start(voice,0,0)==S_OK,8);
#ifdef AUDIO_LIFECYCLE
	CHECK(CreateThread(0,0,worker,0,0,0)!=0,12);
	Sleep(20);
	return 0; // Leave playback and a computing worker for process teardown to unwind.
#endif
	Sleep(250); IXAudio2SourceVoice_GetState(voice,&state,0);
	CHECK(state.BuffersQueued==0 && state.SamplesPlayed>=1200,9);
	CHECK(started==1 && ended==1 && streams==1,10);
	IXAudio2SourceVoice_DestroyVoice(voice); IXAudio2SubmixVoice_DestroyVoice(submix);
	IXAudio2MasteringVoice_DestroyVoice(master); IXAudio2_Release(engine);
	return 0;
}
