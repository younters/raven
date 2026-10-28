// SPDX-License-Identifier: GPL-3.0-or-later
// DirectComposition visual trees over Raven's IOSurface presentation transport.
// Written against the public Windows SDK interfaces; no Windows implementation code.
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <new>
#include <initializer_list>
#include <math.h>

void *operator new(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
void operator delete(void *p) noexcept { if(p) HeapFree(GetProcessHeap(),0,p); }
void operator delete(void *p, size_t) noexcept { operator delete(p); }
extern "C" void __cxa_pure_virtual() { TerminateProcess(GetCurrentProcess(), 127); }
static const GUID visualIID={0x4d93059d,0x097b,0x4651,{0x9a,0x60,0xf0,0xf2,0x51,0x16,0xe2,0xf3}};
static const GUID targetIID={0xeacdd04c,0x117e,0x4e17,{0x88,0xf4,0xd1,0xb1,0x2b,0x0e,0x3d,0x89}};
// Shared with the source DXMT patch. Private data never escapes this implementation.
static const GUID compositionWindow={0x274d7681,0xd49f,0x46d6,{0xa4,0x9b,0x23,0x78,0xf0,0xcd,0x7e,0x71}};
static bool same(REFIID a, REFIID b) { return InlineIsEqualGUID(a,b); }
static HANDLE bits(float f) { union {float f; ULONG u;} b{f}; return (HANDLE)(ULONG_PTR)b.u; }
static bool finite(float f) { return __builtin_isfinite(f); }
struct ClipObject : IDCompositionRectangleClip {
 LONG refs=1;float values[12]={};
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
  if(!out)return E_POINTER;*out=nullptr;
  static const GUID rectangleIID={0x9842ad7d,0xd9cf,0x4908,{0xae,0xd7,0x48,0xb5,0x1d,0xa5,0xe7,0xc2}};
  if(same(iid,IID_IUnknown)||same(iid,rectangleIID)){*out=this;AddRef();return S_OK;}return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{ULONG r=InterlockedDecrement(&refs);if(!r)delete this;return r;}
 HRESULT STDMETHODCALLTYPE SetLeft(float v) override{if(!finite(v))return E_INVALIDARG;values[0]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetLeft(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTop(float v) override{if(!finite(v))return E_INVALIDARG;values[1]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTop(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetRight(float v) override{if(!finite(v))return E_INVALIDARG;values[2]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetRight(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetBottom(float v) override{if(!finite(v))return E_INVALIDARG;values[3]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetBottom(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTopLeftRadiusX(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[4]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTopLeftRadiusX(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTopLeftRadiusY(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[5]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTopLeftRadiusY(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTopRightRadiusX(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[6]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTopRightRadiusX(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTopRightRadiusY(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[7]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTopRightRadiusY(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetBottomLeftRadiusX(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[8]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetBottomLeftRadiusX(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetBottomLeftRadiusY(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[9]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetBottomLeftRadiusY(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetBottomRightRadiusX(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[10]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetBottomRightRadiusX(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetBottomRightRadiusY(float v) override{if(!finite(v)||v<0)return E_INVALIDARG;values[11]=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetBottomRightRadiusY(IDCompositionAnimation*) override{return E_NOTIMPL;}
};
struct Device;
struct Visual;
struct Target;
struct Child { Visual *visual; Child *next; };
struct Binding { HWND proxy; Binding *next; };
struct Device : IDCompositionDesktopDevice {
 LONG refs=1;
 Target *targets=nullptr;
 Binding *bindings=nullptr;
 CRITICAL_SECTION lock;
 Device() { InitializeCriticalSection(&lock); }
 ~Device();
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
  if(!out)return E_POINTER; *out=nullptr;
  if(same(iid,IID_IUnknown)||same(iid,__uuidof(IDCompositionDevice2))||same(iid,__uuidof(IDCompositionDesktopDevice))) {
   *out=static_cast<IDCompositionDesktopDevice*>(this); AddRef(); return S_OK;
  } return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override {ULONG r=InterlockedDecrement(&refs);if(!r)delete this;return r;}
 HRESULT STDMETHODCALLTYPE Commit() override;
 HRESULT STDMETHODCALLTYPE WaitForCommitCompletion() override { GdiFlush(); return S_OK; }
 HRESULT STDMETHODCALLTYPE GetFrameStatistics(DCOMPOSITION_FRAME_STATISTICS *out) override {
  if(!out)return E_POINTER; *out={}; QueryPerformanceCounter(&out->lastFrameTime); QueryPerformanceFrequency(&out->timeFrequency);
  out->currentTime=out->lastFrameTime; out->nextEstimatedFrameTime.QuadPart=out->currentTime.QuadPart+out->timeFrequency.QuadPart/60;
  out->currentCompositionRate={60,1}; return S_OK;
 }
 HRESULT STDMETHODCALLTYPE CreateVisual(IDCompositionVisual2 **out) override;
 HRESULT STDMETHODCALLTYPE CreateTargetForHwnd(HWND hwnd,BOOL top,IDCompositionTarget **out) override;
 HRESULT STDMETHODCALLTYPE CreateSurfaceFromHandle(HANDLE,IUnknown **out) override {if(out)*out=nullptr;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE CreateSurfaceFromHwnd(HWND,IUnknown **out) override {if(out)*out=nullptr;return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE CreateSurfaceFactory(IUnknown* a0, IDCompositionSurfaceFactory** a1) override { if (a1) *a1=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateSurface(UINT a0, UINT a1, DXGI_FORMAT a2, DXGI_ALPHA_MODE a3, IDCompositionSurface** a4) override { if (a4) *a4=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateVirtualSurface(UINT a0, UINT a1, DXGI_FORMAT a2, DXGI_ALPHA_MODE a3, IDCompositionVirtualSurface** a4) override { if (a4) *a4=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateTranslateTransform(IDCompositionTranslateTransform** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateScaleTransform(IDCompositionScaleTransform** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateRotateTransform(IDCompositionRotateTransform** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateSkewTransform(IDCompositionSkewTransform** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateMatrixTransform(IDCompositionMatrixTransform** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateTransformGroup(IDCompositionTransform** a0, UINT a1, IDCompositionTransform** a2) override { if (a2) *a2=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateTranslateTransform3D(IDCompositionTranslateTransform3D** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateScaleTransform3D(IDCompositionScaleTransform3D** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateRotateTransform3D(IDCompositionRotateTransform3D** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateMatrixTransform3D(IDCompositionMatrixTransform3D** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateTransform3DGroup(IDCompositionTransform3D** a0, UINT a1, IDCompositionTransform3D** a2) override { if (a2) *a2=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateEffectGroup(IDCompositionEffectGroup** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
 HRESULT STDMETHODCALLTYPE CreateRectangleClip(IDCompositionRectangleClip** out) override {if(!out)return E_POINTER;*out=new ClipObject;return *out?S_OK:E_OUTOFMEMORY;}
 HRESULT STDMETHODCALLTYPE CreateAnimation(IDCompositionAnimation** a0) override { if (a0) *a0=nullptr; return E_NOTIMPL; }
};
struct Visual : IDCompositionVisual2 {
 LONG refs=1; Device *device;
 float x=0,y=0;
 D2D_MATRIX_3X2_F matrix={1,0,0,1,0,0};
 D2D_RECT_F clip={-1000000,-1000000,1000000,1000000};
 bool clipped=false;
 ClipObject *rectangle=nullptr;
 IUnknown *content=nullptr;
 Child *children=nullptr;
 Visual *parent=nullptr;
 Visual(Device *d):device(d){d->AddRef();}
 ~Visual(){if(rectangle)rectangle->Release();RemoveAllVisuals();if(content)content->Release();device->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
  if(!out)return E_POINTER;*out=nullptr;
  if(same(iid,IID_IUnknown)||same(iid,visualIID)||same(iid,__uuidof(IDCompositionVisual2))){*out=this;AddRef();return S_OK;}
  return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{ULONG r=InterlockedDecrement(&refs);if(!r)delete this;return r;}
 HRESULT STDMETHODCALLTYPE SetOffsetX(float v) override{if(!finite(v))return E_INVALIDARG;x=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetOffsetY(float v) override{if(!finite(v))return E_INVALIDARG;y=v;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetOffsetX(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetOffsetY(IDCompositionAnimation*) override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetTransform(const D2D_MATRIX_3X2_F &v) override{
  for(float f:{v._11,v._12,v._21,v._22,v._31,v._32})if(!finite(f))return E_INVALIDARG;
  if(v._12||v._21||v._11<=0||v._22<=0)return E_NOTIMPL;matrix=v;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetTransform(IDCompositionTransform *v) override{if(v)return E_NOTIMPL;matrix={1,0,0,1,0,0};return S_OK;}
 HRESULT STDMETHODCALLTYPE SetTransformParent(IDCompositionVisual *v) override{return v?E_NOTIMPL:S_OK;}
 HRESULT STDMETHODCALLTYPE SetEffect(IDCompositionEffect *v) override{return v?E_NOTIMPL:S_OK;}
 HRESULT STDMETHODCALLTYPE SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE v) override{
  return v>=DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR&&v<=DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT?S_OK:E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE SetBorderMode(DCOMPOSITION_BORDER_MODE v) override{return v>=0&&v<=2?S_OK:E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE SetClip(IDCompositionClip *v) override{
  if(v)v->AddRef();if(rectangle)rectangle->Release();rectangle=static_cast<ClipObject*>(v);clipped=false;return S_OK;}
 HRESULT STDMETHODCALLTYPE SetClip(const D2D_RECT_F &v) override{
  if(!finite(v.left)||!finite(v.top)||!finite(v.right)||!finite(v.bottom)||v.right<v.left||v.bottom<v.top)return E_INVALIDARG;
  if(rectangle){rectangle->Release();rectangle=nullptr;}clip=v;clipped=true;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetContent(IUnknown *v) override{
  if(v){IDXGISwapChain1 *s=nullptr;HRESULT hr=v->QueryInterface(__uuidof(IDXGISwapChain1),(void**)&s);
   if(FAILED(hr))return E_INVALIDARG;HWND proxy=nullptr;UINT size=sizeof(proxy);hr=s->GetPrivateData(compositionWindow,&size,&proxy);s->Release();
   if(FAILED(hr)||!proxy)return E_INVALIDARG;v->AddRef();}
  if(content)content->Release();content=v;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE AddVisual(IDCompositionVisual *v,BOOL above,IDCompositionVisual *reference) override{
  if(!v)return E_INVALIDARG;Visual *child=nullptr;
  if(FAILED(v->QueryInterface(__uuidof(IDCompositionVisual2),(void**)&child)))return E_INVALIDARG;
  if(child->device!=device){child->Release();return E_INVALIDARG;}
  for(Visual *p=this;p;p=p->parent)if(p==child){child->Release();return E_INVALIDARG;}
  if(child->parent){child->Release();return E_INVALIDARG;}
  Child **at=&children;
  if(reference){while(*at && static_cast<IDCompositionVisual*>((*at)->visual)!=reference)at=&(*at)->next;
   if(!*at){child->Release();return E_INVALIDARG;}if(above)at=&(*at)->next;
  }else if(above){while(*at)at=&(*at)->next;}
  auto node=new Child{child,*at};*at=node;child->parent=this;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE RemoveVisual(IDCompositionVisual *v) override{
  Child **at=&children;while(*at&&static_cast<IDCompositionVisual*>((*at)->visual)!=v)at=&(*at)->next;
  if(!*at)return E_INVALIDARG;Child *old=*at;*at=old->next;old->visual->parent=nullptr;old->visual->Release();delete old;return S_OK;
 }
 HRESULT STDMETHODCALLTYPE RemoveAllVisuals() override{
  while(children){auto old=children;children=old->next;old->visual->parent=nullptr;old->visual->Release();delete old;}return S_OK;
 }
 HRESULT STDMETHODCALLTYPE SetCompositeMode(DCOMPOSITION_COMPOSITE_MODE v) override{return v==DCOMPOSITION_COMPOSITE_MODE_SOURCE_OVER||v==DCOMPOSITION_COMPOSITE_MODE_INHERIT?S_OK:E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SetOpacityMode(DCOMPOSITION_OPACITY_MODE v) override{return v>=0&&v<=2?S_OK:E_INVALIDARG;}
 HRESULT STDMETHODCALLTYPE SetBackFaceVisibility(DCOMPOSITION_BACKFACE_VISIBILITY v) override{return v>=0&&v<=2?S_OK:E_INVALIDARG;}
};
struct Target : IDCompositionTarget {
 LONG refs=1; Device *device;HWND hwnd;Visual *root=nullptr;Target *next;
 Target(Device *d,HWND w):device(d),hwnd(w){d->AddRef();next=d->targets;d->targets=this;}
 ~Target(){Target **p=&device->targets;while(*p&&*p!=this)p=&(*p)->next;if(*p)*p=next;if(root)root->Release();device->Release();}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override{
  if(!out)return E_POINTER;*out=nullptr;if(same(iid,IID_IUnknown)||same(iid,targetIID)){*out=this;AddRef();return S_OK;}return E_NOINTERFACE;}
 ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs);}
 ULONG STDMETHODCALLTYPE Release() override{ULONG r=InterlockedDecrement(&refs);if(!r)delete this;return r;}
 HRESULT STDMETHODCALLTYPE SetRoot(IDCompositionVisual *v) override{
  Visual *next=nullptr;if(v){if(FAILED(v->QueryInterface(__uuidof(IDCompositionVisual2),(void**)&next)))return E_INVALIDARG;
   if(next->device!=device){next->Release();return E_INVALIDARG;}}
  if(root)root->Release();root=next;return S_OK;
 }
};
Device::~Device(){while(bindings){auto b=bindings;bindings=b->next;RemovePropW(b->proxy,L"RavenDCompTarget");delete b;}DeleteCriticalSection(&lock);}
HRESULT Device::CreateVisual(IDCompositionVisual2 **out){if(!out)return E_POINTER;*out=new Visual(this);return *out?S_OK:E_OUTOFMEMORY;}
HRESULT Device::CreateTargetForHwnd(HWND hwnd,BOOL,IDCompositionTarget **out){
 if(!out)return E_POINTER;*out=nullptr;if(!IsWindow(hwnd))return E_INVALIDARG;
 for(auto p=targets;p;p=p->next)if(p->hwnd==hwnd)return E_INVALIDARG;
 *out=new Target(this,hwnd);return *out?S_OK:E_OUTOFMEMORY;
}
static float maximum(float a,float b){return a>b?a:b;}
static float minimum(float a,float b){return a<b?a:b;}
static void apply(Visual *v,HWND target,float x,float y,float sx,float sy,D2D_RECT_F clip,int &z,Binding *&bindings){
 x+=(v->x+v->matrix._31)*sx;y+=(v->y+v->matrix._32)*sy;sx*=v->matrix._11;sy*=v->matrix._22;
 if(v->clipped||v->rectangle){auto c=v->clip;if(v->rectangle)c={v->rectangle->values[0],v->rectangle->values[1],v->rectangle->values[2],v->rectangle->values[3]};
  clip={maximum(clip.left,x+c.left*sx),maximum(clip.top,y+c.top*sy),minimum(clip.right,x+c.right*sx),minimum(clip.bottom,y+c.bottom*sy)};}
 if(v->content){IDXGISwapChain1 *s=nullptr;v->content->QueryInterface(__uuidof(IDXGISwapChain1),(void**)&s);
  HWND proxy=nullptr;UINT size=sizeof(proxy);s->GetPrivateData(compositionWindow,&size,&proxy);s->Release();
  SetPropW(proxy,L"RavenDCompX",bits(x));SetPropW(proxy,L"RavenDCompY",bits(y));
  SetPropW(proxy,L"RavenDCompSX",bits(sx));SetPropW(proxy,L"RavenDCompSY",bits(sy));
  SetPropW(proxy,L"RavenDCompClipL",bits(clip.left));SetPropW(proxy,L"RavenDCompClipT",bits(clip.top));
  SetPropW(proxy,L"RavenDCompClipR",bits(clip.right));SetPropW(proxy,L"RavenDCompClipB",bits(clip.bottom));
  SetPropW(proxy,L"RavenDCompZ",(HANDLE)(ULONG_PTR)++z);
  SetPropW(proxy,L"RavenDCompTarget",target);
  bindings=new Binding{proxy,bindings};
 }
 for(auto c=v->children;c;c=c->next)apply(c->visual,target,x,y,sx,sy,clip,z,bindings);
}
HRESULT Device::Commit(){
 EnterCriticalSection(&lock);
 while(bindings){auto b=bindings;bindings=b->next;RemovePropW(b->proxy,L"RavenDCompTarget");delete b;}
 for(auto t=targets;t;t=t->next){int z=0;if(t->root)apply(t->root,t->hwnd,0,0,1,1,{-1000000,-1000000,1000000,1000000},z,bindings);}
 LeaveCriticalSection(&lock);return S_OK;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DCompositionCreateDevice2(IUnknown*,REFIID iid,void **out){
 if(!out)return E_POINTER;*out=nullptr;Device *d=new Device; if(!d)return E_OUTOFMEMORY;
 HRESULT hr=d->QueryInterface(iid,out);d->Release();return hr;
}
extern "C" __declspec(dllexport) HRESULT WINAPI DCompositionCreateDevice3(IUnknown *d,REFIID iid,void **out){return DCompositionCreateDevice2(d,iid,out);}
extern "C" __declspec(dllexport) HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *d,REFIID iid,void **out){return DCompositionCreateDevice2(d,iid,out);}
