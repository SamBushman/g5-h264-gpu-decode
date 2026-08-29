/* PROMO4 Stage 2a: full real context memory allocation via a hand-built client -
 * replicates _gldCreateContext's exact real decompiled sequence (Stage 0) byte-for-byte,
 * through the connection Stage 1 already proved coexists safely with a live GL context.
 *
 * Deliberately conservative: performs every real IOConnectMapMemory call a genuine GL
 * context creation performs (types 0, 1, 2, 4 - real GART-backed memory allocation, the
 * same operation every OpenGL app on this system already triggers constantly), but does
 * NOT write any payload into the mapped command buffer and does NOT trigger a re-map/
 * consumption cycle - deferred pending resolution of a genuine open question (whether the
 * command-buffer payload area expects raw PM4 or an Apple-intermediate format - see
 * promo4-client/reveng/stage2-prep-command-buffer.md). This program only proves an
 * independent client can perform the real allocation dance cleanly; it does not yet submit
 * anything.
 *
 * Usage: stage2a-context-alloc
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

static const char *krstr(kern_return_t kr) {
    static char buf[32];
    snprintf(buf, sizeof buf, "0x%x", kr);
    return buf;
}

int main(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                         IOServiceMatching("ATIRadeonX1000"));
    if (service == 0) {
        fprintf(stderr, "[stage2a] no matching service\n");
        return 1;
    }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[stage2a] IOServiceOpen failed: %s\n", krstr(kr));
        return 1;
    }
    fprintf(stderr, "[stage2a] IOServiceOpen OK, connect=0x%x\n", (unsigned)conn);

    /* real init/query call, same as Stage 0/1 */
    int out0 = 0, out1 = 0, out2 = 0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &out0, &out1, &out2);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[stage2a] selector 3 FAILED: %s\n", krstr(kr));
        IOServiceClose(conn);
        return 1;
    }
    fprintf(stderr, "[stage2a] selector 3 OK: out=(0x%x, 0x%x, 0x%x)\n", out0, out1, out2);

    struct { uint32_t type; uint32_t options; } maps[4] = {
        {0, 0x101}, {1, 1}, {2, 1}, {4, 1}
    };
    vm_address_t addrs[4] = {0, 0, 0, 0};
    vm_size_t sizes[4] = {0, 0, 0, 0};
    int mapped = 0;

    for (int i = 0; i < 4; i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(),
                                 &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[stage2a] IOConnectMapMemory(type=%u) FAILED: %s\n",
                    maps[i].type, krstr(kr));
            break;
        }
        mapped = i + 1;
        fprintf(stderr, "[stage2a] IOConnectMapMemory(type=%u) OK: addr=0x%lx size=0x%lx\n",
                maps[i].type, (unsigned long)addrs[i], (unsigned long)sizes[i]);
    }

    if (mapped == 4) {
        fprintf(stderr, "[stage2a] all four real memory types mapped successfully - "
                         "full real context allocation confirmed. No payload written, "
                         "no remap triggered (deliberately deferred).\n");
    } else {
        fprintf(stderr, "[stage2a] stopped after %d/4 mappings\n", mapped);
    }

    /* clean unmap in reverse order, then close */
    for (int i = mapped - 1; i >= 0; i--) {
        kr = IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
        fprintf(stderr, "[stage2a] IOConnectUnmapMemory(type=%u): %s\n", maps[i].type, krstr(kr));
    }

    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage2a] IOServiceClose: %s\n", krstr(kr));
    return (mapped == 4) ? 0 : 1;
}
