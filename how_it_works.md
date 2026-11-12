# Cơ chế hoạt động của 8 phương pháp liệt kê process trên Windows
## 1. Tổng quan nguồn dữ liệu

| File | Nhóm API | Hàm/cơ chế liệt kê | Nguồn PID | Nguồn NAME | Nguồn CMD |
| --- | --- | --- | --- | --- | --- |
| `enumprocess.c` | PSAPI | `EnumProcesses()` | `pid[i]` | `QueryFullProcessImageNameA()` | `NtQueryInformationProcess(60)` |
| `snapshot.c` | Tool Help | `CreateToolhelp32Snapshot()` + `Process32First/Next()` | `p.th32ProcessID` | `PROCESSENTRY32.szExeFile` | `NtQueryInformationProcess(60)` |
| `NtQuery.c` | Native API | `NtQuerySystemInformation(5)` | `p->UniqueProcessId` | `SYSTEM_PROCESS_INFORMATION.ImageName` | `NtQueryInformationProcess(60)` |
| `WTSEnumerateProcessesW.c` | WTS | `WTSEnumerateProcessesW()` | `items[i].ProcessId` | `WTS_PROCESS_INFOW.pProcessName` | `NtQueryInformationProcess(60)` |
| `NtGetNextProcess.c` | Native API | `NtGetNextProcess()` | `GetProcessId(next)` | `QueryFullProcessImageNameW()` | `NtQueryInformationProcess(60)` |
| `WTSEnumerateProcessesExW.c` | WTS (dynamic loading) | `WTSEnumerateProcessesExW()` qua `GetProcAddress()` | `items[i].ProcessId` | `MY_WTS_PROCESS_INFO_EXW.pProcessName` | `NtQueryInformationProcess(60)` |
| `WMI.c` | COM/WMI | Truy vấn WQL tới `Win32_Process` | `Win32_Process.ProcessId` | `Win32_Process.Name` | `Win32_Process.CommandLine` |
| `SystemBasicProcessInformation.c` | Native API | `NtQuerySystemInformation(252)` | `p->UniqueProcessId` | `SYSTEM_BASICPROCESS_INFORMATION.ImageName` | `NtQueryInformationProcess(60)` |


## 2. Hai thao tác dùng chung: xác định ARCH và đọc CMD

### 2.1. `ARCH`: `OpenProcess()` + `IsWow64Process2()`

Hầu hết các file (trừ `NtGetNextProcess.c`) cần mở handle bằng PID:

```c
HANDLE h = OpenProcess(
    PROCESS_QUERY_LIMITED_INFORMATION,
    FALSE,
    pid
);
```

- `pid`: ID của process cần truy vấn.
- `PROCESS_QUERY_LIMITED_INFORMATION`: xin quyền đọc một số thông tin cơ bản, không phải quyền can thiệp/chỉnh sửa tiến trình.
- `h`: handle được trả về; phải `CloseHandle(h)` sau khi dùng xong.

Trong các hàm `arch(HANDLE h)`, chương trình nạp động địa chỉ `IsWow64Process2()` từ `kernel32.dll` bằng `GetProcAddress`. Sau đó:

```c
USHORT p = 0, n = 0;
IsWow64Process2_(h, &p, &n);
USHORT m = p ? p : n;
```

- `p` (**process machine**): mã kiến trúc thực thi dưới WOW64, hoặc `IMAGE_FILE_MACHINE_UNKNOWN` nếu process không chạy theo kiểu WOW64.
- `n` (**native machine**): kiến trúc native của hệ thống.
- Nếu `p != 0`, lấy `p`; nếu không, dùng `n` để xác định kiến trúc theo logic hiện tại.
- `IMAGE_FILE_MACHINE_I386` → `x86`; `IMAGE_FILE_MACHINE_AMD64` → `x64`; `IMAGE_FILE_MACHINE_ARM64` → `ARM64`; các trường hợp còn lại → `?`.

**Hạn chế:** Đây là cách phân loại ngắn gọn trong mã nguồn, chưa xử lý mọi tình huống kiến trúc đặc biệt/hệ điều hành giả lập (chẳng hạn các biến thể ARM). `?` có nghĩa là **chưa xác định**, không đồng nghĩa process không tồn tại.

### 2.2. `CMD`: `NtQueryInformationProcess(60)`

Trong **7/8 file**, command line được lấy bằng Native API:

```c
NtQueryInformationProcess(
    h,
    60,       // ProcessCommandLineInformation
    buffer,
    bufferSize,
    &requiredSize
);
```

