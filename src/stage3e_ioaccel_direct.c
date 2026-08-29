/* PROMO4 Stage 3e: call the real IOAccel* functions directly, linked from IOKit.framework
 * (confirmed via `nm` to export these symbols at the same addresses observed live via gdb -
 * these are real, callable C functions, not something to hand-replicate at the IOKit
 * external-method level). Prototypes are best-effort reconstructions from real, live gdb
 * argument observations (stage3-surface-client-success.md), not from a header (Apple didn't
 * ship one for these specific functions) - genuine uncertainty remains on exact types, so
 * this is deliberately incremental: try the first, minimal call, observe the real result,
 * before chaining further. A wrong low-level prototype risks a crash in *this* process only
 * (recoverable, does not affect the G5's own health) - not a system-level risk.
 *
 * Usage: stage3e-ioaccel-direct
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* best-effort prototypes from real gdb-observed argument registers */
extern kern_return_t IOAccelFindAccelerator(int displayID, void *outA, void *outB, uint32_t size);

int main(void) {
    /* real observed call used a4=0x30 (48) as a size constant - allocate generously */
    unsigned char bufA[64], bufB[64];
    memset(bufA, 0, sizeof bufA);
    memset(bufB, 0, sizeof bufB);

    fprintf(stderr, "[stage3e] calling real IOAccelFindAccelerator(0, &bufA, &bufB, 0x30)\n");
    kern_return_t kr = IOAccelFindAccelerator(0, bufA, bufB, 0x30);
    fprintf(stderr, "[stage3e] IOAccelFindAccelerator returned 0x%x\n", kr);
    fprintf(stderr, "[stage3e] bufA[0..15]: ");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02x ", bufA[i]);
    fprintf(stderr, "\n[stage3e] bufB[0..15]: ");
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02x ", bufB[i]);
    fprintf(stderr, "\n");

    /* real health check via the already-proven GL context connection */
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (service) {
        io_connect_t conn = MACH_PORT_NULL;
        kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
        IOObjectRelease(service);
        if (kr == KERN_SUCCESS) {
            int a=0,b=0,c=0;
            kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &a, &b, &c);
            fprintf(stderr, "[stage3e] health check: %s out=(0x%x,0x%x,0x%x)\n",
                    kr == KERN_SUCCESS ? "OK" : "FAIL", a, b, c);
            IOServiceClose(conn);
        }
    }
    fprintf(stderr, "[stage3e] done, process exiting normally\n");
    return 0;
}
