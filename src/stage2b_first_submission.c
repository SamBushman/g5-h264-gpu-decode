/* PROMO4 Stage 2b: the actual first real PM4 submission from a hand-built client.
 *
 * Builds on Stage 2a's real, confirmed context allocation. Fills the ENTIRE mapped
 * command-buffer payload area (buffer offset 0x20 through the end, per the confirmed
 * VendorCommandBufferHeader layout) with real PM4 Type-2 filler packets (0x80000000) -
 * not just one, so no stale/uninitialized memory beyond a single packet could ever be
 * mis-parsed as something else. This exact value is confirmed safe by two independent
 * real sources (AMD's own R5xx Acceleration doc, and this exact kext's own
 * submit_ring_data function, which pads its real hardware ring with the same literal
 * value) - see promo4-client/reveng/stage2-pm4-confirmed.md.
 *
 * Then triggers the real re-map/retire cycle (IOConnectMapMemory on type 1 again) that
 * Stage 2-prep's static analysis confirmed drives real kernel-side buffer lifecycle work,
 * and calls selector 8 (finish/wait) in a BOUNDED retry loop (unlike _gldFinish's real
 * unbounded loop) so this test can't itself wedge waiting on something unexpected.
 *
 * Finishes with a fresh, independent selector-3 health check (a brand new connection,
 * same shape as Stage 1's probe) to directly verify the kext is still in a normal,
 * responsive state after the real submission - not just that this process didn't crash.
 *
 * Usage: stage2b-first-submission
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

static int health_check(const char *label) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                         IOServiceMatching("ATIRadeonX1000"));
    if (service == 0) {
        fprintf(stderr, "[health:%s] no matching service\n", label);
        return 0;
    }
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[health:%s] IOServiceOpen failed: %s\n", label, krstr(kr));
        return 0;
    }
    int o0 = 0, o1 = 0, o2 = 0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    IOServiceClose(conn);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[health:%s] selector 3 FAILED: %s\n", label, krstr(kr));
        return 0;
    }
    fprintf(stderr, "[health:%s] OK: out=(0x%x, 0x%x, 0x%x)\n", label, o0, o1, o2);
    return (o0 == 0x810040 && o1 == 0xf7c8000 && o2 == 0x10000000);
}

int main(void) {
    fprintf(stderr, "[stage2b] pre-test health check\n");
    if (!health_check("pre")) {
        fprintf(stderr, "[stage2b] pre-test health check did not match expected baseline - aborting before any real submission\n");
        return 1;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                                         IOServiceMatching("ATIRadeonX1000"));
    if (service == 0) { fprintf(stderr, "[stage2b] no matching service\n"); return 1; }

    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage2b] IOServiceOpen failed: %s\n", krstr(kr)); return 1; }
    fprintf(stderr, "[stage2b] IOServiceOpen OK, connect=0x%x\n", (unsigned)conn);

    int o0 = 0, o1 = 0, o2 = 0;
    kr = IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[stage2b] selector 3 FAILED: %s\n", krstr(kr)); IOServiceClose(conn); return 1; }
    fprintf(stderr, "[stage2b] selector 3 OK: out=(0x%x, 0x%x, 0x%x)\n", o0, o1, o2);

    struct { uint32_t type; uint32_t options; } maps[4] = { {0, 0x101}, {1, 1}, {2, 1}, {4, 1} };
    vm_address_t addrs[4] = {0,0,0,0};
    vm_size_t sizes[4] = {0,0,0,0};
    int mapped = 0;
    for (int i = 0; i < 4; i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(), &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "[stage2b] IOConnectMapMemory(type=%u) FAILED: %s\n", maps[i].type, krstr(kr));
            break;
        }
        mapped = i + 1;
        fprintf(stderr, "[stage2b] IOConnectMapMemory(type=%u) OK: addr=0x%lx size=0x%lx\n",
                maps[i].type, (unsigned long)addrs[i], (unsigned long)sizes[i]);
    }
    if (mapped != 4) {
        fprintf(stderr, "[stage2b] could not map all four real memory types - aborting, no submission attempted\n");
        for (int i = mapped - 1; i >= 0; i--) IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
        IOServiceClose(conn);
        return 1;
    }

    /* the real command-buffer payload area starts right after the confirmed 32-byte
     * VendorCommandBufferHeader (Stage 2 prep). Fill the ENTIRE remaining region with
     * real, confirmed-safe PM4 Type-2 filler packets - not just one. */
    vm_address_t cmdBuf = addrs[1];
    vm_size_t cmdBufSize = sizes[1];
    const uint32_t HEADER_BYTES = 0x20;
    if (cmdBufSize <= HEADER_BYTES) {
        fprintf(stderr, "[stage2b] command buffer too small (0x%lx) - aborting\n", (unsigned long)cmdBufSize);
        goto cleanup;
    }
    {
        uint32_t *payload = (uint32_t *)(cmdBuf + HEADER_BYTES);
        vm_size_t payloadDwords = (cmdBufSize - HEADER_BYTES) / 4;
        fprintf(stderr, "[stage2b] writing %lu real PM4 Type-2 filler dwords (0x80000000) into payload area at 0x%lx\n",
                (unsigned long)payloadDwords, (unsigned long)payload);
        for (vm_size_t i = 0; i < payloadDwords; i++) payload[i] = 0x80000000u;
        fprintf(stderr, "[stage2b] payload write complete\n");
    }

    /* trigger the real re-map/retire cycle */
    {
        vm_address_t newAddr = 0;
        vm_size_t newSize = 0;
        fprintf(stderr, "[stage2b] triggering real re-map of memory type 1 (this is the actual submission trigger)\n");
        kr = IOConnectMapMemory(conn, 1, mach_task_self(), &newAddr, &newSize, 1);
        fprintf(stderr, "[stage2b] re-map result: %s, newAddr=0x%lx newSize=0x%lx (old was 0x%lx/0x%lx)\n",
                krstr(kr), (unsigned long)newAddr, (unsigned long)newSize,
                (unsigned long)addrs[1], (unsigned long)sizes[1]);
        if (kr == KERN_SUCCESS) {
            addrs[1] = newAddr;
            sizes[1] = newSize;
        }
    }

    /* bounded finish/wait retry - real selector 8, same shape as _gldFinish, but capped */
    {
        int tries = 0;
        const int MAX_TRIES = 200; /* generous but bounded, ~ a few seconds at most */
        kern_return_t fkr;
        do {
            fkr = IOConnectMethodScalarIStructureI(conn, 8, 0, 0, NULL);
            tries++;
        } while (fkr == (kern_return_t)0xE00002D6 && tries < MAX_TRIES);
        fprintf(stderr, "[stage2b] selector 8 (finish) result after %d tries: %s\n", tries, krstr(fkr));
    }

    fprintf(stderr, "[stage2b] submission sequence complete, connection still open, proceeding to cleanup\n");

cleanup:
    for (int i = mapped - 1; i >= 0; i--) {
        kr = IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
        fprintf(stderr, "[stage2b] IOConnectUnmapMemory(type=%u): %s\n", maps[i].type, krstr(kr));
    }
    kr = IOServiceClose(conn);
    fprintf(stderr, "[stage2b] IOServiceClose: %s\n", krstr(kr));

    fprintf(stderr, "[stage2b] post-test health check\n");
    int healthy = health_check("post");
    fprintf(stderr, "[stage2b] post-test health: %s\n", healthy ? "OK, matches pre-test baseline" : "MISMATCH or FAILED");

    return healthy ? 0 : 1;
}
