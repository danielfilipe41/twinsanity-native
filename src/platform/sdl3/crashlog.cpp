// The host's crash log: what the machine was doing when the game died, written to crash.log beside the run and to stderr, so
// a crash outside gdb still says where it happened. The handler is async-signal-safe (open/write/backtrace_symbols_fd only),
// and the host links with -rdynamic so those frames come out with names. SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT are all
// caught: the last three are what the C++ runtime and asserts raise.
#include "common.h"

#include <cstring>
#include <ctime>
#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
// The same crash log on Windows: unhandled exceptions (access violations, divide by zero, the CRT's fastfail on an assert)
// arrive at SetUnhandledExceptionFilter, which writes the fault, the module base and the stack's return addresses to
// crash.log beside the game and to stderr. The addresses are module offsets: resolve one with
//   addr2line -f -C -e game-with-data.exe <offset>
namespace
{
struct Buffer
{
    char text[8192];
    u32 length = 0;

    void Append(const char* string)
    {
        for (; *string != '\0' && length + 1 < sizeof(text);)
        {
            text[length++] = *string++;
        }
    }

    void AppendHex(u64 value)
    {
        static const char* digits = "0123456789abcdef";
        char reversed[24];
        u32 count = 0;
        do
        {
            reversed[count++] = digits[value & 0xF];
            value >>= 4;
        } while (value != 0);
        while (count != 0 && length + 1 < sizeof(text))
        {
            text[length++] = reversed[--count];
        }
    }
};

void WriteAll(HANDLE handle, const char* text, size_t size)
{
    DWORD written = 0;
    WriteFile(handle, text, static_cast<DWORD>(size), &written, nullptr);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    const char* name = "EXCEPTION";
    switch (info->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        name = "EXCEPTION_ACCESS_VIOLATION (bad memory access)";
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        name = "EXCEPTION_ILLEGAL_INSTRUCTION";
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        name = "EXCEPTION_INT_DIVIDE_BY_ZERO";
        break;
    case EXCEPTION_STACK_OVERFLOW:
        name = "EXCEPTION_STACK_OVERFLOW";
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        name = "EXCEPTION_PRIV_INSTRUCTION";
        break;
    case 0xC0000409:
        name = "FAST_FAIL (abort/assert)";
        break;
    default:
        break;
    }

    const auto base = static_cast<u64>(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
    Buffer buffer;
    buffer.Append("\r\n=== Crash Twinsanity host crash ===\r\n");
    buffer.Append(name);
    buffer.Append("\r\nmodule base 0x");
    buffer.AppendHex(base);
    buffer.Append("\r\nfault address 0x");
    buffer.AppendHex(static_cast<u64>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)));
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR* parameters = info->ExceptionRecord->ExceptionInformation;
        buffer.Append(parameters[0] == 0   ? "\r\naccess: read 0x"
                      : parameters[0] == 1 ? "\r\naccess: write 0x"
                                           : "\r\naccess: execute 0x");
        buffer.AppendHex(static_cast<u64>(parameters[1]));
    }

    buffer.Append("\r\nbacktrace (innermost first, module offsets):\r\n");
    void* frames[64];
    const USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    for (u32 index = 0; index < count; index++)
    {
        buffer.Append("0x");
        buffer.AppendHex(static_cast<u64>(reinterpret_cast<uintptr_t>(frames[index])) - base);
        buffer.Append("\r\n");
    }

    HANDLE file = CreateFileA("crash.log", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteAll(file, buffer.text, buffer.length);
        CloseHandle(file);
    }
    WriteAll(GetStdHandle(STD_ERROR_HANDLE), buffer.text, buffer.length);
    return EXCEPTION_EXECUTE_HANDLER;
}
}

void InstallCrashLog()
{
    SetUnhandledExceptionFilter(CrashHandler);
}

#else

namespace
{
constexpr u32 CrashFrames = 64;
// The line being built, written out whole (a signal handler has no stdio) 
struct Line
{
    char text[512];
    size_t length = 0;

    void Clear()
    {
        length = 0;
    }

    void Append(const char* string)
    {
        while (*string != '\0' && length + 1 < sizeof(text))
        {
            text[length++] = *string++;
        }
    }

    void Append(const char* string, size_t size)
    {
        for (size_t index = 0; index < size && length + 1 < sizeof(text); index++)
        {
            text[length++] = string[index];
        }
    }

    void Number(u64 value, u32 base = 10, u32 width = 0)
    {
        const char* digits = "0123456789abcdef";
        char reversed[24];
        u32 count = 0;
        do
        {
            reversed[count++] = digits[value % base];
            value /= base;
        } while (value != 0);

        for (u32 pad = count < width ? width - count : 0; pad != 0; pad--)
        {
            if (length + 1 < sizeof(text))
            {
                text[length++] = '0';
            }
        }

        while (count != 0 && length + 1 < sizeof(text))
        {
            text[length++] = reversed[--count];
        }
    }

