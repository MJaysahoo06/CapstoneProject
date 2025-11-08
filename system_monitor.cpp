// system_monitor.cpp
// Single-file Windows console system monitor (MinGW GCC 6.3.0 compatible).
// Compile: g++ -std=c++11 system_monitor.cpp -o system_monitor.exe

#define _WIN32_WINNT 0x0500
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <iomanip>
#include <cctype>

struct ProcInfo
{
    DWORD pid;
    std::string name;
    double cpuPercent;
};

typedef unsigned long long ull;

// Convert FILETIME to 100-ns units
static ull FileTimeToULL(const FILETIME &ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// Get process kernel+user time
static bool GetProcTime100ns(HANDLE hProc, ull &out100ns)
{
    FILETIME ftCreation, ftExit, ftKernel, ftUser;
    if (!GetProcessTimes(hProc, &ftCreation, &ftExit, &ftKernel, &ftUser))
        return false;
    out100ns = FileTimeToULL(ftKernel) + FileTimeToULL(ftUser);
    return true;
}

// Snapshot processes and compute CPU%
static std::vector<ProcInfo> SnapshotProcesses(
    const std::map<DWORD, ull> &prevTimes,
    unsigned long prevTick,
    unsigned long curTick,
    std::map<DWORD, ull> &outNewTimes)
{
    std::vector<ProcInfo> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return list;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe))
    {
        CloseHandle(snap);
        return list;
    }

    do
    {
        ProcInfo p;
        p.pid = pe.th32ProcessID;
        p.name = pe.szExeFile;
        p.cpuPercent = 0.0;
        list.push_back(p);
    } while (Process32Next(snap, &pe));
    CloseHandle(snap);

    unsigned long deltaMs = (curTick >= prevTick) ? (curTick - prevTick) : (0xFFFFFFFF - prevTick + curTick);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    unsigned int numCores = si.dwNumberOfProcessors;
    if (numCores < 1)
        numCores = 1;

    for (size_t i = 0; i < list.size(); ++i)
    {
        ProcInfo &p = list[i];
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, p.pid);
        if (!h)
            continue;

        ull t100ns = 0;
        if (GetProcTime100ns(h, t100ns))
        {
            outNewTimes[p.pid] = t100ns;
            std::map<DWORD, ull>::const_iterator it = prevTimes.find(p.pid);
            if (it != prevTimes.end() && deltaMs > 0)
            {
                ull prev100ns = it->second;
                ull deltaProc = (t100ns > prev100ns) ? (t100ns - prev100ns) : 0;
                double wall100ns = (double)deltaMs * 10000.0;
                double usage = (double)deltaProc / (wall100ns * (double)numCores) * 100.0;
                if (usage < 0.0)
                    usage = 0.0;
                p.cpuPercent = usage;
            }
        }
        CloseHandle(h);
    }

    return list;
}

static void PrintHeader(int refreshSec, bool sortByCPU)
{
    std::cout << "Simple Windows Monitor  |  Refresh " << refreshSec << "s  |  Sort: "
              << (sortByCPU ? "CPU" : "PID") << "\n";
    std::cout << std::left << std::setw(8) << "PID"
              << std::setw(40) << "Process"
              << std::right << std::setw(10) << "CPU(%)" << "\n";
    std::cout << "------------------------------------------------------------------\n";
}

static void PrintProcesses(const std::vector<ProcInfo> &procs, int topN)
{
    int count = 0;
    std::cout << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < procs.size() && count < topN; ++i, ++count)
    {
        const ProcInfo &p = procs[i];
        std::string name = p.name;
        if (name.size() > 38)
            name = name.substr(0, 37) + "...";
        std::cout << std::left << std::setw(8) << p.pid
                  << std::setw(40) << name
                  << std::right << std::setw(10) << p.cpuPercent << "\n";
    }
    std::cout << "\n";
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

int main()
{
    const int refreshSec = 2;
    const int topN = 25;
    bool sortByCPU = true;

    std::map<DWORD, ull> prevTimes, curTimes;
    unsigned long prevTick = GetTickCount();

    // Initial snapshot
    {
        std::map<DWORD, ull> tmp;
        SnapshotProcesses(tmp, prevTick, prevTick, prevTimes);
    }

    while (true)
    {
        Sleep(refreshSec * 1000); // use Win32 Sleep for compatibility
        unsigned long curTick = GetTickCount();

        curTimes.clear();
        std::vector<ProcInfo> procs = SnapshotProcesses(prevTimes, prevTick, curTick, curTimes);

        prevTimes.swap(curTimes);
        prevTick = curTick;

        // sort using C++11-compatible comparators (no 'auto' lambda params)
        if (sortByCPU)
        {
            std::sort(procs.begin(), procs.end(), [](const ProcInfo &a, const ProcInfo &b)
                      { return a.cpuPercent > b.cpuPercent; });
        }
        else
        {
            std::sort(procs.begin(), procs.end(), [](const ProcInfo &a, const ProcInfo &b)
                      { return a.pid < b.pid; });
        }

        system("cls");
        PrintHeader(refreshSec, sortByCPU);
        PrintProcesses(procs, topN);

        std::cout << "Commands: (s)ort  (k)ill PID  (q)uit  (Enter) refresh\n";
        std::cout << "Enter: ";
        std::string cmd;
        std::getline(std::cin, cmd);
        cmd = trim(cmd);
        if (cmd.empty())
            continue;
        if (cmd == "q" || cmd == "Q")
            break;
        if (cmd == "s" || cmd == "S")
        {
            sortByCPU = !sortByCPU;
            continue;
        }

        // handle kill PID
        std::string pidStr;
        if (cmd[0] == 'k' || cmd[0] == 'K')
            pidStr = trim(cmd.substr(1));
        else
        {
            bool alld = true;
            for (size_t i = 0; i < cmd.size(); ++i)
                if (!std::isdigit((unsigned char)cmd[i]))
                {
                    alld = false;
                    break;
                }
            if (alld)
                pidStr = cmd;
        }

        if (!pidStr.empty())
        {
            try
            {
                DWORD pid = (DWORD)std::stoul(pidStr);
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (h)
                {
                    if (TerminateProcess(h, 1))
                        std::cout << "PID " << pid << " terminated.\n";
                    else
                        std::cout << "Failed to terminate PID " << pid << ". Error: " << GetLastError() << "\n";
                    CloseHandle(h);
                }
                else
                {
                    std::cout << "Cannot open PID " << pid << " (Error: " << GetLastError() << ")\n";
                }
            }
            catch (...)
            {
                std::cout << "Invalid PID input.\n";
            }
            std::cout << "Press Enter to continue...";
            std::string dummy;
            std::getline(std::cin, dummy);
        }
    }

    std::cout << "Exiting monitor.\n";
    return 0;
}