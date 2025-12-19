// Programa para Windows/Posix que muestra cómo liberar memoria del proceso actual
// y cómo listar procesos que consumen mucha memoria. Incluye una opción para
// terminar procesos (requiere permisos y se debe usar con cuidado).

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <tuple>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <tchar.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <fstream>
#endif

// Contract (inputs/outputs):
// - trimCurrentProcessWorkingSet(): no input, attempts to reduce current process
//   working set (free memory back to OS). Prints before/after stats.
// - listHighMemoryProcesses(thresholdMB): returns vector of (pid, name, rssBytes)
// - tryTerminateProcess(pid): attempts to terminate process, returns success bool
// Error modes: lack of privileges, process gone between enumeration and action.

#ifdef _WIN32
static SIZE_T getProcessWorkingSet(HANDLE hProcess) {
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize;
    }
    return 0;
}

// Helper: convert TCHAR (ANSI or WCHAR) to UTF-8 std::string
static std::string tchar_to_utf8(const TCHAR* s) {
#ifdef UNICODE
    if (!s) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], needed, nullptr, nullptr);
    // remove trailing null
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
#else
    return std::string(s ? s : "");
#endif
}

void trimCurrentProcessWorkingSet() {
    HANDLE h = GetCurrentProcess();
    SIZE_T before = getProcessWorkingSet(h);
    std::cout << "Before trim: " << (before / 1024) << " KB\n";

    // Ask the OS to trim the working set for this process.
    // Passing -1 for min and max is a documented technique to tell Windows to trim.
    if (!SetProcessWorkingSetSize(h, (SIZE_T)-1, (SIZE_T)-1)) {
        std::cerr << "SetProcessWorkingSetSize failed, error=" << GetLastError() << "\n";
    }

    SIZE_T after = getProcessWorkingSet(h);
    std::cout << "After  trim: " << (after / 1024) << " KB\n";
}

std::vector<std::tuple<DWORD, std::string, SIZE_T>> listHighMemoryProcesses(size_t thresholdMB) {
    std::vector<std::tuple<DWORD, std::string, SIZE_T>> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            DWORD pid = pe.th32ProcessID;
            std::string name = tchar_to_utf8(pe.szExeFile);
            HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            SIZE_T rss = 0;
            if (h) {
                rss = getProcessWorkingSet(h);
                CloseHandle(h);
            }
            if (rss >= thresholdMB * 1024ULL * 1024ULL) {
                out.emplace_back(pid, name, rss);
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool tryTerminateProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok == TRUE;
}

#else // POSIX (Linux/macOS) - best-effort implementations

void trimCurrentProcessWorkingSet() {
#ifdef __linux__
    // On glibc systems, malloc_trim can return free pages to kernel
    // but it's not guaranteed. We'll attempt it if available.
    std::cout << "Requesting malloc_trim (glibc) if available...\n";
    #if defined(__GLIBC__)
    extern int malloc_trim(size_t);
    int r = malloc_trim(0);
    std::cout << "malloc_trim returned " << r << "\n";
    #else
    std::cout << "malloc_trim not available on this platform.\n";
    #endif
#else
    std::cout << "No portable trim available on this POSIX platform.\n";
#endif
}

std::vector<std::tuple<pid_t, std::string, size_t>> listHighMemoryProcesses(size_t thresholdMB) {
    std::vector<std::tuple<pid_t, std::string, size_t>> out;
#ifdef __linux__
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        pid_t pid = atoi(e->d_name);
        if (pid <= 0) continue;
        std::string statm = std::string("/proc/") + e->d_name + "/statm";
        std::ifstream f(statm);
        if (!f) continue;
        size_t sizePages = 0, resident = 0;
        f >> sizePages >> resident;
        long pageSize = sysconf(_SC_PAGESIZE);
        size_t rss = resident * pageSize;
        if (rss >= thresholdMB * 1024ULL * 1024ULL) {
            // try to read name
            std::string commPath = std::string("/proc/") + e->d_name + "/comm";
            std::ifstream c(commPath);
            std::string name;
            if (c) std::getline(c, name);
            out.emplace_back(pid, name, rss);
        }
    }
    closedir(d);
#endif
    return out;
}

bool tryTerminateProcess(pid_t pid) {
    if (kill(pid, SIGTERM) == 0) return true;
    return false;
}

#endif

