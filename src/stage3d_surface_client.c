/* PROMO4 Stage 3d: real, documented IOAccelerator surface client - userClientType=0
 * (kIOAccelSurfaceClientType per IOAccelClientConnect.h, a real, public, shipped Apple
 * header), as opposed to the type=1 (IOATIR500GLContext) this whole project has used
 * until now. Real, documented selector numbers from IOAccelSurfaceConnect.h's
 * eIOAccelSurfaceMethods enum - not guessed.
 *
 * Starts with the safest real, documented operation: kIOAccelSurfaceGetState (selector 2),
 * a real query, before attempting anything that changes state.
 *
 * Usage: stage3d-surface-client
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

/* real, documented selector numbers, IOAccelSurfaceConnect.h eIOAccelSurfaceMethods */
#define kIOAccelSurfaceReadLockOptions   0
#define kIOAccelSurfaceReadUnlockOptions 1
#define kIOAccelSurfaceGetState         2
#define kIOAccelSurfaceWriteLockOptions 3
#define kIOAccelSurfaceWriteUnlockOptions 4
#define kIOAccelSurfaceRead             5
#define kIOAccelSurfaceSetShapeBacking  6
#define kIOAccelSurfaceSetIDMode        7
#define kIOAccelSurfaceSetScale         8
#define kIOAccelSurfaceSetShape         9
#define kIOAccelSurfaceFlush            10
#define kIOAccelSurfaceQueryLock        11
#define kIOAccelSurfaceReadLock         12
#define kIOAccelSurfaceReadUnlock       13
#define kIOAccelSurfaceWriteLock        14
#define kIOAccelSurfaceWriteUnlock      15
#define kIOAccelSurfaceControl          16
#define kIOAccelSurfaceSetShapeBackingAndLength 17

#define kIOAccelSurfaceClientType 0

static const char *krstr(kern_return_t kr) { static char b[32]; snprintf(b,sizeof b,"0x%x",kr); return b; }

int main(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (!service) { fprintf(stderr, "[stage3d] no matching service\n"); return 1; }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), kIOAccelSurfaceClientType, &conn);
    IOObjectRelease(service);
    fprintf(stderr, "[stage3d] IOServiceOpen(type=%d, kIOAccelSurfaceClientType) -> %s, connect=0x%x\n",
            kIOAccelSurfaceClientType, krstr(kr), (unsigned)conn);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[stage3d] could not open a surface-type connection - stopping here\n");
        return 1;
    }

    /* real, documented query - GetState. Try both calling shapes since we don't yet know
     * whether this selector expects scalar or structure I/O on this real client type. */
    int o0 = 0;
    kr = IOConnectMethodScalarIScalarO(conn, kIOAccelSurfaceGetState, 0, 1, &o0);
    fprintf(stderr, "[stage3d] kIOAccelSurfaceGetState via scalarO: %s out=0x%x\n", krstr(kr), o0);

    kr = IOConnectMethodScalarIStructureI(conn, kIOAccelSurfaceGetState, 0, 0, NULL);
    fprintf(stderr, "[stage3d] kIOAccelSurfaceGetState via structureI(0): %s\n", krstr(kr));

    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage3d] IOServiceClose: %s\n", krstr(kr));

    /* real health check on the already-proven GL context connection, to confirm this
     * experiment left the driver in a normal state */
    service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    io_connect_t healthConn = MACH_PORT_NULL;
    kr = IOServiceOpen(service, mach_task_self(), 1, &healthConn);
    IOObjectRelease(service);
    if (kr == KERN_SUCCESS) {
        int a=0,b=0,c=0;
        kr = IOConnectMethodScalarIScalarO(healthConn, 3, 0, 3, &a, &b, &c);
        fprintf(stderr, "[stage3d] health check (type=1, selector 3): %s out=(0x%x,0x%x,0x%x)\n", krstr(kr), a, b, c);
        IOServiceClose(healthConn);
    }
    return 0;
}