    void Send(int fd)
    {
        text[length++] = '\n';
        ssize_t written = write(fd, text, length);
        (void)written;
    }
};

// The file the log goes to: crash.log beside the game (the working directory), plus stderr
int g_CrashFd = -1;

void WriteText(int fd, const char* text)
{
    ssize_t written = write(fd, text, std::strlen(text));
    (void)written;
}

void WriteLine(int fd, Line& line)
{
    line.Send(fd);
    line.Send(2);
}

void HexLine(int fd, const char* name, u64 value)
{
    Line line;
    line.Append(name);
    line.Append(" 0x");
    line.Number(value & 0xFFFFFFFFu, 16, 8);
    WriteLine(fd, line);
}

// The faulting frame and the frames under it, walked through the frame pointers the host build keeps (a signal handler can't
// unwind through the kernel's signal frame, so backtrace() stops at its own trampoline). Returns how many were found.
u32 WalkStack(void* context, void** frames, u32 most)
{
#if defined(__i386__)
    const auto* machine = static_cast<const ucontext_t*>(context);
    u32 pc = static_cast<u32>(machine->uc_mcontext.gregs[REG_EIP]);
    u32 bp = static_cast<u32>(machine->uc_mcontext.gregs[REG_EBP]);
#else
    (void)context;
    u32 pc = 0;
    u32 bp = 0;
#endif
    u32 count = 0;
    while (count < most && pc != 0)
    {
        frames[count++] = reinterpret_cast<void*>(static_cast<uintptr_t>(pc));
        if (bp == 0 || (bp & 3) != 0)
        {
            break;
        }

        // The frame's saved frame pointer and return address (i386: [ebp] and [ebp+4])
        u32* frame = reinterpret_cast<u32*>(static_cast<uintptr_t>(bp));
        u32 next = frame[0];
        u32 returned = frame[1];
        if (next <= bp || returned == 0)
        {
            break;
        }

        bp = next;
        pc = returned;
    }

    return count;
}

// The registers the fault was taken with, as i386 keeps them
void WriteRegisters(int fd, void* context)
{
#if defined(__i386__)
    const auto* machine = static_cast<const ucontext_t*>(context);
    static const char* names[] = {"gs",  "fs",  "es", "ds", "edi", "esi", "ebp", "esp",
                                  "ebx", "edx", "ecx", "eax", "trapno", "err", "eip", "cs", "eflags", "uesp", "ss"};
    for (u32 index = 0; index < sizeof(names) / sizeof(names[0]); index++)
    {
        HexLine(fd, names[index], static_cast<u64>(machine->uc_mcontext.gregs[index]));
    }
#endif
}

void CrashHandler(int number, siginfo_t* info, void* context)
{
    if (g_CrashFd < 0)
    {
        g_CrashFd = open("crash.log", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    }

    const char* name = "signal";
    switch (number)
    {
    case SIGSEGV:
        name = "SIGSEGV (bad memory access)";
        break;
    case SIGBUS:
        name = "SIGBUS (bad address)";
        break;
    case SIGILL:
        name = "SIGILL (illegal instruction)";
        break;
    case SIGFPE:
        name = "SIGFPE (arithmetic fault)";
        break;
    case SIGABRT:
        name = "SIGABRT (abort)";
        break;
    default:
        break;
    }

    const int sinks[2] = {g_CrashFd, 2};
    for (u32 sink = 0; sink < 2; sink++)
    {
        int fd = sinks[sink];
        WriteText(fd, "\n=== Crash Twinsanity host crash ===\n");
        Line line;
        line.Append(name);
        WriteLine(fd, line);

        time_t now = time(nullptr);
        if (now != static_cast<time_t>(-1))
        {
            const tm* local = localtime(&now);
            if (local != nullptr)
            {
                char stamp[64];
                if (strftime(stamp, sizeof(stamp), "at %Y-%m-%d %H:%M:%S", local) != 0)
                {
                    line.Clear();
                    line.Append(stamp);
                    WriteLine(fd, line);
                }
            }
        }

        if (info != nullptr)
        {
            HexLine(fd, "si_addr", reinterpret_cast<u64>(info->si_addr));
            HexLine(fd, "si_code", static_cast<u64>(info->si_code));
        }

        if (context != nullptr)
        {
            WriteRegisters(fd, context);
        }

        WriteText(fd, "\nbacktrace (innermost first; the addresses can be read with\n"
                      "  addr2line -f -C -e <the built binary> <address>\n):\n");

        void* frames[CrashFrames];
        u32 count = WalkStack(context, frames, CrashFrames);
        if (count == 0)
        {
            count = static_cast<u32>(backtrace(frames, CrashFrames));
        }

        backtrace_symbols_fd(frames, static_cast<int>(count), fd);
    }

    // The signal again, uncaught this time, so the run still ends as a crash
    signal(number, SIG_DFL);
    raise(number);
}
}

void InstallCrashLog()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = CrashHandler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
}

#endif
