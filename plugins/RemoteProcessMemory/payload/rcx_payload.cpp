/*
 * rcx_payload  --  injected into target process.
 *
 * Pure Win32 / POSIX, NO Qt, minimal footprint.
 * Creates the main IPC channel (shared memory + events/semaphores)
 * using PID-only naming and uses a timer queue for polling.
 */

#include "../rcx_rpc_protocol.h"

#ifdef _WIN32
/* ===================================================================
 * WINDOWS implementation
 * =================================================================== */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>

/* ── globals ──────────────────────────────────────────────────────── */
static HANDLE  g_hShm          = nullptr;
static void*   g_mappedView    = nullptr;
static HANDLE  g_hReqEvent     = nullptr;
static HANDLE  g_hRspEvent     = nullptr;
static HANDLE  g_hTimerQueue   = nullptr;
static HANDLE  g_hPollTimer    = nullptr;
static volatile LONG g_initialized = 0;

/* ── memory safety via VirtualQuery ────────────────────────────────── */

inline bool IsReadableProtect(DWORD p)
{
    if (p & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    const DWORD readable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    return (p & readable) != 0;
}

inline bool IsWritableProtect(DWORD p)
{
    if (p & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    const DWORD writable =
        PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

    return (p & writable) != 0;
}

/* Check that the full range [addr, addr+len) is covered by readable pages. */
static bool IsRangeReadable(uintptr_t addr, uint32_t len)
{
    uintptr_t end = addr + len;
    uintptr_t cur = addr;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT || !IsReadableProtect(mbi.Protect))
            return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        cur = regionEnd;
    }
    return true;
}

static bool IsRangeWritable(uintptr_t addr, uint32_t len)
{
    uintptr_t end = addr + len;
    uintptr_t cur = addr;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT || !IsWritableProtect(mbi.Protect))
            return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        cur = regionEnd;
    }
    return true;
}

/* ── command handlers ─────────────────────────────────────────────── */

static void handle_read_batch(RcxRpcHeader* hdr, uint8_t* data)
{
    auto* entries = reinterpret_cast<RcxRpcReadEntry*>(data);
    for (uint32_t i = 0; i < hdr->requestCount; ++i) {
        uint8_t* dest = data + entries[i].dataOffset;
        uintptr_t src = static_cast<uintptr_t>(entries[i].address);
        if (IsRangeReadable(src, entries[i].length)) {
            memcpy(dest, reinterpret_cast<const void*>(src), entries[i].length);
        } else {
            memset(dest, 0, entries[i].length);
            hdr->status = RCX_RPC_STATUS_PARTIAL;
        }
        /* SEH fallback (commented out, kept for reference):
        __try {
            memcpy(dest, reinterpret_cast<const void*>(src), entries[i].length);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(dest, 0, entries[i].length);
            hdr->status = RCX_RPC_STATUS_PARTIAL;
        }
        */
    }
    hdr->responseCount = hdr->requestCount;
}

static void handle_write(RcxRpcHeader* hdr, uint8_t* data)
{
    uintptr_t dst = static_cast<uintptr_t>(hdr->writeAddress);
    if (IsRangeWritable(dst, hdr->writeLength)) {
        memcpy(reinterpret_cast<void*>(dst), data, hdr->writeLength);
    } else {
        hdr->status = RCX_RPC_STATUS_ERROR;
    }
    /* SEH fallback (commented out, kept for reference):
    __try {
        memcpy(reinterpret_cast<void*>(dst), data, hdr->writeLength);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hdr->status = RCX_RPC_STATUS_ERROR;
    }
    */
}

