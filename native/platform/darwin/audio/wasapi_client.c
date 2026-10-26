#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#define CHECK(x,n) if (!(x)) return n
int raven_entry(void) {
	IMMDeviceEnumerator *enumerator=0;
	CHECK(SUCCEEDED(CoInitializeEx(0,0)),1);
	CHECK(CoCreateInstance(&CLSID_MMDeviceEnumerator,0,CLSCTX_INPROC_SERVER,&IID_IMMDeviceEnumerator,(void**)&enumerator)==S_OK,2);
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
		IMMEndpoint_Release(endpoint); IMMDevice_Release(microphone);
	}
	IMMDeviceCollection_Release(inputs);
	IMMDevice *device=0;
	CHECK(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator,eRender,eConsole,&device)==S_OK,5);
	IPropertyStore *properties=0; PROPVARIANT value={0};
	CHECK(IMMDevice_OpenPropertyStore(device,STGM_READ,&properties)==S_OK,6);
	CHECK(IPropertyStore_GetValue(properties,&PKEY_Device_FriendlyName,&value)==S_OK && value.vt==VT_LPWSTR && value.pwszVal[0],7);
	CoTaskMemFree(value.pwszVal); IPropertyStore_Release(properties);
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
