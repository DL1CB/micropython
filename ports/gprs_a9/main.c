#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "lib/utils/pyexec.h"
#include "py/stackctrl.h"
#include "py/mpstate.h"
#include "py/mphal.h"

#include "stdbool.h"
#include "api_os.h"
#include "api_event.h"
#include "api_debug.h"
#include "api_hal_pm.h"
#include "api_hal_uart.h"
#include "buffer.h"
#include "api_network.h"
#include "time.h"
#include "api_fs.h"

#include "mphalport.h"
#include "mpconfigport.h"
#include "modcellular.h"
#include "modgps.h"
#include "modmachine.h"

#define AppMain_TASK_STACK_SIZE    (2048 * 2)
#define AppMain_TASK_PRIORITY      0
#define MICROPYTHON_TASK_STACK_SIZE     (2048 * 4)
#define MICROPYTHON_TASK_PRIORITY       1
#define UART_CIRCLE_FIFO_BUFFER_MAX_LENGTH 2048

HANDLE mainTaskHandle  = NULL;
HANDLE microPyTaskHandle = NULL;
Buffer_t fifoBuffer; // ringbuf
uint8_t  fifoBufferData[UART_CIRCLE_FIFO_BUFFER_MAX_LENGTH];

typedef enum
{
    MICROPY_EVENT_ID_UART_RECEIVED = 1, //param1: length, param2:data(uint8_t*)
    MICROPY_EVENT_ID_UART_MAX
} MicroPy_Event_ID_t;

typedef struct
{
    MicroPy_Event_ID_t id;
    uint32_t param1;
    uint8_t* pParam1;
} MicroPy_Event_t;

#if MICROPY_ENABLE_COMPILER
void do_str(const char *src, mp_parse_input_kind_t input_kind) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, input_kind);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, MP_EMIT_OPT_NONE, true);
        mp_call_function_0(module_fun);
        nlr_pop();
    } else {
        // uncaught exception
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}
#endif

static char *stack_top;


