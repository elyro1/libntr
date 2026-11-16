// log.txt, crash reports and the stall watchdog.
//
// Crash reports run inside a signal handler (Linux) or an unhandled-exception filter (Windows), where the heap or
// stdio may be the thing that broke, so everything they write goes through SIM_Crash_Printf: a small formatter
// that writes straight to the file descriptors with no allocation or locks.

#include "simulator/sim_crash.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>

extern "C" const char *SIM_GetLibntrGitHash();
extern "C" const char *SIM_GetProjectGitHash();

#if defined(_WIN32)
#define SIM_CRASH_WINDOWS 1
#include <io.h>
#include <windows.h>
#include <csignal>
#elif defined(__linux__) && !defined(__SWITCH__)
#define SIM_CRASH_LINUX 1
#include <csignal>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <link.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#define SIM_CRASH_BACKTRACE 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define SIM_CRASH_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SIM_CRASH_ASAN 1
#endif
#endif

#if defined(SIM_CRASH_WINDOWS)
#define sim_write _write
#define sim_read _read
#define sim_dup _dup
#define sim_dup2 _dup2
#define sim_close _close
#define sim_fileno _fileno
#else
#define sim_write write
#define sim_read read
#define sim_dup dup
#define sim_dup2 dup2
#define sim_close close
#define sim_fileno fileno
#endif

