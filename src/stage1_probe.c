/* PROMO4 Stage 1: open a real, SECOND, independent IOKit connection to the
 * same ATIRadeonX1000 kext Apple's own GL driver talks to - bypassing
 * OpenGL/AGL entirely, going straight through IOServiceOpen exactly as
 * decompiled from _gldCreateContext/_gldGetRendererInfo (Stage 0, see
 * promo4-client/reveng/stage0-dispatch-table.md).
 *
 * Read-only per Stage 1's own scope: userClientType=1 (confirmed), then
 * ONLY the confirmed-safe selector 3 call (scalarI_scalarO, 0 in / 3 out) -
 * the exact same minimal round trip _gldGetRendererInfo performs on its
 * own short-lived connection. No memory mapping, no command submission.
 *
 * Usage: stage1-probe [label]
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *label = (argc > 1) ? argv[1] : "probe";

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                         IOServiceMatching("ATIRadeonX1000"));
    if (service == 0) {
        fprintf(stderr, "[stage1-probe:%s] IOServiceGetMatchingService: no match\n", label);
        return 1;
    }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[stage1-probe:%s] IOServiceOpen failed: 0x%x\n", label, kr);
        return 1;
    }
    fprintf(stderr, "[stage1-probe:%s] IOServiceOpen OK, connect=0x%x\n", label, (unsigned)conn);

    int out0 = 0, out1 = 0, out2 = 0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &out0, &out1, &out2);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[stage1-probe:%s] selector 3 call FAILED: 0x%x\n", label, kr);
        IOServiceClose(conn);
        return 1;
    }
    fprintf(stderr, "[stage1-probe:%s] selector 3 OK: out=(0x%x, 0x%x, 0x%x)\n",
            label, out0, out1, out2);

    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage1-probe:%s] IOServiceClose: 0x%x\n", label, kr);
    return 0;
}