static void handle_enum_modules(RcxRpcHeader* hdr, uint8_t* data)
{
    HANDLE hProc = GetCurrentProcess();
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(hProc, mods, sizeof(mods), &needed)) {
        hdr->status = RCX_RPC_STATUS_ERROR;
        hdr->responseCount = 0;
        return;
    }
    int count = (int)(needed / sizeof(HMODULE));
    if (count > 1024) count = 1024;

    uint32_t entryBytes = (uint32_t)(count * sizeof(RcxRpcModuleEntry));
    uint32_t nameDataOff = entryBytes;

    for (int i = 0; i < count; ++i) {
        MODULEINFO mi{};
        WCHAR modName[MAX_PATH];
        GetModuleInformation(hProc, mods[i], &mi, sizeof(mi));
        int nameLen = (int)GetModuleBaseNameW(hProc, mods[i], modName, MAX_PATH);
        uint32_t nameBytes = (uint32_t)(nameLen * sizeof(WCHAR));

        auto* entry = reinterpret_cast<RcxRpcModuleEntry*>(data + i * sizeof(RcxRpcModuleEntry));
        entry->base       = reinterpret_cast<uint64_t>(mi.lpBaseOfDll);
        entry->size       = static_cast<uint64_t>(mi.SizeOfImage);
        entry->nameOffset = nameDataOff;
        entry->nameLength = nameBytes;

        if (nameDataOff + nameBytes <= RCX_RPC_DATA_SIZE) {
            memcpy(data + nameDataOff, modName, nameBytes);
            nameDataOff += nameBytes;
        }
    }

    hdr->responseCount = (uint32_t)count;
    hdr->totalDataUsed = nameDataOff;
    hdr->status        = RCX_RPC_STATUS_OK;
}

/* forward declaration */
void RcxPayloadCleanup();

/* ── timer callback (non-blocking poll) ───────────────────────────── */

static VOID CALLBACK RcxPollTimerCallback(PVOID, BOOLEAN)
{
    if (!g_mappedView || !g_hReqEvent || !g_hRspEvent)
        return;

    /* non-blocking check: is there a pending request? */
    DWORD rc = WaitForSingleObject(g_hReqEvent, 0);
    if (rc != WAIT_OBJECT_0)
        return;

    auto* hdr  = static_cast<RcxRpcHeader*>(g_mappedView);
    auto* data = reinterpret_cast<uint8_t*>(g_mappedView) + RCX_RPC_DATA_OFFSET;

    hdr->status = RCX_RPC_STATUS_OK;

    switch (static_cast<RcxRpcCommand>(hdr->command)) {
    case RPC_CMD_READ_BATCH:   handle_read_batch(hdr, data); break;
    case RPC_CMD_WRITE:        handle_write(hdr, data);      break;
    case RPC_CMD_ENUM_MODULES: handle_enum_modules(hdr, data); break;
    case RPC_CMD_PING:         break;
    case RPC_CMD_SHUTDOWN:
        RcxPayloadCleanup();
        return;
    default:
        hdr->status = RCX_RPC_STATUS_ERROR;
        break;
    }

    SetEvent(g_hRspEvent);
}

/* ── cleanup ──────────────────────────────────────────────────────── */

void RcxPayloadCleanup()
{
    if (!InterlockedCompareExchange(&g_initialized, 0, 0))
        return;

    /* stop the poll timer first */
    if (g_hTimerQueue) {
        DeleteTimerQueueEx(g_hTimerQueue, INVALID_HANDLE_VALUE); /* waits for callbacks */
        g_hTimerQueue = nullptr;
        g_hPollTimer  = nullptr;
    }

    /* mark not-ready */
    if (g_mappedView) {
        auto* hdr = static_cast<RcxRpcHeader*>(g_mappedView);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->payloadReady), 0);
    }

    if (g_mappedView) { UnmapViewOfFile(g_mappedView); g_mappedView = nullptr; }
    if (g_hShm)       { CloseHandle(g_hShm);           g_hShm       = nullptr; }
    if (g_hReqEvent)  { CloseHandle(g_hReqEvent);      g_hReqEvent  = nullptr; }
    if (g_hRspEvent)  { CloseHandle(g_hRspEvent);      g_hRspEvent  = nullptr; }

    InterlockedExchange(&g_initialized, 0);
}

/* ── init (called AFTER DllMain returns — safe for timer queues) ── */