- `60` là information class **`ProcessCommandLineInformation`**.
- Hàm nhận process handle (`h`) và đưa về `UNICODE_STRING` chứa nội dung command line (UTF-16).
- Code thường gọi lần đầu với buffer rỗng để hỏi kích thước cần cấp phát, sau đó dùng `malloc()` và gọi lần hai.
- `WideCharToMultiByte(CP_UTF8, ...)` chuyển UTF-16 sang UTF-8 để `printf()` in ra console; cuối cùng `free()` bộ nhớ đã cấp phát.

Luồng tổng quát:

```text
PID / process handle
        |
        v
OpenProcess(...)  [bo qua neu da co handle]
        |
        +--> IsWow64Process2(...) --> ARCH
        |
        +--> NtQueryInformationProcess(class = 60)
                       |
                       v
                 UNICODE_STRING
                       |
                       v
               WideCharToMultiByte
                       |
                       v
                      CMD
```

**Ngoại lệ:** `WMI.c` lấy `CommandLine` trực tiếp từ thuộc tính WMI, **không** gọi `NtQueryInformationProcess` để đọc CMD.

**CMD không phải danh sách tham số (`argv`) đã được parser tách sẵn** mà là chuỗi command line của process. Nó có thể chứa đường dẫn, tham số, dấu nháy, hoặc thay đổi trong khi process đang chạy. Không nên coi CMD là chứng cứ bất biến hoặc đáng tin cậy tuyệt đối.

---


## 3. PSAPI — `EnumProcesses`

**File:** `enumprocess.c` · **Nhóm:** Process Status API (PSAPI)
### Cơ chế hoạt động

`EnumProcesses()` chỉ trả về **mảng PID**, không trả trực tiếp tên process, kiến trúc hay command line.

1. Khởi tạo `DWORD pid[4096]` và `DWORD bytes`.
2. Gọi `EnumProcesses(pid, sizeof(pid), &bytes)` để hệ thống ghi PID vào mảng.
3. Tính số PID bằng `bytes / sizeof(DWORD)`.
4. Với mỗi PID, gọi `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, ...)`.
5. Gọi `QueryFullProcessImageNameA()` để lấy đường dẫn executable, rồi `strrchr(path, '\\')` để lấy **tên file cuối đường dẫn** (`NAME`).
6. Với handle đó, gọi `arch(h)` và `cmdline(h)`.
7. In `PID | NAME | ARCH | CMD`, lấy **PID trực tiếp từ `pid[i]`**; sau đó `free(cmd)` và `CloseHandle(h)`.

Đoạn mã quyết định phần liệt kê:

```c
DWORD pid[4096], bytes;
if (!EnumProcesses(pid, sizeof(pid), &bytes))
    return 1;

for (DWORD i = 0; i < bytes / sizeof(DWORD); i++) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid[i]);
    if (!h) continue;
    /* lay NAME, ARCH, CMD; sau do in kem pid[i] */
    CloseHandle(h);
}
```

Lệnh in trong **phiên bản đã bổ sung PID** là:

```c
printf("%-7lu %-30s %-6s %s\n",
       (unsigned long)pid[i],
       name,
       arch(h),
       cmd ? cmd : "?");
```

Giá trị `pid[i]` được lấy ngay từ mảng do `EnumProcesses()` cung cấp, không cần gọi API khác để tìm PID.

### Điểm cần chú ý

- Mảng `pid[4096]` có dung lượng cố định. Nếu số PID lớn đến mức buffer bị đầy, kết quả **có thể thiếu process**. API không báo rõ việc bị cắt; một phiên bản chắc chắn hơn cần tăng buffer và thử lại khi `bytes == sizeof(pid)`.
- Source **`continue` nếu `OpenProcess` thất bại**, nên một số PID có thể **không xuất hiện** trong output.
- Hàm trả về tên thông qua đường dẫn executable; `QueryFullProcessImageNameA` là bản ANSI, có thể gặp vấn đề với đường dẫn chứa ký tự Unicode ngoài bảng mã máy.
- Lợi thế: cách tiếp cận rất trực tiếp — **lấy danh sách PID trước, truy vấn từng PID sau**.


## 4. Tool Help — `CreateToolhelp32Snapshot`

**File:** `snapshot.c` · **Nhóm:** Tool Help API
### Cơ chế hoạt động

Khác với `EnumProcesses`, Tool Help tạo một **snapshot** của danh sách process rồi cung cấp các record `PROCESSENTRY32` có cả PID và tên exe.

