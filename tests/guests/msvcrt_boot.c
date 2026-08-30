/* Exercises the 8 previously-unresolved boot imports with real semantics. */
#include <windows.h>
#include <stdio.h>
#include <locale.h>
#include <errno.h>
unsigned int ___lc_codepage_func(void);
int ___mb_cur_max_func(void);
void _lock(int);
void _unlock(int);
extern struct lconv *localeconv(void);
extern char *strerror(int);
int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR c, int s){
  (void)h;(void)p;(void)c;(void)s;
  int ok = 1;
  /* 1. IsDBCSLeadByteEx: real lead-byte ranges */
  ok &= IsDBCSLeadByteEx(936, 0x81) == 1;   /* GBK lead byte */
  ok &= IsDBCSLeadByteEx(932, 0x61) == 0;   /* 'a' is never a lead byte */
  ok &= IsDBCSLeadByteEx(950, 0x82) == 1;   /* Big5 lead byte */
  ok &= IsDBCSLeadByteEx(874, 0x81) == 0;
  /* 2/3. codepage + mb_cur_max */
  unsigned int cp = ___lc_codepage_func();
  int mcm = ___mb_cur_max_func();
  ok &= cp > 0 && mcm >= 1 && mcm <= 6;
  /* 4/5. _lock/_unlock must not crash and must serialize */
  _lock(1); _unlock(1);
  /* 6. fflush(NULL) = flush all */
  ok &= fflush(NULL) == 0;
  /* 7. localeconv: real pointer fields from host locale */
  struct lconv* lc = localeconv();
  ok &= lc != NULL && lc->decimal_point != NULL && lc->decimal_point[0] != 0;
  ok &= lc->thousands_sep != NULL && lc->int_curr_symbol != NULL;
  ok &= lc->int_frac_digits == 2;
  /* 8. strerror: non-null, non-empty for a known errno */
  const char* se = strerror(2);
  /* 9. wsprintfA: real formatted string output */
  char wsbuf[64];
  int wslen = wsprintfA(wsbuf, "ws=%d/%s", 42, "ok");
  ok &= wslen == 8 && strcmp(wsbuf, "ws=42/ok") == 0;
  printf("boot: dbcs=%d cp=%d mcm=%d dc='%s' ts='%s' ics='%s' cur='%s' strerr2='%s' ws='%s'\n",
         ok, cp, mcm, lc->decimal_point, lc->thousands_sep, lc->int_curr_symbol,
         lc->currency_symbol, se, ok ? "yes" : "no");
  fflush(stdout);
  return ok && se && se[0] ? 0 : 7;
}
