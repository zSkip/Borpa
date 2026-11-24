// Stand-in for a solver in the process-runner tests. Usage: sim_test_child <mode> [arg]
//   exit0           exit 0, print nothing
//   fail            print an error line, exit 3
//   hang            print nothing, use no CPU, never exit
//   talk-then-hang  print three lines, then hang
//   busy <seconds>  spin the CPU silently, then exit 0
//   grandchild <f>  start "hang" as a child of its own, write its pid to file f, then hang
//   env             print PATH and OMP_NUM_THREADS
//   lines           print "a\r\nb\nc" with no final line ending
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static void hang()
{
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    std::string mode = argv[1];
    if (mode == "exit0")
        return 0;
    if (mode == "fail") {
        std::printf("Error   : boom\n");
        return 3;
    }
    if (mode == "hang")
        hang();
    if (mode == "talk-then-hang") {
        std::printf("one\ntwo\nthree\n");
        std::fflush(stdout);
        hang();
    }
    if (mode == "busy") {
        double     seconds = argc > 2 ? std::atof(argv[2]) : 1.0;
        auto       until   = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        volatile unsigned long long x = 0;
        while (std::chrono::steady_clock::now() < until)
            for (int i = 0; i < 100000; ++i)
                x = x + i;
        return 0;
    }
    if (mode == "grandchild" && argc > 2) {
#ifdef _WIN32
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string         cmd = std::string("\"") + self + "\" hang";
        STARTUPINFOA        si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return 4;
        unsigned long pid = pi.dwProcessId;
#else
        pid_t pid = fork();
        if (pid == 0)
            hang();
#endif
        if (FILE* f = std::fopen(argv[2], "w")) {
            std::fprintf(f, "%lu", (unsigned long) pid);
            std::fclose(f);
        }
        hang();
    }
    if (mode == "env") {
        const char* path = std::getenv("PATH");
        const char* omp  = std::getenv("OMP_NUM_THREADS");
        std::printf("PATH=%s\nOMP_NUM_THREADS=%s\n", path ? path : "", omp ? omp : "");
        return 0;
    }
    if (mode == "lines") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY); // write the bytes exactly, no \n to \r\n
#endif
        std::fputs("a\r\nb\nc", stdout);
        return 0;
    }
    return 2;
}
