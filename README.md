# Windows Process Enumeration

Project nghiên cứu **8 cách liệt kê tiến trình trên Windows bằng C**, sử dụng PSAPI, Tool Help, NT Native API, Windows Terminal Services (WTS) và WMI.

Mỗi chương trình hướng tới việc hiển thị bốn trường:

| Trường | Ý nghĩa |
| --- | --- |
| `PID` | Mã định danh tiến trình |
| `NAME` | Tên tiến trình (ví dụ `explorer.exe`) |
| `ARCH` | Kiến trúc: `x86`, `x64`, `ARM64` hoặc `?` |
| `CMD` | Chuỗi command line dùng khi chạy tiến trình (nếu đọc được) |

## Các phương pháp

| File C | Cơ chế liệt kê |
| --- | --- |
| `enumprocess.c` | PSAPI — `EnumProcesses` |
| `snapshot.c` | Tool Help — `CreateToolhelp32Snapshot`, `Process32First/Next` |
| `NtQuery.c` | `NtQuerySystemInformation(SystemProcessInformation = 5)` |
| `WTSEnumerateProcessesW.c` | WTS — `WTSEnumerateProcessesW` |
| `NtGetNextProcess.c` | Native API — `NtGetNextProcess` |
| `WTSEnumerateProcessesExW.c` | WTS mở rộng — `WTSEnumerateProcessesExW` (nạp động DLL) |
| `WMI.c` | WMI — truy vấn `Win32_Process` bằng WQL |
| `SystemBasicProcessInformation.c` | `NtQuerySystemInformation(SystemBasicProcessInformation = 252)` |

> Đây là 8 chương trình dùng các API hoặc biến thể khác nhau, không phải 8 nguồn dữ liệu kernel độc lập.

## Cấu trúc repository

```text
Windows-Process-Enumeration/
├── README.md
├── how_it_works.md
└── process/
    ├── enumprocess.c
    ├── snapshot.c
    ├── NtQuery.c
    ├── WTSEnumerateProcessesW.c
    ├── NtGetNextProcess.c
    ├── WTSEnumerateProcessesExW.c
    ├── WMI.c
    └── SystemBasicProcessInformation.c
```

## Biên dịch và chạy

Yêu cầu: **Windows 10/11** và **GCC cho Windows** (MinGW-w64, MSYS2 MinGW hoặc Strawberry GCC). Mở CMD trong thư mục repository, rồi chạy `cd process` trước khi biên dịch. Nếu file `.c` ở ngay thư mục gốc thì bỏ qua `cd process`.

```bat
cd process

gcc -DPSAPI_VERSION=1 enumprocess.c -o enumprocess.exe -lpsapi
gcc snapshot.c -o snapshot.exe
gcc NtQuery.c -o NtQuery.exe
gcc WTSEnumerateProcessesW.c -o wts.exe -lwtsapi32
gcc NtGetNextProcess.c -o next.exe
gcc WTSEnumerateProcessesExW.c -o wtsex.exe -Wall -Wextra
gcc WMI.c -o wmi.exe -lwbemuuid -lole32 -loleaut32
gcc SystemBasicProcessInformation.c -o basic.exe
```

Chạy một chương trình, ví dụ:

```bat
snapshot.exe
```

Ví dụ **định dạng đầu ra** (dữ liệu minh họa):

```text
PID     NAME                           ARCH   CMD
4820    explorer.exe                   x64    C:\Windows\Explorer.EXE
9352    notepad.exe                    x64    "C:\Windows\System32\notepad.exe" test.txt
4       System                         ?      ?
```

**Lưu ý:**

- `WTSEnumerateProcessesExW.c` trong repository cần là **bản đã sửa cho MinGW/Strawberry GCC**, dùng `LoadLibraryW` + `GetProcAddress`; vì thế lệnh build của file này **không cần** `-lwtsapi32`.
- `SystemBasicProcessInformation.c` yêu cầu **Windows 11 build 26100.4770 trở lên**; kiểm tra bằng `winver`.
- Một số tiến trình hệ thống hoặc protected process không thể đọc đầy đủ `ARCH`/`CMD`; chương trình sẽ hiển thị `?`. `enumprocess.c` hiện bỏ qua tiến trình khi `OpenProcess` thất bại.
- Các bản C trong repository mang mục đích học tập và nghiên cứu; chưa được xác nhận kiểm thử trên mọi phiên bản Windows.

## Giải thích kỹ thuật

Xem **[how_it_works.md](./how_it_works.md)** để tìm hiểu luồng xử lý của từng API, nguồn lấy `PID / NAME / ARCH / CMD`, cách quản lý buffer/handle và những giới hạn kỹ thuật.
