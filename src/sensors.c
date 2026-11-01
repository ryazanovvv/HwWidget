// The readings: CPU temperature through the PawnIO driver (AMD Zen), GPU temperature and video
// memory through the kernel graphics interface (D3DKMT, where Task Manager gets its GPU numbers),
// RAM through GlobalMemoryStatusEx. Each poll is a handful of system calls; nothing is allocated.
#include "widget.h"
#include <stddef.h>
#include <winioctl.h>

// ---- CPU ----

// The PawnIO driver's interface, as its PawnIOLib.dll calls it. Talking to the driver directly keeps
// that library out of the process: it imports __chkstk from ntoskrnl.exe, so loading it maps the whole
// kernel image (20 MB, 6 MB of it committed) into the process.
#define PAWNIO_LOAD_BINARY CTL_CODE(0xA1B2, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define PAWNIO_EXECUTE_FN CTL_CODE(0xA1B2, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    char name[32]; // of the module's function
    ULONG64 in[1];
} PawnCall;

_Static_assert(sizeof(PawnCall) == 32 + 8, "PawnIO call: the name, then the arguments");

static WCHAR cpu_name[64];
static int cpu_status = CPU_TEMP_UNSUPPORTED;
static float tdie_offset; // added to Tctl on the first Ryzen and Threadripper models
static float last_cpu_temp = NAN;
static HANDLE pawn;      // the driver, with the AMD module loaded
static HANDLE pci_mutex; // shared by hardware monitors that touch the same registers

static void cpuid(unsigned leaf, unsigned r[4])
{
    __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

// "AMD Ryzen 9 7950X3D 16-Core Processor" → "AMD Ryzen 9 7950X3D",
// "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz" → "Intel Core i7-9700K".
static void set_cpu_name(const char *brand)
{
    size_t len = 0, cap = ARRAYSIZE(cpu_name) - 1;
    for (const char *p = brand; *p;) {
        while (*p == ' ') p++;
        const char *word = p;
        while (*p && *p != ' ') p++;
        size_t n = (size_t)(p - word);
        if (!n || *word == '@') break; // the clock speed ends the name
        if ((n == 3 && !strncmp(word, "CPU", 3)) || (n == 9 && !strncmp(word, "Processor", 9)) ||
            (n > 5 && !strncmp(p - 5, "-Core", 5)))
            continue;
        if (len && len < cap) cpu_name[len++] = L' ';
        for (const char *c = word; c < p && len < cap; c++) {
            if (!strncmp(c, "(R)", 3) || !strncmp(c, "(r)", 3)) c += 2;
            else if (!strncmp(c, "(TM)", 4) || !strncmp(c, "(tm)", 4)) c += 3;
            else cpu_name[len++] = (WCHAR)(unsigned char)*c;
        }
    }
    cpu_name[len] = 0;
}

static bool open_pawnio(void)
{
    HANDLE driver = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (driver == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        // Not installed, or no admin rights: both expected.
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && error != ERROR_ACCESS_DENIED)
            log_error("PawnIO: cannot open the driver (error %lu)", error);
        return false;
    }
    // The module that reads AMD's SMU registers, the same one LibreHardwareMonitor ships.
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAWNIO_AMD), MAKEINTRESOURCEW(10)); // RT_RCDATA
    void *blob = res ? LockResource(LoadResource(NULL, res)) : NULL;
    DWORD got;
    if (!blob || !DeviceIoControl(driver, PAWNIO_LOAD_BINARY, blob, SizeofResource(NULL, res), NULL, 0, &got, NULL)) {
        log_error("PawnIO: cannot load the AMD module (error %lu)", GetLastError());
        CloseHandle(driver);
        return false;
    }
    pawn = driver;
    pci_mutex = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");
    if (!pci_mutex) pci_mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
    return true;
}

static void open_cpu(void)
{
    unsigned r[4];
    char vendor[12], brand[49] = "";
    cpuid(0, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    cpuid(1, r);
    unsigned family = (r[0] >> 8) & 0xF;
    if (family == 0xF) family += (r[0] >> 20) & 0xFF;
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004)
        for (unsigned i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, r);
            memcpy(brand + 16 * i, r, 16);
        }
    set_cpu_name(brand);

    // Zen and later; Intel CPUs would need another PawnIO module.
    if (memcmp(vendor, "AuthenticAMD", 12) != 0 || family < 0x17) return;
    if (strstr(brand, "1600X") || strstr(brand, "1700X") || strstr(brand, "1800X")) tdie_offset = -20;
    else if (strstr(brand, "Threadripper 19") || strstr(brand, "Threadripper 29")) tdie_offset = -27;
    else if (strstr(brand, "2700X")) tdie_offset = -10;
    cpu_status = open_pawnio() ? CPU_TEMP_OK : CPU_TEMP_NO_DRIVER;
}