namespace {

constexpr const char *kLogPath = "log.txt";
constexpr const char *kPrevLogPath = "log.prev.txt";
// A runaway print loop must not fill the disk
constexpr long long kLogLimit = 64ll << 20;

int sLogFd = -1;
FILE *sLogFile;
// Where stdout and stderr went before the tee, -1 when nowhere
int sTermOut = -1, sTermErr = -1;
struct Tee {
    int readFd = -1;
    int termFd = -1;
    std::thread thread;
    std::atomic<bool> done{false};
};
Tee sTees[2];
std::atomic<long long> sLogBytes{0};
std::atomic<bool> sLogFull{false};

SIM_LogConfigPrinter sConfigPrinter;
struct WatchedFile {
    const char *path;
    long long modified;
};
WatchedFile sWatched[8];
int sWatchedCount;
bool sConfigDirty;
unsigned sConfigCheckFrame, sConfigChangedFrame;

// Identifies the exact binary, for symbolize_crash.py: the ELF build ID, or the PE link timestamp.
char sBinaryId[48] = "unknown";

std::atomic<unsigned> sHeartbeat{0};
std::atomic<const char *> sPhase{"starting"};
SIM_CrashContextWriter sContextWriter;
std::atomic<int> sInReport{0};

void WriteAll(int fd, const char *data, size_t length) {
    while (fd >= 0 && length > 0) {
        const int written = (int)sim_write(fd, data, (unsigned)length);
        if (written <= 0) return;
        data += written;
        length -= (size_t)written;
    }
}

void WriteLog(const char *data, size_t length) {
    if (sLogFd < 0) return;
    if (sLogFull.load(std::memory_order_relaxed)) return;
    if (sLogBytes.fetch_add((long long)length) + (long long)length > kLogLimit) {
        sLogFull = true;
        static const char kNote[] = "\n[log] log.txt is full; later output goes to the terminal only\n";
        WriteAll(sLogFd, kNote, sizeof(kNote) - 1);
        return;
    }
    WriteAll(sLogFd, data, length);
}

// Reports bypass the size limit: they are what the log is for.
void WriteReport(const char *data, size_t length) {
    WriteAll(sLogFd, data, length);
    WriteAll(sTermErr, data, length);
}

void TeeLoop(Tee *tee) {
    char buffer[4096];
    for (;;) {
        const int count = (int)sim_read(tee->readFd, buffer, sizeof(buffer));
        if (count <= 0) break;
        WriteAll(tee->termFd, buffer, (size_t)count);
        WriteLog(buffer, (size_t)count);
    }
    tee->done = true;
}

bool StreamUsable(FILE *stream) {
    const int fd = sim_fileno(stream);
    if (fd < 0) return false;
#if defined(SIM_CRASH_WINDOWS)
    return _get_osfhandle(fd) != -1 && _get_osfhandle(fd) != -2;
#else
    return fcntl(fd, F_GETFD) != -1;
#endif
}

// Points the stream's descriptor at a pipe whose other end a thread copies to the terminal and the log.
void StartTee(Tee &tee, FILE *stream, int &termFd) {
    fflush(stream);
#if defined(SIM_CRASH_WINDOWS)
    // A GUI program started from Explorer has no stdout; give it one so the pipe has a descriptor to replace.
    if (!StreamUsable(stream)) {
        freopen("NUL", "w", stream);
        termFd = -1;
    } else {
        termFd = sim_dup(sim_fileno(stream));
    }
#else
    termFd = StreamUsable(stream) ? sim_dup(sim_fileno(stream)) : -1;
#endif
    int fds[2];
#if defined(SIM_CRASH_WINDOWS)
    if (_pipe(fds, 1 << 16, _O_BINARY) != 0) return;
#else
    if (pipe(fds) != 0) return;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
#endif
    sim_dup2(fds[1], sim_fileno(stream));
    sim_close(fds[1]);
    tee.readFd = fds[0];
    tee.termFd = termFd;
    tee.thread = std::thread(TeeLoop, &tee);
}

void StopTees() {
    if (sLogFd < 0) return;
    if (sLogFile) fflush(sLogFile);
    fflush(stdout);
    fflush(stderr);
    // Closing the pipes' write ends (by restoring the terminal) ends the copy threads.
    FILE *streams[2] = {stdout, stderr};
    for (int i = 0; i < 2; i++) {
        if (!sTees[i].thread.joinable()) continue;
        if (sTees[i].termFd >= 0) {
            sim_dup2(sTees[i].termFd, sim_fileno(streams[i]));
        } else {
#if defined(SIM_CRASH_WINDOWS)
            freopen("NUL", "w", streams[i]);
#else
            const int null = open("/dev/null", O_WRONLY);
            if (null >= 0) {
                sim_dup2(null, sim_fileno(streams[i]));
                sim_close(null);
            }
#endif
        }
        // A child process (an opened URL, say) may still hold the write end; don't let it block the exit.
        for (int wait = 0; wait < 100 && !sTees[i].done; wait++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (sTees[i].done) {
            sTees[i].thread.join();
        } else {
            sTees[i].thread.detach();
        }
    }
}

// Copies what the tees haven't read yet, so the lines just before a crash are in the log.
void DrainTees() {
#if defined(SIM_CRASH_LINUX)
    for (Tee &tee : sTees) {
        if (tee.readFd < 0) continue;
        const int flags = fcntl(tee.readFd, F_GETFL);
        fcntl(tee.readFd, F_SETFL, flags | O_NONBLOCK);
        char buffer[4096];
        int count;
        while ((count = (int)read(tee.readFd, buffer, sizeof(buffer))) > 0) {
            WriteAll(tee.termFd, buffer, (size_t)count);
            WriteAll(sLogFd, buffer, (size_t)count);
        }
    }
#elif defined(SIM_CRASH_WINDOWS)
    // The copy threads keep running while the filter does; give them a moment.
    Sleep(50);
#endif
}

long long ModifiedTime(const char *path) {
#if defined(SIM_CRASH_WINDOWS)
    struct _stat64 info;
    return _stat64(path, &info) == 0 ? (long long)info.st_mtime : -1;
#else
    struct stat info;
    return stat(path, &info) == 0 ? (long long)info.st_mtim.tv_sec * 1000000000ll + info.st_mtim.tv_nsec : -1;
#endif
}

void PrintConfig(const char *reason);

// Settings menus save on every change, sliders on every step; log once the files have been quiet for 2 s.
void CheckConfigFiles(unsigned frame) {
    if (!sConfigPrinter || frame - sConfigCheckFrame < 30) return;
    sConfigCheckFrame = frame;
    for (int i = 0; i < sWatchedCount; i++) {
        const long long modified = ModifiedTime(sWatched[i].path);
        if (modified != sWatched[i].modified) {
            sWatched[i].modified = modified;
            sConfigDirty = true;
            sConfigChangedFrame = frame;
        }
    }
    if (sConfigDirty && frame - sConfigChangedFrame >= 120) {
        sConfigDirty = false;
        PrintConfig("after a settings change");
    }
}

void PrintConfig(const char *reason) {
    if (!sLogFile || !sConfigPrinter) return;
    fprintf(sLogFile, "\n[log] effective settings (%s):\n", reason);
    sConfigPrinter(sLogFile);
    fprintf(sLogFile, "[log] end of settings\n\n");
    fflush(sLogFile);
}

// --- Signal-safe formatting ---

struct Out {
    char buffer[512];
    size_t length = 0;
    void Flush() {
        WriteReport(buffer, length);
        length = 0;
    }
    void Put(char c) {
        if (length == sizeof(buffer)) Flush();
        buffer[length++] = c;
    }
    void Puts(const char *s) {
        if (!s) s = "(null)";
        while (*s) Put(*s++);
    }
    void Number(unsigned long long value, unsigned base, int width) {
        char digits[24];
        int count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value % base];
            value /= base;
        } while (value);
        while (count < width && count < (int)sizeof(digits)) digits[count++] = '0';
        while (count) Put(digits[--count]);
    }
};

