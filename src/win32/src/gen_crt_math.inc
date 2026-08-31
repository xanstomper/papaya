// AUTO-GENERATED on 2026-08-31 by scripts/gen_crt_math.py — host-libc backed,
// real CRT math forwards (not codegen stubs). Kept small and obviously correct.
static PAPAYA_MS_ABI double hle_fwd_erf(double x) { return ::erf(x); }
static PAPAYA_MS_ABI double hle_fwd_erfc(double x)  { return ::erfc(x); }
static PAPAYA_MS_ABI double hle_fwd_expm1(double x) { return ::expm1(x); }
static PAPAYA_MS_ABI double hle_fwd_log1p(double x) { return ::log1p(x); }
static PAPAYA_MS_ABI double hle_fwd_rint(double x)  { return ::rint(x); }
static PAPAYA_MS_ABI double hle_fwd_tgamma(double x){ return ::tgamma(x); }
static PAPAYA_MS_ABI double hle_fwd_trunc(double x) { return ::trunc(x); }
static PAPAYA_MS_ABI float  hle_fwd_erff(float x)   { return ::erff(x); }
static PAPAYA_MS_ABI float  hle_fwd_erfcf(float x)  { return ::erfcf(x); }
static PAPAYA_MS_ABI float  hle_fwd_expm1f(float x) { return ::expm1f(x); }
static PAPAYA_MS_ABI float  hle_fwd_log1pf(float x) { return ::log1pf(x); }
static PAPAYA_MS_ABI float  hle_fwd_rintf(float x)  { return ::rintf(x); }
static PAPAYA_MS_ABI float  hle_fwd_tgammaf(float x){ return ::tgammaf(x); }
static PAPAYA_MS_ABI float  hle_fwd_truncf(float x) { return ::truncf(x); }

static void register_crt_math_forwards(Win32ApiHle& hle) {
    hle.register_function("UCRTBASE.DLL", "erf",     reinterpret_cast<void*>(&hle_fwd_erf));
    hle.register_function("UCRTBASE.DLL", "erfc",    reinterpret_cast<void*>(&hle_fwd_erfc));
    hle.register_function("UCRTBASE.DLL", "expm1",   reinterpret_cast<void*>(&hle_fwd_expm1));
    hle.register_function("UCRTBASE.DLL", "log1p",   reinterpret_cast<void*>(&hle_fwd_log1p));
    hle.register_function("UCRTBASE.DLL", "rint",    reinterpret_cast<void*>(&hle_fwd_rint));
    hle.register_function("UCRTBASE.DLL", "tgamma",  reinterpret_cast<void*>(&hle_fwd_tgamma));
    hle.register_function("UCRTBASE.DLL", "trunc",   reinterpret_cast<void*>(&hle_fwd_trunc));
    hle.register_function("UCRTBASE.DLL", "erff",    reinterpret_cast<void*>(&hle_fwd_erff));
    hle.register_function("UCRTBASE.DLL", "erfcf",   reinterpret_cast<void*>(&hle_fwd_erfcf));
    hle.register_function("UCRTBASE.DLL", "expm1f",  reinterpret_cast<void*>(&hle_fwd_expm1f));
    hle.register_function("UCRTBASE.DLL", "log1pf",  reinterpret_cast<void*>(&hle_fwd_log1pf));
    hle.register_function("UCRTBASE.DLL", "rintf",   reinterpret_cast<void*>(&hle_fwd_rintf));
    hle.register_function("UCRTBASE.DLL", "tgammaf", reinterpret_cast<void*>(&hle_fwd_tgammaf));
    hle.register_function("UCRTBASE.DLL", "truncf",  reinterpret_cast<void*>(&hle_fwd_truncf));
}