// Tctl from the SMU's THM_TCON_CUR_TMP register, decoded as LibreHardwareMonitor does.
static float read_cpu_temp(void)
{
    if (!pawn) return NAN;
    if (pci_mutex) {
        DWORD wait = WaitForSingleObject(pci_mutex, 10);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return last_cpu_temp; // busy: next time
    }
    PawnCall call = { "ioctl_read_smn", { 0x59800 } };
    ULONG64 out = 0;
    DWORD got = 0;
    BOOL ok = DeviceIoControl(pawn, PAWNIO_EXECUTE_FN, &call, sizeof call, &out, sizeof out, &got, NULL);
    if (pci_mutex) ReleaseMutex(pci_mutex);
    if (!ok || got < sizeof out) return last_cpu_temp = NAN;
    uint32_t raw = (uint32_t)out;
    float t = (float)((raw >> 21) * 125) * 0.001f;
    if (raw & 0x80000) t -= 49; // the extended range flag
    return last_cpu_temp = t + tdie_offset;
}

// ---- GPU ----

typedef LONG(APIENTRY *KmtCall)(void *args); // NTSTATUS, 0 on success
typedef UINT KmtHandle;

typedef struct {
    KmtHandle handle;
    LUID luid;
    ULONG sources;
    BOOL precise_present_regions;
} KmtAdapterInfo;

typedef struct {
    ULONG count;
    KmtAdapterInfo *adapters;
} KmtEnumAdapters2;

typedef struct {
    KmtHandle adapter;
    UINT type;
    void *data;
    UINT size;
} KmtQueryAdapterInfo;

typedef struct {
    KmtHandle adapter;
} KmtCloseAdapter;

typedef struct {
    UINT64 commit_limit, bytes_committed, bytes_resident;
    UINT64 bytes_evicted;
    ULONG allocs_committed, allocs_resident;
    ULONG aperture; // system memory seen through the GPU, not video memory
} KmtSegmentInfo;

// D3DKMT_QUERYSTATISTICS
typedef struct {
    UINT type;
    LUID luid;
    HANDLE process;
    union {
        ULONG segment_count; // D3DKMT_QUERYSTATISTICS_ADAPTER_INFORMATION.NbSegments
        KmtSegmentInfo segment;
        BYTE size[0x320 - 24];
    } result;
    ULONG segment_id;
} KmtStatistics;

_Static_assert(sizeof(KmtStatistics) == 0x328, "D3DKMT_QUERYSTATISTICS size");
_Static_assert(offsetof(KmtStatistics, result.segment.aperture) == 64, "D3DKMT_QUERYSTATISTICS layout");

// D3DKMT_ADAPTER_PERFDATA
typedef struct {
    UINT physical_adapter_index;
    UINT64 memory_frequency, max_memory_frequency, max_memory_frequency_oc, memory_bandwidth, pcie_bandwidth;
    ULONG fan_rpm, power, temperature; // temperature in tenths of a degree
    BYTE power_state_override;
} KmtPerfData;

// D3DKMT_ADAPTERREGISTRYINFO
typedef struct {
    WCHAR adapter[MAX_PATH], bios[MAX_PATH], dac[MAX_PATH], chip[MAX_PATH];
} KmtRegistryInfo;

enum { KMT_REGISTRY_INFO = 8, KMT_PERF_DATA = 62, KMT_STATS_ADAPTER = 0, KMT_STATS_SEGMENT = 3 };

static KmtCall kmt_enum, kmt_query_info, kmt_query_stats, kmt_close;
static KmtHandle gpu; // 0 without a GPU
static LUID gpu_luid;
static WCHAR gpu_name[128];
static int gpu_retry; // polls until the next search for a GPU