1. Khởi tạo `PROCESSENTRY32 p = { sizeof(p) };`. Trường `dwSize` **phải** được điền.
2. Gọi `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` để lấy snapshot handle.
3. Dùng `Process32First(snapshot, &p)` để đọc bản ghi đầu tiên.
4. Dùng `do { ... } while (Process32Next(snapshot, &p));` để duyệt hết bản ghi.
5. `p.th32ProcessID` chứa PID; `p.szExeFile` chứa `NAME`.
6. `OpenProcess(...)` theo PID để truy vấn `ARCH`/`CMD`.
7. In `PID | NAME | ARCH | CMD`, với PID là `p.th32ProcessID`, không phụ thuộc vào kết quả mở handle.
8. Gọi `CloseHandle(snapshot)` sau khi dùng xong.

Đoạn code cốt lõi:

```c
PROCESSENTRY32 p = { sizeof(p) };
HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
if (s == INVALID_HANDLE_VALUE) return 1;
if (!Process32First(s, &p)) return 1;

do {
    /* p.th32ProcessID va p.szExeFile */
} while (Process32Next(s, &p));

CloseHandle(s);
```

Lệnh in trong **phiên bản đã bổ sung PID**:

```c
printf("%-7lu %-30s %-6s %s\n",
       (unsigned long)p.th32ProcessID,
       p.szExeFile,
       h ? arch(h) : "?",
       cmd ? cmd : "?");
```

Do `PROCESSENTRY32` đã chứa PID và tên process, chương trình vẫn có thể hiển thị hai trường này ngay cả khi `OpenProcess()` thất bại.

### Điểm cần chú ý

- Đây là **snapshot**, nên process khởi chạy/thoát sau thời điểm chụp có thể khiến danh sách không còn khớp trạng thái hiện tại.
- Vì `NAME` đã nằm trong `PROCESSENTRY32`, code **vẫn có thể in tên** khi `OpenProcess` thất bại; khi đó `ARCH` và `CMD` là `?`.
- `TH32CS_SNAPPROCESS` lấy danh sách process; không cần liệt kê module hoặc thread để đáp ứng mục tiêu project.


## 5. Native API — `NtQuerySystemInformation(5)`

**File:** `NtQuery.c` · **Nhóm:** NT Native API, `ntdll.dll`
### Cơ chế hoạt động

`NtQuerySystemInformation()` là một API chung: tham số `SystemInformationClass` quyết định loại dữ liệu trả về. Trong code này, giá trị **`5`** tương ứng **`SystemProcessInformation`**.

1. Tìm địa chỉ `NtQuerySystemInformation` trong `ntdll.dll` bằng `GetProcAddress()`.
2. Gọi với buffer `NULL` để lấy kích thước dữ liệu cần cấp phát vào `len`.
3. Code cấp phát `malloc(len + 65536)` rồi gọi lại `NtQuerySystemInformation(5, ...)`.
4. Buffer chứa các record kiểu `SYSTEM_PROCESS_INFORMATION` liên tiếp, với các trường quan trọng: `NextEntryOffset`, `ImageName`, `UniqueProcessId`.
5. Lấy PID bằng `UniqueProcessId`, lấy `NAME` từ `ImageName` (`UNICODE_STRING`).
6. Gọi `OpenProcess(pid)` để đọc `ARCH` và `CMD`.
7. In `PID | NAME | ARCH | CMD`, với PID được ép kiểu từ `p->UniqueProcessId`.
8. Tới record kế tiếp bằng phép cộng địa chỉ: `p = (SPI *)((BYTE *)p + p->NextEntryOffset)`.
9. Dừng khi `NextEntryOffset == 0`.

Đoạn code quan trọng:

```c
NtQSI(5, NULL, 0, &len);
BYTE *buf = malloc(len + 65536);
NtQSI(5, buf, len + 65536, &len);

SPI *p = (SPI *)buf;
while (1) {
    DWORD pid = (DWORD)(ULONG_PTR)p->UniqueProcessId;
    /* lay NAME tu ImageName, mo handle de lay ARCH/CMD */
    if (!p->NextEntryOffset) break;
    p = (SPI *)((BYTE *)p + p->NextEntryOffset);
}
```

Lệnh in trong **phiên bản đã bổ sung PID**:

```c
printf("%-7lu %-30s %-6s %s\n",
       (unsigned long)pid,
       name,
       h ? arch(h) : "?",
       cmd ? cmd : "?");
```

`UniqueProcessId` có kiểu tương thích với handle/pointer nên mã dùng `(DWORD)(ULONG_PTR)` để lấy PID, sau đó `(unsigned long)` khi đưa vào `printf` với định dạng `%lu`.

