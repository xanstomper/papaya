#include <windows.h>
#include <stdio.h>
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s){
  (void)h;(void)p;(void)c;(void)s;
  char b[128]; int r;
  r=wsprintfA(b, "[%5d][%-5d]", 42, 42); printf("c1 r=%d b='%s'\n", r, b);
  r=wsprintfA(b, "%05d/%x", 42, 0x1a2b); printf("c2 r=%d b='%s'\n", r, b);
  r=wsprintfA(b, "%X/%c", 0x1a2b, 'Z');  printf("c3 r=%d b='%s'\n", r, b);
  fflush(stdout);
  return 0;
}