void Format(Out &out, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out.Put(*fmt);
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int size = 0;  // 1: long, 2: long long, 3: size_t
        while (*fmt == 'l' || *fmt == 'z') size = *fmt++ == 'z' ? 3 : size + 1;
        auto unsignedArg = [&]() -> unsigned long long {
            switch (size) {
            case 0: return va_arg(args, unsigned);
            case 1: return va_arg(args, unsigned long);
            case 3: return va_arg(args, size_t);
            default: return va_arg(args, unsigned long long);
            }
        };
        switch (*fmt) {
        case 's': out.Puts(va_arg(args, const char *)); break;
        case 'c': out.Put((char)va_arg(args, int)); break;
        case 'd': {
            long long value = size == 0 ? va_arg(args, int) : size == 1 ? va_arg(args, long)
                : size == 3 ? (long long)va_arg(args, size_t) : va_arg(args, long long);
            if (value < 0) {
                out.Put('-');
                out.Number(0ull - (unsigned long long)value, 10, width);
            } else {
                out.Number((unsigned long long)value, 10, width);
            }
            break;
        }
        case 'u': out.Number(unsignedArg(), 10, width); break;
        case 'x': out.Number(unsignedArg(), 16, width); break;
        case 'p':
            out.Puts("0x");
            out.Number((uintptr_t)va_arg(args, void *), 16, 0);
            break;
        case '%': out.Put('%'); break;
        case '\0': return;
        default: out.Put('%'); out.Put(*fmt); break;
        }
    }
}

void ReportPrintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Out out;
    Format(out, fmt, args);
    out.Flush();
    va_end(args);
}

// --- Addresses and stacks ---

#if defined(SIM_CRASH_WINDOWS)
uintptr_t sExeBase, sExeEnd;

void PrintModuleAddress(const char *prefix, uintptr_t address, bool resolveOthers) {
    if (address >= sExeBase && address < sExeEnd) {
        ReportPrintf("%smain.exe+0x%lx [0x%llx]\n", prefix, (unsigned long)(address - sExeBase), (unsigned long long)address);
        return;
    }
    HMODULE module = nullptr;
    char path[MAX_PATH];
    // GetModuleHandleEx would take a null address for the executable
    if (resolveOthers && address >= 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)address, &module) && GetModuleFileNameA(module, path, sizeof(path))) {
        const char *name = path;
        for (const char *c = path; *c; c++) if (*c == '\\' || *c == '/') name = c + 1;
        ReportPrintf("%s%s+0x%lx [0x%llx]\n", prefix, name, (unsigned long)(address - (uintptr_t)module), (unsigned long long)address);
        return;
    }
    ReportPrintf("%s? [0x%llx]\n", prefix, (unsigned long long)address);
}

// x64 unwinding from a context, as the debugger does it; no symbols needed.
int WalkStack(CONTEXT context, uintptr_t *pcs, int max) {
    int count = 0;
#if defined(_M_X64) || defined(__x86_64__)
    // The first frame may be pc 0, a call through a null pointer; it unwinds like a leaf function.
    while (count < max && (context.Rip || count == 0)) {
        pcs[count++] = (uintptr_t)context.Rip;
        DWORD64 imageBase;
        PRUNTIME_FUNCTION function = context.Rip ? RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr) : nullptr;
        if (function) {
            PVOID handlerData;
            DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher,
                nullptr);
        } else {
            // A leaf function, or a call through a bad pointer: the return address is on top of the stack.
            if (IsBadReadPtr((void *)context.Rsp, sizeof(DWORD64))) break;
            context.Rip = *(DWORD64 *)context.Rsp;
            context.Rsp += 8;
        }
    }
