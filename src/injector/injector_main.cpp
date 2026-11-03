#include <Windows.h>
#include <Psapi.h>
#include <algorithm>
#include <cwchar>
#include <string>
#include <iostream>
#include <vector>

namespace {

// Full long-form path, so a short (8.3) or relative spelling compares equal to the loader's path.
std::wstring normalizePath(const std::wstring &path) {
    std::wstring full(MAX_PATH, L'\0');
    DWORD len = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (len >= full.size()) {
        full.resize(len);
        len = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    }
    if (len == 0 || len >= full.size()) {
        return path;
    }
    full.resize(len);
    std::wstring longPath(MAX_PATH, L'\0');
    len = GetLongPathNameW(full.c_str(), longPath.data(), static_cast<DWORD>(longPath.size()));
    if (len >= longPath.size()) {
        longPath.resize(len);
        len = GetLongPathNameW(full.c_str(), longPath.data(), static_cast<DWORD>(longPath.size()));
    }
    if (len == 0 || len >= longPath.size()) {
        return full;
    }
    longPath.resize(len);
    return longPath;
}

// The remote thread's exit code is a DWORD, so on x64 it holds only the low 32 bits of the HMODULE that
// LoadLibraryW returned; a module based on a 4 GB boundary would read as 0. Ask the target instead.
bool isModuleLoaded(HANDLE process, const std::wstring &dllPath) {
    std::vector<HMODULE> modules(512);
    DWORD needed = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const DWORD capacity = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (!EnumProcessModulesEx(process, modules.data(), capacity, &needed, LIST_MODULES_ALL)) {
            return false;
        }
        if (needed <= capacity) {
            break;
        }
        modules.resize(needed / sizeof(HMODULE) + 16);
    }
    const std::size_t count = (std::min)(modules.size(), static_cast<std::size_t>(needed / sizeof(HMODULE)));
    const std::wstring want = normalizePath(dllPath);
    std::wstring name(32768, L'\0');
    for (std::size_t i = 0; i < count; ++i) {
        const DWORD len = GetModuleFileNameExW(process, modules[i], name.data(), static_cast<DWORD>(name.size()));
        if (len == 0) {
            continue;
        }
        if (_wcsicmp(normalizePath(std::wstring(name.data(), len)).c_str(), want.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

int wmain_impl(int argc, wchar_t **argv) {
    if (argc < 3) {
        std::wcerr << L"Usage: krkr_injector.exe <pid> <dll_path>\n";
        return 2;
    }
    DWORD pid = std::wcstoul(argv[1], nullptr, 10);
    std::wstring dllPath = argv[2];
    if (pid == 0 || dllPath.empty()) {
        std::wcerr << L"Invalid arguments\n";
        return 2;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                     PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return 3;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMemory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMemory) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        CloseHandle(process);
        return 4;
    }

    if (!WriteProcessMemory(process, remoteMemory, dllPath.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel, "LoadLibraryW"));
    if (!loadLibrary) {
        std::wcerr << L"GetProcAddress LoadLibraryW failed\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 6;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remoteMemory, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        CloseHandle(process);
        return 7;
    }

    // Keep in sync with the controller, which waits longer than this for the injector itself.
    DWORD waitResult = WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);

    CloseHandle(thread);
    if (waitResult == WAIT_OBJECT_0) {
        VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
    }
    // Not freed otherwise: LoadLibraryW may still be reading the path (e.g. blocked on the loader lock),
    // and freeing it now would make the target read freed memory, so the few bytes are leaked instead.

    const bool loaded = (waitResult == WAIT_OBJECT_0) && (exitCode != 0 || isModuleLoaded(process, dllPath));
    CloseHandle(process);

    if (waitResult == WAIT_TIMEOUT) return 8;
    if (waitResult == WAIT_FAILED) return 9;
    if (!loaded) return 10;
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    return wmain_impl(argc, argv);
}
