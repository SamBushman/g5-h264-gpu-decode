/* PROMO4 Stage 3c: safe, bounded empirical probe of selector 0 ("attach drawable" candidate,
 * per stage3-attach-drawable-selectors.md's real decompile of _gldAttachDrawable). External
 * methods go through the kernel's own validated dispatch and argument-count checking - unlike
 * a raw PM4/register write, a wrong guess here fails safely with a real IOReturn code rather
 * than risking undefined hardware behavior. Read-only in effect: only ever calls documented,
 * validated external methods, never touches raw MMIO or PM4 content.
 *
 * Tries selector 0 with a real 4-dword structure input, several reasoned parameter guesses,
 * on top of the already-proven real context allocation (Stage 2a). Reports the real result
 * code for each - informative regardless of success, since it confirms whether the kernel's
 * dispatch table treats selector 0 distinctly from an out-of-range/unimplemented selector.
 *
 * Usage: stage3c-attach-probe
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

static const char *krstr(kern_return_t kr) { static char b[32]; snprintf(b,sizeof b,"0x%x",kr); return b; }

static void try_selector0(io_connect_t conn, const char *label, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t payload[4] = {a, b, c, d};
    kern_return_t kr = IOConnectMethodScalarIStructureI(conn, 0, 0, sizeof(payload), payload);
    fprintf(stderr, "[stage3c] selector 0 (%s): payload=(0x%x,0x%x,0x%x,0x%x) -> %s\n",
            label, a, b, c, d, krstr(kr));
}

int main(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (!service) { fprintf(stderr, "[stage3c] no matching service\n"); return 1; }
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage3c] IOServiceOpen failed: %s\n", krstr(kr)); return 1; }
    fprintf(stderr, "[stage3c] IOServiceOpen OK, connect=0x%x\n", (unsigned)conn);

    int o0=0,o1=0,o2=0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    fprintf(stderr, "[stage3c] selector 3 baseline: %s out=(0x%x,0x%x,0x%x)\n", krstr(kr), o0, o1, o2);

    struct { uint32_t type; uint32_t options; } maps[4] = { {0,0x101}, {1,1}, {2,1}, {4,1} };
    vm_address_t addrs[4]={0,0,0,0}; vm_size_t sizes[4]={0,0,0,0}; int mapped=0;
    for (int i=0;i<4;i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(), &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage3c] map type=%u failed: %s\n", maps[i].type, krstr(kr)); break; }
        mapped = i+1;
    }
    fprintf(stderr, "[stage3c] real context allocation: %d/4 types mapped\n", mapped);

    /* try a few reasoned parameter sets - all safe, all go through validated kernel dispatch */
    try_selector0(conn, "all-zero", 0, 0, 0, 0);
    try_selector0(conn, "flags-guess", 0, 0x803f, 0, 0);
    try_selector0(conn, "small-dims", 4, 0, 4, 4);

    fprintf(stderr, "[stage3c] post-probe health check\n");
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    fprintf(stderr, "[stage3c] selector 3 post-probe: %s out=(0x%x,0x%x,0x%x)\n", krstr(kr), o0, o1, o2);

    for (int i=mapped-1;i>=0;i--) IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage3c] IOServiceClose: %s\n", krstr(kr));
    return 0;
}
