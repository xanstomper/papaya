#include <windows.h>
#include <stdio.h>
#include <string.h>
int WINAPI WinMain(HINSTANCE h,HINSTANCE p,LPSTR c,int s){
  (void)h;(void)p;(void)c;(void)s;
  // GlobalAlloc/Free
  HGLOBAL mem=GlobalAlloc(GMEM_FIXED,64);
  HGLOBAL freed=mem?GlobalFree(mem):0;
  // LocalAlloc (alias of global alloc)
  HLOCAL lm=LocalAlloc(LMEM_FIXED,32);
  HLOCAL lf=lm?LocalFree(lm):0;
  // lstrcat / lstrlen / wcslen
  char a[32]="hello"; char* r=lstrcatA(a," world");
  int len=lstrlenA(r);
  int wlen=wcslen(L"hello");
  // locale
  WORD sys=GetSystemDefaultLangID(), usr=GetUserDefaultLangID();
  DWORD tl=GetThreadLocale();
  // GetProcessId / GetHandleInformation
  DWORD pid=GetProcessId(GetCurrentProcess());
  DWORD hif=0; BOOL ghi=GetHandleInformation(GetCurrentProcess(),&hif);
  // SecureZeroMemory
  char buf[16]="secret"; SecureZeroMemory(buf,sizeof(buf));
  int zeroed=(buf[0]==0);
  printf("globalfr=%d localfr=%d cat='%s' llen=%d wlen=%d lang=(%u,%u) tloc=%u pid>0=%d hif=%d zeroed=%d\n",
    freed==0, lf==0, a, len, wlen, (unsigned)sys, (unsigned)usr, tl, pid>0, ghi, zeroed);
  fflush(stdout);
  ExitProcess((freed==0&&lf==0&&len==11&&wlen==5&&sys==0x409&&usr==0x409&&pid>0&&ghi&&zeroed)?0:9); return 1;
}