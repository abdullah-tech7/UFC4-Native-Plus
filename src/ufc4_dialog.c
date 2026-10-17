// SPDX-License-Identifier: GPL-2.0-or-later
/* Save dialog ABI diagnostics. This result describes the dialog, not disk I/O. */
extern uintptr_t runtime_services_resolve(const char *name);
typedef struct {
    uint64_t base_size;uint8_t reserved_base[36];uint32_t magic;
    uint32_t size,mode,type,pad;
    const void *animation,*items,*user_message,*system_message,*error,*progress;
    void *user_data;const void *option;uint8_t reserved[24];
} U4SaveDialogParam;
typedef struct {
    uint32_t mode,result,button,pad;void *directory,*param,*user_data;uint8_t reserved[32];
} U4SaveDialogResult;
_Static_assert(sizeof(U4SaveDialogParam)==152 && sizeof(U4SaveDialogResult)==72,"Save dialog ABI");
typedef int32_t (GUEST *U4DialogOpen)(const void *);
typedef int32_t (GUEST *U4DialogResult)(void *);
static GUEST int32_t u4_dialog_open(const U4SaveDialogParam *param) {
    U4DialogOpen original=(U4DialogOpen)runtime_services_resolve("4tPhsP6FpDI#libSceSaveDataDialog");
    if(param) {
        printf("UFC4 dialog: open size=%u base=%llu mode=%u type=%u\n",param->size,(unsigned long long)param->base_size,param->mode,param->type);
        if(param->size>=sizeof(*param) && param->system_message)
            printf("UFC4 dialog: system message=%u value=%llu caller=%p\n",*(const uint32_t *)param->system_message,
                (unsigned long long)((const uint64_t *)param->system_message)[1],__builtin_return_address(0));
    }
    return original(param);
}
static GUEST int32_t u4_dialog_result(U4SaveDialogResult *result) {
    U4DialogResult original=(U4DialogResult)runtime_services_resolve("yEiJ-qqr6Cg#libSceSaveDataDialog");
    int32_t code=original(result);
    if(result)printf("UFC4 dialog: result code=%08x mode=%u result=%u button=%u\n",(unsigned)code,result->mode,result->result,result->button);
    return code;
}
static uintptr_t u4_dialog_resolve(const char *name) {
    if(!strcmp(name,"4tPhsP6FpDI#libSceSaveDataDialog"))return (uintptr_t)u4_dialog_open;
    if(!strcmp(name,"yEiJ-qqr6Cg#libSceSaveDataDialog"))return (uintptr_t)u4_dialog_result;
    return 0;
}
