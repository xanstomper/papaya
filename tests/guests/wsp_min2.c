#include <windows.h>
#include <stdio.h>
static FILE* lg;
static void mark(const char* m) { if (lg) { fprintf(lg, "%s\n", m); fflush(lg); } }
int main(void) {
    char buf[64];
    lg = fopen("C:\\wsp_markers.txt", "w");   /* via papaya fs HLE -> real file */
    if (!lg) lg = fopen("wsp_markers.txt", "w");
    mark("A: start");
    wsprintfA(buf, "%d", 7);
    mark("B: 1-vararg ok");
    wsprintfA(buf, "%d-%d", 1, 2);
    mark("C: 2-vararg ok");
    wsprintfA(buf, "%d-%d-%d", 1, 2, 3);
    mark("D: 3-vararg (stack) ok");
    wsprintfA(buf, "%d-%d-%d-%d-%d", 1, 2, 3, 4, 5);
    mark("E: 5-vararg (stack) ok");
    fprintf(lg, "buf5='%s'\n", buf); fflush(lg);
    fclose(lg);
    return 0;
}
