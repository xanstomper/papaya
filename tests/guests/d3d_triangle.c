/* d3d_triangle: full D3D11 vtable flow with real DXBC shaders (Stage 4h).
 *
 * Creates the device+swapchain through D3D11CreateDeviceAndSwapChain, then:
 * CreateVertexShader/CreatePixelShader with embedded DXBC (the papaya
 * translation layer turns these into GLSL -> SPIR-V -> a VkPipeline),
 * CreateInputLayout (POSITION), a vertex buffer filled via Map/Unmap with a
 * triangle, binds everything, and presents. With PAPAYA_VULKAN=1 the papaya
 * runtime renders the translated pipeline into the swapchain and presents it;
 * without Vulkan it falls back to the CPU clear path (still exit 0).
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef long HRESULT;
typedef struct { unsigned w,h; struct { unsigned n,d; } rr; int fmt, so, sc; } MODE;
typedef struct { MODE bd; struct { unsigned c,q; } sd; unsigned usage,count; void* hwnd; unsigned windowed,effect,flags; } SCD;
typedef struct { const char* name; unsigned index, format, slot, offset, klass, rate; } EL;
typedef struct { void* p, *rp, *dp; } MAPPED;
typedef long (WINAPI *PFN)(void*,unsigned,void*,unsigned,const unsigned*,unsigned,unsigned,const void*,void**,void**,const unsigned*,void**);

static const unsigned char vs_dxbc[] = {
    0x44, 0x58, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x53, 0x48, 0x44, 0x52,
    0x34, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x5f, 0x00, 0x00, 0x03, 0xf3, 0x10, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x03, 0xf3, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x05, 0xf3, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x1e, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const unsigned char ps_dxbc[] = {
    0x44, 0x58, 0x42, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x53, 0x48, 0x44, 0x52,
    0x34, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x03, 0xf3, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x08, 0xf3, 0x20, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x43, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
};

#define V(r) ((unsigned long)(r))
void P(const char* fmt, ...){
  char buf[512]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
  DWORD n; HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
  WriteFile(h,buf,(DWORD)strlen(buf),&n,0);
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s){
  (void)p;(void)c;(void)s;
  WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=h; wc.lpszClassName="D3DTri";
  RegisterClassA(&wc);
  HWND w=CreateWindowA("D3DTri","tri",WS_OVERLAPPEDWINDOW,0,0,320,240,0,0,h,0);
  ShowWindow(w,SW_SHOW);
  HMODULE d3d=LoadLibraryA("d3d11.dll");
  PFN fn=(PFN)GetProcAddress(d3d,"D3D11CreateDeviceAndSwapChain");
  if(!fn){P("FAIL gpa\n");return 2;}
  SCD scd; memset(&scd,0,sizeof(scd));
  scd.bd.w=320; scd.bd.h=240; scd.hwnd=w; scd.windowed=1; scd.count=1;
  void *swap=0,*dev=0,*ctx=0; unsigned fl=0;
  HRESULT hr=fn(0,0,0,0,0,0,7,&scd,&swap,&dev,&fl,&ctx);
  if(hr!=0||!swap||!dev||!ctx){printf("FAIL create 0x%lx\n",V(hr));return 3;}
  void** devv=*(void***)dev; void** ctxv=*(void***)ctx; void** swapv=*(void***)swap;
  /* device slots: 3 CreateBuffer, 11 CreateInputLayout, 12 CreateVertexShader, 15 CreatePixelShader */
  /* context slots: 9 PSSetShader, 11 VSSetShader, 14 Map, 15 Unmap, 17 IASetInputLayout, 18 IASetVertexBuffers */
  typedef long (STDMETHODCALLTYPE *FCRS)(void*,void*,unsigned,void*,void**);
  typedef long (STDMETHODCALLTYPE *FCB)(void*,void*,void**);
  typedef long (STDMETHODCALLTYPE *FCIL)(void*,void*,unsigned,void*,unsigned,void**);
  typedef void (STDMETHODCALLTYPE *FSET)(void*,void*,void*,unsigned);
  typedef long (STDMETHODCALLTYPE *FMAP)(void*,void*,unsigned,unsigned,unsigned,void*);
  typedef void (STDMETHODCALLTYPE *FUNMAP)(void*,void*,unsigned);
  typedef void (STDMETHODCALLTYPE *FIAVB)(void*,unsigned,unsigned,void**,unsigned*,unsigned*);
  typedef unsigned (STDMETHODCALLTYPE *FPRES)(void*,unsigned,unsigned);
  void* vs=0; FCRS fvs=(FCRS)devv[12];
  { long hr2=fvs(dev,(void*)vs_dxbc,sizeof(vs_dxbc),0,&vs);
    P("VS call hr=0x%lx vs=%p\n",V(hr2),vs);
    if(hr2!=0||!vs){return 4;} }
  P("before PS\n");
  void* ps=0; FCRS fps=(FCRS)devv[15];
  if(fps(dev,(void*)ps_dxbc,sizeof(ps_dxbc),0,&ps)!=0||!ps){P("FAIL PS\n");return 5;}
  P("after PS ps=%p\n",ps);
  Sleep(2000);
  P("after sleep\n");
  P("slots v3=%p v11=%p v12=%p v15=%p\n", devv[3], devv[11], devv[12], devv[15]);
  void* layout=0; FCIL fil=(FCIL)devv[11];
  EL els[1]={"POSITION",0,6,0,0xFFFFFFFFu,0,0};
  if(fil(dev,els,1,(void*)vs_dxbc,sizeof(vs_dxbc),&layout)!=0||!layout){P("FAIL IL\n");return 6;}
  unsigned desc[6]={ 36, 0, 2, 0, 0, 0 };  /* ByteWidth=36, BindFlags=VB(2) */
  void* vbuf=0; FCB fcb=(FCB)devv[3];
  P("before CB\n"); if(fcb(dev,desc,&vbuf)!=0||!vbuf){P("FAIL CB\n");return 7;} P("after CB\n");
  MAPPED m={0}; FMAP fmap=(FMAP)ctxv[14];
  P("before MAP\n"); if(fmap(ctx,vbuf,0,0,0,&m)!=0||!m.p){P("FAIL MAP\n");return 8;} P("after MAP\n");
  const float tri[9]={ 0.0f,0.5f,0.0f, 0.5f,-0.5f,0.0f, -0.5f,-0.5f,0.0f };
  memcpy(m.p,tri,sizeof(tri));
  P("before UNMAP\n"); FUNMAP fun=(FUNMAP)ctxv[15]; fun(ctx,vbuf,0); P("after UNMAP\n");
  void* bufs[1]={vbuf}; unsigned st[1]={12}, off[1]={0};
  P("before VBBIND\n"); FIAVB fia=(FIAVB)ctxv[18]; fia(ctx,0,1,bufs,st,off); P("after VBBIND\n");
  P("before VSSET\n"); FSET fvs2=(FSET)ctxv[11]; fvs2(ctx,vs,0,0); P("after VSSET\n");
  P("before PSSET\n"); FSET fps2=(FSET)ctxv[9];  fps2(ctx,ps,0,0); P("after PSSET\n");
  typedef void (STDMETHODCALLTYPE *FIAIL)(void*,void*);
  P("before IAIL\n"); FIAIL fial=(FIAIL)ctxv[17]; fial(ctx,layout); P("after IAIL\n");
  P("before PRESENT\n"); FPRES fp=(FPRES)swapv[8];
  HRESULT pr=fp(swap,1,0);
  P("d3dtri ok create=0 vs,ps,il,vb bound present=0x%lx\n",V(pr));
  Sleep(3000);
  return pr==0?0:9;
}