**Tại sao cộng `NextEntryOffset`?** `SYSTEM_PROCESS_INFORMATION` trả về chuỗi record có kích thước thay đổi (ví dụ tùy thông tin thread). Vì thế, không thể giả định `p++` luôn nhảy đến record tiếp theo; phải dùng offset do hệ thống cung cấp.

### Điểm cần chú ý

- `SPI` trong `NtQuery.c` là **struct rút gọn** được người viết đặt tên, gồm phần đầu của `SYSTEM_PROCESS_INFORMATION` đủ cho các trường mà chương trình đọc; nó không định nghĩa đầy đủ mọi trường.
- Với process hệ thống không mở được handle, **tên vẫn có thể được lấy từ buffer**, còn `ARCH`/`CMD` là `?`.
- Kích thước process list có thể thay đổi giữa hai lần gọi. Code gốc chỉ cấp thêm **64 KiB** dự phòng và **không có vòng lặp retry** khi buffer tiếp tục thiếu; đây là một giới hạn thực tế.
- Native API có đặc điểm giao diện/structure không ổn định bằng Win32 API ở một số phiên bản Windows; tránh dùng các giá trị tự định nghĩa chưa kiểm tra trong sản phẩm production.


## 6. WTS — `WTSEnumerateProcessesW`

**File:** `WTSEnumerateProcessesW.c` · **Nhóm:** Windows Terminal Services / Remote Desktop Services
### Cơ chế hoạt động

WTS có thể liệt kê process gắn với các session Windows. Source dùng bản hàm `...W` (**Unicode/UTF-16**) và truy vấn trên máy hiện tại:

```c
PWTS_PROCESS_INFOW items = NULL;
DWORD count = 0;

if (!WTSEnumerateProcessesW(
        WTS_CURRENT_SERVER_HANDLE, 0, 1, &items, &count)) {
    return 1;
}
```

- `WTS_CURRENT_SERVER_HANDLE`: truy vấn server hiện tại, không phải tự kết nối máy từ xa.
- `0` (`Reserved`): giá trị bắt buộc.
- `1` (`Version`): version của API này.
- `items`: con trỏ tới mảng `WTS_PROCESS_INFOW` trả về.
- `count`: số phần tử.

Sau đó chương trình duyệt các phần tử:

1. Đọc `items[i].ProcessId` và `items[i].pProcessName`.
2. `OpenProcess()` với PID.
3. `print_process()` chuyển `NAME` từ UTF-16 sang UTF-8, gọi `arch()` và `cmdline()`.
4. `CloseHandle(h)` sau khi xử lý từng process.
5. **Quan trọng:** giải phóng mảng WTS bằng `WTSFreeMemory(items)`, không dùng `free(items)`.

### Điểm cần chú ý

- API WTS thường cung cấp `SessionId`, `ProcessId`, tên process và SID người dùng; **không có sẵn CMD**.
- Muốn liệt kê process thuộc các session/người dùng khác có thể cần quyền Administrator; kết quả phụ thuộc quyền WTS của caller.
- Khác Tool Help, WTS rất phù hợp khi muốn phân tích **session/user context**; bản code hiện tại mới in `PID | NAME | ARCH | CMD`.


## 7. Native API — `NtGetNextProcess`

**File:** `NtGetNextProcess.c` · **Nhóm:** NT Native API, `ntdll.dll`
### Cơ chế hoạt động

Đây là phương pháp khác rõ rệt: thay vì lấy **mảng PID** hoặc **snapshot**, `NtGetNextProcess` trả về **handle của process tiếp theo mà caller được phép mở**.

Source lấy địa chỉ API từ `ntdll.dll`:

```c
NtGNP_t NtGetNextProcess_ = (NtGNP_t)GetProcAddress(
    GetModuleHandleA("ntdll.dll"), "NtGetNextProcess"
);
```

Cú pháp:

```c
NTSTATUS NtGetNextProcess(
    HANDLE ProcessHandle,
    ACCESS_MASK DesiredAccess,
    ULONG HandleAttributes,
    ULONG Flags,
    PHANDLE NewProcessHandle
);
```

Trong file, chương trình bắt đầu với `previous = NULL`, sau đó lặp:

```c
while ((status = NtGetNextProcess_(
    previous,
    PROCESS_QUERY_LIMITED_INFORMATION,
    0, 0, &next)) >= 0) {

    if (previous) CloseHandle(previous);
    /* su dung handle next de doc thong tin */
    previous = next;
    next = NULL;
}
if (previous) CloseHandle(previous);
```

Luồng thực hiện:

