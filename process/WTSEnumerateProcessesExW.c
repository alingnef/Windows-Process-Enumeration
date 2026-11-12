/*
 * WTSEnumerateProcessesExW - PID + NAME + ARCH + CMD
 * Compatible with older MinGW-w64 / Strawberry GCC headers.
 * Build: gcc WTSEnumerateProcessesExW.c -o wtsex.exe -Wall -Wextra
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

/* Some MinGW headers lack these newer WTS declarations.
 * Names are kept local to avoid conflicts with SDK headers. */
typedef struct {
    DWORD SessionId;
    DWORD ProcessId;
    LPWSTR pProcessName;
    PSID pUserSid;
    DWORD NumberOfThreads;
    DWORD HandleCount;
    DWORD PagefileUsage;
    DWORD PeakPagefileUsage;
    DWORD WorkingSetSize;
    DWORD PeakWorkingSetSize;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
} MY_WTS_PROCESS_INFO_EXW;

typedef BOOL (WINAPI *WtsEnumExW_t)(HANDLE, DWORD *, DWORD, LPWSTR *, DWORD *);
typedef BOOL (WINAPI *WtsFreeExW_t)(DWORD, PVOID, ULONG);
typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT *, USHORT *);
typedef NTSTATUS (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

const char *arch(HANDLE h) {
    static IsWow64Process2_t fn = NULL;
    USHORT p = 0, n = 0;
    if (!h) return "?";
    if (!fn) fn = (IsWow64Process2_t)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "IsWow64Process2");
    if (fn && fn(h, &p, &n)) {
        USHORT m = p ? p : n;
        if (m == IMAGE_FILE_MACHINE_I386)  return "x86";
        if (m == IMAGE_FILE_MACHINE_AMD64) return "x64";
        if (m == IMAGE_FILE_MACHINE_ARM64) return "ARM64";
    }
    return "?";
}

char *utf8(const WCHAR *w, int chars) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, chars, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = (char *)malloc((size_t)n + 1);
    if (!out) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, chars, out, n, NULL, NULL)) {
        free(out);
        return NULL;
    }
    out[n] = '\0';
    return out;
}

char *cmdline(HANDLE h) {
    static NtQIP_t NtQIP = NULL;
    if (!h) return NULL;
    if (!NtQIP) NtQIP = (NtQIP_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) return NULL;

    ULONG len = 0;
    NtQIP(h, 60 /* ProcessCommandLineInformation */, NULL, 0, &len);
    if (len < sizeof(UNICODE_STRING) || len > 1024 * 1024) return NULL;

    UNICODE_STRING *u = (UNICODE_STRING *)malloc(len);
    if (!u) return NULL;
    if (NtQIP(h, 60, u, len, &len) < 0 || !u->Buffer) {
        free(u);
        return NULL;
    }
    char *s = utf8(u->Buffer, u->Length / sizeof(WCHAR));
    free(u);
    return s;
}

int main(void) {
    /* Load DLL at runtime rather than depend on old wtsapi32.h. */
    HMODULE dll = LoadLibraryW(L"wtsapi32.dll");
    if (!dll) {
        fprintf(stderr, "LoadLibrary(wtsapi32.dll) failed: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    WtsEnumExW_t enumEx = (WtsEnumExW_t)GetProcAddress(dll, "WTSEnumerateProcessesExW");
    WtsFreeExW_t freeEx = (WtsFreeExW_t)GetProcAddress(dll, "WTSFreeMemoryExW");
    if (!enumEx || !freeEx) {
        fprintf(stderr, "WTSEnumerateProcessesExW/WTSFreeMemoryExW unavailable\n");
        FreeLibrary(dll);
        return 1;
    }

    DWORD level = 1, count = 0;
    LPWSTR buffer = NULL;
    const DWORD ANY_SESSION = (DWORD)-2; /* WTS_ANY_SESSION */

    if (!enumEx(NULL /* current server */, &level, ANY_SESSION, &buffer, &count)) {
        fprintf(stderr, "WTSEnumerateProcessesExW failed: %lu\n",
                (unsigned long)GetLastError());
        FreeLibrary(dll);
        return 1;
    }

    MY_WTS_PROCESS_INFO_EXW *items = (MY_WTS_PROCESS_INFO_EXW *)buffer;
    SetConsoleOutputCP(CP_UTF8);
    printf("%-7s %-30s %-6s %s\n", "PID", "NAME", "ARCH", "CMD");

    for (DWORD i = 0; i < count; ++i) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               items[i].ProcessId);
        char *name = utf8(items[i].pProcessName, -1);
        char *cmd = cmdline(h);
        printf("%-7lu %-30s %-6s %s\n",
               (unsigned long)items[i].ProcessId,
               name && *name ? name : "?", arch(h), cmd ? cmd : "?");
        free(name);
        free(cmd);
        if (h) CloseHandle(h);
    }

    if (buffer && !freeEx(1 /* WTSTypeProcessInfoLevel1 */, buffer, count))
        fprintf(stderr, "WTSFreeMemoryExW failed: %lu\n",
                (unsigned long)GetLastError());
    FreeLibrary(dll);
    return 0;
}
