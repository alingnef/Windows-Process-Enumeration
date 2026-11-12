#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

typedef NTSTATUS (NTAPI *NtQIP_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT *, USHORT *);

typedef NTSTATUS (NTAPI *NtQSI_t)(
    ULONG, PVOID, ULONG, PULONG
);

typedef struct {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    BYTE Reserved[48];
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE ParentProcessId;
} SPI;

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
    NtQSI_t NtQSI = (NtQSI_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"),
        "NtQuerySystemInformation"
    );

    ULONG len = 0;

    NtQSI(5, NULL, 0, &len);

    BYTE *buf = malloc(len + 65536);
    if (!buf)
        return 1;

    if (NtQSI(5, buf, len + 65536, &len) < 0)
        return 1;

    printf("%-30s %-6s %s\n", "NAME", "ARCH", "CMD");

    SPI *p = (SPI *)buf;

    while (1) {
        DWORD pid = (DWORD)(ULONG_PTR)p->UniqueProcessId;

        char name[MAX_PATH] = "?";

        if (p->ImageName.Buffer) {
            int n = WideCharToMultiByte(
                CP_UTF8, 0,
                p->ImageName.Buffer,
                p->ImageName.Length / 2,
                name,
                sizeof(name) - 1,
                NULL, NULL
            );

            name[n] = 0;
        }

        HANDLE h = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid
        );

        char *cmd = h ? cmdline(h) : NULL;

        printf("%-30s %-6s %s\n",
            name,
            h ? arch(h) : "?",
            cmd ? cmd : "?"
        );

        free(cmd);

        if (h)
            CloseHandle(h);

        if (!p->NextEntryOffset)
            break;

        p = (SPI *)((BYTE *)p + p->NextEntryOffset);
    }

    free(buf);
    return 0;
}