#else
    (void)context;
    (void)max;
#endif
    return count;
}

void PrintStack(const uintptr_t *pcs, int count) {
    for (int i = 0; i < count; i++) {
        const char prefix[] = {' ', ' ', '#', (char)('0' + i / 10), (char)('0' + i % 10), ' ', '\0'};
        PrintModuleAddress(prefix, pcs[i], true);
    }
}
#endif

#if defined(SIM_CRASH_BACKTRACE)
void PrintBacktrace(void *const *frames, int count) {
    backtrace_symbols_fd(frames, count, sLogFd);
    if (sTermErr >= 0) backtrace_symbols_fd(frames, count, sTermErr);
}
#endif

#if defined(SIM_CRASH_LINUX)
int FindBuildId(struct dl_phdr_info *info, size_t, void *) {
    // The first object is the executable itself.
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &header = info->dlpi_phdr[i];
        if (header.p_type != PT_NOTE) continue;
        const char *note = (const char *)(info->dlpi_addr + header.p_vaddr);
        const char *end = note + header.p_memsz;
        while (note + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *nhdr = (const ElfW(Nhdr) *)note;
            const char *desc = note + sizeof(ElfW(Nhdr)) + ((nhdr->n_namesz + 3) & ~3u);
            if (nhdr->n_type == NT_GNU_BUILD_ID && nhdr->n_namesz == 4 && memcmp(note + sizeof(ElfW(Nhdr)), "GNU", 4) == 0) {
                size_t length = 0;
                for (unsigned j = 0; j < nhdr->n_descsz && length + 2 < sizeof(sBinaryId); j++) {
                    sBinaryId[length++] = "0123456789abcdef"[(unsigned char)desc[j] >> 4];
                    sBinaryId[length++] = "0123456789abcdef"[(unsigned char)desc[j] & 15];
                }
                sBinaryId[length] = '\0';
                return 1;
            }
            note = desc + ((nhdr->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}
#endif

void FindBinaryId() {
#if defined(SIM_CRASH_LINUX)
    dl_iterate_phdr(FindBuildId, nullptr);
#elif defined(SIM_CRASH_WINDOWS)
    const char *exe = (const char *)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    snprintf(sBinaryId, sizeof(sBinaryId), "pe-%08lx", (unsigned long)nt->FileHeader.TimeDateStamp);
#endif
}

void PrintCommonContext() {
    ReportPrintf("frame %u, game thread phase: %s\n", sHeartbeat.load(), sPhase.load());
    ReportPrintf("build: %s, libntr %s, binary %s\n", SIM_GetProjectGitHash(), SIM_GetLibntrGitHash(), sBinaryId);
#if defined(SIM_CRASH_WINDOWS)
    ReportPrintf("main.exe loaded at 0x%llx\n", (unsigned long long)sExeBase);
#endif
    if (sContextWriter) sContextWriter();
    if (sLogFd >= 0 && sConfigPrinter) ReportPrintf("settings: the last \"effective settings\" block above\n");
}

#if defined(SIM_CRASH_LINUX)
const char *SignalName(int signal) {
    switch (signal) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT (abort)";
    default: return "signal";
    }
}

const int kCrashSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
pid_t sGameTid;
alignas(16) char sAltStack[64 * 1024];

void CrashHandler(int signal, siginfo_t *info, void *contextPtr) {
    if (sInReport.exchange(1)) {
        // A second thread crashing, or the report itself crashing: leave the first report alone.
        if ((pid_t)syscall(SYS_gettid) != sGameTid) pause();
        ::signal(signal, SIG_DFL);
        raise(signal);
        return;
    }
    DrainTees();
    char thread[17] = {0};
    prctl(PR_GET_NAME, thread, 0, 0, 0);
    ReportPrintf("\n=== CRASH: %s", SignalName(signal));
    if (signal == SIGSEGV || signal == SIGBUS) ReportPrintf(" at address %p", info->si_addr);
    ReportPrintf(" in thread \"%s\"%s ===\n", thread, (pid_t)syscall(SYS_gettid) == sGameTid ? " (game thread)" : "");
    const ucontext_t *context = (const ucontext_t *)contextPtr;
    void *pc = nullptr;
#if defined(__x86_64__)
    pc = (void *)context->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (void *)context->uc_mcontext.pc;
#else
    (void)context;
#endif
#if defined(SIM_CRASH_BACKTRACE)
    if (pc) {
        ReportPrintf("faulting instruction:\n");
        PrintBacktrace(&pc, 1);
    }
#if defined(__x86_64__)
    // A call through a null pointer: the unwinder can't go on from pc 0, but the caller is on top of the stack.
    if ((uintptr_t)pc < 4096) {
        void *caller = *(void **)context->uc_mcontext.gregs[REG_RSP];
        ReportPrintf("called through a bad pointer from:\n");
        PrintBacktrace(&caller, 1);
    }
#endif
    void *frames[64];
    const int count = backtrace(frames, 64);
    ReportPrintf("stack (the first lines are the crash handler):\n");
    PrintBacktrace(frames, count);
#else
    ReportPrintf("pc %p (no stack trace on this platform)\n", pc);
#endif
    PrintCommonContext();
    ReportPrintf("=== end of crash report; please send log.txt ===\n");
    if (sLogFd >= 0) fsync(sLogFd);
    // Re-raised with the default action once the handler returns, so exit status and core dumps stay as before.
    ::signal(signal, SIG_DFL);
    if (signal == SIGABRT || info->si_code <= 0) raise(signal);
}

// The watchdog asks the game thread for its own stack with this signal.
void *sStallFrames[64];
std::atomic<int> sStallFrameCount{-1};

void StallTraceHandler(int, siginfo_t *, void *) {
#if defined(SIM_CRASH_BACKTRACE)
    sStallFrameCount = backtrace(sStallFrames, 64);
#else
    sStallFrameCount = 0;
#endif
}

bool DebuggerAttached() {
    const int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return false;
    char buffer[2048];
    const int count = (int)read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count <= 0) return false;
    buffer[count] = '\0';
    const char *tracer = strstr(buffer, "TracerPid:");
    return tracer && atoi(tracer + 10) != 0;
}

void PrintGameThreadStack() {
    sStallFrameCount = -1;
    if (syscall(SYS_tgkill, getpid(), sGameTid, SIGUSR2) != 0) return;
    for (int i = 0; i < 100 && sStallFrameCount < 0; i++) usleep(2000);
    if (sStallFrameCount < 0) {
        ReportPrintf("the game thread did not answer\n");
        return;
    }
#if defined(SIM_CRASH_BACKTRACE)
    ReportPrintf("game thread stack (the first lines are the stack sampler):\n");
    PrintBacktrace(sStallFrames, sStallFrameCount);
#endif
}
#endif // SIM_CRASH_LINUX

#if defined(SIM_CRASH_WINDOWS)
HANDLE sGameThread;
DWORD sGameThreadId;

const char *ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    default: return "exception";
    }
}

