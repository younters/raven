#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#define CHECK(x,n) if (!(x)) return n
static HRESULT STDMETHODCALLTYPE notify_query(IMMNotificationClient *self,REFIID iid,void **out) { (void)self; (void)iid; *out=0; return E_NOINTERFACE; }
static int notify_adds,notify_releases;
static ULONG STDMETHODCALLTYPE notify_add(IMMNotificationClient *self) { (void)self; ++notify_adds; return 2; }
static ULONG STDMETHODCALLTYPE notify_release(IMMNotificationClient *self) { (void)self; ++notify_releases; return 1; }
static HRESULT STDMETHODCALLTYPE notify_state(IMMNotificationClient *self,LPCWSTR id,DWORD state) { (void)self; (void)id; (void)state; return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_id(IMMNotificationClient *self,LPCWSTR id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_default(IMMNotificationClient *self,EDataFlow flow,ERole role,LPCWSTR id) { (void)self; (void)flow; (void)role; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_property(IMMNotificationClient *self,LPCWSTR id,const PROPERTYKEY key) { (void)self; (void)id; (void)key; return S_OK; }
static IMMNotificationClientVtbl notification_vtable={notify_query,notify_add,notify_release,notify_state,notify_id,notify_id,notify_default,notify_property};
int raven_entry(void) {
	IMMDeviceEnumerator *enumerator=0;
	CHECK(SUCCEEDED(CoInitializeEx(0,0)),1);
	CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator,0,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&enumerator)==S_OK,2);
	IMMNotificationClient notification={&notification_vtable};
	CHECK(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(enumerator,&notification)==S_OK,41);
	CHECK(IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(enumerator,&notification)==S_OK,42);
	CHECK(notify_adds==1 && notify_releases==1,49);
	IMMDeviceCollection *collection=0; UINT count=0;
	CHECK(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,eRender,DEVICE_STATE_ACTIVE,&collection)==S_OK,3);
	CHECK(IMMDeviceCollection_GetCount(collection,&count)==S_OK && count==1,4);
	IMMDeviceCollection *inputs=0; UINT input_count=0;
	CHECK(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator,eCapture,DEVICE_STATE_ACTIVE,&inputs)==S_OK,34);
	CHECK(IMMDeviceCollection_GetCount(inputs,&input_count)==S_OK && input_count<=1,35);
	if (input_count) {
		IMMDevice *microphone=0; IMMEndpoint *endpoint=0; EDataFlow flow=eAll;
		CHECK(IMMDeviceCollection_Item(inputs,0,&microphone)==S_OK,36);
		CHECK(IMMDevice_QueryInterface(microphone,&IID_IMMEndpoint,(void**)&endpoint)==S_OK,37);
		CHECK(IMMEndpoint_GetDataFlow(endpoint,&flow)==S_OK && flow==eCapture,38);
		IAudioClient3 *input_client=0; WAVEFORMATEX *input_format=0;
		CHECK(IMMDevice_Activate(microphone,&IID_IAudioClient3,CLSCTX_ALL,0,(void**)&input_client)==S_OK,39);
		CHECK(IAudioClient3_GetMixFormat(input_client,&input_format)==S_OK && input_format->nChannels==2,40);
		CoTaskMemFree(input_format); IAudioClient3_Release(input_client);
		IMMEndpoint_Release(endpoint); IMMDevice_Release(microphone);
	}
	IMMDeviceCollection_Release(inputs);
	IMMDevice *device=0;
	CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,eRender,eConsole,&device)==S_OK,5);
	IPropertyStore *properties=0; PROPVARIANT value={0};
	CHECK(IMMDevice_OpenPropertyStore(device,STGM_READ,&properties)==S_OK,6);
	CHECK(IPropertyStore_GetValue(properties,&PKEY_Device_FriendlyName,&value)==S_OK && value.vt==VT_LPWSTR && value.pwszVal[0],7);
	CoTaskMemFree(value.pwszVal); IPropertyStore_Release(properties);
	IAudioEndpointVolume *endpoint_volume=0; UINT channels=0; BOOL muted=TRUE; float level=0;
	CHECK(IMMDevice_Activate(device,&IID_IAudioEndpointVolume,CLSCTX_ALL,0,(void**)&endpoint_volume)==S_OK,43);
	CHECK(IAudioEndpointVolume_GetChannelCount(endpoint_volume,&channels)==S_OK && channels==2,44);
	CHECK(IAudioEndpointVolume_SetMasterVolumeLevelScalar(endpoint_volume,0.5f,0)==S_OK,45);
	CHECK(IAudioEndpointVolume_GetMasterVolumeLevelScalar(endpoint_volume,&level)==S_OK && level==0.5f,46);
	CHECK(IAudioEndpointVolume_SetMute(endpoint_volume,FALSE,0)==S_OK,47);
	CHECK(IAudioEndpointVolume_GetMute(endpoint_volume,&muted)==S_OK && !muted,48);
	IAudioEndpointVolume_Release(endpoint_volume);
	IAudioSessionManager *session_manager=0; IAudioSessionControl *session_control=0; ISimpleAudioVolume *session_volume=0;
	CHECK(IMMDevice_Activate(device,&IID_IAudioSessionManager,CLSCTX_ALL,0,(void**)&session_manager)==S_OK,50);
	CHECK(IAudioSessionManager_GetAudioSessionControl(session_manager,0,0,&session_control)==S_OK,51);
	AudioSessionState session_state=AudioSessionStateActive;
	CHECK(IAudioSessionControl_GetState(session_control,&session_state)==S_OK && session_state==AudioSessionStateInactive,52);
	CHECK(IAudioSessionManager_GetSimpleAudioVolume(session_manager,0,0,&session_volume)==S_OK,53);
	CHECK(ISimpleAudioVolume_SetMasterVolume(session_volume,0.5f,0)==S_OK,54);
	CHECK(ISimpleAudioVolume_GetMasterVolume(session_volume,&level)==S_OK && level==0.5f,55);
	ISimpleAudioVolume_Release(session_volume); IAudioSessionControl_Release(session_control); IAudioSessionManager_Release(session_manager);
	IAudioClient3 *client=0;
	CHECK(IMMDevice_Activate(device,&IID_IAudioClient3,CLSCTX_ALL,0,(void**)&client)==S_OK,8);
	WAVEFORMATEX *format=0;
	CHECK(IAudioClient3_GetMixFormat(client,&format)==S_OK && format->nChannels==2 && format->wBitsPerSample==32,9);
	CHECK(IAudioClient3_IsFormatSupported(client,AUDCLNT_SHAREMODE_SHARED,format,0)==S_OK,10);
	REFERENCE_TIME period=0,minimum=0;
	CHECK(IAudioClient3_GetDevicePeriod(client,&period,&minimum)==S_OK && period>0,11);
	CHECK(IAudioClient3_Initialize(client,AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,1000000,0,format,0)==S_OK,12);
	UINT32 capacity=0,padding=99;
	CHECK(IAudioClient3_GetBufferSize(client,&capacity)==S_OK && capacity>=format->nSamplesPerSec/10,13);
	CHECK(IAudioClient3_GetCurrentPadding(client,&padding)==S_OK && padding==0,14);
	HANDLE event=CreateEventW(0,FALSE,FALSE,0);
	CHECK(IAudioClient3_SetEventHandle(client,event)==S_OK,15);
	IAudioRenderClient *render=0; ISimpleAudioVolume *volume=0; IAudioClock *clock=0;
	CHECK(IAudioClient3_GetService(client,&IID_IAudioRenderClient,(void**)&render)==S_OK,16);
	CHECK(IAudioClient3_GetService(client,&IID_ISimpleAudioVolume,(void**)&volume)==S_OK,17);
	CHECK(ISimpleAudioVolume_SetMasterVolume(volume,0.25f,0)==S_OK,18);
	float gain=0;
	CHECK(ISimpleAudioVolume_GetMasterVolume(volume,&gain)==S_OK && gain==0.25f,19);
	CHECK(ISimpleAudioVolume_SetMute(volume,TRUE,0)==S_OK,20);
	CHECK(IAudioClient3_GetService(client,&IID_IAudioClock,(void**)&clock)==S_OK,21);
	UINT64 frequency=0,position=0,qpc=0;
	CHECK(IAudioClock_GetFrequency(clock,&frequency)==S_OK && frequency==format->nSamplesPerSec,22);
	BYTE *data=0; UINT32 submitted=format->nSamplesPerSec/20;
	CHECK(IAudioRenderClient_GetBuffer(render,submitted,&data)==S_OK && data,23);
	CHECK(IAudioRenderClient_GetBuffer(render,1,&data)==AUDCLNT_E_OUT_OF_ORDER,24);
	CHECK(IAudioRenderClient_ReleaseBuffer(render,submitted,AUDCLNT_BUFFERFLAGS_SILENT)==S_OK,25);
	CHECK(IAudioClient3_GetCurrentPadding(client,&padding)==S_OK && padding==submitted,26);
	CHECK(IAudioClient3_Start(client)==S_OK,27);
	CHECK(WaitForSingleObject(event,1000)==WAIT_OBJECT_0,28);
	Sleep(200);
	CHECK(IAudioClient3_GetCurrentPadding(client,&padding)==S_OK && padding==0,29);
	CHECK(IAudioClock_GetPosition(clock,&position,&qpc)==S_OK && position>=submitted && qpc>0,30);
	CHECK(IAudioClient3_Stop(client)==S_OK,31);
	CHECK(IAudioClient3_Reset(client)==S_OK,32);
	CHECK(IAudioClock_GetPosition(clock,&position,0)==S_OK && position==0,33);
	IAudioClock_Release(clock); ISimpleAudioVolume_Release(volume); IAudioRenderClient_Release(render);
	IAudioClient3_Release(client); IMMDevice_Release(device); IMMDeviceCollection_Release(collection); IMMDeviceEnumerator_Release(enumerator);
	CoTaskMemFree(format); CloseHandle(event); CoUninitialize();
	return 0;
}