// Dedicated video memory as Task Manager counts it: what is resident in the non-aperture segments.
static bool video_memory(LUID luid, double *used, double *total)
{
    KmtStatistics q = { .type = KMT_STATS_ADAPTER, .luid = luid };
    if (kmt_query_stats(&q) != 0) return false;
    ULONG count = q.result.segment_count;
    UINT64 u = 0, t = 0;
    for (ULONG i = 0; i < count; i++) {
        q = (KmtStatistics){ .type = KMT_STATS_SEGMENT, .luid = luid, .segment_id = i };
        if (kmt_query_stats(&q) != 0 || q.result.segment.aperture) continue;
        t += q.result.segment.commit_limit;
        u += q.result.segment.bytes_resident;
    }
    *used = (double)u;
    *total = (double)t;
    return t > 0;
}

// The adapter with the most video memory: a discrete card over an integrated one, never the
// software renderer, which has none.
static void open_gpu(void)
{
    KmtAdapterInfo list[16];
    KmtEnumAdapters2 e = { ARRAYSIZE(list), list };
    if (!kmt_enum || kmt_enum(&e) != 0) return;
    int pick = -1;
    double most = 0;
    for (ULONG i = 0; i < e.count; i++) {
        double used, total;
        if (video_memory(list[i].luid, &used, &total) && total > most) {
            most = total;
            pick = (int)i;
        }
    }
    for (ULONG i = 0; i < e.count; i++) {
        if ((int)i == pick) continue;
        KmtCloseAdapter close = { list[i].handle };
        kmt_close(&close);
    }
    if (pick < 0) return;

    gpu = list[pick].handle;
    gpu_luid = list[pick].luid;
    KmtRegistryInfo info;
    KmtQueryAdapterInfo q = { gpu, KMT_REGISTRY_INFO, &info, sizeof info };
    if (kmt_query_info(&q) == 0 && info.adapter[0]) {
        wcsncpy(gpu_name, info.adapter, ARRAYSIZE(gpu_name) - 1);
        gpu_name[ARRAYSIZE(gpu_name) - 1] = 0;
    } else {
        wcscpy(gpu_name, L"Видеокарта");
    }
}

static void close_gpu(void)
{
    if (!gpu) return;
    KmtCloseAdapter close = { gpu };
    kmt_close(&close);
    gpu = 0;
}

static void read_gpu(Reading *r)
{
    r->gpu_temp = r->vram_load = NAN;
    r->vram_used = r->vram_total = 0;
    if (!gpu) {
        if (gpu_retry-- > 0) return;
        gpu_retry = 15; // every half a minute: the driver may come up late or be reinstalled
        open_gpu();
        if (!gpu) return;
    }
    if (!video_memory(gpu_luid, &r->vram_used, &r->vram_total)) {
        close_gpu(); // a driver update or reset retires the adapter; look for it again
        return;
    }
    r->vram_load = (float)(r->vram_used / r->vram_total * 100);
    KmtPerfData perf = { 0 };
    KmtQueryAdapterInfo q = { gpu, KMT_PERF_DATA, &perf, sizeof perf };
    if (kmt_query_info(&q) == 0 && perf.temperature > 0) r->gpu_temp = perf.temperature / 10.0f;
}

// ---- All of them ----

void sensors_open(void)
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    kmt_enum = PROC(KmtCall, gdi, "D3DKMTEnumAdapters2");
    kmt_query_info = PROC(KmtCall, gdi, "D3DKMTQueryAdapterInfo");
    kmt_query_stats = PROC(KmtCall, gdi, "D3DKMTQueryStatistics");
    kmt_close = PROC(KmtCall, gdi, "D3DKMTCloseAdapter");
    if (!kmt_query_info || !kmt_query_stats || !kmt_close) kmt_enum = NULL;
    open_cpu();
    open_gpu();
    if (!gpu) gpu_retry = 15;
}

void sensors_read(Reading *r)
{
    r->cpu_temp = read_cpu_temp();
    read_gpu(r);
    MEMORYSTATUSEX m = { sizeof m };
    GlobalMemoryStatusEx(&m);
    r->ram_total = (double)m.ullTotalPhys;
    r->ram_used = (double)(m.ullTotalPhys - m.ullAvailPhys);
    r->ram_load = m.ullTotalPhys ? (float)(r->ram_used / r->ram_total * 100) : 0;
}

void sensors_close(void)
{
    close_gpu();
    if (pawn) CloseHandle(pawn);
    if (pci_mutex) CloseHandle(pci_mutex);
    pawn = pci_mutex = NULL;
}

const WCHAR *sensors_cpu_name(void) { return cpu_name[0] ? cpu_name : L"Процессор"; }

const WCHAR *sensors_gpu_name(void) { return gpu ? gpu_name : NULL; }

int sensors_cpu_status(void) { return cpu_status; }