extern "C" __declspec(dllexport)
bool RcxPayloadInit()
{
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0)
        return true;   /* already initialized */

    uint32_t pid = GetCurrentProcessId();

    char shmName[128], reqName[128], rspName[128];
    rcx_rpc_shm_name(shmName, sizeof(shmName), pid);
    rcx_rpc_req_name(reqName, sizeof(reqName), pid);
    rcx_rpc_rsp_name(rspName, sizeof(rspName), pid);

    g_hShm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                PAGE_READWRITE, 0, RCX_RPC_SHM_SIZE, shmName);
    if (!g_hShm) {
        InterlockedExchange(&g_initialized, 0);
        return false;
    }

    g_mappedView = MapViewOfFile(g_hShm, FILE_MAP_ALL_ACCESS, 0, 0, RCX_RPC_SHM_SIZE);
    if (!g_mappedView) {
        CloseHandle(g_hShm); g_hShm = nullptr;
        InterlockedExchange(&g_initialized, 0);
        return false;
    }

    memset(g_mappedView, 0, RCX_RPC_HEADER_SIZE);
    auto* hdr = static_cast<RcxRpcHeader*>(g_mappedView);
    hdr->version = RCX_RPC_VERSION;

    /* image base from PEB */
    {
#if defined(_MSC_VER)
        uint64_t peb = (uint64_t)__readgsqword(0x60);
#else
        uint64_t peb;
        asm volatile("mov %%gs:0x60, %0" : "=r"(peb));
#endif
        uint64_t ldr       = *reinterpret_cast<uint64_t*>(peb + 0x18);
        uint64_t firstLink = *reinterpret_cast<uint64_t*>(ldr + 0x10);
        hdr->imageBase     = *reinterpret_cast<uint64_t*>(firstLink + 0x30);
    }

    g_hReqEvent = CreateEventA(nullptr, FALSE, FALSE, reqName);
    g_hRspEvent = CreateEventA(nullptr, FALSE, FALSE, rspName);
    if (!g_hReqEvent || !g_hRspEvent) {
        RcxPayloadCleanup();
        return false;
    }

    /* create dedicated timer queue + fast poll timer (10ms interval) */
    g_hTimerQueue = CreateTimerQueue();
    if (!g_hTimerQueue) {
        RcxPayloadCleanup();
        return false;
    }

    if (!CreateTimerQueueTimer(&g_hPollTimer, g_hTimerQueue,
                               RcxPollTimerCallback, nullptr,
                               0,     /* start immediately */
                               10,    /* 10ms repeat */
                               WT_EXECUTEDEFAULT)) {
        RcxPayloadCleanup();
        return false;
    }

    /* mark ready */
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&hdr->payloadReady), 1);
    return true;
}

/* ── DllMain — minimal, no heavy work under loader lock ───────────── */

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH) {
        RcxPayloadCleanup();
    }
    return TRUE;
}

#else
/* ===================================================================
 * LINUX implementation
 * =================================================================== */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

/* ── globals ──────────────────────────────────────────────────────── */
static int       g_shmFd     = -1;
static void*     g_mappedView = nullptr;
static sem_t*    g_reqSem    = SEM_FAILED;
static sem_t*    g_rspSem    = SEM_FAILED;
static pthread_t g_thread;
static volatile int g_shutdown  = 0;
static volatile int g_threadRunning = 0;
static int       g_memFd     = -1;   /* /proc/self/mem for safe access */
static char      g_shmName[128];
static char      g_reqName[128];
static char      g_rspName[128];

/* ── safe memory access via /proc/self/mem ────────────────────────── */

static void safe_read(uint64_t addr, void* dest, uint32_t len, uint32_t* status)
{
    ssize_t n = pread(g_memFd, dest, len, (off_t)addr);
    if (n < (ssize_t)len) {
        if (n > 0)
            memset((uint8_t*)dest + n, 0, len - (uint32_t)n);
        else
            memset(dest, 0, len);
        *status = RCX_RPC_STATUS_PARTIAL;
    }
}

static void safe_write(uint64_t addr, const void* src, uint32_t len, uint32_t* status)
{
    ssize_t n = pwrite(g_memFd, src, len, (off_t)addr);
    if (n < (ssize_t)len)
        *status = RCX_RPC_STATUS_ERROR;
}

/* ── command handlers ─────────────────────────────────────────────── */

static void handle_read_batch(RcxRpcHeader* hdr, uint8_t* data)
{
    auto* entries = reinterpret_cast<RcxRpcReadEntry*>(data);
    for (uint32_t i = 0; i < hdr->requestCount; ++i) {
        uint8_t* dest = data + entries[i].dataOffset;
        safe_read(entries[i].address, dest, entries[i].length, &hdr->status);
    }
    hdr->responseCount = hdr->requestCount;
}