void PrintContextStack(const CONTEXT &context) {
    uintptr_t pcs[64];
    const int count = WalkStack(context, pcs, 64);
    ReportPrintf("stack:\n");
    PrintStack(pcs, count);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS *pointers) {
    if (sInReport.exchange(1)) return EXCEPTION_CONTINUE_SEARCH;
    DrainTees();
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    ReportPrintf("\n=== CRASH: exception 0x%08x (%s)", (unsigned)record->ExceptionCode, ExceptionName(record->ExceptionCode));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const ULONG_PTR kind = record->ExceptionInformation[0];
        ReportPrintf(", %s 0x%llx", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
            (unsigned long long)record->ExceptionInformation[1]);
    }
    ReportPrintf(" in thread %u%s ===\n", (unsigned)GetCurrentThreadId(),
        GetCurrentThreadId() == sGameThreadId ? " (game thread)" : "");
    // A stack overflow leaves too little stack to walk safely.
    if (record->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        PrintContextStack(*pointers->ContextRecord);
    } else {
        PrintModuleAddress("faulting instruction: ", (uintptr_t)record->ExceptionAddress, true);
    }
    PrintCommonContext();
    ReportPrintf("=== end of crash report; please send log.txt ===\n");
    if (sLogFd >= 0) _commit(sLogFd);
    return EXCEPTION_EXECUTE_HANDLER;
}

