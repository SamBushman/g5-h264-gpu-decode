#include "x1900_hook.h"
#include <stddef.h>

static X1900MbHookFn g_hook = NULL;
static void *g_hook_userdata = NULL;

void ff_x1900_set_mb_hook(X1900MbHookFn fn, void *userdata) {
    g_hook = fn;
    g_hook_userdata = userdata;
}

int ff_x1900_call_mb_hook(const X1900MbInfo *info) {
    if (!g_hook) return 0;
    return g_hook(info, g_hook_userdata);
}

/* Item 9 frame-scale restructure (2026-08-28): lets h264_slice.c decide
 * whether to postpone deblocking for this decode. Real bug found and
 * fixed during verification: this must NOT just check "is a hook
 * installed" (g_hook != NULL) - the test harness installs the SAME hook
 * object for both its "live" pass (hook actively taking over
 * macroblocks) AND its "ref"/CPU-only pass (hook installed but its own
 * g_live flag makes it decline everything immediately) - checking
 * g_hook!=NULL alone would ALSO postpone deblocking for the ref pass,
 * silently turning the "stable, untouched baseline" this project's whole
 * verification methodology depends on into something that was itself
 * being changed. The test harness now calls ff_x1900_set_postpone_wanted
 * explicitly, tied to its own g_live flag, not hook installation. */
static int g_postpone_wanted = 0;
void ff_x1900_set_postpone_wanted(int enable) {
    g_postpone_wanted = enable;
}
int ff_x1900_hook_installed(void) {
    return g_postpone_wanted;
}

static X1900DeblockHookFn g_deblock_hook = NULL;
static void *g_deblock_hook_userdata = NULL;

void ff_x1900_set_deblock_hook(X1900DeblockHookFn fn, void *userdata) {
    g_deblock_hook = fn;
    g_deblock_hook_userdata = userdata;
}

int ff_x1900_call_deblock_hook(uint8_t *pix, int stride, int alpha, int beta,
                                const int8_t *tc0) {
    if (!g_deblock_hook) return 0;
    return g_deblock_hook(pix, stride, alpha, beta, tc0, g_deblock_hook_userdata);
}
