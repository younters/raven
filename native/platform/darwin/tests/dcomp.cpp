#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <dcomp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
static void check(HRESULT hr,int n){if(FAILED(hr))ExitProcess(n);}
static void stage(char digit){
 char path[]="C:\\dcomp-stage0";path[14]=digit;DWORD n;HANDLE f=CreateFileA(path,GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);
 if(f==INVALID_HANDLE_VALUE)ExitProcess(40);WriteFile(f,"OK",2,&n,0);CloseHandle(f);
 char next[]="C:\\dcomp-next0";next[13]=digit;
 for(unsigned i=0;i<300;++i){if(GetFileAttributesA(next)!=INVALID_FILE_ATTRIBUTES)return;
  MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}
 ExitProcess(41);
}
static void render(HWND w){
 ID3D11Device *d=nullptr;ID3D11DeviceContext *c=nullptr;
 check(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,D3D11_CREATE_DEVICE_BGRA_SUPPORT,0,0,D3D11_SDK_VERSION,&d,0,&c),2);
 IDXGIDevice *gd=nullptr;IDXGIAdapter *a=nullptr;IDXGIFactory2 *f=nullptr;
 check(d->QueryInterface(__uuidof(IDXGIDevice),(void**)&gd),3);check(gd->GetAdapter(&a),4);check(a->GetParent(__uuidof(IDXGIFactory2),(void**)&f),5);
 DXGI_SWAP_CHAIN_DESC1 desc={};desc.Width=200;desc.Height=120;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
 desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
 IDXGISwapChain1 *s=nullptr;check(f->CreateSwapChainForComposition(d,&desc,0,&s),6);
 HWND none=(HWND)1;if(s->GetHwnd(&none)!=DXGI_ERROR_INVALID_CALL||none)ExitProcess(7);
 IDCompositionDevice2 *dc=nullptr;check(DCompositionCreateDevice2(gd,__uuidof(IDCompositionDevice2),(void**)&dc),8);
 IDCompositionDesktopDevice *desktop=nullptr;check(dc->QueryInterface(__uuidof(IDCompositionDesktopDevice),(void**)&desktop),9);
 IDCompositionTarget *target=nullptr;check(desktop->CreateTargetForHwnd(w,TRUE,&target),10);
 IDCompositionVisual2 *v=nullptr;check(dc->CreateVisual(&v),11);check(v->SetContent(s),12);check(v->SetOffsetX(25.0f),13);check(v->SetOffsetY(20.0f),14);
 check(target->SetRoot(v),15);check(dc->Commit(),16);
 ID3D11Texture2D *b=nullptr;ID3D11RenderTargetView *rt=nullptr;check(s->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&b),17);check(d->CreateRenderTargetView(b,0,&rt),18);
 const float color[]={0,1,0,1};
 for(int i=0;i<60;i++){c->ClearRenderTargetView(rt,color);check(s->Present(0,0),19);
 if(i==0){IDXGIDevice2 *gd2=nullptr;check(gd->QueryInterface(__uuidof(IDXGIDevice2),(void**)&gd2),20);
 HANDLE event=CreateEventW(0,FALSE,FALSE,0);check(gd2->EnqueueSetEvent(event),21);if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)ExitProcess(22);CloseHandle(event);gd2->Release();}
 MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);}
 stage('1');
 check(v->SetOffsetX(80.0f),23);check(v->SetOffsetY(60.0f),24);check(dc->Commit(),25);
 stage('2');
 rt->Release();b->Release();target->Release();v->Release();desktop->Release();dc->Release();s->Release();f->Release();a->Release();gd->Release();c->Release();d->Release();DestroyWindow(w);ExitProcess(0);
}

static bool argument(const wchar_t *word){
 const wchar_t *s=GetCommandLineW();for(;*s;++s){const wchar_t *a=s,*b=word;while(*a&&*b&&*a==*b){++a;++b;}if(!*b)return true;}return false;
}
extern "C" void entry(){
 if(argument(L"--hidpi")) SetProcessDPIAware();
 if(argument(L"--child")){
  HWND hidden=CreateWindowExW(0,L"STATIC",L"",WS_POPUP,0,0,1,1,0,0,0,0);
  HWND child=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,1,1,hidden,0,0,0);
  if(!child)ExitProcess(31);
  DWORD n;HANDLE file=CreateFileW(L"C:\\dcomp-child.bin",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);
  if(file==INVALID_HANDLE_VALUE||!WriteFile(file,&child,sizeof(child),&n,0))ExitProcess(32);CloseHandle(file);
  render(child);
 }
 HWND w=CreateWindowExW(0,L"STATIC",L"Raven DirectComposition",WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,400,300,0,0,0,0);
 if(!w)ExitProcess(1);
 if(!argument(L"--foreign"))render(w);
 wchar_t path[1024],command[1100];if(!GetModuleFileNameW(0,path,1024))ExitProcess(33);
 size_t at=0;command[at++]=L'"';for(size_t i=0;path[i];++i)command[at++]=path[i];command[at++]=L'"';
 for(const wchar_t *q=L" --child";*q;++q)command[at++]=*q;
 if(argument(L"--hidpi"))for(const wchar_t *q=L" --hidpi";*q;++q)command[at++]=*q;command[at]=0;
 STARTUPINFOW si={};si.cb=sizeof(si);PROCESS_INFORMATION pi={};
 if(!CreateProcessW(path,command,0,0,FALSE,0,0,0,&si,&pi))ExitProcess(34);
 HWND child=0;
 for(unsigned i=0;i<400&&!child;++i){HANDLE f=CreateFileW(L"C:\\dcomp-child.bin",GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
  if(f!=INVALID_HANDLE_VALUE){DWORD n=0;ReadFile(f,&child,sizeof(child),&n,0);CloseHandle(f);if(n!=sizeof(child))child=0;}
  MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(25);
 }
 if(!child)ExitProcess(35);
 SetLastError(0);HWND old=SetParent(child,w);if(!old&&GetLastError())ExitProcess(36);
 if(!SetWindowPos(child,0,0,0,320,200,SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW))ExitProcess(37);
 while(WaitForSingleObject(pi.hProcess,25)==WAIT_TIMEOUT){MSG m;while(PeekMessageW(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}}
 DWORD code=99;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);DestroyWindow(w);ExitProcess(code);
}
