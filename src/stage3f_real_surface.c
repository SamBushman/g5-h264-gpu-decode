/* PROMO4 Stage 3f: real IOAccel* surface creation using prototypes confirmed via raw PowerPC
 * disassembly (not decompiler inference) of the exact real call sites in _CGLSetPBuffer -
 * see promo4-client/reveng/stage3-disasm-confirmed-prototypes.md for the full derivation.
 *
 * Real, disassembly-confirmed shape structure for IOAccelSetSurfaceFramebufferShape (20 bytes):
 *   offset 0x00: uint32_t   = 1 (constant, always)
 *   offset 0x04: uint16_t   = 0
 *   offset 0x06: uint16_t   = 0
 *   offset 0x08: uint16_t   = width
 *   offset 0x0a: uint16_t   = height
 *   offset 0x0c-0x12: uint16_t x3 = 0
 *
 * Real, disassembly-confirmed IOAccelCreateSurface(r3,r4,r5,r6) - 4 args, r6 a real output
 * pointer (computed via addi, not loaded). Matches the shape already used in Stage 3e.
 *
 * Usage: stage3f-real-surface
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* corrected via real, exact disassembly of the call sites (not decompiler inference) - see
 * promo4-client/reveng/stage3-consolidated-summary.md's disassembly follow-up. FindAccelerator
 * and CreateAccelID were each called with FEWER real arguments than originally guessed; the
 * extra, bogus arguments in the earlier attempt landed in unused registers for CreateAccelID
 * (explaining why it worked anyway) but corrupted the real call for FindAccelerator. */
extern kern_return_t IOAccelFindAccelerator(uint32_t param1, void *outA, void *outB);
extern kern_return_t IOAccelCreateAccelID(uint32_t param1, uint32_t *outValue);
extern kern_return_t IOAccelCreateSurface(uint32_t service, uint32_t uniqueID, uint32_t type, uint32_t *outSurfaceID);
extern kern_return_t IOAccelSetSurfaceFramebufferShape(uint32_t surfaceID, void *shapePtr, uint32_t opt, uint32_t accelID);

int main(void) {
    unsigned char bufA[64], bufB[64];
    memset(bufA, 0, sizeof bufA);
    memset(bufB, 0, sizeof bufB);
    fprintf(stderr, "[stage3f] IOAccelFindAccelerator(0, &bufA, &bufB)  [corrected: 3 real args]\n");
    kern_return_t kr = IOAccelFindAccelerator(0, bufA, bufB);
    fprintf(stderr, "[stage3f]   -> 0x%x\n", kr);
    fprintf(stderr, "[stage3f]   bufA: "); for (int i=0;i<16;i++) fprintf(stderr,"%02x ",bufA[i]); fprintf(stderr,"\n");
    fprintf(stderr, "[stage3f]   bufB: "); for (int i=0;i<16;i++) fprintf(stderr,"%02x ",bufB[i]); fprintf(stderr,"\n");

    uint32_t accelID = 0;
    fprintf(stderr, "[stage3f] IOAccelCreateAccelID(0, &accelID)  [corrected: 2 real args]\n");
    kr = IOAccelCreateAccelID(0, &accelID);
    fprintf(stderr, "[stage3f]   -> 0x%x, accelID=0x%x\n", kr, accelID);

    /* real, confirmed service value from every live gdb trace this session was 0x1f07 - try
     * both that literal (in case it's stable across runs/processes) and whatever accelID we
     * just got back, since we don't yet know for certain which one IOAccelCreateSurface's
     * real first argument expects. */
    uint32_t surfaceID = 0;
    uint32_t uniq = (uint32_t)((uintptr_t)&surfaceID) & 0x0fffffff; /* a real, process-local unique-ish value */

    /* try our own accelID first */
    fprintf(stderr, "[stage3f] IOAccelCreateSurface(accelID=0x%x, uniq=0x%x, type=0, &surfaceID)\n", accelID, uniq);
    kr = IOAccelCreateSurface(accelID, uniq, 0, &surfaceID);
    fprintf(stderr, "[stage3f]   -> 0x%x, surfaceID=0x%x\n", kr, surfaceID);

    /* real, stable value observed in every live gdb trace of Apple's own working PBuffer setup
     * this session (hold-context, three separate runs) - try it literally in case it's what
     * IOAccelCreateSurface's first argument really expects, distinct from our own accelID */
    surfaceID = 0;
    fprintf(stderr, "[stage3f] IOAccelCreateSurface(service=0x1f07 [real observed value], uniq=0x%x, type=0, &surfaceID)\n", uniq);
    kr = IOAccelCreateSurface(0x1f07, uniq, 0, &surfaceID);
    fprintf(stderr, "[stage3f]   -> 0x%x, surfaceID=0x%x\n", kr, surfaceID);

    if (kr == KERN_SUCCESS && surfaceID != 0) {
        struct __attribute__((packed)) {
            uint32_t one;
            uint16_t z1, z2, width, height, z3, z4, z5;
        } shape = { 1, 0, 0, 4, 4, 0, 0, 0 };
        fprintf(stderr, "[stage3f] IOAccelSetSurfaceFramebufferShape(surfaceID=0x%x, &shape(4x4), 0x81, accelID=0x%x)\n",
                surfaceID, accelID);
        kr = IOAccelSetSurfaceFramebufferShape(surfaceID, &shape, 0x81, accelID);
        fprintf(stderr, "[stage3f]   -> 0x%x\n", kr);
    } else {
        fprintf(stderr, "[stage3f] skipping SetSurfaceFramebufferShape - no valid surfaceID\n");
    }

    /* real health check */
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (service) {
        io_connect_t conn = MACH_PORT_NULL;
        kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
        IOObjectRelease(service);
        if (kr == KERN_SUCCESS) {
            int a=0,b=0,c=0;
            kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &a, &b, &c);
            fprintf(stderr, "[stage3f] health check: %s out=(0x%x,0x%x,0x%x)\n",
                    kr == KERN_SUCCESS ? "OK" : "FAIL", a, b, c);
            IOServiceClose(conn);
        }
    }
    fprintf(stderr, "[stage3f] done, process exiting normally\n");
    return 0;
}
