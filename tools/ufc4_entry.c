// SPDX-License-Identifier: GPL-2.0-or-later
/* Native entry point for the self-contained development launcher. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,LPWSTR args,int show) {
    (void)instance;(void)previous;(void)show;
    wchar_t root[32768],python[32768],command[32768];
    DWORD count=GetModuleFileNameW(NULL,root,32768);
    if (!count || count>=32768) return 2;
    wchar_t *slash=wcsrchr(root,L'\\');if (!slash)return 2;*slash=0;
    if (swprintf(python,32768,L"%ls\\python\\python.exe",root)<0 ||
        swprintf(command,32768,L"\"%ls\" -B \"%ls\\launcher.py\" %ls",python,root,args)<0) return 2;
    STARTUPINFOW si={.cb=sizeof(si)};PROCESS_INFORMATION process={0};
    if (!CreateProcessW(python,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,root,&si,&process)) {
        MessageBoxW(NULL,L"No se pudo iniciar Python. Conservá las carpetas python, scripts, tools y bin junto a UFC4.exe.",
            L"UFC 4 — prototipo de desarrollo",MB_ICONERROR);return 2;
    }
    WaitForSingleObject(process.hProcess,INFINITE);DWORD code=1;
    GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);CloseHandle(process.hThread);
    return (int)code;
}
