#include <windows.h>
static DWORD WINAPI worker(void *p) { Sleep(60000); return 0; }
int raven_entry(void) {
 HANDLE t=CreateThread(0,0,worker,0,0,0);
 WaitForSingleObject(t,INFINITE);
 return 7;
}