1. `previous = NULL` yêu cầu process đầu tiên có thể mở.
2. API trả handle trong `next` theo access mask đã yêu cầu.
3. Chương trình đóng handle trước đó; handle mới sẽ dùng để truy vấn thông tin.
4. Dùng `QueryFullProcessImageNameW(next, ...)` lấy đường dẫn image, rồi cắt phần tên bằng `wcsrchr(path, L'\\')`.
5. Dùng `GetProcessId(next)` lấy PID, `arch(next)` để lấy kiến trúc và `cmdline(next)` để lấy CMD.
6. Gán `previous = next` để tiếp tục vòng lặp.
7. Khi API không trả thêm process, vòng lặp dừng. `STATUS_NO_MORE_ENTRIES = 0x8000001A` là trạng thái hoàn tất thường gặp; nếu trạng thái khác, code ghi cảnh báo.

### Điểm cần chú ý

- **Không cần `OpenProcess(pid)` trong vòng lặp** vì API đã trả về process handle.
- API **bỏ qua** process không đáp ứng quyền `PROCESS_QUERY_LIMITED_INFORMATION`, nên không nên dùng kết quả làm danh sách tuyệt đối đầy đủ.
- Phải đóng handle đúng cách: nếu quên `CloseHandle()`, chương trình sẽ rò rỉ tài nguyên.
- Đây là Native API không phải lựa chọn Win32 công khai phổ thông; nên coi code là ví dụ nghiên cứu Windows internals.


## 8. WTS — `WTSEnumerateProcessesExW` (nạp API động)

**File:** `WTSEnumerateProcessesExW.c`. Bản mã đang phân tích gọi WTS Ex thông qua `LoadLibraryW()` và `GetProcAddress()`, đồng thời khai báo cấu trúc nhận kết quả cục bộ vì một số bộ header MinGW thiếu khai báo.

### 8.1. Nạp hàm từ `wtsapi32.dll` tại runtime

Các thao tác quan trọng trong phiên bản mới:

```c
HMODULE dll = LoadLibraryW(L"wtsapi32.dll");

WtsEnumExW_t enumEx = (WtsEnumExW_t)GetProcAddress(
    dll, "WTSEnumerateProcessesExW");

WtsFreeExW_t freeEx = (WtsFreeExW_t)GetProcAddress(
    dll, "WTSFreeMemoryExW");
```

1. `LoadLibraryW()` nạp `wtsapi32.dll` vào process hiện tại.
2. `GetProcAddress()` tra cứu hai hàm export theo tên và trả về địa chỉ hàm.
3. Nếu nạp DLL hoặc tìm symbol thất bại, chương trình báo lỗi và kết thúc, thay vì gọi con trỏ hàm `NULL`.
4. Sau khi hoàn tất truy vấn, gọi `FreeLibrary(dll)` để giải phóng tham chiếu module.

**Khác với WTS thường:** `WTSEnumerateProcessesW.c` liên kết trực tiếp qua `-lwtsapi32`. Bản WTS Ex đã sửa chỉ cần các thư viện mặc định của Windows/MinGW vì địa chỉ hàm WTS được giải quyết **lúc chạy**. Đây vẫn là cùng API Windows, **không phải một cơ chế liệt kê kernel mới**.

### 8.2. Khai báo cấu trúc và tham số session

Bản sửa định nghĩa cấu trúc tên riêng `MY_WTS_PROCESS_INFO_EXW` với bố cục tương ứng `WTS_PROCESS_INFO_EXW` của Windows SDK:

```c
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
```

`pProcessName` là chuỗi UTF-16; `ProcessId` cung cấp PID. Ngoài ra, record còn có `SessionId`, SID, số thread, số handle, bộ nhớ và thời gian CPU. **Mã hiện tại chỉ in `PID | NAME | ARCH | CMD`**, không in những trường mở rộng khác.

Mã gọi hàm theo cách:

```c
DWORD level = 1, count = 0;
LPWSTR buffer = NULL;
const DWORD ANY_SESSION = (DWORD)-2; /* WTS_ANY_SESSION */

if (!enumEx(NULL, &level, ANY_SESSION, &buffer, &count)) {
    /* bao loi va giai phong DLL */
}
```

- `NULL` ở tham số thứ nhất tương ứng handle cho server hiện tại (`WTS_CURRENT_SERVER_HANDLE`).
- `level = 1` yêu cầu mảng các record mở rộng `WTS_PROCESS_INFO_EXW`.
- `(DWORD)-2` tương đương `WTS_ANY_SESSION` để yêu cầu tất cả session mà caller được phép truy vấn.
- `buffer` nhận mảng record; `count` là số record.
- Tài khoản gọi API có thể phải có quyền Administrator để xem process thuộc session của người dùng khác.

