/* WMI Win32_Process through COM, pure C.
   Build: gcc WMI.c -o wmi.exe -lwbemuuid -lole32 -loleaut32 */
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    IWbemLocator *loc = NULL;
    IWbemServices *svc = NULL;
    IEnumWbemClassObject *en = NULL;
    BSTR ns = NULL, lang = NULL, sql = NULL;

    hr = CoInitializeSecurity(NULL, -1, NULL, NULL,
                              RPC_C_AUTHN_LEVEL_DEFAULT,
                              RPC_C_IMP_LEVEL_IMPERSONATE,
                              NULL, EOAC_NONE, NULL);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) goto cleanup;

    hr = CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWbemLocator, (void **)&loc);
    if (FAILED(hr)) goto cleanup;

    ns = SysAllocString(L"ROOT\\CIMV2");
    if (!ns) { hr = E_OUTOFMEMORY; goto cleanup; }
    hr = IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL,
                                    0, NULL, NULL, &svc);
    if (FAILED(hr)) goto cleanup;

    hr = CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT,
                           RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                           RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) goto cleanup;

    lang = SysAllocString(L"WQL");
    sql = SysAllocString(L"SELECT ProcessId, Name, CommandLine FROM Win32_Process");
    if (!lang || !sql) { hr = E_OUTOFMEMORY; goto cleanup; }
    hr = IWbemServices_ExecQuery(svc, lang, sql,
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                NULL, &en);
    if (FAILED(hr)) goto cleanup;

    SetConsoleOutputCP(CP_UTF8);
    printf("%-7s %-30s %-6s %s\n", "PID", "NAME", "ARCH", "CMD");

    for (;;) {
        IWbemClassObject *obj = NULL;
        ULONG count = 0;
        hr = IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &count);
        if (count == 0) break;

        VARIANT vp, vn, vc;
        VariantInit(&vp); VariantInit(&vn); VariantInit(&vc);
        IWbemClassObject_Get(obj, L"ProcessId", 0, &vp, NULL, NULL);
        IWbemClassObject_Get(obj, L"Name", 0, &vn, NULL, NULL);
        IWbemClassObject_Get(obj, L"CommandLine", 0, &vc, NULL, NULL);

        DWORD pid = V_VT(&vp) == VT_UI4 ? V_UI4(&vp) :
                    V_VT(&vp) == VT_I4 ? (DWORD)V_I4(&vp) : 0;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        char *name = utf8(V_VT(&vn) == VT_BSTR ? V_BSTR(&vn) : NULL);
        char *cmd = utf8(V_VT(&vc) == VT_BSTR ? V_BSTR(&vc) : NULL);

        printf("%-7lu %-30s %-6s %s\n", (unsigned long)pid,
               name ? name : "?", arch(h), cmd ? cmd : "?");
        free(name); free(cmd);
        if (h) CloseHandle(h);
        VariantClear(&vp); VariantClear(&vn); VariantClear(&vc);
        IWbemClassObject_Release(obj);
    }

cleanup:
    if (sql) SysFreeString(sql);
    if (lang) SysFreeString(lang);
    if (ns) SysFreeString(ns);
    if (en) IEnumWbemClassObject_Release(en);
    if (svc) IWbemServices_Release(svc);
    if (loc) IWbemLocator_Release(loc);
    CoUninitialize();
    if (FAILED(hr)) {
        fprintf(stderr, "WMI failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    return 0;
}
