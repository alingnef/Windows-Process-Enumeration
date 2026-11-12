#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <psapi.h>
#include <string.h>

typedef NTSTATUS (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT *, USHORT *);

const char *arch(HANDLE h) {
    static IsWow64Process2_t IsWow64Process2_;
    USHORT p = 0, n = 0;

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

char *cmdline(HANDLE h) {
    static NtQIP_t NtQIP;
    if (!NtQIP)
        NtQIP = (NtQIP_t)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                        "NtQueryInformationProcess");

    ULONG len = 0;
    NtQIP(h, 60 /* ProcessCommandLineInformation */, NULL, 0, &len);
    if (!len) return NULL;

    UNICODE_STRING *u = malloc(len);
    if (!u) return NULL;

    if (NtQIP(h, 60, u, len, &len) < 0) { free(u); return NULL; }

    int n = WideCharToMultiByte(CP_UTF8, 0, u->Buffer, u->Length / 2, NULL, 0, NULL, NULL);
    char *s = malloc(n + 1);
    if (s) {
        WideCharToMultiByte(CP_UTF8, 0, u->Buffer, u->Length / 2, s, n, NULL, NULL);
        s[n] = 0;
    }
    free(u);
    return s;
}

int main() {
    DWORD pid[4096], bytes;

    SetConsoleOutputCP(CP_UTF8);

    if (!EnumProcesses(pid, sizeof(pid), &bytes))
        return 1;

    printf("%-30s %-6s %s\n", "NAME", "ARCH", "CMD");

    for (DWORD i = 0; i < bytes / sizeof(DWORD); i++) {
        HANDLE h = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid[i]
        );

        if (!h)
            continue;

        char path[MAX_PATH] = "?";
        DWORD n = sizeof(path);

        QueryFullProcessImageNameA(h, 0, path, &n);

        char *name = strrchr(path, '\\');
        name = name ? name + 1 : path;

        char *cmd = cmdline(h);

        printf("%-30s %-6s %s\n",
            name,
            arch(h),
            cmd ? cmd : "?"
        );

        free(cmd);
        CloseHandle(h);
    }

    return 0;
}