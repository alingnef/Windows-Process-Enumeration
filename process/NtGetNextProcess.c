/* NtGetNextProcess: traverse process handles using ntdll.
   Build: gcc NtGetNextProcess.c -o ntgetnext.exe */
/* Helpers reused from enumprocess.c, NtQuery.c and snapshot.c */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef NTSTATUS (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT *, USHORT *);

const char *arch(HANDLE h) {
    static IsWow64Process2_t IsWow64Process2_;
    USHORT p = 0, n = 0;

    if (!h) return "?";
    if (!IsWow64Process2_)
        IsWow64Process2_ = (IsWow64Process2_t)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "IsWow64Process2");

    if (IsWow64Process2_ && IsWow64Process2_(h, &p, &n)) {
        USHORT m = p ? p : n;
        if (m == IMAGE_FILE_MACHINE_I386)  return "x86";
        if (m == IMAGE_FILE_MACHINE_AMD64) return "x64";
        if (m == IMAGE_FILE_MACHINE_ARM64) return "ARM64";
    }
    return "?";
}

char *utf8(const WCHAR *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (s && !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

char *cmdline(HANDLE h) {
    static NtQIP_t NtQIP;
    if (!h) return NULL;
    if (!NtQIP)
        NtQIP = (NtQIP_t)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQIP) return NULL;

    ULONG len = 0;
    NtQIP(h, 60 /* ProcessCommandLineInformation */, NULL, 0, &len);
    if (len < sizeof(UNICODE_STRING) || len > 1024 * 1024) return NULL;

    UNICODE_STRING *u = malloc(len);
    if (!u) return NULL;
    if (NtQIP(h, 60, u, len, &len) < 0 || !u->Buffer) {
        free(u);
        return NULL;
    }

    int n = WideCharToMultiByte(CP_UTF8, 0, u->Buffer,
                                u->Length / sizeof(WCHAR), NULL, 0, NULL, NULL);
    if (n <= 0) { free(u); return NULL; }
    char *s = malloc((size_t)n + 1);
    if (s) {
        WideCharToMultiByte(CP_UTF8, 0, u->Buffer,
                            u->Length / sizeof(WCHAR), s, n, NULL, NULL);
        s[n] = 0;
    }
    free(u);
    return s;
}

void print_process(DWORD pid, const WCHAR *name, HANDLE h) {
    char *name8 = utf8(name);
    char *cmd = cmdline(h);
    printf("%-7lu %-30s %-6s %s\n", (unsigned long)pid,
           name8 && *name8 ? name8 : "?", arch(h), cmd ? cmd : "?");
    free(name8);
    free(cmd);
}

void print_header(void) {
    SetConsoleOutputCP(CP_UTF8);
    printf("%-7s %-30s %-6s %s\n", "PID", "NAME", "ARCH", "CMD");
}

typedef NTSTATUS (NTAPI *NtGNP_t)(
    HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

int main(void) {
    NtGNP_t NtGetNextProcess_ = (NtGNP_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtGetNextProcess");
    if (!NtGetNextProcess_) {
        fprintf(stderr, "NtGetNextProcess is unavailable\n");
        return 1;
    }

    print_header();
    HANDLE previous = NULL;
    HANDLE next = NULL;
    NTSTATUS status;

    while ((status = NtGetNextProcess_(previous,
                                      PROCESS_QUERY_LIMITED_INFORMATION,
                                      0, 0, &next)) >= 0) {
        if (previous) CloseHandle(previous);

        WCHAR path[2048] = L"?";
        DWORD n = sizeof(path) / sizeof(path[0]);
        if (!QueryFullProcessImageNameW(next, 0, path, &n))
            wcscpy(path, L"?");
        const WCHAR *name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;

        print_process(GetProcessId(next), name, next);
        previous = next;
        next = NULL;
    }

    if (previous) CloseHandle(previous);
    /* STATUS_NO_MORE_ENTRIES (0x8000001A) means normal completion. */
    if ((ULONG)status != 0x8000001AUL)
        fprintf(stderr, "NtGetNextProcess stopped: 0x%08lx\n",
                (unsigned long)(ULONG)status);
    return 0;
}