void AbortHandler(int) {
    if (sInReport.exchange(1)) _exit(3);
    DrainTees();
    ReportPrintf("\n=== CRASH: abort() in thread %u%s ===\n", (unsigned)GetCurrentThreadId(),
        GetCurrentThreadId() == sGameThreadId ? " (game thread)" : "");
    CONTEXT context;
    RtlCaptureContext(&context);
    PrintContextStack(context);
    PrintCommonContext();
    ReportPrintf("=== end of crash report; please send log.txt ===\n");
    if (sLogFd >= 0) _commit(sLogFd);
    _exit(3);
}

void PrintGameThreadStack() {
    if (!sGameThread) return;
    uintptr_t pcs[64];
    int count = 0;
    // Walk while the thread is stopped, but look up module names only after it runs again: that takes the loader
    // lock, which the stopped thread might hold.
    if (SuspendThread(sGameThread) != (DWORD)-1) {
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(sGameThread, &context)) count = WalkStack(context, pcs, 64);
        ResumeThread(sGameThread);
    }
    ReportPrintf("game thread stack:\n");
    PrintStack(pcs, count);
}

bool DebuggerAttached() {
    return IsDebuggerPresent();
}
#endif // SIM_CRASH_WINDOWS

#if defined(SIM_CRASH_LINUX) || defined(SIM_CRASH_WINDOWS)
void Watchdog(int stallSeconds) {
    unsigned last = 0;
    int stalled = 0;
    bool reported = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned now = sHeartbeat.load();
        // Not armed until the first frame: first-launch ROM extraction can take a while.
        if (now == 0) continue;
        if (now != last) {
            if (reported) ReportPrintf("=== stall over: frames resumed after about %d s (frame %u) ===\n", stalled, now);
            last = now;
            stalled = 0;
            reported = false;
            continue;
        }
        if (++stalled != stallSeconds || DebuggerAttached() || sInReport.load()) continue;
        reported = true;
        ReportPrintf("\n=== STALL: no frame for %d s after frame %u, game thread phase: %s ===\n", stallSeconds, now,
            sPhase.load());
        PrintGameThreadStack();
        PrintCommonContext();
        ReportPrintf("=== end of stall report ===\n");
    }
}
#endif

} // namespace

