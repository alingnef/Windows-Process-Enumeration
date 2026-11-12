/* NtQuerySystemInformation(SystemBasicProcessInformation = 252)
   Requires Windows 11 build 26100.4770 or newer.
   Build: gcc SystemBasicProcessInformation.c -o basicprocess.exe */
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

typedef NTSTATUS (NTAPI *NtQSI_t)(ULONG, PVOID, ULONG, PULONG);

typedef struct {
    ULONG NextEntryOffset;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG64 SequenceNumber;
    UNICODE_STRING ImageName;
} BASIC_PROCESS_INFO;

int main(void) {
    NtQSI_t NtQSI = (NtQSI_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    if (!NtQSI) return 1;

    ULONG size = 65536, required = 0;
    BYTE *buf = NULL;
    NTSTATUS status = (NTSTATUS)0xC0000004L;
    for (int i = 0; i < 10; ++i) {
        BYTE *tmp = realloc(buf, size);
        if (!tmp) { free(buf); return 1; }
        buf = tmp;

        status = NtQSI(252 /* SystemBasicProcessInformation */,
                       buf, size, &required);
        if (status >= 0) break;
        if ((ULONG)status != 0xC0000004UL &&
            (ULONG)status != 0xC0000023UL) break;
        size = required > size ? required + 65536 : size * 2;
    }
    if (status < 0) {
        fprintf(stderr, "NtQuerySystemInformation(252) failed: 0x%08lx\n"
                "Requires Windows 11 build 26100.4770 or newer.\n",
                (unsigned long)(ULONG)status);
        free(buf);
        return 1;
    }

    print_header();
    ULONG offset = 0;
    while (offset <= size - sizeof(BASIC_PROCESS_INFO)) {
        BASIC_PROCESS_INFO *p = (BASIC_PROCESS_INFO *)(buf + offset);
        DWORD pid = (DWORD)(ULONG_PTR)p->UniqueProcessId;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        WCHAR name[260] = L"?";
        if (p->ImageName.Buffer && p->ImageName.Length) {
            size_t n = p->ImageName.Length / sizeof(WCHAR);
            if (n >= 260) n = 259;
            wmemcpy(name, p->ImageName.Buffer, n);
            name[n] = L'\0';
        } else {
            wcscpy(name, pid == 0 ? L"Idle" : pid == 4 ? L"System" : L"?");
        }
        print_process(pid, name, h);
        if (h) CloseHandle(h);

        if (!p->NextEntryOffset || p->NextEntryOffset > size - offset)
            break;
        offset += p->NextEntryOffset;
    }

    free(buf);
    return 0;
}