// Interactive menu: 1=free memory, 2=handle processes, 3=both, 4=exit
void runInteractiveMenu() {
    while (true) {
        std::cout << "\nMenu:\n";
        std::cout << " 1) Eliminar memoria del proceso actual\n";
        std::cout << " 2) Listar procesos que consumen memoria (y opcionalmente terminarlos)\n";
        std::cout << " 3) Eliminar memoria y procesar procesos (1+2)\n";
        std::cout << " 4) Salir\n";
        std::cout << "Elige una opción: ";
        int opt = 0;
        if (!(std::cin >> opt)) {
            std::cin.clear();
            std::string skip; std::getline(std::cin, skip);
            std::cout << "Entrada no válida. Intenta de nuevo.\n";
            continue;
        }
        if (opt == 4) break;
        if (opt == 1 || opt == 3) {
            trimCurrentProcessWorkingSet();
        }
        if (opt == 2 || opt == 3) {
            std::cout << "Umbral en MB para listar procesos: ";
            size_t threshold = 0;
            if (!(std::cin >> threshold)) {
                std::cin.clear(); std::string s; std::getline(std::cin, s);
                std::cout << "Umbral inválido. Volviendo al menú.\n";
                continue;
            }
            // ask whether to kill
            std::cout << "Intentar terminar procesos listados? (s/n): ";
            char killch = 'n';
            std::cin >> killch;
            bool doKill = (killch == 's' || killch == 'S' || killch == 'y' || killch == 'Y');
            std::cout << "Procesos con >= " << threshold << " MB:\n";

#ifdef _WIN32
            auto procs = listHighMemoryProcesses(threshold);
            for (auto &t : procs) {
                DWORD pid; std::string name; SIZE_T rss;
                std::tie(pid, name, rss) = t;
                std::cout << " PID=" << pid << " name=" << name << " rssMB=" << (rss / 1024 / 1024) << "\n";
                if (doKill) {
                    std::cout << "  Intentando terminar PID " << pid << " ... ";
                    if (tryTerminateProcess(pid)) std::cout << "OK\n"; else std::cout << "FAILED\n";
                }
            }
#else
            auto procs = listHighMemoryProcesses(threshold);
            for (auto &t : procs) {
                pid_t pid; std::string name; size_t rss;
                std::tie(pid, name, rss) = t;
                std::cout << " PID=" << pid << " name=" << name << " rssMB=" << (rss / 1024 / 1024) << "\n";
                if (doKill) {
                    std::cout << "  Intentando terminar PID " << pid << " ... ";
                    if (tryTerminateProcess(pid)) std::cout << "OK\n"; else std::cout << "FAILED\n";
                }
            }
#endif
        }
    }
}

// alternate_main: una entrada alternativa que ejecuta una demostración simple
void alternate_main() {
    std::cout << "Alternate main: trimming current process working set...\n";
    trimCurrentProcessWorkingSet();
    std::cout << "Done. Use the program with arguments to list/kill processes.\n";
}

int main(int argc, char** argv) {
    // Si no hay argumentos, iniciar modo interactivo con menú
    if (argc < 2) {
        runInteractiveMenu();
        return 0;
    }
    // Uso:
    // ex1.exe trim                       -> recorta el working set del proceso actual
    // ex1.exe list <thresholdMB>         -> lista procesos que usan >= thresholdMB
    // ex1.exe list <thresholdMB> --kill  -> intenta terminar esos procesos (USE CON CUIDADO)
    // ex1.exe alt                        -> ejecuta "alternate_main"

    if (argc >= 2) {
        std::string cmd = argv[1];
        if (cmd == "trim") {
            trimCurrentProcessWorkingSet();
            return 0;
        } else if (cmd == "list" && argc >= 3) {
            size_t threshold = std::stoul(argv[2]);
            bool doKill = false;
            if (argc >= 4 && std::string(argv[3]) == "--kill") doKill = true;

#ifdef _WIN32
            auto procs = listHighMemoryProcesses(threshold);
            if (procs.empty()) {
                std::cout << "No processes found using >= " << threshold << " MB\n";
            }
            for (auto &t : procs) {
                DWORD pid; std::string name; SIZE_T rss;
                std::tie(pid, name, rss) = t;
                std::cout << "PID=" << pid << " name=" << name << " rssMB=" << (rss / 1024 / 1024) << "\n";
                if (doKill) {
                    std::cout << "  Attempting to terminate PID " << pid << " ... ";
                    if (tryTerminateProcess(pid)) std::cout << "OK\n"; else std::cout << "FAILED\n";
                }
            }
#else
            auto procs = listHighMemoryProcesses(threshold);
            if (procs.empty()) {
                std::cout << "No processes found using >= " << threshold << " MB\n";
            }
            for (auto &t : procs) {
                pid_t pid; std::string name; size_t rss;
                std::tie(pid, name, rss) = t;
                std::cout << "PID=" << pid << " name=" << name << " rssMB=" << (rss / 1024 / 1024) << "\n";
                if (doKill) {
                    std::cout << "  Attempting to terminate PID " << pid << " ... ";
                    if (tryTerminateProcess(pid)) std::cout << "OK\n"; else std::cout << "FAILED\n";
                }
            }
#endif
            return 0;
        } else if (cmd == "alt") {
            alternate_main();
            return 0;
        }
    }

    std::cout << "Usage:\n";
    std::cout << "  " << argv[0] << " trim\n";
    std::cout << "  " << argv[0] << " list <thresholdMB> [--kill]\n";
    std::cout << "  " << argv[0] << " alt\n";
    return 1;
}
