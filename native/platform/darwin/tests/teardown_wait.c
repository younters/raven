#include <windows.h>
static DWORD WINAPI worker(void *p) { Sleep(60000); return 0; }
int raven_entry(void)
{
	unsigned count=1;
	for (const WCHAR *p=GetCommandLineW(); *p; ++p)
		if (p[0]==' ' && p[1]=='m') count=4;
	HANDLE workers[4];
	for (unsigned i=0; i<count; ++i) {
		workers[i]=CreateThread(0,0,worker,0,0,0);
		if (!workers[i]) return 8;
	}
	HANDLE t=workers[0];
	WaitForSingleObject(t,INFINITE);
	return 7;
}
