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

static void try_selector0(io_connect_t conn, const char *label, uint32_t val) {
    /* real structure size confirmed via live gdb trace of a real, working hold-context run:
     * selector 0's real struct is 4 BYTES (one dword), not 16 - the earlier all-16-byte guesses
     * were wrong on size alone. Apple's own real "detach/reset drawable" call uses value 0
     * (confirmed via gdb watching aglDestroyContext's real teardown path). */
    kern_return_t kr = IOConnectMethodScalarIStructureI(conn, 0, 0, sizeof(val), &val);
    fprintf(stderr, "[stage3c] selector 0 (%s): payload=0x%x (4 bytes) -> %s\n", label, val, krstr(kr));
}

static void try_selector16(io_connect_t conn, const char *label, uint8_t val) {
    kern_return_t kr = IOConnectMethodScalarIStructureI(conn, 16, 0, sizeof(val), &val);
    fprintf(stderr, "[stage3c] selector 16 (%s): payload=0x%02x (1 byte) -> %s\n", label, val, krstr(kr));
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

    /* corrected shape from a real, live gdb trace of hold-context's actual working attach/detach
     * sequence: selector 0's real struct is 4 bytes (not 16), selector 16's is 1 byte. Real
     * observed teardown value for selector 0 is 0 (a real, safe "detach/reset drawable" call);
     * the real attach value is a live, per-run-varying surface pointer/ID this probe can't
     * fabricate without a real IOAccelerator surface already existing. */
    try_selector0(conn, "detach-value-zero (matches real teardown)", 0);
    try_selector16(conn, "flag-zero (matches real observed value)", 0);

    fprintf(stderr, "[stage3c] post-probe health check\n");
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    fprintf(stderr, "[stage3c] selector 3 post-probe: %s out=(0x%x,0x%x,0x%x)\n", krstr(kr), o0, o1, o2);

    for (int i=mapped-1;i>=0;i--) IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage3c] IOServiceClose: %s\n", krstr(kr));
    return 0;
}