static void handle_write(RcxRpcHeader* hdr, uint8_t* data)
{
    safe_write(hdr->writeAddress, data, hdr->writeLength, &hdr->status);
}

static void handle_enum_modules(RcxRpcHeader* hdr, uint8_t* data)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        hdr->status = RCX_RPC_STATUS_ERROR;
        hdr->responseCount = 0;
        return;
    }

    /* first pass: collect unique file-backed mappings */
    struct ModRange { uint64_t base; uint64_t end; char path[512]; };
    static ModRange modules[512];  /* static to avoid large stack alloc */
    int modCount = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f) && modCount < 512) {
        uint64_t start, end;
        char perms[8] = {}, path[512] = {};
        if (sscanf(line, "%lx-%lx %7s %*x %*x:%*x %*u %511[^\n]",
                   &start, &end, perms, path) < 4)
            continue;

        /* skip non-file / special mappings */
        /* trim leading whitespace from path */
        char* p = path;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '/') continue;
        if (strncmp(p, "/dev/", 5) == 0) continue;
        if (strncmp(p, "/memfd:", 7) == 0) continue;

        bool found = false;
        for (int i = 0; i < modCount; ++i) {
            if (strcmp(modules[i].path, p) == 0) {
                if (start < modules[i].base) modules[i].base = start;
                if (end   > modules[i].end)  modules[i].end  = end;
                found = true;
                break;
            }
        }
        if (!found) {
            modules[modCount].base = start;
            modules[modCount].end  = end;
            strncpy(modules[modCount].path, p, 511);
            modules[modCount].path[511] = '\0';
            ++modCount;
        }
    }
    fclose(f);

    /* write entries + name strings into data region */
    uint32_t entryBytes  = (uint32_t)(modCount * sizeof(RcxRpcModuleEntry));
    uint32_t nameDataOff = entryBytes;

    for (int i = 0; i < modCount; ++i) {
        const char* basename = strrchr(modules[i].path, '/');
        basename = basename ? basename + 1 : modules[i].path;
        uint32_t nameLen = (uint32_t)strlen(basename);

        auto* entry = reinterpret_cast<RcxRpcModuleEntry*>(
            data + (uint32_t)i * sizeof(RcxRpcModuleEntry));
        entry->base       = modules[i].base;
        entry->size       = modules[i].end - modules[i].base;
        entry->nameOffset = nameDataOff;
        entry->nameLength = nameLen;

        if (nameDataOff + nameLen <= RCX_RPC_DATA_SIZE) {
            memcpy(data + nameDataOff, basename, nameLen);
            nameDataOff += nameLen;
        }
    }

    hdr->responseCount = (uint32_t)modCount;
    hdr->totalDataUsed = nameDataOff;
    hdr->status        = RCX_RPC_STATUS_OK;
}

/* ── server thread ────────────────────────────────────────────────── */

static void* server_thread_func(void*)
{
    auto* hdr  = static_cast<RcxRpcHeader*>(g_mappedView);
    auto* data = reinterpret_cast<uint8_t*>(g_mappedView) + RCX_RPC_DATA_OFFSET;

    __atomic_store_n(&hdr->payloadReady, 1, __ATOMIC_RELEASE);

    while (!__atomic_load_n(&g_shutdown, __ATOMIC_ACQUIRE)) {
        /* timed wait: 250ms */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec  += 1;
            ts.tv_nsec -= 1000000000;
        }

        int rc = sem_timedwait(g_reqSem, &ts);
        if (rc != 0) {
            if (errno == ETIMEDOUT) continue;
            break;
        }

        hdr->status = RCX_RPC_STATUS_OK;

        switch (static_cast<RcxRpcCommand>(hdr->command)) {
        case RPC_CMD_READ_BATCH:   handle_read_batch(hdr, data); break;
        case RPC_CMD_WRITE:        handle_write(hdr, data);      break;
        case RPC_CMD_ENUM_MODULES: handle_enum_modules(hdr, data); break;
        case RPC_CMD_PING:         break;
        case RPC_CMD_SHUTDOWN:
            __atomic_store_n(&g_shutdown, 1, __ATOMIC_RELEASE);
            break;
        default:
            hdr->status = RCX_RPC_STATUS_ERROR;
            break;
        }

        sem_post(g_rspSem);

        if (static_cast<RcxRpcCommand>(hdr->command) == RPC_CMD_SHUTDOWN)
            break;
    }

    __atomic_store_n(&hdr->payloadReady, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_threadRunning, 0, __ATOMIC_RELEASE);
    return nullptr;
}