void gc_collect(void) {
    // WARNING: This gc_collect implementation doesn't try to get root
    // pointers from CPU registers, and thus may function incorrectly.
    void *dummy;
    gc_collect_start();
    gc_collect_root(&dummy, ((mp_uint_t)stack_top - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
    gc_dump_info();
}


void nlr_jump_fail(void *val) {
    Assert(false, "nlr_jump_fail");
    while (1);//never reach here actully
}

void NORETURN __fatal_error(const char *msg) {
    Assert(false,msg);
    while (1);//never reach here actully
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    Trace(1,"Assertion '%s' failed, at file %s:%d\n", expr, file, line);
    __fatal_error("Assertion failed");
}
#endif

void ShowStackInfo()
{
    OS_Task_Info_t info;
    OS_GetTaskInfo(microPyTaskHandle,&info);
    volatile uint32_t j = 0;
    uint32_t last_bytes = (uint32_t)&j - info.stackTop;
    uint32_t all_bytes  = info.stackSize*4;
    char msg[32];
    snprintf(msg, sizeof(msg), "Stack used: %d/%d\r\n", all_bytes - last_bytes, all_bytes);
    mp_hal_stdout_tx_str(msg);

    API_FS_INFO fsInfo;
    if (API_FS_GetFSInfo(FS_DEVICE_NAME_FLASH, &fsInfo) != 0) {
        mp_hal_stdout_tx_str("No flash info!\r\n");
    } else {
        snprintf(msg, sizeof(msg), "Flash used: %d/%d\r\n", (int)fsInfo.usedSize, (int)fsInfo.totalSize);
        mp_hal_stdout_tx_str(msg);
        
        mp_hal_stdout_tx_str("Files:\r\n");
        Dir_t* dir = API_FS_OpenDir("/");
        Dirent_t* dirent;
        while ((dirent = API_FS_ReadDir(dir))) {

            snprintf(msg, sizeof(msg), "/%s", dirent->d_name);

            mp_hal_stdout_tx_str(" ");
            mp_hal_stdout_tx_str(msg);
            mp_hal_stdout_tx_str(" ");

            int32_t fd;

            if ((fd = API_FS_Open(msg, FS_O_RDONLY, 0)) < 0) {
                snprintf(msg, sizeof(msg), "[FAIL: %d]\r\n", fd);
                mp_hal_stdout_tx_str(msg);
            } else {
                snprintf(msg, sizeof(msg), "%d\r\n", (int)API_FS_GetFileSize(fd));
                API_FS_Close(fd);
                mp_hal_stdout_tx_str(msg);
            }
        }
        API_FS_CloseDir(dir);
    }
}

bool mp_Init()
{   
    //stack check info
    OS_Task_Info_t info;
    OS_GetTaskInfo(microPyTaskHandle, &info);
    mp_stack_ctrl_init();
    mp_stack_set_top((void *)(info.stackTop+info.stackSize*4));
    mp_stack_set_limit(MICROPYTHON_TASK_STACK_SIZE*4 - 1024);
    ShowStackInfo();

    mp_init();

    // Startup scripts
    int file_descriptor;
    pyexec_frozen_module("_boot.py");
    if ((file_descriptor = API_FS_Open("boot.py", FS_O_RDONLY, 0)) > 0) {
        API_FS_Close(file_descriptor);
        pyexec_file("boot.py");
    }
    if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL && (file_descriptor = API_FS_Open("main.py", FS_O_RDONLY, 0)) > 0) {
        API_FS_Close(file_descriptor);
        pyexec_file("main.py");
    }

    pyexec_event_repl_init();
    return true;
}


void soft_reset(void) {
    mp_hal_stdout_tx_str("PYB: soft reboot\r\n");
    mp_hal_delay_us(10000); // allow UART to flush output
    mp_Init();
    cellular_init0();
}


void MicroPyTask(void *pData)
{
    MicroPy_Event_t* event;

    Buffer_Init(&fifoBuffer, fifoBufferData, sizeof(fifoBufferData));
    UartInit();
    mp_Init();
    cellular_init0();
    
    uint8_t reset;
soft_reset:
    reset = 0;
    while (1) if (OS_WaitEvent(microPyTaskHandle, (void**)&event, OS_TIME_OUT_WAIT_FOREVER)) {

        Trace(1,"microPy task received event:%d",event->id);
        // PM_SetSysMinFreq(PM_SYS_FREQ_178M);

        switch(event->id) {
            case MICROPY_EVENT_ID_UART_RECEIVED: {
                uint8_t c;
                Trace(1, "micropy task received data length: %d", event->param1);
                while (Buffer_Gets(&fifoBuffer, &c, 1))
                    if (pyexec_event_repl_process_char((int)c)) {
                        reset = 1;
                        break;
                    }
                Trace(1, "REPL complete");
                break;
            }
            default:
                break;
        }

        OS_Free(event->pParam1);
        OS_Free(event);
        if (reset) break;
        // PM_SetSysMinFreq(PM_SYS_FREQ_32K);
    }
    soft_reset();
    goto soft_reset;
}

void EventDispatch(API_Event_t* pEvent)
{
    uint32_t len = 0;
    switch(pEvent->id)
    {
        case API_EVENT_ID_POWER_ON:
            notify_power_on(pEvent);
            break;

        // Network
        // =======

        case API_EVENT_ID_NO_SIMCARD:
            modcellular_notify_no_sim(pEvent);
            break;

        case API_EVENT_ID_SIMCARD_DROP:
            modcellular_notify_sim_drop(pEvent);
            break;

        case API_EVENT_ID_NETWORK_REGISTERED_HOME:
            modcellular_notify_reg_home(pEvent);
            break;

        case API_EVENT_ID_NETWORK_REGISTERED_ROAMING:
            modcellular_notify_reg_roaming(pEvent);
            break;

        case API_EVENT_ID_NETWORK_REGISTER_SEARCHING:
            modcellular_notify_reg_searching(pEvent);
            break;

        case API_EVENT_ID_NETWORK_REGISTER_DENIED:
            modcellular_notify_reg_denied(pEvent);
            break;

        case API_EVENT_ID_NETWORK_REGISTER_NO:
            // TODO: WTF is this?
            modcellular_notify_reg_denied(pEvent);
            break;

        case API_EVENT_ID_NETWORK_DEREGISTER:
            modcellular_notify_dereg(pEvent);
            break;

        case API_EVENT_ID_NETWORK_DETACHED:
            modcellular_notify_det(pEvent);
            break;

        case API_EVENT_ID_NETWORK_ATTACH_FAILED:
            modcellular_notify_att_failed(pEvent);
            break;

        case API_EVENT_ID_NETWORK_ATTACHED:
            modcellular_notify_att(pEvent);
            break;

        case API_EVENT_ID_NETWORK_DEACTIVED:
            modcellular_notify_deact(pEvent);
            break;

        case API_EVENT_ID_NETWORK_ACTIVATE_FAILED:
            modcellular_notify_act_failed(pEvent);
            break;

        case API_EVENT_ID_NETWORK_ACTIVATED:
            modcellular_notify_act(pEvent);
            break;

        // SMS
        // ===

        case API_EVENT_ID_SMS_SENT:
            modcellular_notify_sms_sent(pEvent);
            break;

        case API_EVENT_ID_SMS_ERROR:
            modcellular_notify_sms_error(pEvent);
            break;

        case API_EVENT_ID_SMS_LIST_MESSAGE:
            modcellular_notify_sms_list(pEvent);
            break;

        case API_EVENT_ID_SMS_RECEIVED:
            modcellular_notify_sms_receipt(pEvent);
            break;

        // Signal
        // ======

        case API_EVENT_ID_SIGNAL_QUALITY:
            modcellular_notify_signal(pEvent);
            break;

        // UART
        // ====
        case API_EVENT_ID_UART_RECEIVED:
            Trace(1,"UART%d received:%d,%s",pEvent->param1,pEvent->param2,pEvent->pParam1);
            if(pEvent->param1 == UART1)
            {
                MicroPy_Event_t* event = (MicroPy_Event_t*)malloc(sizeof(MicroPy_Event_t));
                if(!event)
                {
                    Trace(1,"malloc fail");
                    break;
                }
                for(uint16_t i=0; i<pEvent->param2; ++i)
                {
                   if (pEvent->pParam1[i] == mp_interrupt_char) {
                        // inline version of mp_keyboard_interrupt();
                        MP_STATE_VM(mp_pending_exception) = MP_OBJ_FROM_PTR(&MP_STATE_VM(mp_kbd_exception));
                        #if MICROPY_ENABLE_SCHEDULER
                        if (MP_STATE_VM(sched_state) == MP_SCHED_IDLE) {
                            MP_STATE_VM(sched_state) = MP_SCHED_PENDING;
                        }
                        #endif
                   }
                   else
                   {
                       Buffer_Puts(&fifoBuffer,pEvent->pParam1+i,1);
                       len++;
                   }
                }
                memset((void*)event,0,sizeof(MicroPy_Event_t));
                event->id = MICROPY_EVENT_ID_UART_RECEIVED;
                event->param1 = len;
                OS_SendEvent(microPyTaskHandle, (void*)event, OS_TIME_OUT_WAIT_FOREVER, 0);
            }
            break;

        // GPS
        // ===
        case API_EVENT_ID_GPS_UART_RECEIVED:
            modgps_notify_gps_update(pEvent);
            break;

        default:
            break;
    }
}


void AppMainTask(void *pData)
{
    API_Event_t* event=NULL;
    
    TIME_SetIsAutoUpdateRtcTime(true);

    microPyTaskHandle = OS_CreateTask(MicroPyTask, NULL, NULL, MICROPYTHON_TASK_STACK_SIZE, MICROPYTHON_TASK_PRIORITY, 0, 0, "mpy Task");                                    
    while(1)
    {
        if(OS_WaitEvent(mainTaskHandle, (void**)&event, OS_TIME_OUT_WAIT_FOREVER))
        {
            // PM_SetSysMinFreq(PM_SYS_FREQ_178M);//set back system min frequency to 178M or higher(/lower) value
            EventDispatch(event);
            OS_Free(event->pParam1);
            OS_Free(event->pParam2);
            OS_Free(event);
            // PM_SetSysMinFreq(PM_SYS_FREQ_32K);//release system freq to enter sleep mode to save power,
                                              //system remain runable but slower, and close eripheral not using
        }
    }
}
void _Main(void)
{
    mainTaskHandle = OS_CreateTask(AppMainTask, NULL, NULL, AppMain_TASK_STACK_SIZE, AppMain_TASK_PRIORITY, 0, 0, "main Task");
    OS_SetUserMainHandle(&mainTaskHandle);
}


int mp_hal_stdin_rx_chr(void) {
    uint8_t c;
    for (;;) {
        if (Buffer_Gets(&fifoBuffer, &c, 1))
            return c;
        MICROPY_EVENT_POLL_HOOK
        OS_Sleep(1);
    }
}


#if MICROPY_DEBUG_VERBOSE
int DEBUG_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = mp_vprintf(MICROPY_DEBUG_PRINTER, fmt, ap);
    va_end(ap);
    return ret;
}

STATIC void debug_print_strn(void *env, const char *str, size_t len) {
    (void)env;
    char p[len+1];
    // char* p =  (char*)OS_Malloc(len+1);
    // if(!p)
    //     return;
    memcpy(p,str,len);
    p[len] = 0;
    Trace(2,p);
}

const mp_print_t mp_debug_print = {NULL, debug_print_strn};

#endif