extern "C" {

void SIM_Log_Open(void) {
    if (sLogFd >= 0) return;
    FindBinaryId();
#if defined(SIM_CRASH_WINDOWS)
    remove(kPrevLogPath);
    rename(kLogPath, kPrevLogPath);
    sLogFd = _open(kLogPath, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#elif defined(SIM_CRASH_LINUX)
    rename(kLogPath, kPrevLogPath);
    sLogFd = open(kLogPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
#endif
    if (sLogFd < 0) return;
    sLogFile = fdopen(sim_dup(sLogFd), "a");
    StartTee(sTees[0], stdout, sTermOut);
    StartTee(sTees[1], stderr, sTermErr);
#if defined(SIM_CRASH_WINDOWS)
    // Windows treats _IOLBF as full buffering; the pipe makes unbuffered writes cheap anyway.
    setvbuf(stdout, nullptr, _IONBF, 0);
#else
    setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    setvbuf(stderr, nullptr, _IONBF, 0);
    atexit(StopTees);

    if (sLogFile) {
        char date[64];
        const time_t now = time(nullptr);
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(sLogFile, "[log] %s, build %s, libntr %s, binary %s\n", date, SIM_GetProjectGitHash(),
            SIM_GetLibntrGitHash(), sBinaryId);
#if defined(SIM_CRASH_LINUX)
        struct utsname name;
        if (uname(&name) == 0) fprintf(sLogFile, "[log] %s %s %s\n", name.sysname, name.release, name.machine);
#elif defined(SIM_CRASH_WINDOWS)
        OSVERSIONINFOA version = {sizeof(version)};
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOA *);
        auto getVersion = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        if (getVersion && getVersion(&version) == 0)
            fprintf(sLogFile, "[log] Windows %lu.%lu build %lu\n", version.dwMajorVersion, version.dwMinorVersion,
                version.dwBuildNumber);
#endif
        fflush(sLogFile);
    }
}

FILE *SIM_Log_File(void) {
    return sLogFile;
}

void SIM_Log_SetConfigPrinter(SIM_LogConfigPrinter printer) {
    sConfigPrinter = printer;
    SIM_Log_WatchConfigFile("sim_config.ini");
    PrintConfig("at startup");
}

void SIM_Log_WatchConfigFile(const char *path) {
    for (int i = 0; i < sWatchedCount; i++) if (strcmp(sWatched[i].path, path) == 0) return;
    if (sWatchedCount == (int)(sizeof(sWatched) / sizeof(sWatched[0]))) return;
    sWatched[sWatchedCount++] = {path, ModifiedTime(path)};
}

void SIM_Crash_Install(int handlers, int stallSeconds) {
    FindBinaryId();
    // Without log.txt, reports still reach stderr.
    if (sLogFd < 0) sTermErr = sim_fileno(stderr);
#if defined(SIM_CRASH_LINUX)
    sGameTid = (pid_t)syscall(SYS_gettid);
#if defined(SIM_CRASH_BACKTRACE)
    // Loads libgcc's unwinder now; loading it inside a crash handler can fail.
    void *warmup[1];
    backtrace(warmup, 1);
#endif
#if !defined(SIM_CRASH_ASAN)
    // ASan reports these itself, better than we can.
    if (handlers) {
        stack_t altStack = {};
        altStack.ss_sp = sAltStack;
        altStack.ss_size = sizeof(sAltStack);
        sigaltstack(&altStack, nullptr);
        struct sigaction action = {};
        action.sa_sigaction = CrashHandler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int signal : kCrashSignals) sigaction(signal, &action, nullptr);
    }
#endif
    if (stallSeconds > 0) {
        struct sigaction trace = {};
        trace.sa_sigaction = StallTraceHandler;
        trace.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&trace.sa_mask);
        sigaction(SIGUSR2, &trace, nullptr);
        std::thread(Watchdog, stallSeconds).detach();
    }
#elif defined(SIM_CRASH_WINDOWS)
    const HMODULE exe = GetModuleHandleA(nullptr);
    sExeBase = (uintptr_t)exe;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)exe;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)exe + dos->e_lfanew);
    sExeEnd = sExeBase + nt->OptionalHeader.SizeOfImage;
    sGameThreadId = GetCurrentThreadId();
    sGameThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
        sGameThreadId);
#if !defined(SIM_CRASH_ASAN)
    if (handlers) {
        SetUnhandledExceptionFilter(UnhandledFilter);
        signal(SIGABRT, AbortHandler);
        // No Windows Error Reporting popup on top of our report
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    }
#endif
    if (stallSeconds > 0) std::thread(Watchdog, stallSeconds).detach();
#else
    (void)handlers;
    (void)stallSeconds;
#endif
}

void SIM_Crash_Heartbeat(void) {
    const unsigned frame = sHeartbeat.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sLogFile) CheckConfigFiles(frame);
}

void SIM_Crash_SetPhase(const char *phase) {
    sPhase.store(phase, std::memory_order_relaxed);
}

void SIM_Crash_SetContextWriter(SIM_CrashContextWriter writer) {
    sContextWriter = writer;
}

void SIM_Crash_Printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Out out;
    Format(out, fmt, args);
    out.Flush();
    va_end(args);
}

void SIM_Crash_PrintAddress(const char *label, const void *address) {
#if defined(SIM_CRASH_BACKTRACE)
    ReportPrintf("%s", label);
    void *frame = (void *)address;
    PrintBacktrace(&frame, 1);
#elif defined(SIM_CRASH_WINDOWS)
    // No module lookups here: this runs while the game thread may be stopped.
    PrintModuleAddress(label, (uintptr_t)address, false);
#else
    ReportPrintf("%s%p\n", label, address);
#endif
}

} // extern "C"
