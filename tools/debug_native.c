// SPDX-License-Identifier: GPL-2.0-or-later
/* Command-line debugger: report execute faults, then pass them to the runtime. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static void byte_at(HANDLE process,uintptr_t address,unsigned char value) {
    DWORD old=0,ignored=0; SIZE_T done=0;
    VirtualProtectEx(process,(void *)address,1,PAGE_EXECUTE_READWRITE,&old);
    WriteProcessMemory(process,(void *)address,&value,1,&done);
    FlushInstructionCache(process,(void *)address,1);
    VirtualProtectEx(process,(void *)address,1,old,&ignored);
}

static void memory(HANDLE process, uintptr_t address, size_t bytes) {
    unsigned char data[256]; SIZE_T got=0;
    if (bytes>sizeof(data)) bytes=sizeof(data);
    if (!ReadProcessMemory(process,(void *)address,data,bytes,&got)) return;
    printf("DEBUG memory %016llx (%llu bytes):",(unsigned long long)address,(unsigned long long)got);
    for (size_t i=0;i<got;i++) {
        if (!(i%16)) printf("\n  %016llx:",(unsigned long long)(address+i));
        printf(" %02x",data[i]);
    }
    puts("");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc!=2) return 2;
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
        .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
        .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process={0};
    if (!CreateProcessA(NULL,argv[1],NULL,NULL,TRUE,DEBUG_ONLY_THIS_PROCESS,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"DEBUG CreateProcess error %lu\n",GetLastError()); return 2;
    }
    DEBUG_EVENT event; unsigned execute_faults=0;
    uintptr_t create_site=0,dirty_site=0,exe_base=0,watched=0;
    unsigned char create_byte=0,dirty_byte=0; DWORD stepping=0,watch_thread=0; unsigned writes=0;
    for (;;) {
        if (!WaitForDebugEvent(&event,1000)) continue;
        DWORD continuation=DBG_CONTINUE;
        if (event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_RECORD *e=&event.u.Exception.ExceptionRecord;
            if (e->ExceptionCode!=EXCEPTION_BREAKPOINT) continuation=DBG_EXCEPTION_NOT_HANDLED;
            if (e->ExceptionCode==EXCEPTION_BREAKPOINT && (uintptr_t)e->ExceptionAddress==dirty_site) {
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_FULL|CONTEXT_DEBUG_REGISTERS};
                if (thread && GetThreadContext(thread,&c)) {
                    printf("DEBUG DirtyThread initial callback=%016llx record=%016llx\n",c.R12,c.Rbx);
                    watched=c.Rbx; watch_thread=event.dwThreadId;
                    byte_at(process.hProcess,dirty_site,dirty_byte);
                    c.Rip--; c.Dr0=watched; c.Dr7=0x90001; c.Dr6=0;
                    SetThreadContext(thread,&c);
                }
                if (thread) CloseHandle(thread);
                continuation=DBG_CONTINUE;
            } else if (e->ExceptionCode==EXCEPTION_SINGLE_STEP && event.dwThreadId==watch_thread) {
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_FULL|CONTEXT_DEBUG_REGISTERS};
                if (thread && GetThreadContext(thread,&c) && (c.Dr6&1)) {
                    printf("DEBUG watched write %u RIP=%016llx host-rva=%llx\n",++writes,c.Rip,c.Rip-exe_base);
                    memory(process.hProcess,watched,32); memory(process.hProcess,c.Rsp,80);
                    c.Dr6=0;
                    if (writes>=2) {c.Dr7=0; watch_thread=0;}
                    SetThreadContext(thread,&c); continuation=DBG_CONTINUE;
                }
                if (thread) CloseHandle(thread);
            }
            if (e->ExceptionCode==EXCEPTION_BREAKPOINT && (uintptr_t)e->ExceptionAddress==create_site) {
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_FULL};
                if (thread && GetThreadContext(thread,&c)) {
                    if (!dirty_byte) {
                        SIZE_T found=0;
                        ReadProcessMemory(process.hProcess,(void *)dirty_site,&dirty_byte,1,&found);
                        if (found && dirty_byte==0x4c) byte_at(process.hProcess,dirty_site,0xcc);
                    }
                    char name[64]={0}; SIZE_T got=0;
                    ReadProcessMemory(process.hProcess,(void *)c.R8,name,63,&got);
                    if (strstr(name,"NetLib") || strstr(name,"Socket")) {
                        printf("DEBUG create %s out=%016llx attr=%016llx entry=%016llx arg=%016llx\n",
                               name,c.Rdi,c.Rsi,c.Rdx,c.Rcx);
                        memory(process.hProcess,c.Rdi,32);
                        memory(process.hProcess,c.Rcx,128);
                    }
                    byte_at(process.hProcess,create_site,create_byte);
                    c.Rip--; c.EFlags|=0x100; SetThreadContext(thread,&c); stepping=event.dwThreadId;
                }
                if (thread) CloseHandle(thread);
                continuation=DBG_CONTINUE;
            } else if (e->ExceptionCode==EXCEPTION_SINGLE_STEP && event.dwThreadId==stepping) {
                byte_at(process.hProcess,create_site,0xcc); stepping=0; continuation=DBG_CONTINUE;
            }
            if (e->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && e->ExceptionInformation[0]==8 && execute_faults++<2) {
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_FULL};
                if (thread && GetThreadContext(thread,&c)) {
                    printf("DEBUG execute fault thread %lu RIP=%016llx RAX=%016llx RBX=%016llx\n",
                        event.dwThreadId,c.Rip,c.Rax,c.Rbx);
                    printf("DEBUG RDI=%016llx RSI=%016llx R14=%016llx R15=%016llx RSP=%016llx RBP=%016llx\n",
                        c.Rdi,c.Rsi,c.R14,c.R15,c.Rsp,c.Rbp);
                    memory(process.hProcess,c.Rsp,96);
                    memory(process.hProcess,c.R15,64);
                    memory(process.hProcess,c.R14,96);
                    memory(process.hProcess,c.Rbx,128);
                    MEMORY_BASIC_INFORMATION page={0};
                    VirtualQueryEx(process.hProcess,(void *)c.Rip,&page,sizeof(page));
                    printf("DEBUG fault region base=%p allocation=%p protect=%lx type=%lx\n",
                        page.BaseAddress,page.AllocationBase,page.Protect,page.Type);
                }
                if (thread) CloseHandle(thread);
            }
        } else if (event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            exe_base=(uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage;
            create_site=exe_base+0x72870;
            SIZE_T got=0;
            ReadProcessMemory(process.hProcess,(void *)create_site,&create_byte,1,&got);
            byte_at(process.hProcess,create_site,0xcc);
            dirty_site=UINT64_C(0x80137b777);
        } else if (event.dwDebugEventCode==OUTPUT_DEBUG_STRING_EVENT) {
            /* No output collection is needed; stdout is inherited. */
        }
        else if (event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)
            CloseHandle(event.u.LoadDll.hFile);
        else if (event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            DWORD code=event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE);
            CloseHandle(process.hProcess); CloseHandle(process.hThread); return (int)code;
        }
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation);
    }
}