### 8.3. Lấy NAME, ARCH và CMD

Sau khi thành công, chương trình ép kiểu `buffer` thành mảng cấu trúc cục bộ và lặp qua từng process:

```c
MY_WTS_PROCESS_INFO_EXW *items = (MY_WTS_PROCESS_INFO_EXW *)buffer;

for (DWORD i = 0; i < count; ++i) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, items[i].ProcessId);
    char *name = utf8(items[i].pProcessName, -1);
    char *cmd = cmdline(h);

    /* In PID, NAME, arch(h), CMD; sau do free va CloseHandle */
}
```

- **PID:** trực tiếp từ `items[i].ProcessId`.
- **NAME:** trực tiếp từ `items[i].pProcessName`, chuyển UTF-16 sang UTF-8 bằng `WideCharToMultiByte` thông qua helper `utf8()`.
- **ARCH:** `OpenProcess()` lấy handle, `arch(h)` dùng `IsWow64Process2()`.
- **CMD:** `cmdline(h)` dùng `NtQueryInformationProcess(60)` và chuyển về UTF-8.
- Khi thiếu quyền mở handle, vẫn có thể in PID/NAME đã liệt kê, còn ARCH/CMD hiển thị `?`.

### 8.4. Giải phóng tài nguyên đúng API

Vì bộ nhớ do WTS API cấp phát, **không dùng `free(buffer)`**. Bản sửa lấy địa chỉ `WTSFreeMemoryExW()` bằng `GetProcAddress()` và giải phóng như sau:

```c
if (buffer && !freeEx(1 /* WTSTypeProcessInfoLevel1 */, buffer, count))
    fprintf(stderr, "WTSFreeMemoryExW failed\n");

FreeLibrary(dll);
```

- `freeEx(1, buffer, count)` tương ứng `WTSFreeMemoryExW(WTSTypeProcessInfoLevel1, buffer, count)`.
- `free(name)` và `free(cmd)` giải phóng chuỗi do helper cấp phát.
- `CloseHandle(h)` đóng process handle nếu mở thành công.
- `FreeLibrary(dll)` đóng tham chiếu đến `wtsapi32.dll` **sau khi dùng xong các function pointer**.

### 8.5. So sánh WTS thường và WTS Ex

| Tiêu chí | `WTSEnumerateProcessesW.c` | `WTSEnumerateProcessesExW.c` (đã sửa) |
| --- | --- | --- |
| Giao diện liệt kê | `WTSEnumerateProcessesW()` | `WTSEnumerateProcessesExW()` |
| Liên kết | Trực tiếp, `-lwtsapi32` | Runtime, `LoadLibraryW()` + `GetProcAddress()` |
| Record nhận về | `WTS_PROCESS_INFOW` | `MY_WTS_PROCESS_INFO_EXW` tương thích `WTS_PROCESS_INFO_EXW` |
| Thông tin cơ bản | PID, NAME, SessionId, SID | Các trường cơ bản cùng thông tin threads, handles, bộ nhớ và CPU |
| Lọc session | Không có tham số SessionId | Tham số SessionId, dùng `WTS_ANY_SESSION` |
| Giải phóng kết quả | `WTSFreeMemory()` | `WTSFreeMemoryExW()` (nạp động) |
| Lấy ARCH/CMD | Mở handle + helper chung | Mở handle + helper chung |

**Giới hạn:** Nạp hàm động chỉ khắc phục sự thiếu hụt khai báo ở header; `wtsapi32.dll` trên Windows vẫn phải export các API được yêu cầu.


## 9. WMI — `Win32_Process`

**File:** `WMI.c` · **Nhóm:** Windows Management Instrumentation (WMI) qua COM
### Cơ chế hoạt động

Đây là cách tiếp cận ở mức **quản trị hệ thống**. Khác những API phía trên, code không tự lấy command line bằng class 60: nó yêu cầu WMI trả về các property của process bằng **WQL (WMI Query Language)**.

Truy vấn trong mã:

```sql
SELECT ProcessId, Name, CommandLine FROM Win32_Process
```

Các bước trong `WMI.c`:

1. **`CoInitializeEx(NULL, COINIT_MULTITHREADED)`**: khởi tạo COM trong MTA.
2. **`CoInitializeSecurity(...)`**: đặt cơ chế bảo mật COM cho phiên truy vấn.
3. **`CoCreateInstance(CLSID_WbemLocator, ...)`**: tạo đối tượng `IWbemLocator`.
4. **`IWbemLocator_ConnectServer()`**: kết nối namespace `ROOT\\CIMV2` của WMI trên máy hiện tại.
5. **`CoSetProxyBlanket()`**: cấu hình mức xác thực và impersonation cho proxy `IWbemServices`.
6. **`IWbemServices_ExecQuery(..., L"WQL", sql, ...)`**: chạy câu truy vấn `Win32_Process`.
7. **`IEnumWbemClassObject_Next()`**: lấy lần lượt các object process từ tập kết quả.
8. **`IWbemClassObject_Get()`**: lấy ba property `ProcessId`, `Name`, `CommandLine` (qua `VARIANT`).
9. Mở handle bằng PID **chỉ để lấy ARCH** qua `IsWow64Process2()`.
10. `VariantClear()`, `Release()` cho các COM object, `SysFreeString()` cho BSTR, `CoUninitialize()` để dọn tài nguyên.

### Đọc thuộc tính WMI

| Property | Kiểu giá trị | Dùng để |
| --- | --- | --- |
| `ProcessId` | `uint32` | PID của process |
| `Name` | `string` | Tên process |
| `CommandLine` | `string` hoặc NULL | CMD (nếu có thể cung cấp) |

Source chuyển `BSTR` (Unicode) thành UTF-8 để in ra console; cột CMD được lấy **trực tiếp từ WMI**, nên `WMI.c` **không cần** khai báo hoặc dùng `NtQueryInformationProcess`.

### Điểm cần chú ý

- WMI thuận tiện nếu muốn lấy thêm `ParentProcessId`, `ExecutablePath`, `CreationDate`, v.v. bằng cách sửa WQL.
- Thường có nhiều overhead hơn việc chỉ gọi một Win32/NT API liệt kê process, vì phải khởi tạo COM, kết nối provider và tạo object kết quả.
- WMI có thể trả `CommandLine = NULL` khi không có thông tin hoặc thiếu quyền.
- Nếu dịch vụ/provider WMI gặp sự cố, toàn bộ câu truy vấn có thể thất bại; source ghi mã lỗi HRESULT ở dạng hex.


## 10. Native API — `SystemBasicProcessInformation (252)`

**File:** `SystemBasicProcessInformation.c` · **Nhóm:** `NtQuerySystemInformation` với information class mới
**Yêu cầu runtime:** Windows 11 **build 26100.4770 hoặc mới hơn**. Trên Windows không hỗ trợ class này, chương trình có thể biên dịch thành công nhưng **chạy không thành công**.

### Cơ chế hoạt động

Hàm chính vẫn là `NtQuerySystemInformation`, nhưng sử dụng **class 252** (`SystemBasicProcessInformation`) thay vì class 5 ở `NtQuery.c`.

```c
status = NtQSI(
    252,     // SystemBasicProcessInformation
    buf,
    size,
    &required
);
```

Các bước:

1. Lấy địa chỉ `NtQuerySystemInformation` từ `ntdll.dll`.
2. Bắt đầu với buffer 64 KiB.
3. Gọi `NtQuerySystemInformation(252, ...)` để lấy tập record **`SYSTEM_BASICPROCESS_INFORMATION`**.
4. Nếu thiếu buffer (`STATUS_INFO_LENGTH_MISMATCH` hoặc `STATUS_BUFFER_TOO_SMALL`), tăng buffer rồi thử lại. Source hiện có tối đa 10 lần thử.
5. Với từng record, lấy `UniqueProcessId` làm PID, `ImageName` làm NAME.
6. Mở process handle theo PID, gọi `arch()` và `cmdline()`.
7. Nhảy sang record tiếp theo theo `NextEntryOffset`; dừng ở record cuối.
8. Giải phóng buffer khi hoàn tất.

Struct rút gọn trong source:

```c
typedef struct {
    ULONG NextEntryOffset;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG64 SequenceNumber;
    UNICODE_STRING ImageName;
} BASIC_PROCESS_INFO;
```

### Khác gì với `SystemProcessInformation (5)`?

| Đặc điểm | `SystemProcessInformation (5)` | `SystemBasicProcessInformation (252)` |
| --- | --- | --- |
| API gọi | `NtQuerySystemInformation()` | Cùng API |
| Dữ liệu | Thông tin process kèm các thống kê chi tiết | Thông tin cơ bản về process |
| Nhận dạng | PID, tên và dữ liệu như thông tin thread/timing | PID, tên, `SequenceNumber` |
| Bộ nhớ và chi phí | Thường nhiều dữ liệu hơn | Theo tài liệu Microsoft, nhẹ hơn và không cần đồng bộ timing data |
| Tương thích | Rộng hơn | Windows 11 build 26100.4770+ |
| Nguồn CMD | `NtQueryInformationProcess(60)` | Cùng API để lấy CMD |

