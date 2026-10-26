#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static ULONG_PTR words[3];
static DWORD page[17*11];

static unsigned hexadecimal(WCHAR *line,unsigned at,ULONG_PTR value)
{
	line[at++]=' ';
	for (unsigned n=0;n<sizeof(value)*2;++n) line[at++]=L"0123456789abcdef"[(value>>((sizeof(value)*2-n-1)*4))&15];
	line[at]=0;return at;
}

static int child(DWORD expected)
{
	/* Deliberately choose the opposite awareness from the creator. The desktop
	 * coordinate space belongs to the process family, not this helper's manifest. */
	if (!SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)(LONG_PTR)(expected==96 ? -4 : -1))) return 10;
	if (GetDpiForSystem()!=expected) return 11;
	HDC dc=GetDC(NULL);
	if (!dc || GetDeviceCaps(dc,LOGPIXELSX)!=(int)expected || GetDeviceCaps(dc,LOGPIXELSY)!=(int)expected) return 12;
	if (!ReleaseDC(NULL,dc)) return 13;
	HWND window=(HWND)words[0]; RECT clip={0};
	dc=GetDC(window);
	if (!dc || GetClipBox(dc,&clip)==ERROR || clip.right!=1 || clip.bottom!=1 || !ReleaseDC(window,dc)) return 14;
	if (!SetEvent((HANDLE)words[1]) || WaitForSingleObject((HANDLE)words[2],5000)!=WAIT_OBJECT_0) return 15;
	dc=GetDC(window);
	if (!dc || GetClipBox(dc,&clip)==ERROR || clip.right!=17 || clip.bottom!=11) return 16;
	BITMAPINFO info={0};info.bmiHeader.biSize=40;info.bmiHeader.biWidth=17;info.bmiHeader.biHeight=-11;
	info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
	for (unsigned i=0;i<17*11;++i) page[i]=0x00123400+i;
	if (StretchDIBits(dc,0,0,17,11,0,0,17,11,page,&info,DIB_RGB_COLORS,SRCCOPY)!=11 || !ReleaseDC(window,dc)) return 17;
	return 0;
}

static int launch(DWORD expected)
{
	WCHAR line[1024]; STARTUPINFOW startup={0}; PROCESS_INFORMATION process={0}; DWORD code;
	startup.cb=sizeof(startup);
	SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
	HANDLE ready=CreateEventW(&security,TRUE,FALSE,NULL),go=CreateEventW(&security,TRUE,FALSE,NULL);
	HWND window=CreateWindowExW(0,L"STATIC",L"foreign DPI pixels",WS_POPUP|WS_VISIBLE,100,100,1,1,NULL,NULL,GetModuleHandleW(NULL),NULL);
	if (!ready || !go || !window) return 3;
	unsigned at=0;line[at++]='"';at+=GetModuleFileNameW(NULL,line+at,900);line[at++]='"';line[at++]=' ';
	line[at++]='-';line[at++]='c';line[at++]=expected==96 ? '0' : '1';line[at]=0;
	at=hexadecimal(line,at,(ULONG_PTR)window);at=hexadecimal(line,at,(ULONG_PTR)ready);at=hexadecimal(line,at,(ULONG_PTR)go);
	if (!CreateProcessW(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) return 1;
	if (WaitForSingleObject(ready,5000)!=WAIT_OBJECT_0 || !SetWindowPos(window,NULL,100,100,17,11,SWP_NOZORDER|SWP_NOACTIVATE) || !SetEvent(go)) return 4;
	if (WaitForSingleObject(process.hProcess,10000)!=WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess,&code)) return 2;
	CloseHandle(process.hThread);CloseHandle(process.hProcess);CloseHandle(ready);CloseHandle(go);
	if (!code) {
		MSG message;
		/* Polling imports the completed foreign frame into the owner surface. */
		PeekMessageW(&message,NULL,0,0,PM_NOREMOVE);
		HDC dc=GetWindowDC(window),memory=CreateCompatibleDC(dc);
		BITMAPINFO info={0};info.bmiHeader.biSize=40;info.bmiHeader.biWidth=17;info.bmiHeader.biHeight=-11;
		info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
		DWORD *bits=NULL;HBITMAP bitmap=CreateDIBSection(memory,&info,0,(void **)&bits,NULL,0);
		HGDIOBJ old=SelectObject(memory,bitmap);
		if (!dc || !memory || !bitmap || !old || !BitBlt(memory,0,0,17,11,dc,0,0,SRCCOPY)) return 5;
		for (unsigned i=0;i<17*11;++i) if ((bits[i]&0xffffff)!=0x00123400+i) return 6;
		SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);
	}
	DestroyWindow(window);
	return (int)code;
}

int raven_entry(void)
{
	const WCHAR *line=GetCommandLineW();
	for (unsigned i=0;line[i];++i)
		if (line[i]=='-' && line[i+1]=='c') {
			unsigned at=i+3;
			for (unsigned n=0;n<3;++n) {
				while (line[at]==' ') ++at;
				for (unsigned k=0;k<sizeof(ULONG_PTR)*2;++k) { WCHAR c=line[at++];words[n]=words[n]*16+(c<='9' ? c-'0' : c-'a'+10); }
			}
			return child(line[i+2]=='0' ? 96 : 192);
		}
	if (GetDpiForSystem()!=96) return 1;
	int result=launch(96);if (result) return 20+result;
	if (!SetProcessDPIAware() || GetDpiForSystem()!=192) return 2;
	result=launch(192);return result ? 40+result : 0;
}
