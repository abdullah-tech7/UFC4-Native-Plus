// SPDX-License-Identifier: GPL-2.0-or-later
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR args, int show) {
    (void)instance; (void)previous; (void)show;
    wchar_t root[32768], config[32768], selected[32768] = {0};
    wchar_t executable[32768], command[32768];
    DWORD n = GetModuleFileNameW(NULL, root, 32768);
    if (!n || n >= 32768) return 2;
    wchar_t *last = wcsrchr(root, L'\\');
    if (!last) return 2;
    *last = 0;
    if (!SetCurrentDirectoryW(root)) return 2;
#ifdef PORT_NHL24
    const wchar_t *title = L"NHL 24 - experimental community port";
    CreateDirectoryW(L"user", NULL);
    wcscpy(config, L"user\\game_path.txt");
    swprintf(executable, 32768, L"%ls\\nhl24.exe", root);
    if (swprintf(command, 32768, L"\"%ls\" %ls", executable, args) < 0) return 2;
#else
    const wchar_t *title = L"UFC 4 - experimental community port";
    wcscpy(config, L"game_path.txt");
    swprintf(executable, 32768, L"%ls\\python\\python.exe", root);
    if (swprintf(command, 32768, L"\"%ls\" -B \"%ls\\launcher.py\" %ls", executable, root, args) < 0) return 2;
#endif
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"Extrae el ZIP completo y conserva todas sus carpetas juntas.", title, MB_ICONERROR);
        return 2;
    }
    if (!wcscmp(args, L"--check")) return 0;
    if (!*args && GetFileAttributesW(config) == INVALID_FILE_ATTRIBUTES) {
        OPENFILENAMEW dialog = {0};
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrTitle = L"Selecciona eboot.bin de tu propia copia extraida del juego";
        dialog.lpstrFilter = L"Ejecutable de tu copia PS4 (eboot.bin)\0eboot.bin\0\0";
        dialog.lpstrFile = selected;
        dialog.nMaxFile = 32768;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return 1;
#ifndef PORT_NHL24
        last = wcsrchr(selected, L'\\');
        if (!last) return 2;
        *last = 0;
#endif
        char utf8[131072];
        int bytes = WideCharToMultiByte(CP_UTF8, 0, selected, -1, utf8, sizeof(utf8), NULL, NULL);
        if (bytes <= 0) return 2;
        utf8[bytes - 1] = '\n';
        HANDLE file = CreateFileW(config, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) return 2;
        DWORD written = 0;
        BOOL saved = WriteFile(file, utf8, (DWORD)bytes, &written, NULL);
        CloseHandle(file);
        if (!saved || written != (DWORD)bytes) return 2;
    }
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, root, &startup, &process)) {
        MessageBoxW(NULL, L"No se pudo iniciar el runtime. Comprueba que extrajiste todo el ZIP.", title, MB_ICONERROR);
        return 2;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
