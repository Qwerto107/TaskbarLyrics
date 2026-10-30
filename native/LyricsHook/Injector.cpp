// 注入目标必须是拥有 DesktopLyrics 窗口的 64 位 cloudmusic.exe。
#include <windows.h>
#include <string>
#include <cstdio>
#include <tlhelp32.h>

// 按目标进程的模块基址重定位，不能假设两个进程的 LoadLibraryW 地址相同。
static LPTHREAD_START_ROUTINE RemoteLoadLibrary(DWORD pid) {
    const auto local = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(local), &owner)) return nullptr;
    WCHAR name[MAX_PATH]{};
    if (!GetModuleFileNameW(owner, name, MAX_PATH)) return nullptr;
    const auto baseName = wcsrchr(name, L'\\') + 1;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    LPTHREAD_START_ROUTINE result = nullptr;
    if (Module32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szModule, baseName) == 0) {
            result = reinterpret_cast<LPTHREAD_START_ROUTINE>(entry.modBaseAddr +
                (reinterpret_cast<BYTE*>(local) - reinterpret_cast<BYTE*>(owner)));
            break;
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}

static BOOL CALLBACK FindWindowInProcess(HWND window, LPARAM value) {
    auto* args = reinterpret_cast<DWORD*>(value);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    WCHAR name[64]{};
    GetClassNameW(window, name, 64);
    if (pid == args[0] && wcscmp(name, L"DesktopLyrics") == 0) args[1] = 1;
    return TRUE;
}

int wmain(int argc, WCHAR** argv) {
    if (argc != 3) return 1;
    DWORD args[]{wcstoul(argv[1], nullptr, 10), 0};
    EnumWindows(FindWindowInProcess, reinterpret_cast<LPARAM>(args));
    if (!args[0] || !args[1]) return 2;
    const HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, args[0]);
    if (!process) return 3;
    WCHAR image[32768]{};
    DWORD length = 32768;
    BOOL wow64 = TRUE;
    const bool valid = QueryFullProcessImageNameW(process, 0, image, &length) &&
        IsWow64Process(process, &wow64) && !wow64 &&
        _wcsicmp(wcsrchr(image, L'\\') ? wcsrchr(image, L'\\') + 1 : image, L"cloudmusic.exe") == 0;
    if (!valid) { CloseHandle(process); return 4; }
    WCHAR dll[32768]{};
    if (!GetFullPathNameW(argv[2], 32768, dll, nullptr) || GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(process); return 5;
    }
    const SIZE_T bytes = (wcslen(dll) + 1) * sizeof(WCHAR);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(process, remote, dll, bytes, nullptr)) {
        if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process); return 6;
    }
    const auto load = RemoteLoadLibrary(args[0]);
    if (!load) { VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return 9; }
    const HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr);
    if (!thread) { VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return 7; }
    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD loaded = 0;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeThread(thread, &loaded);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    }
    // 超时不能释放参数：远端线程仍可能正在读取 DLL 路径。
    CloseHandle(thread);
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0 || !loaded)
        fwprintf(stderr, L"LoadLibraryW: wait=%lu, result=0x%08lX\n", wait, loaded);
    return wait == WAIT_OBJECT_0 && loaded ? 0 : 8;
}
