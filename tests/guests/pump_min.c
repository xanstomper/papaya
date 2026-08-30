#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: printf("EVT create\n"); fflush(stdout); return 0;
    case WM_SIZE: printf("EVT size\n"); fflush(stdout); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); EndPaint(h,&ps);
        printf("EVT paint\n"); fflush(stdout); return 0; }
    case WM_TIMER: printf("EVT timer\n"); fflush(stdout); PostQuitMessage(0); return 0;
  }
  return DefWindowProcA(h,m,w,l);
}
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {
  (void)p;(void)c;(void)s;
  WNDCLASSA wc={0}; wc.lpfnWndProc=WndProc; wc.hInstance=h; wc.lpszClassName="MinPump";
  if(!RegisterClassA(&wc)){printf("FAIL reg\n");return 1;}
  HWND w=CreateWindowA("MinPump","x",WS_OVERLAPPEDWINDOW,0,0,200,150,0,0,h,0);
  if(!w){printf("FAIL create\n");return 2;}
  ShowWindow(w,SW_SHOW); UpdateWindow(w);
  SetTimer(w,1,40,NULL);
  int n=0; MSG msg;
  while(GetMessageA(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageA(&msg);n++;if(n>200)break;}
  printf("pumped=%d quit=%d\n",n,msg.message==WM_QUIT); fflush(stdout);
  return 0;
}
