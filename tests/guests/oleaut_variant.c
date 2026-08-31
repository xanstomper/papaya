#include <windows.h>
#include <oleauto.h>
#include <stdio.h>

/* Regression: OLEAUT32 Variant functions (self-contained, clean signatures).
 *   VariantInit, VariantClear
 * Must exit 0. */
int main(void) {
    /* VariantInit sets vt=VT_EMPTY. */
    VARIANT v;
    VariantInit(&v);
    if (v.vt != VT_EMPTY) { printf("fail: vt=%d\n", (int)v.vt); return 1; }

    /* An int variant + clear resets to empty. */
    v.vt = VT_I4;
    v.lVal = 42;
    VariantClear(&v);
    if (v.vt != VT_EMPTY) return 2;

    /* BSTR-typed variant + clear (frees the BSTR, resets). */
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(L"test");
    if (!v.bstrVal) return 3;
    VariantClear(&v);
    if (v.vt != VT_EMPTY) return 4;

    return 0;
}