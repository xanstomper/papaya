#include <windows.h>
#include <stdio.h>
static HWND g_wnd = 0;
static int got_create=0, got_size=0, got_paint=0, got_activate=0, got_focus=0, got_ncdestroy=0;
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_CREATE: got_create=1; return 0;
    case WM_SIZE: if (l>0) got_size=1; return 0;
    case WM_ACTIVATE: got_activate=1; return 0;
    case WM_SETFOCUS: got_focus=1; return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        Rectangle(dc,10,10,100,100); EndPaint(h,&ps); got_paint=1; return 0; }
    case WM_NCCREATE: return 1; /* allow creation */
    case WM_NCDESTROY: got_ncdestroy=1; return 0;
    case WM_TIMER: KillTimer(h,1); PostQuitMessage(0); return 0;
  }
  return DefWindowProcA(h,m,w,l);
}
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s) {
  (void)p;(void)c;
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = WndProc; wc.hInstance = h; wc.lpszClassName = "MsgPumpTest";
  wc.hCursor = LoadCursorA(0,(LPCSTR)IDC_ARROW);
  if (!RegisterClassA(&wc)) { printf("FAIL register\n"); return 1; }
  g_wnd = CreateWindowA("MsgPumpTest","MsgPump",WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT,CW_USEDEFAULT,320,240,0,0,h,0);
  if (!g_wnd) { printf("FAIL create\n"); return 2; }
  UpdateWindow(g_wnd);
  InvalidateRect(g_wnd, NULL, TRUE);   /* queue a WM_PAINT */
  SetTimer(g_wnd, 1, 30, NULL);        /* exit via WM_TIMER -> PostQuitMessage */
  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  printf("create=%d size=%d paint=%d activate=%d focus=%d ncdestroy=%d\n",
         got_create, got_size, got_paint, got_activate, got_focus, got_ncdestroy);
  fflush(stdout);
  return (got_create&&got_size&&got_paint&&got_activate&&got_focus&&!got_ncdestroy)?0:7;
}
