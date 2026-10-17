// SPDX-License-Identifier: GPL-2.0-or-later
/* One-shot diagnostic breakpoints, for the audited UFC4 v28 image only. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

typedef struct { uintptr_t offset; const char *name; unsigned char saved; int armed; } Point;
static Point points[] = {
    {0x54e845,"dispatcher config"}, {0x1447e10,"dispatcher create"},
    {0x13256b0,"NetConn startup"}, {0x137cf30,"network dispatcher binding"},
    {0x132578e,"dispatcher binding result"}, {0x137c360,"context manager create"},
    {0x137cdd0,"context manager status"},
    {0x137c410,"context manager destroy"}, {0x132579b,"context manager result"},
    {0x13258ab,"network control result"}, {0x13258f2,"socket subsystem result"},
    {0x1325904,"NetConn failed result"},
};
static void byte_at(HANDLE p,uintptr_t a,unsigned char v) {
    DWORD old,ignored; SIZE_T done;
    if (!VirtualProtectEx(p,(void *)a,1,PAGE_EXECUTE_READWRITE,&old)) return;
    WriteProcessMemory(p,(void *)a,&v,1,&done);
    FlushInstructionCache(p,(void *)a,1);
    VirtualProtectEx(p,(void *)a,1,old,&ignored);
}
static uint64_t word(HANDLE p,uintptr_t a) {
    uint64_t value=0; SIZE_T got; ReadProcessMemory(p,(void *)a,&value,8,&got); return value;
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    STARTUPINFOA si={.cb=sizeof(si),.dwFlags=STARTF_USESTDHANDLES,
        .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
        .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process={0};
    if (!CreateProcessA(NULL,argv[1],NULL,NULL,TRUE,DEBUG_ONLY_THIS_PROCESS,NULL,NULL,&si,&process)) return 2;
    uintptr_t arm_site=0; unsigned char arm_byte=0; int armed=0; DEBUG_EVENT event;
    const uintptr_t guest=UINT64_C(0x800000000);
    for (;;) {
        if (!WaitForDebugEvent(&event,1000)) continue;
        DWORD continuation=DBG_CONTINUE;
        if (event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            arm_site=(uintptr_t)event.u.CreateProcessInfo.lpBaseOfImage+0x72870;
            SIZE_T got; ReadProcessMemory(process.hProcess,(void *)arm_site,&arm_byte,1,&got);
            byte_at(process.hProcess,arm_site,0xcc);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
            EXCEPTION_RECORD *e=&event.u.Exception.ExceptionRecord;
            continuation=DBG_EXCEPTION_NOT_HANDLED;
            if (e->ExceptionCode==EXCEPTION_BREAKPOINT) {
                uintptr_t address=(uintptr_t)e->ExceptionAddress;
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,event.dwThreadId);
                CONTEXT c={.ContextFlags=CONTEXT_FULL};
                if (thread && GetThreadContext(thread,&c)) {
                    if (!armed && address==arm_site) {
                        byte_at(process.hProcess,arm_site,arm_byte); c.Rip--;
                        for (unsigned i=0;i<sizeof(points)/sizeof(points[0]);i++) {
                            SIZE_T got=0; ReadProcessMemory(process.hProcess,(void *)(guest+points[i].offset),&points[i].saved,1,&got);
                            if (got==1) { byte_at(process.hProcess,guest+points[i].offset,0xcc); points[i].armed=1; }
                        }
                        armed=1; continuation=DBG_CONTINUE;
                    } else for (unsigned i=0;i<sizeof(points)/sizeof(points[0]);i++) {
                        Point *p=&points[i];
                        if (!p->armed || address!=guest+p->offset) continue;
                        printf("TRACE %s thread=%lu RAX=%llx RDI=%llx RSI=%llx RDX=%llx dispatcher=%llx context=%llx\n",
                            p->name,event.dwThreadId,c.Rax,c.Rdi,c.Rsi,c.Rdx,
                            word(process.hProcess,guest+0x2d66a18),word(process.hProcess,guest+0x2d64480));
                        printf("TRACE stack:"); for (unsigned j=0;j<12;j++) printf(" %llx",word(process.hProcess,c.Rsp+j*8)); puts("");
                        if (p->offset==0x13256b0) {
                            char text[257]={0}; SIZE_T got; ReadProcessMemory(process.hProcess,(void *)c.Rdi,text,256,&got);
                            printf("TRACE NetConn options: %s\n",text);
                        }
                        byte_at(process.hProcess,address,p->saved); p->armed=0; c.Rip--; continuation=DBG_CONTINUE; break;
                    }
                    SetThreadContext(thread,&c);
                }
                if (thread) CloseHandle(thread);
                if (!armed) continuation=DBG_CONTINUE; /* Windows initial breakpoint. */
            }
        } else if (event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        else if (event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            DWORD code=event.u.ExitProcess.dwExitCode;
            ContinueDebugEvent(event.dwProcessId,event.dwThreadId,DBG_CONTINUE);
            CloseHandle(process.hProcess); CloseHandle(process.hThread); return code;
        }
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,continuation);
    }
}