/* ── init / cleanup ───────────────────────────────────────────────── */

static void payload_cleanup()
{
    __atomic_store_n(&g_shutdown, 1, __ATOMIC_RELEASE);

    /* wake the thread if blocked */
    if (g_reqSem != SEM_FAILED) sem_post(g_reqSem);

    if (__atomic_load_n(&g_threadRunning, __ATOMIC_ACQUIRE)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 2;
        pthread_timedjoin_np(g_thread, nullptr, &ts);
    }

    if (g_mappedView && g_mappedView != MAP_FAILED) {
        munmap(g_mappedView, RCX_RPC_SHM_SIZE);
        g_mappedView = nullptr;
    }
    if (g_shmFd >= 0) { close(g_shmFd); g_shmFd = -1; }
    if (g_reqSem != SEM_FAILED) { sem_close(g_reqSem); g_reqSem = SEM_FAILED; }
    if (g_rspSem != SEM_FAILED) { sem_close(g_rspSem); g_rspSem = SEM_FAILED; }

    /* unlink named objects */
    if (g_shmName[0]) shm_unlink(g_shmName);
    if (g_reqName[0]) sem_unlink(g_reqName);
    if (g_rspName[0]) sem_unlink(g_rspName);

    if (g_memFd >= 0) { close(g_memFd); g_memFd = -1; }
}

__attribute__((constructor))
static void payload_init()
{
    uint32_t pid = (uint32_t)getpid();

    /* ── open /proc/self/mem for safe access ── */
    g_memFd = open("/proc/self/mem", O_RDWR);
    if (g_memFd < 0) return;

    /* ── create main shared memory (PID-only naming) ── */
    rcx_rpc_shm_name(g_shmName, sizeof(g_shmName), pid);
    rcx_rpc_req_name(g_reqName, sizeof(g_reqName), pid);
    rcx_rpc_rsp_name(g_rspName, sizeof(g_rspName), pid);

    g_shmFd = shm_open(g_shmName, O_CREAT | O_RDWR, 0600);
    if (g_shmFd < 0) return;
    if (ftruncate(g_shmFd, RCX_RPC_SHM_SIZE) != 0) {
        close(g_shmFd); g_shmFd = -1; return;
    }

    g_mappedView = mmap(nullptr, RCX_RPC_SHM_SIZE, PROT_READ | PROT_WRITE,
                        MAP_SHARED, g_shmFd, 0);
    if (g_mappedView == MAP_FAILED) {
        g_mappedView = nullptr;
        close(g_shmFd); g_shmFd = -1;
        return;
    }

    memset(g_mappedView, 0, RCX_RPC_HEADER_SIZE);
    auto* hdr = static_cast<RcxRpcHeader*>(g_mappedView);
    hdr->version = RCX_RPC_VERSION;

    /* image base from /proc/self/maps: first executable mapping */
    {
        FILE* f = fopen("/proc/self/maps", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                uint64_t start;
                char perms[8] = {};
                if (sscanf(line, "%lx-%*x %7s", &start, perms) >= 2 && perms[2] == 'x') {
                    hdr->imageBase = start;
                    break;
                }
            }
            fclose(f);
        }
    }

    /* ── create semaphores ── */
    g_reqSem = sem_open(g_reqName, O_CREAT, 0600, 0);
    g_rspSem = sem_open(g_rspName, O_CREAT, 0600, 0);
    if (g_reqSem == SEM_FAILED || g_rspSem == SEM_FAILED) {
        payload_cleanup();
        return;
    }

    /* ── start server thread (it will set payloadReady = 1) ── */
    __atomic_store_n(&g_threadRunning, 1, __ATOMIC_RELEASE);
    if (pthread_create(&g_thread, nullptr, server_thread_func, nullptr) != 0) {
        __atomic_store_n(&g_threadRunning, 0, __ATOMIC_RELEASE);
        payload_cleanup();
        return;
    }
    pthread_detach(g_thread);
}

__attribute__((destructor))
static void payload_deinit()
{
    payload_cleanup();
}

#endif /* _WIN32 / linux */
