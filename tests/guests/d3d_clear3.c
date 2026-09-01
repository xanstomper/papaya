/* Real Windows 12-arg D3D11CreateDeviceAndSwapChain: (adapter,driver,swrast,flags,feats,levels,sdk,desc,swapOut,devOut,featOut,ctxOut) */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef long HRESULT;
typedef struct { unsigned w,h; struct { unsigned n,d; } rr; int fmt, so, sc; } MODE;
typedef struct { MODE bd; struct { unsigned c,q; } sd; unsigned usage,count; void* hwnd; unsigned windowed,effect,flags; } SCD;
static unsigned (STDMETHODCALLTYPE *pClear)(void*, void*, const float[4]);
static unsigned (STDMETHODCALLTYPE *pOMSetRT)(void*, unsigned, void**, void*);
static unsigned (STDMETHODCALLTYPE *pPresent)(void*, unsigned, unsigned);
static unsigned (STDMETHODCALLTYPE *pGetBuf)(void*, unsigned, void*, void**);
typedef long (WINAPI *PFN)(void*,unsigned,void*,unsigned,const unsigned*,unsigned,unsigned,const void*,void**,void**,const unsigned*,void**);
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s){
  (void)p;(void)c;(void)s;
  WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=h; wc.lpszClassName="D3DClear3";
  RegisterClassA(&wc);
  HWND w=CreateWindowA("D3DClear3","d3d",WS_OVERLAPPEDWINDOW,0,0,320,240,0,0,h,0);
  ShowWindow(w,SW_SHOW);
  HMODULE d3d=LoadLibraryA("d3d11.dll");
  PFN fn=(PFN)GetProcAddress(d3d,"D3D11CreateDeviceAndSwapChain");
  if(!fn){printf("FAIL gpa\n");return 2;}
  SCD scd; memset(&scd,0,sizeof(scd));
  scd.bd.w=320; scd.bd.h=240; scd.hwnd=w; scd.windowed=1; scd.count=1;
  void *swap=0,*dev=0,*ctx=0; unsigned fl=0;
  HRESULT hr=fn(0,0,0,0,0,0,7,&scd,&swap,&dev,&fl,&ctx);
  if(hr!=0||!swap||!dev||!ctx){printf("FAIL create hr=0x%lx swap=%p dev=%p ctx=%p\n",(unsigned long)hr,swap,dev,ctx);return 3;}
  void** ctxv=*(void***)ctx; void** swapv=*(void***)swap;
  /* ID3D11DeviceContext slots per d3d11.idl (IUnknown 0-2 + ID3D11DeviceChild 3-6):
     33 = OMSetRenderTargets, 49 = ClearRenderTargetView. */
  pClear=(void*)ctxv[49]; pOMSetRT=(void*)ctxv[33];
  pPresent=(void*)swapv[8]; pGetBuf=(void*)swapv[9];
  if(!pClear||!pOMSetRT||!pPresent){printf("FAIL vtbl\n");return 4;}
  void* rtv=0; HRESULT gb=pGetBuf(swap,0,0,&rtv);
  if(gb!=0||!rtv){printf("FAIL getbuf 0x%lx\n",(unsigned long)gb);return 5;}
  void* rtvs[1]={rtv};
  pOMSetRT(ctx,1,rtvs,0);
  const float red[4]={1.0f,0.0f,0.0f,1.0f};
  pClear(ctx,rtv,red);
  HRESULT pr=pPresent(swap,1,0);
  printf("d3d3 ok create=0 gb=0 present=0x%lx\n",(unsigned long)pr); fflush(stdout);
  Sleep(200);
  return pr==0?0:6;
}
