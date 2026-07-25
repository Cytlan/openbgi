/*
 * Simple DLL Injector
 * Usage: dll_injector.exe <path_to_dll> <path_to_target_exe>
 *
 * Creates the target process in suspended state, injects the DLL
 * via remote LoadLibrary, waits for DllMain to execute, then resumes
 * the main thread so the DLL runs before the original process code starts.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        printf("Usage: %s <dll_path> <exe_path>\n", argv[0]);
        printf("Example: %s C:\\hook.dll C:\\Windows\\System32\\notepad.exe\n", argv[0]);
        return 1;
    }

    const char* dllPath = argv[1];
    const char* exePath = argv[2];

    printf("[*] DLL to inject: %s\n", dllPath);
    printf("[*] Target executable: %s\n", exePath);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    // Create the target process in SUSPENDED state
    // This is key: the main thread won't start executing until we resume it
    if (!CreateProcessA(
            exePath,                    // lpApplicationName
            NULL,                       // lpCommandLine (NULL = use application name)
            NULL,                       // lpProcessAttributes
            NULL,                       // lpThreadAttributes
            FALSE,                      // bInheritHandles
            CREATE_SUSPENDED,           // dwCreationFlags - SUSPENDED is critical
            NULL,                       // lpEnvironment
            NULL,                       // lpCurrentDirectory
            &si,                        // lpStartupInfo
            &pi                         // lpProcessInformation
    )) {
        printf("[-] CreateProcessA failed. Error: %lu\n", GetLastError());
        return 1;
    }

    printf("[+] Process created successfully (PID: %lu)\n", pi.dwProcessId);
    printf("[*] Main thread is suspended. Ready to inject DLL...\n");

    // === DLL Injection using CreateRemoteThread + LoadLibraryA ===

    // 1. Allocate memory in the target process for the DLL path string
    SIZE_T dllPathSize = strlen(dllPath) + 1;  // +1 for null terminator
    LPVOID pRemoteDllPath = VirtualAllocEx(
        pi.hProcess,
        NULL,
        dllPathSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );

    if (!pRemoteDllPath) {
        printf("[-] VirtualAllocEx failed. Error: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("[+] Allocated %zu bytes in target process at 0x%p\n", dllPathSize, pRemoteDllPath);

    // 2. Write the DLL path into the allocated memory
    if (!WriteProcessMemory(
            pi.hProcess,
            pRemoteDllPath,
            dllPath,
            dllPathSize,
            NULL
    )) {
        printf("[-] WriteProcessMemory failed. Error: %lu\n", GetLastError());
        VirtualFreeEx(pi.hProcess, pRemoteDllPath, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("[+] Wrote DLL path to remote memory\n");

    // 3. Get the address of LoadLibraryA in kernel32.dll (same in all processes)
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        printf("[-] GetModuleHandleA failed. Error: %lu\n", GetLastError());
        VirtualFreeEx(pi.hProcess, pRemoteDllPath, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    LPTHREAD_START_ROUTINE pLoadLibraryA = (LPTHREAD_START_ROUTINE)GetProcAddress(
        hKernel32,
        "LoadLibraryA"
    );

    if (!pLoadLibraryA) {
        printf("[-] GetProcAddress for LoadLibraryA failed. Error: %lu\n", GetLastError());
        VirtualFreeEx(pi.hProcess, pRemoteDllPath, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("[+] Found LoadLibraryA at 0x%p in remote process\n", (void*)pLoadLibraryA);

    // 4. Create a remote thread that calls LoadLibraryA(dllPath)
    //    This will load our DLL and execute its DllMain while the main thread is still suspended
    HANDLE hRemoteThread = CreateRemoteThread(
        pi.hProcess,
        NULL,                       // default security
        0,                          // default stack size
        pLoadLibraryA,              // start address (LoadLibraryA)
        pRemoteDllPath,             // parameter (pointer to DLL path)
        0,                          // creation flags
        NULL                        // thread id (not needed)
    );

    if (!hRemoteThread) {
        printf("[-] CreateRemoteThread failed. Error: %lu\n", GetLastError());
        VirtualFreeEx(pi.hProcess, pRemoteDllPath, 0, MEM_RELEASE);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("[*] Remote thread created. Waiting for DLL to load (DllMain to execute)...\n");

    // 5. Wait for the remote thread to finish (i.e., LoadLibraryA returns)
    WaitForSingleObject(hRemoteThread, INFINITE);

    // Check if LoadLibrary succeeded (non-zero return value = module handle)
    // Note: On 64-bit, the full HMODULE may be truncated in DWORD exit code.
    // We only check for success/failure here.
    DWORD loadLibraryResult = 0;
    GetExitCodeThread(hRemoteThread, &loadLibraryResult);

    if (loadLibraryResult == 0) {
        printf("[-] LoadLibraryA failed in remote process (returned NULL). DLL may not exist or failed to load.\n");
        // Continue anyway or exit? For this simple injector we'll proceed to resume
    } else {
        printf("[+] DLL loaded successfully (LoadLibraryA returned non-NULL).\n");
    }

    CloseHandle(hRemoteThread);

    // Optional: free the memory used for the path string (not strictly required after load)
    VirtualFreeEx(pi.hProcess, pRemoteDllPath, 0, MEM_RELEASE);

    // 6. Resume the main thread of the target process
    //    Now the original process code will start executing, but our DLL's DllMain
    //    has already run because we injected while the thread was suspended.
    DWORD resumeResult = ResumeThread(pi.hThread);
    if (resumeResult == (DWORD)-1) {
        printf("[-] ResumeThread failed. Error: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    printf("[+] Main thread resumed (suspend count was %lu)\n", resumeResult);

    // Clean up handles (process continues running)
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    printf("\n[+] Injector finished successfully.\n");
    return 0;
}
