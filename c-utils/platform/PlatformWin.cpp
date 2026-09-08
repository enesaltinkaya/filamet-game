#include "Platform.h"  // IWYU pragma: keep

#ifdef _WIN32
#include "logger/Logger.h"
#include "string/String.h"
#include <windows.h>
#include <psapi.h>
#include <sys/stat.h>
#include <stdio.h>
#include <direct.h>

namespace utils {
Platform platform = {};

Platform* getPlatform(void) {
    return &platform;
}

void platformDestroy(void) {
    info("---------------------------");
    debug("bye");
    debug("memory usage   : %zu", memoryUsage());
    info("---------------------------");
}

void platformInit(void) {
    platform.isWindows = 1;

    char temp[200] = {};
    char drive[10] = {};
    char path[190] = {};
    GetModuleFileName(NULL, temp, sizeof(temp) - 1);
    _splitpath_s(temp, drive, sizeof(drive), path, sizeof(path), NULL, 0, NULL, 0);

    strcpy(platform.executablePath, temp);
    strcpy(platform.seperator, "\\");
    sprintf(platform.cwd, "%s%s", drive, path);

    snprintf(platform.dataDirectory, 1023, "%s%s%s", platform.cwd, "data", platform.seperator);
    printf("data directory: %s\n", platform.dataDirectory);
    createDirectory(platform.dataDirectory);

    HANDLE hCurrentProcess = GetCurrentProcess();
    SetPriorityClass(hCurrentProcess, ABOVE_NORMAL_PRIORITY_CLASS);
}

void createDirectory(const char* path) {
    if (!path || strlen(path) == 0) {
        return;
    }

    char* pathCopy = strdup(path);
    if (!pathCopy) {
        return;
    }

    char* lastSep          = strrchr(pathCopy, '\\');
    char* lastForwardSlash = strrchr(pathCopy, '/');

    if (lastForwardSlash && (!lastSep || lastForwardSlash > lastSep)) {
        lastSep = lastForwardSlash;
    }

    if (!lastSep) {
        free(pathCopy);
        return;
    }

    *lastSep = '\0';

    char* p = pathCopy;

    // Skip drive letter if present (e.g., "C:")
    if (strlen(pathCopy) >= 2 && pathCopy[1] == ':') {
        p += 2;
        if (*p == '\\' || *p == '/') p++;
    }

    for (; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            _mkdir(pathCopy);
            *p = '\\';
        }
    }

    _mkdir(pathCopy);

    free(pathCopy);
}

u64 memoryUsage(void) {
    PROCESS_MEMORY_COUNTERS memCounter;
    GetProcessMemoryInfo(GetCurrentProcess(), &memCounter, sizeof(memCounter));
    return memCounter.WorkingSetSize / 1024;
}

int numberOfCores(void) {
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
}

}
#endif
