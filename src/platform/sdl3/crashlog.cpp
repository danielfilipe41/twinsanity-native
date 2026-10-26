// The host's crash log: what the machine was doing when the game died, written to crash.log beside the run and to stderr, so
// a crash outside gdb still says where it happened. The handler is async-signal-safe (open/write/backtrace_symbols_fd only),
// and the host links with -rdynamic so those frames come out with names. SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT are all
// caught: the last three are what the C++ runtime and asserts raise.
#include "common.h"

#include <csignal>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <ucontext.h>
#include <unistd.h>

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
