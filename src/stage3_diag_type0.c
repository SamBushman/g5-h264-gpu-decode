/* Read-only diagnostic: dump the first 256 bytes of memory type 0 (the 4KB status region) to
 * see if it's a live MMIO passthrough window - and, separately, write US_CODE_ADDR via a real
 * PM4 packet and see whether that same window reflects the write anywhere, to help disambiguate
 * Stage 3's real finding (does the register write land at all, or does AGL's own state
 * revalidation simply overwrite it before the next draw). Pure read after the write; the write
 * itself uses the exact same proven submission mechanism as Stage 2/3.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

static const char *krstr(kern_return_t kr) { static char b[32]; snprintf(b,sizeof b,"0x%x",kr); return b; }

static uint32_t type0_header(uint32_t baseIndex, uint32_t countMinus1, int oneRegWr) {
    return (baseIndex & 0x1FFFu) | ((uint32_t)(oneRegWr?1:0)<<15) | ((countMinus1 & 0x3FFFu)<<16);
}

int main(void) {
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("ATIRadeonX1000"));
    if (!service) { fprintf(stderr, "no service\n"); return 1; }
    io_connect_t conn = MACH_PORT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 1, &conn);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "open failed %s\n", krstr(kr)); return 1; }
    int o0=0,o1=0,o2=0;
    IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0, &o1, &o2);

    struct { uint32_t type; uint32_t options; } maps[4] = { {0,0x101}, {1,1}, {2,1}, {4,1} };
    vm_address_t addrs[4]={0,0,0,0}; vm_size_t sizes[4]={0,0,0,0}; int mapped=0;
    for (int i=0;i<4;i++) {
        kr = IOConnectMapMemory(conn, maps[i].type, mach_task_self(), &addrs[i], &sizes[i], maps[i].options);
        if (kr != KERN_SUCCESS) { fprintf(stderr, "map %u failed %s\n", maps[i].type, krstr(kr)); break; }
        mapped = i+1;
    }
    if (mapped != 4) { fprintf(stderr, "could not map all 4\n"); goto cleanup; }

    fprintf(stderr, "type0 addr=0x%lx size=0x%lx\n", (unsigned long)addrs[0], (unsigned long)sizes[0]);
    fprintf(stderr, "--- type0 region, first 256 bytes, BEFORE write ---\n");
    {
        uint32_t *w = (uint32_t*)addrs[0];
        for (int i = 0; i < 64; i += 4) {
            fprintf(stderr, "off 0x%03x: %08x %08x %08x %08x\n", i*4, w[i], w[i+1], w[i+2], w[i+3]);
        }
    }

    /* Write a distinctive value to US_CODE_ADDR (slot 77/77 - a recognizable marker) via real PM4 */
    {
        const uint32_t HEADER_BYTES = 0x20;
        uint32_t *payload = (uint32_t *)(addrs[1] + HEADER_BYTES);
        vm_size_t payloadDwords = (sizes[1] - HEADER_BYTES) / 4;
        uint32_t *p = payload;
        *p++ = type0_header(0x118C, 0, 0);          /* US_CODE_ADDR */
        *p++ = (77u << 16) | 77u;                    /* distinctive marker value 0x004D004D */
        vm_size_t used = (vm_size_t)(p - payload);
        for (vm_size_t i = used; i < payloadDwords; i++) payload[i] = 0x80000000u;

        vm_address_t na=0; vm_size_t ns=0;
        kr = IOConnectMapMemory(conn, 1, mach_task_self(), &na, &ns, 1);
        fprintf(stderr, "submit result: %s\n", krstr(kr));
        int tries=0; kern_return_t fkr;
        do { fkr = IOConnectMethodScalarIStructureI(conn, 8, 0, 0, NULL); tries++; }
        while (fkr == (kern_return_t)0xE00002D6 && tries < 200);
        fprintf(stderr, "finish after %d tries: %s\n", tries, krstr(fkr));
    }

    fprintf(stderr, "--- type0 region, first 256 bytes, AFTER write ---\n");
    {
        uint32_t *w = (uint32_t*)addrs[0];
        for (int i = 0; i < 64; i += 4) {
            fprintf(stderr, "off 0x%03x: %08x %08x %08x %08x\n", i*4, w[i], w[i+1], w[i+2], w[i+3]);
        }
    }

cleanup:
    for (int i=mapped-1;i>=0;i--) IOConnectUnmapMemory(conn, maps[i].type, mach_task_self(), addrs[i]);
    IOServiceClose(conn);
    return 0;
}