**`SequenceNumber` để làm gì?** PID có thể được hệ điều hành tái sử dụng sau khi process cũ kết thúc. Sequence number là giá trị định danh duy nhất theo lần tạo process, giúp phân biệt hai process khác nhau có thể lần lượt dùng **cùng PID**. Code hiện tại không in trường này, nhưng có thể mở rộng để nghiên cứu theo thời gian.

## 11. Giới hạn kỹ thuật và quản lý tài nguyên

### 11.1. Những giới hạn thực tế cần biết

1. **Quyền truy cập:** một process có thể xuất hiện trong danh sách nhưng `OpenProcess()` thất bại, khiến ARCH/CMD là `?`. Protected process/PPL có thể bị chặn dù chạy bằng Administrator.
2. **Process race:** process có thể thoát sau khi được liệt kê và trước khi chương trình mở handle/truy vấn thông tin.
3. **Danh sách có thể khác nhau:** snapshot, WMI và các API trả danh sách tại những thời điểm khác nhau; kết quả không nhất thiết có cùng số process.
4. **Buffer:** `enumprocess.c` dùng mảng cố định 4096 PID; `NtQuery.c` dùng một lần cấp phát với phần dư 64 KiB. Những cách này có thể thiếu record/thất bại khi số process biến động mạnh.
5. **Encoding:** phần lớn file mới dùng Unicode `W` + chuyển sang UTF-8; `enumprocess.c` dùng `QueryFullProcessImageNameA()`, nên tên Unicode có thể phụ thuộc code page.
6. **Architecture:** `IsWow64Process2` được dùng theo mapping x86/x64/ARM64 đơn giản. Không bảo đảm mọi tình huống kiến trúc và giả lập đều được mô tả đầy đủ.
7. **Command line:** có thể không khả dụng, bị thay đổi bởi chính process hoặc không phản ánh đầy đủ dữ liệu đã được ứng dụng xử lý từ tham số. Nên coi đây là dữ liệu để quan sát, không phải nguồn xác thực tuyệt đối.
8. **Độc lập của phương pháp:** dùng hai API khác nhau không đồng nghĩa đi qua hai nguồn dữ liệu kernel tách biệt. Mục tiêu repository là so sánh **giao diện lập trình và cách thực hiện enumeration**.

### 11.2. Các memory/resource cleanup quan trọng

| Tài nguyên | API giải phóng |
| --- | --- |
| `OpenProcess()` / `NtGetNextProcess()` handle | `CloseHandle()` |
| Snapshot handle | `CloseHandle()` |
| Bộ nhớ tạo bởi `malloc/realloc` | `free()` |
| Buffer từ `WTSEnumerateProcessesW` | `WTSFreeMemory()` |
| Buffer từ `WTSEnumerateProcessesExW` (level 1) | `WTSFreeMemoryExW(WTSTypeProcessInfoLevel1, ...)` |
| COM object / enumerator WMI | `Release()` |
| WMI `VARIANT` và `BSTR` | `VariantClear()` và `SysFreeString()` |
| Khởi tạo COM | `CoUninitialize()` |

## 12. Tài liệu tham khảo
- Microsoft - [Process Enumeration](https://learn.microsoft.com/en-us/windows/win32/procthread/process-enumeration)
- Microsoft - [EnumProcesses](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocesses)
- Microsoft - [CreateToolhelp32Snapshot](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-createtoolhelp32snapshot)
- Microsoft - [NtQuerySystemInformation (bao gồm SystemBasicProcessInformation)](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation)
- Microsoft - [WTSEnumerateProcessesW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsenumerateprocessesw)
- Microsoft - [WTSEnumerateProcessesExW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsenumerateprocessesexw)
- Microsoft - [WTS_PROCESS_INFO_EXW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/ns-wtsapi32-wts_process_info_exw)
- Microsoft - [WTSFreeMemoryExW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsfreememoryexw)
- Microsoft - [GetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)
- Microsoft - [LoadLibraryW](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryw)
- Microsoft - [Win32_Process](https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-process)
- Microsoft - [IsWow64Process2](https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-iswow64process2)
- NtDoc - [NtGetNextProcess](https://ntdoc.m417z.com/ntgetnextprocess)
- NtDoc - [ProcessCommandLineInformation (60)](https://ntdoc.m417z.com/processinfoclass)

