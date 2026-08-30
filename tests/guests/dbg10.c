#include <windows.h>
typedef long HRESULT;
typedef struct { unsigned w,h; struct { unsigned n,d; } rr; int fmt, so, sc; } MODE;
typedef struct { MODE bd; struct { unsigned c,q; } sd; unsigned usage,count; void* hwnd; unsigned windowed,effect,flags; } SCD;
static char L[4096]; static int n=0;
static void mark(const char* s){ int l=lstrlenA(s); if(n+l<4000){memcpy(L+n,s,l);n+=l;L[n]=0;}
  HANDLE f=CreateFileA("C:\\mark1.txt",GENERIC_WRITE,0,0,CREATE_ALWAYS,0,0);
  DWORD wr; WriteFile(f,L,n,&wr,0); CloseHandle(f); }
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s){
  (void)h;(void)p;(void)c;(void)s;
  mark("A;");
  WNDCLASSA wc={0}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=h; wc.lpszClassName="Dbg10";
  RegisterClassA(&wc);
  HWND w=CreateWindowA("Dbg10","d",WS_OVERLAPPEDWINDOW,0,0,200,150,0,0,h,0);
  mark("B;");
  HMODULE d3d=LoadLibraryA("d3d11.dll");
  void* fn=(void*)GetProcAddress(d3d,"D3D11CreateDeviceAndSwapChain");
  mark("C;");
  SCD scd; memset(&scd,0,sizeof(scd));
  scd.bd.w=320; scd.bd.h=240; scd.hwnd=w; scd.windowed=1; scd.count=1;
  mark("D;");
  void *swap=0,*dev=0,*ctx=0; unsigned fl=0;
  typedef long (WINAPI *PFN)(void*,unsigned,unsigned,unsigned,const unsigned*,unsigned,unsigned,const void*,void**,void**,const unsigned*,unsigned*,void**);
  HRESULT hr=((PFN)fn)(0,0,0,0,0,0,7,&scd,&swap,&dev,0,&fl,&ctx);
  mark("E;");
  char tail[96]; wsprintfA(tail,"hr=%lx swap=%p dev=%p ctx=%p\r\n",(unsigned long)hr,swap,dev,ctx);
  mark(tail);
  return (hr==0&&swap&&dev&&ctx)?0:3;
}
