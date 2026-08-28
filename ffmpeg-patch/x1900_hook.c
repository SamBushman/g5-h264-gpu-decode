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
