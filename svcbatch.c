/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "svcbatch.h"

#if defined (_DEBUG)
#include <crtdbg.h>
#endif

#if defined(_MSC_VER) && (_MSC_VER < 1600)
# define va_copy(d, s)          (d) = (s)
#endif

#define HAVE_NAMED_MMAP         0
#define HAVE_EXTRA_SVC_PARAMS   0
#define HAVE_LOGDIR_MUTEX       1
#define HAVE_LOGDIR_LOCK        0

#if defined(DEBUG_TRACE)
# define HAVE_DEBUG_TRACE       1
#else
# define HAVE_DEBUG_TRACE       0
#endif

#if HAVE_DEBUG_TRACE
static void                     xtraceprintf(LPCSTR, int, int, LPCSTR, ...);
static void                     xtraceprints(LPCSTR, int, int, LPCSTR);
static void                     xtracefopen(void);


#define XTRACE_STOPSVC_EXT      L"_stop.log"
#define XTRACE_SERVICE_EXT      L"_service.log"
#define DBG_PRINTF(Fmt, ...)    if (xtraceservice) xtraceprintf(__FUNCTION__, __LINE__, 0, Fmt, ##__VA_ARGS__)
#define DBG_PRINTS(Msg)         if (xtraceservice) xtraceprints(__FUNCTION__, __LINE__, 0, Msg)
#else
#define DBG_PRINTF(Fmt, ...)    (void)0
#define DBG_PRINTS(Msg)         (void)0
#endif

/**
 * Error macros
 */
#define SVCBATCH_FATAL(_e)      xfatalerr("!!Fatal error in svcbatch.c at line #" CPP_TOSTR(__LINE__), (_e))
#define SVCBATCH_EEXIT(_o, _e)  xeexiterr("Runtime error in svcbatch.c at line #" CPP_TOSTR(__LINE__), (_o), (_e))
#define xsyserrno(_i, _d, _p)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,       0, wcsmessages[_i], _d, _p)
#define xsyserror(_n, _d, _p)   svcsyserror(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,      _n, NULL, _d, _p)
#define svcerror(_e, _i, _f, ...)   xerrsprintf(__FUNCTION__, __LINE__, EVENTLOG_ERROR_TYPE,       _e, errmessages[_i], _f, ##__VA_ARGS__)
#define xsyswarn(_e, _i, _f, ...)   xerrsprintf(__FUNCTION__, __LINE__, EVENTLOG_WARNING_TYPE,     _e, errmessages[_i], _f, ##__VA_ARGS__)
#define xsysinfo(_e, _i, _f, ...)   xerrsprintf(__FUNCTION__, __LINE__, EVENTLOG_INFORMATION_TYPE, _e, errmessages[_i], _f, ##__VA_ARGS__)

#define xsvcstatus(_s, _p)      if (servicemode) reportsvcstatus(__FUNCTION__, __LINE__, _s, _p)

#define SZ_STATUS_PROCESS_INFO  sizeof(SERVICE_STATUS_PROCESS)
#define SYSTEM_SVC_SUBKEY       L"SYSTEM\\CurrentControlSet\\Services\\"
#define IS_LEAP_YEAR(_y)        ((!(_y % 4)) ? (((_y % 400) && !(_y % 100)) ? 0 : 1) : 0)

#define SYSVARS_COUNT           27
#define GETSYSVAR_VAL(_i)       svariables->var[_i - 64].val
#define GETSYSVAR_KEY(_i)       svariables->var[_i - 64].key
#define SETSYSVAR_VAL(_i, _v)   svariables->var[_i - 64].val = _v
#define SETSYSVAR_KEY(_i, _k)   svariables->var[_i - 64].key = _k

#define INVALID_FILENAME_CHARS  L"/\\:;<>?*|\""
#define INVALID_PATHNAME_CHARS  L";<>?*|"

/**
 * Misc internal buffer size definitions
 */
#define NBUFSIZ                 16
#define TBUFSIZ                 32
#define WBUFSIZ                 64
#define SBUFSIZ                128
#define BBUFSIZ                512
#define MBUFSIZ               1024
#define HBUFSIZ               4096


typedef enum {
    SVCBATCH_WORKER_THREAD = 0,
    SVCBATCH_STDIN_THREAD,
    SVCBATCH_STOP_THREAD,
    SVCBATCH_ROTATE_THREAD,
    SVCBATCH_MAX_THREADS
} SVCBATCH_THREAD_ID;

typedef enum {
    SVCBATCH_REG_TYPE_NONE = 0, /* Unknown registry type    */
    SVCBATCH_REG_TYPE_BIN,      /* [RRF_RT_]REG_BINARY      */
    SVCBATCH_REG_TYPE_BOOL,     /* [RRF_RT_]REG_DWORD       */
    SVCBATCH_REG_TYPE_ESZ,      /* [RRF_RT_]REG_EXPAND_SZ   */
    SVCBATCH_REG_TYPE_MSZ,      /* [RRF_RT_]REG_MULTI_SZ    */
    SVCBATCH_REG_TYPE_NUM,      /* [RRF_RT_]REG_DWORD       */
    SVCBATCH_REG_TYPE_SZ        /* [RRF_RT_]REG_SZ          */
} SVCBATCH_REG_TYPE;

#if HAVE_DEBUG_TRACE
static const char *xtracesvcmodes[] = {
    "STOPSVC",
    "SERVICE"
};

static const char *xtracesvctypes[] = {
    "DEBUG",
    "ERROR",
    "WARN",
    "0003",
    "INFO",
    "0005",
    "0006",
    "0007",
};
#endif

static const DWORD svcbrrfrtypes[] = {
    0,
    RRF_RT_REG_BINARY,
    RRF_RT_REG_DWORD,
    RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
    RRF_RT_REG_MULTI_SZ,
    RRF_RT_REG_DWORD,
    RRF_RT_REG_SZ,
};

static const DWORD svcbregrtypes[] = {
    0,
    REG_BINARY,
    REG_DWORD,
    REG_EXPAND_SZ,
    REG_MULTI_SZ,
    REG_DWORD,
    REG_SZ
};

typedef struct _SVCBATCH_WBUFFER {
    int     siz;
    int     pos;
    LPWSTR  buf;
} SVCBATCH_WBUFFER, *LPSVCBATCH_WBUFFER;

typedef struct _SVCBATCH_VARIABLE {
    DWORD                   iid;
    DWORD                   mod;
    LPCWSTR                 key;
    LPCWSTR                 val;
} SVCBATCH_VARIABLE, *LPSVCBATCH_VARIABLE;

typedef struct _SVCBATCH_VARIABLES {
    int                     siz;
    int                     pos;
    SVCBATCH_VARIABLE       var[SBUFSIZ];
} SVCBATCH_VARIABLES, *LPSVCBATCH_VARIABLES;

typedef struct _SVCBATCH_THREAD {
    volatile HANDLE         thread;
    volatile LONG           started;
    LPTHREAD_START_ROUTINE  startAddress;
    LPVOID                  parameter;
    DWORD                   id;
    DWORD                   exitCode;
    ULONGLONG               duration;
    LPCSTR                  name;
} SVCBATCH_THREAD, *LPSVCBATCH_THREAD;

typedef struct _SVCBATCH_PIPE {
    OVERLAPPED              o;
    HANDLE                  pipe;
    DWORD                   read;
    DWORD                   state;
    BYTE                    buffer[SVCBATCH_PIPE_LEN];
} SVCBATCH_PIPE, *LPSVCBATCH_PIPE;

typedef struct _SVCBATCH_PROCESS {
    volatile LONG           state;
    PROCESS_INFORMATION     pInfo;
    STARTUPINFOW            sInfo;
    DWORD                   exitCode;
    DWORD                   timeout;
    DWORD                   argc;
    DWORD                   optc;
    LPWSTR                  commandLine;
    LPWSTR                  application;
    LPWSTR                  directory;
    LPWSTR                  name;
    LPCWSTR                 args[SVCBATCH_MAX_ARGS];
    LPCWSTR                 opts[SVCBATCH_MAX_ARGS];
} SVCBATCH_PROCESS, *LPSVCBATCH_PROCESS;

typedef struct _SVCBATCH_PROCINFO
{
    DWORD  n;
    DWORD  i;
    HANDLE h;
} SVCBATCH_PROCINFO, *LPSVCBATCH_PROCINFO;

typedef struct _SVCBATCH_SERVICE {
    volatile LONG           state;
    volatile LONG           check;
    volatile LONG           exitCode;
    volatile LONG           killDepth;
    DWORD                   failMode;
    DWORD                   timeout;
    SERVICE_STATUS_HANDLE   handle;
    SERVICE_STATUS          status;
    CRITICAL_SECTION        cs;

    LPWSTR                  environment;
    LPCWSTR                 display;
    LPCWSTR                 name;
    LPWSTR                  home;
    LPWSTR                  uuid;
    LPWSTR                  work;
    LPWSTR                  logs;

} SVCBATCH_SERVICE, *LPSVCBATCH_SERVICE;

typedef struct _SVCBATCH_LOG {
    volatile LONG64         size;
    volatile HANDLE         fd;
    volatile LONG           state;
    int                     maxLogs;
    CRITICAL_SECTION        cs;

    LPCWSTR                 logName;
    LPWSTR                  logFile;
} SVCBATCH_LOG, *LPSVCBATCH_LOG;

/**
 * Length of the shared memory data.
 * Adjust this number so that SVCBATCH_IPC
 * structure aligns to 64K
 */
#define SVCBATCH_DATA_LEN   32614
typedef struct _SVCBATCH_IPC {
    DWORD                   options;
    DWORD                   timeout;
    DWORD                   killDepth;
    DWORD                   maxLogs;
    DWORD                   stdinSize;
    DWORD                   stdinData;
    DWORD                   display;
    DWORD                   name;
    DWORD                   work;
    DWORD                   logs;
    DWORD                   logName;

    DWORD                   argc;
    DWORD                   optc;
    DWORD                   args[SVCBATCH_MAX_ARGS];
    DWORD                   opts[SVCBATCH_MAX_ARGS];

    WCHAR                   data[SVCBATCH_DATA_LEN];
} SVCBATCH_IPC, *LPSVCBATCH_IPC;

typedef struct _SVCBATCH_NAME_MAP {
    LPCWSTR                 name;
    int                     type;
    int                     code;
} SVCBATCH_NAME_MAP, *LPSVCBATCH_NAME_MAP;

typedef struct _SVCBATCH_CONF_VALUE {
    LPCWSTR                 name;
    DWORD                   type;
    DWORD                   size;
    DWORD                   dval;
    DWORD                   hval;
    LPCWSTR                 sval;
    LPBYTE                  data;
} SVCBATCH_CONF_VALUE, *LPSVCBATCH_CONF_VALUE;

typedef struct _SVCBATCH_CONF_PARAM {
    int                     type;
    int                     argc;
    LPCWSTR                 key;
    LPCWSTR                 val;
    LPCWSTR                 args[SBUFSIZ];
} SVCBATCH_CONF_PARAM, *LPSVCBATCH_CONF_PARAM;

typedef struct _SVCBATCH_SCM_PARAMS {
    int                     siz;
    int                     pos;
    SVCBATCH_CONF_PARAM     p[SBUFSIZ];
} SVCBATCH_SCM_PARAMS, *LPSVCBATCH_SCM_PARAMS;

#if HAVE_DEBUG_TRACE
static int                   xtraceservice  = 2;
static int                   xtracesvcstop  = 1;
static volatile HANDLE       xtracefhandle  = NULL;
static CRITICAL_SECTION      xtracesync;
#endif
static SYSTEM_INFO           ssysteminfo;
static HANDLE                processheap    = NULL;
static int                   svcmainargc    = 0;
static int                   stopmaxlogs    = 0;
static LPCWSTR              *svcmainargv    = NULL;

static LPSVCBATCH_SERVICE    service        = NULL;
static LPSVCBATCH_PROCESS    program        = NULL;
static LPSVCBATCH_PROCESS    cmdproc        = NULL;
static LPSVCBATCH_PROCESS    svcstop        = NULL;
static LPSVCBATCH_LOG        outputlog      = NULL;
static LPSVCBATCH_IPC        sharedmem      = NULL;
static LPSVCBATCH_VARIABLES  svariables     = NULL;
static LPSVCBATCH_SCM_PARAMS scmpparams     = NULL;
static LPSVCBATCH_CONF_VALUE svcpparams     = NULL;
static LPSVCBATCH_CONF_VALUE svcsparams     = NULL;
static LPSVCBATCH_CONF_VALUE svcparams[2];
static LPSVCBATCH_THREAD     threads        = NULL;

static volatile LPVOID       xwsystempdir   = NULL;
static volatile HANDLE       wrpipehandle   = NULL;
static volatile LONG         runcounter     = 0;
static volatile LONG         uidcounter     = 0;
static volatile LONG         svcoptions     = 0;
static volatile LONG         errorreported  = 0;
static LONGLONG              rotateinterval = INT64_ZERO;
static LONGLONG              rotatesize     = INT64_ZERO;
static LARGE_INTEGER         rotatetime     = {{ 0, 0 }};

static DWORD     svceventid     = 2300;

static int       servicemode    = 0;
static DWORD     preshutdown    = 0;
static HANDLE    stopstarted    = NULL;
static HANDLE    svcstopdone    = NULL;
static HANDLE    workerended    = NULL;
static HANDLE    dologrotate    = NULL;
static HANDLE    sharedmmap     = NULL;
static HANDLE    svclogmutex    = NULL;
static LPCWSTR   stoplogname    = NULL;

static LPCWSTR   allexportvars  = L"ABDHLNRUVW";
static LPCWSTR   defexportvars  = L"HLNUW";
static LPCWSTR   namevarset     = L"BDNV";
static LPCWSTR   programbase    = CPP_WIDEN(SVCBATCH_BASENAME);
static LPCSTR    cnamestamp     = SVCBATCH_NAME " " SVCBATCH_VERSION_TXT;

static LPCWSTR   hexwchars      = L"0123456789abcdef";
static LPCWSTR   hexuchars      = L"0123456789ABCDEF";
static WCHAR     zerostring[]   = {  0,  0,  0,  0 };
static BYTE      YCRLF[]        = { 89, 13, 10,  0 };
static WCHAR     CRLFW[]        = { 13, 10,  0,  0 };

static LPBYTE   stdindata       = YCRLF;
static int      stdinsize       = 3;

static int      xwoptind        = 1;
static int      xwoptend        = 0;
static int      xwoptarr        = 0;
static LPCWSTR  xwoptarg        = NULL;
static LPCWSTR  xwoption        = NULL;
static LPCWSTR  cmdoptions      = L"d?hlq!s:w";

#define PROPELLER_SIZE 8
static char     xpropeller[PROPELLER_SIZE] = {'|', '/', '-', '\\', '|', '/', '-', '\\'};


typedef enum {
    SVCBATCH_CFG_CMD  = 0,
    SVCBATCH_CFG_ARGS,
    SVCBATCH_CFG_HOME,
    SVCBATCH_CFG_LOGS,
    SVCBATCH_CFG_TEMP,
    SVCBATCH_CFG_WORK,

    SVCBATCH_CFG_PRESHUTDOWN,
    SVCBATCH_CFG_FAILMODE,
    SVCBATCH_CFG_KILLDEPTH,
    SVCBATCH_CFG_SENDBREAK,
    SVCBATCH_CFG_TIMEOUT,
    SVCBATCH_CFG_LOCALTIME,

    SVCBATCH_CFG_STDINDATA,

    SVCBATCH_CFG_ENVSET,
    SVCBATCH_CFG_ENVPREFIX,
    SVCBATCH_CFG_ENVEXPORT,

    SVCBATCH_CFG_NOLOGGING,
    SVCBATCH_CFG_LOGNAME,
    SVCBATCH_CFG_LOGROTATE,
    SVCBATCH_CFG_ROTATEBYSIG,
    SVCBATCH_CFG_ROTATEINT,
    SVCBATCH_CFG_ROTATESIZE,
    SVCBATCH_CFG_ROTATETIME,
    SVCBATCH_CFG_MAXLOGS,
    SVCBATCH_CFG_TRUNCATE,

    SVCBATCH_CFG_STOP,
    SVCBATCH_CFG_SLOGNAME,
    SVCBATCH_CFG_SMAXLOGS,

    SVCBATCH_CFG_MAX
} SVCBATCH_CFG_ID;

static SVCBATCH_NAME_MAP svccparams[] = {
    { L"Command",               SVCBATCH_REG_TYPE_MSZ,  SVCBATCH_CFG_CMD          },
    { L"Arguments",             SVCBATCH_REG_TYPE_MSZ,  SVCBATCH_CFG_ARGS         },
    { L"Home",                  SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_HOME         },
    { L"Logs",                  SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_LOGS         },
    { L"Temp",                  SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_TEMP         },
    { L"Work",                  SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_WORK         },

    { L"AcceptPreshutdown",     SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_PRESHUTDOWN  },
    { L"FailMode",              SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_FAILMODE     },
    { L"KillDepth",             SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_KILLDEPTH    },
    { L"SendBreakOnStop",       SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_SENDBREAK    },
    { L"StopTimeout",           SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_TIMEOUT      },
    { L"UseLocalTime",          SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_LOCALTIME    },

    { L"StdInput",              SVCBATCH_REG_TYPE_BIN,  SVCBATCH_CFG_STDINDATA    },

    { L"Environment",           SVCBATCH_REG_TYPE_MSZ,  SVCBATCH_CFG_ENVSET       },
    { L"EnvironmentPrefix",     SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_ENVPREFIX    },
    { L"Export",                SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_ENVEXPORT    },

    { L"DisableLogging",        SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_NOLOGGING    },
    { L"LogName",               SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_LOGNAME      },
    { L"LogRotate",             SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_LOGROTATE    },
    { L"LogRotateBySignal",     SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_ROTATEBYSIG  },
    { L"LogRotateInterval",     SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_ROTATEINT    },
    { L"LogRotateSize",         SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_ROTATESIZE   },
    { L"LogRotateTime",         SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_ROTATETIME   },
    { L"MaxLogs",               SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_MAXLOGS      },
    { L"TruncateLogs",          SVCBATCH_REG_TYPE_BOOL, SVCBATCH_CFG_TRUNCATE     },


    { L"Stop",                  SVCBATCH_REG_TYPE_MSZ,  SVCBATCH_CFG_STOP         },
    { L"StopLogName",           SVCBATCH_REG_TYPE_SZ,   SVCBATCH_CFG_SLOGNAME     },
    { L"StopMaxLogs",           SVCBATCH_REG_TYPE_NUM,  SVCBATCH_CFG_SMAXLOGS     },


    { NULL,                     0,                      0                         }
};

typedef enum {
    SVCBATCH_SVC_DISPLAY = 0,
    SVCBATCH_SVC_PSDTIMEOUT,
#if HAVE_EXTRA_SVC_PARAMS
    SVCBATCH_SVC_DESC,
    SVCBATCH_SVC_IMAGEPATH,
    SVCBATCH_SVC_USERNAME,
#endif
    SVCBATCH_SVC_MAX
} SVCBATCH_SVC_ID;

static SVCBATCH_NAME_MAP svcbparams[] = {
    { L"DisplayName",           SVCBATCH_REG_TYPE_SZ,   SVCBATCH_SVC_DISPLAY      },
    { L"PreshutdownTimeout",    SVCBATCH_REG_TYPE_NUM,  SVCBATCH_SVC_PSDTIMEOUT   },
#if HAVE_EXTRA_SVC_PARAMS
    { L"Description",           SVCBATCH_REG_TYPE_SZ,   SVCBATCH_SVC_DESC         },
    { L"ImagePath",             SVCBATCH_REG_TYPE_ESZ,  SVCBATCH_SVC_IMAGEPATH    },
    { L"ObjectName",            SVCBATCH_REG_TYPE_SZ,   SVCBATCH_SVC_USERNAME     },
#endif
    { NULL,                     0,                      0                         }
};

/**
 * Service Manager types
 *
 */
typedef enum {
    SVCBATCH_SCM_CREATE = 0,
    SVCBATCH_SCM_CONFIG,
    SVCBATCH_SCM_CONTROL,
    SVCBATCH_SCM_DELETE,
    SVCBATCH_SCM_HELP,
    SVCBATCH_SCM_START,
    SVCBATCH_SCM_STOP,
    SVCBATCH_SCM_VERSION,
    SVCBATCH_SCM_MAX

} SVCBATCH_SCM_CMD;

static LPCWSTR scmcommands[] = {
    L"Create",                  /* SVCBATCH_SCM_CREATE      */
    L"Config",                  /* SVCBATCH_SCM_CONFIG      */
    L"Control",                 /* SVCBATCH_SCM_CONTROL     */
    L"Delete",                  /* SVCBATCH_SCM_DELETE      */
    L"Help",                    /* SVCBATCH_SCM_HELP        */
    L"Start",                   /* SVCBATCH_SCM_START       */
    L"Stop",                    /* SVCBATCH_SCM_STOP        */
    L"Version",                 /* SVCBATCH_SCM_VERSION     */
    NULL
};

/**
 * Long options ...
 *
 * <option><options><option name>
 *
 * option:          Any alphanumeric character
 * mode:            '.' Option without argument
 *                  '+' Argument must be the next argument
 *                      is empty after skipping blanks, returns ERROR_BAD_LENGTH
 *                  ':' Argument can be part of the option separated by ':' or '='
 *                      If the option does not end with ':' or '=', the argument is next option
 *                  '?' Argument is optional and it must be part of the
 *                      current option, separated by ':' or '='
 *
 */

typedef struct _SVCBATCH_LONGOPT {
    int     option;
    int     mode;
    LPCWSTR name;
} SVCBATCH_LONGOPT, *LPSVCBATCH_LONGOPT;

static const SVCBATCH_LONGOPT scmcoptions[] = {
    { 'b',  '+',    L"binpath"      },
    { 'd',  '+',    L"description"  },
    { 'D',  ':',    L"depend"       },
    { 'n',  ':',    L"displayname"  },
    { 'n',  ':',    L"name"         },
    { 'p',  ':',    L"password"     },
    { 'P',  ':',    L"privs"        },
    { 'q',  '.',    L"quiet"        },
    { 'r',  ':',    L"preshutdown"  },
    { 's',  '+',    L"set"          },
    { 't',  ':',    L"start"        },
    { 'u',  ':',    L"username"     },
    { 'w',  '?',    L"wait"         },
    {   0,    0,    NULL            }
};

static LPCWSTR scmallowed[] = {
    L"!w",                      /* SVCBATCH_SCM_CREATE      */
    L"!w",                      /* SVCBATCH_SCM_CONFIG      */
    L"q",                       /* SVCBATCH_SCM_CONTROL     */
    L"q",                       /* SVCBATCH_SCM_DELETE      */
    L"~",                       /* SVCBATCH_SCM_HELP        */
    L"qw",                      /* SVCBATCH_SCM_START       */
    L"qw",                      /* SVCBATCH_SCM_STOP        */
    L"~",                       /* SVCBATCH_SCM_VERSION     */
    NULL
};


static LPCWSTR ucenvvars[] = {
    L"COMMONPROGRAMFILES",
    L"COMSPEC",
    L"PATH",
    L"PROGRAMFILES",
    L"SYSTEMDRIVE",
    L"SYSTEMROOT",
    L"TEMP",
    L"TMP",
    L"WINDIR",
    NULL
};

static LPCWSTR scmsvcaccounts[] = {
    L".\\LocalSystem",
    L"NT AUTHORITY\\LocalService",
    L"NT AUTHORITY\\NetworkService",
    NULL
};

static const SVCBATCH_NAME_MAP starttypemap[] = {
    { L"Automatic", 0, SERVICE_AUTO_START       },
    { L"Auto",      0, SERVICE_AUTO_START       },
    { L"Delayed",   1, SERVICE_AUTO_START       },
    { L"Manual",    0, SERVICE_DEMAND_START     },
    { L"Demand",    0, SERVICE_DEMAND_START     },
    { L"Disabled",  0, SERVICE_DISABLED         },
    { NULL,         0, 0                        }
};

static const SVCBATCH_NAME_MAP boolnamemap[] = {
    { L"True",      0, 1       },
    { L"False",     0, 0       },
    { L"Yes",       0, 1       },
    { L"No",        0, 0       },
    { L"On",        0, 1       },
    { L"Off",       0, 0       },
    { NULL,         0, 0       }
};


static const char *xgenerichelp =
    "\nUsage:\n  " SVCBATCH_NAME " [command] [service name] <option1> <option2>...\n"           \
    "\n    Commands:"                                                                           \
    "\n      Create.....Creates a service."                                                     \
    "\n      Config.....Changes the configuration of a service."                                \
    "\n      Control....Sends a control to a service."                                          \
    "\n      Delete.....Deletes a service."                                                     \
    "\n      Help.......Print this screen and exit."                                            \
    "\n                 Use Help [command] for command help."                                   \
    "\n      Start......Starts a service."                                                      \
    "\n      Stop.......Sends a STOP request to a service."                                     \
    "\n      Version....Print version information."                                             \
    "\n";

#define SCM_CC_OPTIONS    "\n    Options:"                                                      \
    "\n      --binPath      BinaryPathName to the .exe file."                                   \
    "\n      --depend       Dependencies (separated by / (forward slash))."                     \
    "\n      --description  Sets the description of a service."                                 \
    "\n      --displayName  Sets the service display name."                                     \
    "\n      --privs        Sets the required privileges of a service."                         \
    "\n      --set          Sets the service parameter and its value(s)."                       \
    "\n      --start        Sets the service startup type."                                     \
    "\n                     <auto|delayed|manual|disabled> (default = manual)."                 \
    "\n      --preshutdown  Sets the PreshudownTimeout in milliseconds"                         \
    "\n                     used if AcceptPreshutdown is enbled."                               \
    "\n      --username     The name of the account under which the service should run."        \
    "\n                     Default is LocalSystem account."                                    \
    "\n      --password     The password to the account name specified by the"                  \
    "\n                     username parameter."                                                \
    "\n      --quiet        Quiet mode, do not print status or error messages."

static const char *xcommandhelp[] = {
    /* Create */
    "\nDescription:\n  Creates a service entry in the registry and Service Database."           \
    "\nUsage:\n  " SVCBATCH_NAME " create [service name] <options ...> <[-] arguments ...>\n"   \
    SCM_CC_OPTIONS                                                                              \
    "\n",
    /* Config */
    "\nDescription:\n  Modifies a service entry in the registry and Service Database."          \
    "\nUsage:\n  " SVCBATCH_NAME " config [service name] <options ...> <[-] arguments ...>\n"   \
    SCM_CC_OPTIONS                                                                              \
    "\n",
    /* Control */
    "\nDescription:\n  Sends a CONTROL code to a service."                                      \
    "\nUsage:\n  " SVCBATCH_NAME " control [service name] <options ...> [value]\n"              \
    "\n    Options:"                                                                            \
    "\n      --quiet            Quiet mode, do not print status or error messages."             \
    "\n    Value:"                                                                              \
    "\n      User defined control code (128 ... 255)"                                           \
    "\n",
    /* Delete */
    "\nDescription:\n  Deletes a service entry from the registry."                              \
    "\nUsage:\n  " SVCBATCH_NAME " delete [service name] <options ...>\n"                       \
    "\n    Options:"                                                                            \
    "\n      --quiet            Quiet mode, do not print status or error messages."             \
    "\n",
    /* Help */
    "\nDescription:\n  Display command help."                                                   \
    "\nUsage:\n  " SVCBATCH_NAME " help <command>\n"                                            \
    "\n",
    /* Start */
    "\nDescription:\n  Starts a service running."                                               \
    "\nUsage:\n  " SVCBATCH_NAME " start [service name] <options ...> <arguments ...>\n"        \
    "\n    Options:"                                                                            \
    "\n      --quiet            Quiet mode, do not print status or error messages."             \
    "\n      --wait[:seconds]   Wait up to seconds until the service"                           \
    "\n                         enters the RUNNING state."                                      \
    "\n",
    /* Stop */
    "\nDescription:\n  Sends a STOP control request to a service."                              \
    "\nUsage:\n  " SVCBATCH_NAME " stop [service name] <options ...> <reason> <comment>\n"      \
    "\n    Options:"                                                                            \
    "\n      --quiet            Quiet mode, do not print status or error messages."             \
    "\n      --wait[:seconds]   Wait up to seconds for service to stop."                        \
    "\n",
    /* Version */
    "\nDescription:\n  Display version information."                                            \
    "\nUsage:\n  " SVCBATCH_NAME " version\n"                                                   \
    "\n",
    NULL,
};


/**
 * Message strings
 */

static const wchar_t *errmessages[] = {
    NULL,                                                                   /*  0 */
    NULL,                                                                   /*  1 */
    NULL,                                                                   /*  2 */
    NULL,                                                                   /*  3 */
    NULL,                                                                   /*  4 */
    NULL,                                                                   /*  5 */
    NULL,                                                                   /*  6 */
    NULL,                                                                   /*  7 */
    NULL,                                                                   /*  8 */
    NULL,                                                                   /*  9 */
    L"The operation completed successfully",                                /* 10 */
    L"The parameter is outside valid range",                                /* 11 */


    NULL
};

#define XSYSTEM_MSG(_id) errmessages[_id]

static const wchar_t *wcsmessages[] = {
    L"The operation completed successfully",                                /*  0 */
    L"Service stopped",                                                     /*  1 */
    L"The service is not in the RUNNING state",                             /*  2 */
    NULL,                                                                   /*  3 */
    L"Environment variable prefix",                                         /*  4 */
    NULL,                                                                   /*  5 */
    NULL,                                                                   /*  6 */
    NULL,                                                                   /*  7 */
    NULL,                                                                   /*  8 */
    NULL,                                                                   /*  9 */
    L"The %s parameter was already defined",                                /* 10 */
    L"The %s value is empty",                                               /* 11 */
    L"The %s value is invalid",                                             /* 12 */
    L"The %s value is outside valid range",                                 /* 13 */
    NULL,                                                                   /* 14 */
    NULL,                                                                   /* 15 */
    L"Too many arguments for the %s parameter",                             /* 16 */
    L"Too many %s arguments",                                               /* 17 */
    NULL,                                                                   /* 18 */
    NULL,                                                                   /* 19 */
    L"The %s contains invalid characters",                                  /* 20 */
    L"The %s is too large",                                                 /* 21 */
    L"Unknown command option",                                              /* 22 */
    L"The /\\:;<>?*|\" are not valid object name characters",               /* 23 */
    L"The maximum service name length is 256 characters",                   /* 24 */
    NULL,                                                                   /* 25 */
    L"The parameter is outside valid range",                                /* 26 */
    L"Service name starts with invalid character(s)",                       /* 27 */
    L"The %s command option value array is not terminated",                 /* 28 */
    L"The %s are mutually exclusive",                                       /* 29 */
    L"Unknown %s command option modifier",                                  /* 30 */
    L"The %s names are the same",                                           /* 31 */
    L"The %s are both absolute paths",                                      /* 32 */
    L"The Control code is missing. Use control [service name] [code]",      /* 33 */
    L"Stop the service and call Delete again",                              /* 34 */
    L"Stop the service and call Config again",                              /* 35 */

    NULL
};

#define SVCBATCH_MSG(_id) wcsmessages[_id]

static void xfatalerr(LPCSTR msg, int err)
{

    OutputDebugStringA(">>> " SVCBATCH_NAME " " SVCBATCH_VERSION_STR "\r\n");
    OutputDebugStringA(msg);
    OutputDebugStringA("<<<\r\n\r\n");
    _exit(err);
#if 0
    TerminateProcess(GetCurrentProcess(), err);
#endif
}

static int xeexiterr(LPCSTR msg, LPCSTR opt, int err)
{

    OutputDebugStringA(">>> " SVCBATCH_NAME " " SVCBATCH_VERSION_STR "\r\n");
    OutputDebugStringA(msg);
    OutputDebugStringA(opt);
    OutputDebugStringA("<<<\r\n\r\n");

    return err;
}

static void *xmmalloc(size_t size)
{
    void   *p;
    size_t  n;

    n = MEM_ALIGN_DEFAULT(size);
    p = HeapAlloc(processheap, HEAP_ZERO_MEMORY, n);
    if (p == NULL)
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    return p;
}

static void *xmcalloc(size_t size)
{
    void   *p;
    size_t  n;

    n = MEM_ALIGN_DEFAULT(size);
    p = HeapAlloc(processheap, HEAP_ZERO_MEMORY, n);
    if (p == NULL)
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    return p;
}

static __inline void xfree(void *m)
{
    if (m != NULL)
        HeapFree(processheap, 0, m);
}

static void *xrealloc(void *m, size_t size)
{
    void   *p;
    size_t  n;

    if (size == 0) {
        xfree(m);
        return NULL;
    }
    n = MEM_ALIGN_DEFAULT(size);
    if (m == NULL)
        p = HeapAlloc(  processheap, HEAP_ZERO_MEMORY, n);
    else
        p = HeapReAlloc(processheap, HEAP_ZERO_MEMORY, m, n);
    if (p == NULL)
        SVCBATCH_FATAL(ERROR_OUTOFMEMORY);
    return p;
}

static __inline LPWSTR  xwmalloc(size_t size)
{
    return (LPWSTR)xmmalloc(size * sizeof(WCHAR));
}

static __inline LPWSTR *xwaalloc(size_t size)
{
    return (LPWSTR *)xmcalloc((size + 1) * sizeof(LPWSTR));
}

static LPWSTR xwcsdup(LPCWSTR s)
{
    size_t n;
    LPWSTR d;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = wcslen(s);
    d = xwmalloc(n + 2);
    return wmemcpy(d, s, n);
}

static LPWSTR xwcsndup(LPCWSTR s, size_t n)
{
    LPWSTR d;

    if (IS_EMPTY_WCS(s) || (n < 1))
        return NULL;
    d = xwmalloc(n + 2);
    return wmemcpy(d, s, n);
}

static __inline void xmemzero(void *mem, size_t number, size_t size)
{
    memset(mem, 0, number * size);
}

static __inline int xwcslen(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    else
        return (int)wcslen(s);
}

static __inline int xstrlen(LPCSTR s)
{
    if (IS_EMPTY_STR(s))
        return 0;
    else
        return (int)strlen(s);
}

static __inline int xtolower(int ch)
{
    if ((ch > 64) && (ch < 91))
        return ch + 32;
    else
        return ch;
}

static __inline int xtoupper(int ch)
{
    if ((ch > 96) && (ch < 123))
        return ch - 32;
    else
        return ch;
}

static __inline int xisupper(int ch)
{
    if ((ch > 64) && (ch < 91))
        return 1;
    else
        return 0;
}

static __inline void xwcslower(LPWSTR str)
{
    for (; *str != 0; str++)
        *str = xtolower(*str);
}

static __inline void xwcsupper(LPWSTR str)
{
    for (; *str != 0; str++)
        *str = xtoupper(*str);
}

static __inline int xisblank(int ch)
{
    if ((ch > 0) && (ch < 33))
        return 1;
    else
        return 0;
}

static __inline int xisalpha(int ch)
{
    if (((ch > 64) && (ch < 91)) || ((ch > 96) && (ch < 123)))
        return 1;
    else
        return 0;
}

static __inline int xisalnum(int ch)
{
    if (((ch > 64) && (ch < 91)) || ((ch > 96) && (ch < 123)) ||
        ((ch > 47) && (ch < 58)))
        return 1;
    else
        return 0;
}

static __inline int xisdigit(int ch)
{
    if ((ch > 47) && (ch < 58))
        return 1;
    else
        return 0;
}

static __inline int xisxdigit(int ch)
{
    if (((ch > 47) && (ch <  58)) ||
        ((ch > 64) && (ch <  71)) ||
        ((ch > 96) && (ch < 103)))
        return 1;
    else
        return 0;
}

static __inline int xtoxdigit(int ch)
{
    if ((ch > 47) && (ch <  58))
        return ch - 48;
    if ((ch > 64) && (ch <  71))
        return ch - 55;
    if ((ch > 96) && (ch < 103))
        return ch - 87;
    else
        return 16;
}

static __inline LPWSTR xskipblanks(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return (LPWSTR)s;
    while (xisblank(*s))
        s++;
    return (LPWSTR)s;
}

static __inline int xiswcschar(LPCWSTR s, WCHAR c)
{
    if ((s[0] == c) && (s[1] == WNUL))
        return 1;
    else
        return 0;
}

static __inline int xisvalidvarchar(int ch)
{
    if (xisalnum(ch) || (ch == L'_'))
        return 1;
    else
        return 0;
}

static __inline void xchrreplace(LPWSTR str, WCHAR a, WCHAR b)
{
    for (; *str != WNUL; str++) {
        if (*str == a)
            *str =  b;
    }
}

static __inline void xwinpathsep(LPWSTR str)
{
    for (; *str != WNUL; str++) {
        if (*str == L'/')
            *str = L'\\';
    }
}

static __inline void xunxpathsep(LPWSTR str)
{
    for (; *str != WNUL; str++) {
        if (*str == L'\\')
            *str = L'/';
    }
}

static __inline void xpprefix(LPWSTR str)
{
    str[0] = L'\\';
    str[1] = L'\\';
    str[2] = L'?';
    str[3] = L'\\';
}

static __inline int xispprefix(LPCWSTR str)
{
    if (IS_EMPTY_WCS(str))
        return 0;
    if ((str[0] == L'\\') &&
        (str[1] == L'\\') &&
        (str[2] == L'?')  &&
        (str[3] == L'\\'))
        return 1;
    else
        return 0;
}

static __inline LPWSTR xnopprefix(LPCWSTR str)
{
    if (IS_EMPTY_WCS(str))
        return zerostring;
    if ((str[0] == L'\\') &&
        (str[1] == L'\\') &&
        (str[2] == L'?')  &&
        (str[3] == L'\\'))
        return (LPWSTR)(str + 4);
    else
        return (LPWSTR)str;
}

static __inline int xmemalign(unsigned int s)
{
    unsigned int n;
    /**
     * Align to MEMORY_ALLOCATION_ALIGNMENT bytes for sizes lower then 64k
     * For sizes larger then 64k, align to the next 64k
     */
    n = s > 65535 ? 65536 : MEMORY_ALLOCATION_ALIGNMENT;
    while (n < s)
        n = n << 1;
    return n;
}

static int xwbsinit(LPSVCBATCH_WBUFFER wb, int len)
{
    ASSERT_NULL(wb, -1);
    wb->pos = 0;
    wb->siz = xmemalign(len);
    wb->buf = xwmalloc(wb->siz);
    return 0;
}

static int xwbsaddwch(LPSVCBATCH_WBUFFER wb, WCHAR ch)
{
    int c;
    LPWSTR p;

    ASSERT_NULL(wb, -1);
    if (wb->siz == 0) {
        wb->pos  = 0;
        wb->buf  = NULL;
    }
    c = wb->pos + 2;
    p = wb->buf;
    if (c >= wb->siz) {
        wb->siz = xmemalign(c + 1);
        wb->buf = (LPWSTR)xrealloc(p, wb->siz * sizeof(WCHAR));
    }
    wb->buf[wb->pos++] = ch;
    return 0;
}

static int xwbsaddwcs(LPSVCBATCH_WBUFFER wb, LPCWSTR str, int len)
{
    int c;
    LPWSTR p;

    ASSERT_NULL(wb, -1);
    if (len == 0)
        len = xwcslen(str);
    if (len == 0)
        return 0;
    if (wb->siz == 0) {
        wb->pos  = 0;
        wb->buf  = NULL;
    }
    c = wb->pos + len + 2;
    p = wb->buf;
    if (c >= wb->siz) {
        wb->siz = xmemalign(c + 1);
        wb->buf = (LPWSTR)xrealloc(p, wb->siz * sizeof(WCHAR));
    }
    if (len) {
        wmemcpy(wb->buf + wb->pos, str, len);
        wb->pos += len;
    }
    return 0;
}

static int xwbsaddnum(LPSVCBATCH_WBUFFER wb, DWORD n, int np, int pc)
{
    WCHAR  b[NBUFSIZ];
    LPWSTR s;
    int    c = 0;

    ASSERT_NULL(wb, -1);
    s = b + NBUFSIZ;
    *(--s) = WNUL;
    do {
        *(--s) = L'0' + (WCHAR)(n % 10);
        n /= 10;
        c++;
    } while (n);
    while ((np > c) && (s > b)) {
        *(--s) = pc;
        np--;
    }
    return xwbsaddwcs(wb, s, 0);
}

static int xwbsfinish(LPSVCBATCH_WBUFFER wb)
{
    int    c;
    LPWSTR p;

    ASSERT_NULL(wb, -1);

    if (wb->buf && wb->siz) {
        c = wb->pos + 1;
        p = wb->buf;
        if (c >= wb->siz) {
            wb->siz = xmemalign(c + 1);
            wb->buf = (LPWSTR)xrealloc(p, wb->siz * sizeof(WCHAR));
        }
        wb->buf[wb->pos] = WNUL;
    }
    return 0;
}

static LPWSTR xwbsdata(LPSVCBATCH_WBUFFER wb)
{
    ASSERT_NULL(wb, NULL);

    if (xwbsfinish(wb))
        return NULL;
    if (wb->pos)
        return wb->buf;
    else
        return NULL;
}


static int xmszlen(LPCWSTR s)
{
    int     n;

    if (IS_EMPTY_WCS(s))
        return 0;
    for (n = 0; *s; s++, n++) {
        while (*s) {
            n++;
            s++;
        }
    }
    return n;
}

static LPWSTR xwcschr(LPCWSTR str, int c)
{
    ASSERT_WSTR(str, NULL);
    while (*str) {
        if (*str == c)
            return (LPWSTR)str;
        str++;
    }
    return NULL;
}

static LPWSTR xwcsrchr(LPCWSTR str, int c)
{
    LPCWSTR s = str;

    ASSERT_WSTR(s, NULL);
    while (*s)
        s++;
    while (--s >= str) {
        if (*s == c)
            return (LPWSTR)s;
    }
    return NULL;
}

static LPCWSTR xwcsbegins(LPCWSTR src, LPCWSTR str)
{
    LPCWSTR pos = src;
    int sa;
    int sb;

    if (IS_EMPTY_WCS(src))
        return NULL;
    if (IS_EMPTY_WCS(str))
        return NULL;
    while (*src) {
        if (*str == WNUL)
            return pos;
        sa = xtolower(*src++);
        sb = xtolower(*str++);
        if (sa != sb)
            return NULL;
        pos++;
    }
    return *str ? NULL : pos;
}

static LPCWSTR xwcsendswith(LPCWSTR src, LPCWSTR str)
{
    LPCWSTR pos;
    int sa;
    int sb;

    if (IS_EMPTY_WCS(src))
        return NULL;
    if (IS_EMPTY_WCS(str))
        return NULL;
    sa = xwcslen(src);
    sb = xwcslen(str);
    if (sb > sa)
        return NULL;
    pos = src + sa - sb;
    src = pos;
    while (*src) {
        if (xtolower(*src++) != xtolower(*str++))
            return NULL;
    }
    return pos;
}

static int xwcsequals(LPCWSTR str, LPCWSTR src)
{
    int sa;

    if (IS_EMPTY_WCS(str))
        return 0;
    while ((sa = xtolower(*str++)) == xtolower(*src++)) {
        if (sa == 0)
            return 1;
    }
    return 0;
}

/**
 * Count the number of tokens delimited by d
 */
static int xwcsntok(LPCWSTR s, int d)
{
    int n = 1;

    if (IS_EMPTY_WCS(s))
        return 0;
    while (*s == d) {
        s++;
    }
    if (*s == WNUL)
        return 0;
    while (*s != WNUL) {
        if (*(s++) == d) {
            while (*s == d) {
                s++;
            }
            if (*s != WNUL)
                n++;
        }
    }
    return n;
}

/**
 * This is wcstok clone using single character as token delimiter
 */
static LPWSTR xwcsctok(LPWSTR s, int d, LPWSTR *c)
{
    LPWSTR p;

    ASSERT_NULL(c, NULL);
    if ((s == NULL) && ((s = *c) == NULL))
        return NULL;

    *c = NULL;
    /**
     * Skip leading delimiter
     */
    while (*s == d) {
        s++;
    }
    if (*s == WNUL)
        return NULL;
    p = s;

    while (*s != WNUL) {
        if (*s == d) {
            *s = WNUL;
            *c = s + 1;
            break;
        }
        s++;
    }
    return p;
}

static int xisvalidvarname(LPCWSTR s)
{
    if (IS_EMPTY_WCS(s))
        return 0;
    for (; *s != WNUL; s++) {
        if (!xisvalidvarchar(*s))
            return 0;
    }
    return 1;
}

static LPWSTR xwcsconcat(LPCWSTR s1, LPCWSTR s2)
{
    LPWSTR rs;
    int    l1;
    int    l2;

    l1 = xwcslen(s1);
    l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;
    rs = xwmalloc(l1 + l2 + 1);

    if (l1 > 0)
        wmemcpy(rs,  s1, l1);
    if (l2 > 0)
        wmemcpy(rs + l1, s2, l2);
    return rs;
}

static LPWSTR xwcsappend(LPWSTR s1, LPCWSTR s2)
{
    LPWSTR rs;
    int    l1;
    int    l2;

    l1 = xwcslen(s1);
    l2 = xwcslen(s2);

    if ((l1 + l2) == 0)
        return NULL;
    rs = (LPWSTR)xrealloc(s1, (l1 + l2 + 1) * sizeof(WCHAR));
    if (l2 > 0)
        wmemcpy(rs + l1, s2, l2);
    return rs;
}

static LPWSTR xwcspbrk(LPCWSTR str, LPCWSTR set)
{
    LPCWSTR p = str;
    LPCWSTR q;

    ASSERT_WSTR(str, NULL);
    ASSERT_WSTR(set, NULL);
    while (*p) {
        q = set;
        while (*q) {
            if (*p == *q)
                return (LPWSTR)p;
            q++;
        }
        p++;
    }
    return NULL;
}

static LPWSTR xargvtomsz(int argc, LPCWSTR *argv, DWORD *sz)
{
    int    i;
    int    len = 0;
    int    s[SBUFSIZ];
    LPWSTR ep;
    LPWSTR bp;

    ASSERT_ZERO(argc, NULL);
    ASSERT_LESS(argc, SBUFSIZ, NULL);

    for (i = 0; i < argc; i++) {
        s[i]  = xwcslen(argv[i]) + 1;
        len  += s[i];
    }
    bp = xwmalloc(++len);
    ep = bp;
    for (i = 0; i < argc; i++) {
        if (s[i] > 1)
            wmemcpy(ep, argv[i], s[i]);
        ep += s[i];
    }
    *ep = WNUL;
    *sz = len * 2;
    return bp;
}

static LPWSTR xstrtomsz(LPCWSTR s, int c, int *b)
{
    int    i;
    int    n;
    int    x;
    LPWSTR d;

    ASSERT_WSTR(s, NULL);

    n = xwcslen(s);
    d = xwmalloc(n + 2);
    i = 0;
    while (s[i] == c)
        i++;
    for (x = 0; i < n; i++) {
        if (s[i] == c) {
            while (s[i+1] == c)
                i++;
            if (s[i+1] != WNUL)
                d[x++] = WNUL;
        }
        else {
            d[x++] = s[i];
        }
    }
    d[x++] = WNUL;
    d[x++] = WNUL;
    if (b != NULL)
        *b = x * 2;
    return d;
}


/**
 * Simple atoi with ranges between 0 and INT_MAX.
 * Leading white space characters are ignored.
 * Returns negative value on error.
 */
static int xwcstoi(LPCWSTR sp, LPWSTR *ep)
{
    LONGLONG rv = INT64_ZERO;
    int dc = 0;

    if (ep != NULL)
        *ep = zerostring;
    sp = xskipblanks(sp);
    ASSERT_WSTR(sp, -1);
    if (ep != NULL)
        *ep = (LPWSTR)sp;
    while (xisdigit(*sp)) {
        int dv = *sp - L'0';

        if (dv || rv) {
            rv *= 10;
            rv += dv;
        }
        if (rv >= INT_MAX) {
            SetLastError(ERROR_INVALID_DATA);
            return -1;
        }
        dc++;
        sp++;
    }
    if (dc == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    sp = xskipblanks(sp);
    if (ep != NULL) {
        *ep = (LPWSTR)sp;
    }
    else {
        if (*sp != WNUL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
        }
    }
    return (int)rv;
}

static DWORD xwcstod(LPCWSTR sp, DWORD *rp)
{
    ULONGLONG rv = UINT64_ZERO;
    int dc = 0;

    sp = xskipblanks(sp);
    ASSERT_WSTR(sp, ERROR_INVALID_PARAMETER);
    while (xisdigit(*sp)) {
        DWORD dv = *sp - L'0';

        if (dv || rv) {
            rv *= 10;
            rv += dv;
        }
        if (rv >= UINT_MAX)
            return ERROR_INVALID_DATA;
        dc++;
        sp++;
    }
    if (dc == 0)
        return ERROR_INVALID_PARAMETER;
    sp = xskipblanks(sp);
    if (*sp != WNUL)
        return ERROR_INVALID_PARAMETER;
    *rp = (DWORD)rv;
    return ERROR_SUCCESS;
}

static DWORD xwcxtoq(LPCWSTR sp, LPHANDLE h)
{
    LPCWSTR   pp;
    ULONGLONG rv = UINT64_ZERO;
    int dc = 0;

    *h = INVALID_HANDLE_VALUE;
    sp = xskipblanks(sp);
    ASSERT_WSTR(sp, ERROR_INVALID_PARAMETER);
    pp = sp;
    while (*sp == L'0')
        sp++;
    while (xisxdigit(*sp)) {
        DWORD dv;

        if (dc > 15)
            return ERROR_INVALID_DATA;
        dv = xtoxdigit(*sp);
        if (dv || rv) {
            rv *= 16;
            rv += dv;
        }
        dc++;
        sp++;
    }
    if ((dc == 0) && (*pp != L'0'))
        return ERROR_INVALID_PARAMETER;
    sp = xskipblanks(sp);
    if (*sp != WNUL)
        return ERROR_INVALID_PARAMETER;
    *h = (HANDLE)rv;
    return ERROR_SUCCESS;
}

static DWORD xcsbtob(LPCWSTR sp, LPBYTE *rb, DWORD *rc)
{
    LPBYTE  b = NULL;
    LPCWSTR p;
    DWORD   c = 0;
    int     x = 0;

    ASSERT_WSTR(sp, ERROR_INVALID_PARAMETER);
    sp = xskipblanks(sp);
    p  = xwcsbegins(sp, L"hex:");
    if (p != NULL) {
        sp = p;
        while (*p) {
            if (*p == L',')
                c++;
            p++;
        }
    }
    else {
        c = xwcslen(sp);
        x = 1;
    }
    if (c >= SVCBATCH_LINE_MAX)
        return ERROR_FILE_TOO_LARGE;
    b = xmmalloc(c + 1);
    p = sp;
    c = 0;
    while (*p) {
        int h;
        int l;
        int w;

        if (x) {
            w = *(p++);
            if (w == '\\') {
                w = *(p++);
                if (w != 'x') {
                    switch (w) {
                        case 'r':
                            b[c++] = '\r';
                        break;
                        case 'n':
                            b[c++] = '\n';
                        break;
                        default:
                            b[c++] = (BYTE)(w);
                        break;
                    }
                    continue;
                }
            }
            else {
                b[c++] = (BYTE)(w);
                continue;
            }
        }
        h = xtoxdigit(*(p++));
        if (h > 15)
            return ERROR_INVALID_DATA;
        l = xtoxdigit(*(p++));
        if (l > 15)
            return ERROR_INVALID_DATA;
        b[c++] = (BYTE)(h << 4) + (BYTE)(l);
        if ((x == 0) && (*p == L','))
            p++;
    }
    *rb = b;
    *rc = c;
    return ERROR_SUCCESS;
}

static LPCWSTR xntowcs(DWORD n)
{
    static WCHAR b[TBUFSIZ];
    LPWSTR s;

    s = b + TBUFSIZ;
    *(--s) = WNUL;
    do {
        *(--s) = L'0' + (WCHAR)(n % 10);
        n /= 10;
    } while (n);

    return s;
}

static int xxtowcb(HANDLE h, LPWSTR b, int u, int p)
{
    WCHAR     w[TBUFSIZ];
    LPWSTR    s;
    LPCWSTR   x;
    ULONGLONG n = (ULONGLONG)h;
    int       c = 0;

    s = w + TBUFSIZ;
    x = u ? hexuchars : hexwchars;
    *(--s) = WNUL;
    do {
        *(--s) = x[n & 0x0F];
        n = n >> 4;
        c++;
    } while (n);
    p -= c;
    while (p > 0) {
        *(--s) = L'0';
        p--;
        c++;
    }
    wmemcpy(b, s, c + 1);
    return c;
}


static LPCWSTR xwctowcs(int c)
{
    static WCHAR b[] = { 0, 0, 0, 0 };

    b[0] = c;
    return b;
}

static int xwcslcat(LPWSTR dst, int siz, int pos, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst + pos;
    int     c = pos;
    int     n;

    ASSERT_NULL(dst, 0);
    n = siz - pos;
    if (n < 2)
        return siz;
    *d = WNUL;
    ASSERT_WSTR(src, c);

    while ((n-- != 1) && (*s != WNUL)) {
        *d++ = *s++;
         c++;
    }
    *d = WNUL;
    if (*s != WNUL)
        c++;
    return c;
}

static int xwcslcpyn(LPWSTR dst, int siz, LPCWSTR src, int m)
{
    LPCWSTR s = src;
    LPWSTR  d = dst;
    int     n = siz;
    int     c = 0;

    ASSERT_NULL(d, 0);
    ASSERT_SIZE(n, 2, 0);
    *d = WNUL;
    ASSERT_WSTR(s, 0);
    ASSERT_SIZE(m, 1, 0);

    while ((n-- != 1) && (m-- != 0) && (*s != WNUL)) {
        *d++ = *s++;
         c++;
    }
    *d = WNUL;
    if ((m > 0) && (*s != WNUL))
        c++;
    return c;
}

static int xwcslcpy(LPWSTR dst, int siz, LPCWSTR src)
{
    LPCWSTR s = src;
    LPWSTR  d = dst;
    int     n = siz;
    int     c = 0;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);
    *d = WNUL;
    ASSERT_WSTR(src, 0);

    while ((n-- != 1) && (*s != WNUL)) {
        *d++ = *s++;
         c++;
    }
    *d = WNUL;
    if (*s != WNUL)
        c++;
    return c;
}

static int xvsnwprintf(LPWSTR dst, int siz,
                       LPCWSTR fmt, va_list ap)
{
    int c = siz - 1;
    int n;
    va_list cp;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);
    *dst = WNUL;
    ASSERT_WSTR(fmt, 0);

    va_copy(cp, ap);
    n = _vsnwprintf(dst, c, fmt, cp);
    va_end(cp);
    if (n < 0)
        n = 0;
    if (n > c)
        n = c;
    dst[n] = WNUL;
    return n;
}

static int xsnwprintf(LPWSTR dst, int siz, LPCWSTR fmt, ...)
{
    int     rv;
    va_list ap;

    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);
    *dst = WNUL;
    ASSERT_WSTR(fmt, 0);

    va_start(ap, fmt);
    rv = xvsnwprintf(dst, siz, fmt, ap);
    va_end(ap);
    return rv;

}

static int xvsnprintf(char *dst, int siz,
                      LPCSTR fmt, va_list ap)
{
    int c = siz - 1;
    int n;
    va_list cp;

    ASSERT_CSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);

    dst[0] = '\0';
    va_copy(cp, ap);
    n = _vsnprintf(dst, c, fmt, cp);
    va_end(cp);
    if (n < 0)
        n = 0;
    if (n > c)
        n = c;
    dst[n] = '\0';
    return n;
}

static int xsnprintf(char *dst, int siz, LPCSTR fmt, ...)
{
    int     rv;
    va_list ap;

    ASSERT_CSTR(fmt, 0);
    ASSERT_NULL(dst, 0);
    ASSERT_SIZE(siz, 2, 0);

    va_start(ap, fmt);
    rv = xvsnprintf(dst, siz, fmt, ap);
    va_end(ap);
    return rv;

}

static LPWSTR xuuidstring(LPWSTR b, int h, int u)
{
    BYTE d[16];
    int  i;
    int  x;
    LPCWSTR hc;

    if (BCryptGenRandom(NULL, d, 16,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return NULL;
    if (b == NULL)
        b = xwmalloc(WBUFSIZ);

    hc = u ? hexuchars : hexwchars;
    for (i = 0, x = 0; i < 16; i++) {
        if (h) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                b[x++] = L'-';
        }
        b[x++] = hc[d[i] >> 4];
        b[x++] = hc[d[i] & 0x0F];
    }
    b[x] = WNUL;
    return b;
}

static int getdayofyear(int y, int m, int d)
{
    static const int dayoffset[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int r;

    r = dayoffset[m - 1] + d;
    if (IS_LEAP_YEAR(y) && (r > 59))
        r++;
    return r;
}

static int xchkftime(LPCWSTR fmt)
{
    LPCWSTR s = fmt;

    ASSERT_WSTR(s, -1);
    while (*s) {
        if (*s == L'@') {
            s++;
            if (xwcschr(L"@123456FHLMSYdjmswy", *s) == NULL) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return -1;
            }
        }
        s++;
    }
    return 0;
}

static LPWSTR xwcsftime(LPCWSTR fmt)
{
    static const int m[8] = { 10, 10, 100, 1000, 10000, 100000, 1000000, 10000000 };
    int              c = 0;
    LPCWSTR          s = fmt;
    WCHAR            ub[WBUFSIZ];
    SYSTEMTIME       tm;
    SVCBATCH_WBUFFER wb;

    ASSERT_WSTR(s, NULL);
    if (xwbsinit(&wb, SVCBATCH_NAME_MAX))
        return NULL;

    if (IS_OPT_SET(SVCBATCH_OPT_LOCALTIME))
        GetLocalTime(&tm);
    else
        GetSystemTime(&tm);

    while (*s) {
        if (*s == L'@') {
            int w;
            s++;
            switch (*s) {
                case L'@':
                    xwbsaddwch(&wb, L'@');
                break;
                case L'y':
                    xwbsaddwch(&wb, tm.wYear % 100 / 10 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 10  + L'0');
                break;
                case L'Y':
                    xwbsaddwch(&wb, tm.wYear / 1000 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 1000 / 100 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 100  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 10   + L'0');
                break;
                case L'd':
                    xwbsaddwch(&wb, tm.wDay  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wDay % 10 + L'0');
                break;
                case L'm':
                    xwbsaddwch(&wb, tm.wMonth / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMonth % 10 + L'0');
                break;
                case L'H':
                    xwbsaddwch(&wb, tm.wHour / 10 + L'0');
                    xwbsaddwch(&wb, tm.wHour % 10 + L'0');
                break;
                case L'M':
                    xwbsaddwch(&wb, tm.wMinute / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMinute % 10 + L'0');
                break;
                case L'S':
                    xwbsaddwch(&wb, tm.wSecond / 10 + L'0');
                    xwbsaddwch(&wb, tm.wSecond % 10 + L'0');
                break;
                case L'j':
                    w = getdayofyear(tm.wYear, tm.wMonth, tm.wDay);
                    xwbsaddwch(&wb, w / 100 + L'0');
                    xwbsaddwch(&wb, w % 100 / 10 + L'0');
                    xwbsaddwch(&wb, w % 10  + L'0');
                break;
                case 'L':
                    xwbsaddwch(&wb, tm.wYear   / 1000 + L'0');
                    xwbsaddwch(&wb, tm.wYear   % 1000 / 100 + L'0');
                    xwbsaddwch(&wb, tm.wYear   % 100  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wYear   % 10 + L'0');
                    xwbsaddwch(&wb, tm.wMonth  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMonth  % 10 + L'0');
                    xwbsaddwch(&wb, tm.wDay    / 10 + L'0');
                    xwbsaddwch(&wb, tm.wDay    % 10 + L'0');
                    xwbsaddwch(&wb, tm.wHour   / 10 + L'0');
                    xwbsaddwch(&wb, tm.wHour   % 10 + L'0');
                    xwbsaddwch(&wb, tm.wMinute / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMinute % 10 + L'0');
                    xwbsaddwch(&wb, tm.wSecond / 10 + L'0');
                    xwbsaddwch(&wb, tm.wSecond % 10 + L'0');
                break;
                case L'F':
                    xwbsaddwch(&wb, tm.wYear / 1000 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 1000 / 100 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 100  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wYear % 10   + L'0');
                    xwbsaddwch(&wb, L'-');
                    xwbsaddwch(&wb, tm.wMonth / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMonth % 10 + L'0');
                    xwbsaddwch(&wb, L'-');
                    xwbsaddwch(&wb, tm.wDay  / 10 + L'0');
                    xwbsaddwch(&wb, tm.wDay  % 10 + L'0');
                break;
                case L'w':
                    xwbsaddwch(&wb, L'0' + tm.wDayOfWeek);
                break;
                /** Custom formatting codes */
                case L's':
                    xwbsaddwch(&wb, tm.wMilliseconds / 100 + L'0');
                    xwbsaddwch(&wb, tm.wMilliseconds % 100 / 10 + L'0');
                    xwbsaddwch(&wb, tm.wMilliseconds % 10 + L'0');
                break;
                case L'1':
                case L'2':
                case L'3':
                case L'4':
                case L'5':
                case L'6':
                    w = *s - L'0';
                    xwbsaddnum(&wb, runcounter % m[w], w, L'0');
                    c++;
                break;
                case L'r':
                case L'R':
                    xuuidstring(ub, 0, xisupper(*s));
                    xwbsaddwcs(&wb, ub, 0);
                break;
                case L'u':
                case L'U':
                    xuuidstring(ub, 1, xisupper(*s));
                    xwbsaddwcs(&wb, ub, 0);
                break;
                default:
                    xfree(wb.buf);
                    SetLastError(ERROR_INVALID_PARAMETER);
                    return NULL;
                break;
            }
        }
        else {
            xwbsaddwch(&wb, *s);
        }
        s++;
    }
    if (c > 0)
        InterlockedIncrement(&runcounter);

    return xwbsdata(&wb);
}

static int xnamemap(LPCWSTR src, SVCBATCH_NAME_MAP const *map, int *type, int def)
{
    int i;

    if (IS_EMPTY_WCS(src))
        return def;
    for (i = 0; map[i].name != NULL; i++) {
        if (xwcsequals(src, map[i].name)) {
            if (type)
                *type = map[i].type;
            return map[i].code;
        }
    }
    return def;
}

static LPCWSTR xcodemap(SVCBATCH_NAME_MAP const *map, int c)
{
    int i;

    for (i = 0; map[i].name != NULL; i++) {
        if (map[i].code == c)
            return map[i].name;
    }
    return zerostring;
}

static DWORD xwcstob(LPCWSTR str, DWORD *ret)
{
    int r;

    ASSERT_NULL(ret, ERROR_INVALID_PARAMETER);
    ASSERT_WSTR(str, ERROR_INVALID_DATA);
    if (xisdigit(str[0]) && (str[1] == WNUL)) {
        *ret = str[0] - L'0';
        return ERROR_SUCCESS;
    }
    r = xnamemap(str, boolnamemap, NULL, -1);
    if (r < 0)
        return ERROR_INVALID_PARAMETER;
    *ret = r;
    return ERROR_SUCCESS;
}

static int xsetsysvar(int iid, LPCWSTR key, LPCWSTR val)
{
    int idx = iid - 64;

    ASSERT_SPAN(idx, 1, 26, -1);

    svariables->var[idx].iid = iid;
    svariables->var[idx].key = key;
    svariables->var[idx].val = val;

    return 0;
}

static LPCWSTR xgetsysvar(LPCWSTR name, LPCWSTR set)
{
    int i;

    for (i = 1; i < SYSVARS_COUNT; i++) {
        if (svariables->var[i].iid) {
            if (xwcsequals(svariables->var[i].key, name)) {
                if ((set != NULL) && (xwcschr(set, svariables->var[i].iid) == NULL))
                    return NULL;
                else
                    return svariables->var[i].val;
            }
        }
    }
    return NULL;
}

static int xaddsysvar(int iid, LPCWSTR key, LPCWSTR val)
{
    if (svariables->pos == svariables->siz) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return 0;
    }
    svariables->var[svariables->pos].iid = iid;
    svariables->var[svariables->pos].key = key;
    svariables->var[svariables->pos].val = val;
    return svariables->pos++;
}

static LPWSTR xappendarg(int qp, LPWSTR s1, LPCWSTR s2)
{
    LPCWSTR c;
    LPWSTR  e;
    LPWSTR  d;

    int l1;
    int l2;
    int nn;
    int nq = qp;

    l2 = xwcslen(s2);
    if (l2 == 0)
        return s1;
    if (l2 <  3)
        nq = 0;
    if (nq) {
        nq = 0;
        if (*s2 == L'"') {
            /**
             * Presume the user provided properly
             * quoted argument.
             */
        }
        else if (xwcspbrk(s2, L" \t\"")) {
            for (c = s2; ; c++, nq++) {
                int b = 0;

                while (*c == L'\\') {
                    b++;
                    c++;
                }

                if (*c == WNUL) {
                    nq += b * 2;
                    break;
                }
                else {
                    if (*c == L'"')
                        nq += b * 2 + 1;
                    else
                        nq += b;
                }
            }
            l2 = nq + 2;
        }
    }
    l1 = xwcslen(s1);
    nn = l1 + l2 + 2;
    e  = (LPWSTR)xrealloc(s1, nn * sizeof(WCHAR));
    d  = e;

    if (l1) {
        d += l1;
        *(d++) = L' ';
    }
    if (nq) {
        *(d++) = L'"';
        for (c = s2; ; c++, d++) {
            int b = 0;

            while (*c == '\\') {
                b++;
                c++;
            }

            if (*c == WNUL) {
                if (b) {
                    wmemset(d, L'\\', b * 2);
                    d += b * 2;
                }
                break;
            }
            else {
                if (*c == L'"') {
                    wmemset(d, L'\\', b * 2 + 1);
                    d += b * 2 + 1;
                }
                else {
                    if (b) {
                        wmemset(d, L'\\', b);
                        d += b;
                    }
                }
                *d = *c;
            }
        }
        *(d++) = L'"';
    }
    else {
        wmemcpy(d, s2, l2);
        d += l2;
    }
    *d = WNUL;
    return e;
}

static int xlongopt(int nargc, LPCWSTR *nargv,
                    SVCBATCH_LONGOPT const *options, LPCWSTR allowed)
{
    int i = 0;

    xwoptarg = NULL;
    if (xwoptind >= nargc) {
        /* No more arguments */
        return 0;
    }
    xwoption = nargv[xwoptind];
    if (xwoptarr) {
        if (xiswcschar(xwoption, L']')) {
            if (xwoptend < 1)
                return 0;
            xwoptarg = xwoption;
            xwoptind++;
            xwoptend--;
            if (xwoptend > 0)
                return xwoptarr;
            else
                return ']';
        }
        if (xiswcschar(xwoption, L'[')) {
            xwoptarg = xwoption;
            xwoptind++;
            xwoptend++;
            if (xwoptend > 1)
                return xwoptarr;
            else
                return '[';
        }
        if (xwoptend) {
            xwoptarg = xwoption;
            xwoptind++;
            return xwoptarr;
        }
    }
    xwoptend = 0;
    xwoptarr = 0;
    if (xwoption[0] != L'-') {
        /* Not an option */
        return 0;
    }
    if (xwoption[1] != L'-') {
        /* The single '-'  is command delimiter */
        if (xwoption[1] == WNUL)
            xwoptind++;
        return 0;
    }
    if (xwoption[2] == WNUL) {
        /* The single '--' is command delimiter */
        xwoptind++;
        return 0;
    }

    while (options[i].option) {
        int     optsep = 0;
        LPCWSTR optopt = NULL;
        LPCWSTR optstr = xwoption + 2;

        if (options[i].mode == '.') {
            if (xwcsequals(optstr, options[i].name))
                optopt = zerostring;
        }
        else if (options[i].mode == '+') {
            if (xwcsequals(optstr, options[i].name))
                optopt = zerostring;
        }
        else {
            LPCWSTR oo = xwcsbegins(optstr, options[i].name);
            if (oo) {
                /* Check for --option= or --option: */
                if (*oo == WNUL) {
                    optopt = zerostring;
                }
                else if ((*oo == L'=') || (*oo == L':')) {
                    optsep = *oo;
                    optopt =  oo + 1;
                }
            }
        }
        if (optopt == NULL) {
            i++;
            continue;
        }
        /* Found long option */
        if (xwcschr(allowed, options[i].option) == NULL) {
            /**
             * The --option is not enabled for the
             * current command.
             *
             */
            if (*allowed != L'!')
                return EACCES;
        }
        else {
            if (*allowed == L'!')
                return EACCES;
        }
        if (options[i].mode == '.') {
            /* No arguments needed */
            xwoptind++;
            return options[i].option;
        }
        /* Skip blanks */
        while (xisblank(*optopt))
            optopt++;
        if (*optopt) {
            if (options[i].mode == '+') {
                /* Argument must be on the next line */
                return ERROR_INVALID_DATA;
            }
            /* Argument is part of the option */
            xwoptarg = optopt;
            xwoptind++;
            return options[i].option;
        }
        if (optsep) {
            /* Empty in place argument */
            return ERROR_BAD_LENGTH;
        }
        if (options[i].mode == '?') {
            /* No optional argument */
            xwoptind++;
            return options[i].option;
        }
        if (nargc > xwoptind)
            optopt = nargv[++xwoptind];
        while (xisblank(*optopt))
            optopt++;
        if (*optopt == WNUL)
            return ERROR_BAD_LENGTH;
        xwoptind++;
        xwoptarg = optopt;
        return options[i].option;
    }
    /* Option not found */
    return ERROR_INVALID_FUNCTION;
}

static int xwgetopt(int nargc, LPCWSTR *nargv, LPCWSTR opts)
{
    LPCWSTR  optpos;
    LPCWSTR  optarg;
    int      optmod;
    int      option;
    int      optsep;

    xwoptarg = NULL;
    if (xwoptind >= nargc)
        return 0;
    xwoption = nargv[xwoptind];
    if ((xwoption[0] != L'-') && (xwoption[0] != L'/'))
        return 0;
    if (xwoption[1] == WNUL) {
        if (xwoption[0] == L'-') {
            /* The single '-' is command delimiter */
            xwoptind++;
            return 0;
        }
        else {
            /* The single '/' is error */
            return ERROR_BAD_FORMAT;
        }
    }
    option = xtolower(xwoption[1]);
    if ((option < 97) || (option > 122))
        return ERROR_INVALID_FUNCTION;
    optpos = xwcschr(opts, option);
    if (optpos == NULL)
        return ERROR_INVALID_FUNCTION;
    optmod = optpos[1];
    optsep = xwoption[2];
    if (optmod == L'!') {
        if (optsep) {
            /* Extra data */
            return ERROR_INVALID_DATA;
        }
        else {
            xwoptind++;
            return option;
        }
    }
    if ((xwoption[0] == L'/') && (optsep == L':')) {
        optarg = xwoption + 3;
        while (xisblank(*optarg))
            ++optarg;
        if (*optarg == WNUL) {
            /* Missing argument */
            return ERROR_BAD_LENGTH;
        }
        else {
            xwoptarg = optarg;
            xwoptind++;
            return option;
        }
    }
    if (optsep) {
        /* Extra data */
        return ERROR_INVALID_DATA;
    }
    if (optmod == L'?') {
        /* No optional inplace argument */
        xwoptind++;
        return option;
    }
    if (optmod == L':') {
        /* Missing inplace argument */
        return ERROR_BAD_LENGTH;
    }
    optarg = zerostring;
    if (nargc > xwoptind)
        optarg = nargv[++xwoptind];
    while (xisblank(*optarg))
        optarg++;
    if (*optarg == WNUL)
        return ERROR_BAD_LENGTH;
    xwoptind++;
    xwoptarg = optarg;
    return option;
}

#if HAVE_LOGDIR_MUTEX
static LPWSTR xgenresname(LPCWSTR name)
{
    static WCHAR b[SVCBATCH_NAME_MAX] = { L'L', L'o', L'c', L'a', L'l', L'\\', WNUL, WNUL };
    int n;
    int i = 6;

    n = xwcslen(name);
    if (n > 248)
        name += n - 248;
    n = xwcslcat(b, SVCBATCH_NAME_MAX - 2, i, name);
    for (; i < n; i++) {
        if (b[i] > 126)
            b[i] = 'a' + (b[i] % 26);
        else if ((b[i] == 47) || (b[i] == 58) || (b[i] == 92) || (b[i] == 95))
            b[i] = '-';
        else
            b[i] = xtolower(b[i]);
    }
    b[i] = WNUL;
    return b;
}
#endif
/**
 * Runtime debugging functions
 */

static const char *threadnames[] = {
    "workerthread",
    "stdinthread",
    "stopthread",
    "rotatethread",
    NULL
};

#if HAVE_DEBUG_TRACE
static void xtraceprintf(LPCSTR funcname, int line, int type, LPCSTR format, ...)
{
    static char sep = ':';
    int     n = SVCBATCH_LINE_MAX - 4;
    int     i = 0;
    int     o[8];
    char    b[SVCBATCH_LINE_MAX];
    SYSTEMTIME tm;
    HANDLE  h;

    GetLocalTime(&tm);
    b[i++] = '[';
    i += xsnprintf(b + i, n, "%lu", GetCurrentProcessId());
    o[0] = i;
    b[i++] = sep;
    i += xsnprintf(b + i, n, "%lu", GetCurrentThreadId());
    o[1]   = i;
    b[i++] = sep;
    b[i++] = tm.wMonth  / 10 + '0';
    b[i++] = tm.wMonth  % 10 + '0';
    b[i++] = tm.wDay    / 10 + '0';
    b[i++] = tm.wDay    % 10 + '0';
    b[i++] = tm.wYear   % 100 / 10 + '0';
    b[i++] = tm.wYear   % 10 + '0';
    b[i++] = '/';
    b[i++] = tm.wHour   / 10 + '0';
    b[i++] = tm.wHour   % 10 + '0';
    b[i++] = tm.wMinute / 10 + '0';
    b[i++] = tm.wMinute % 10 + '0';
    b[i++] = tm.wSecond / 10 + '0';
    b[i++] = tm.wSecond % 10 + '0';
    b[i++] = '.';
    b[i++] = tm.wMilliseconds / 100 + '0';
    b[i++] = tm.wMilliseconds % 100 / 10 + '0';
    b[i++] = tm.wMilliseconds % 10 + '0';
    o[2]   = i;
    b[i++] = sep;
    i += xsnprintf(b + i, n - i, "%s",
                   xtracesvctypes[type & 0x07]);
    o[3]   = i;
    b[i++] = sep;
    i += xsnprintf(b + i, n - i, "%s(%d)",
                   funcname, line);
    o[4]   = i;
    b[i++] = ']';
    if (format) {
        va_list ap;

        va_start(ap, format);
        b[i++] = ' ';
        i += xvsnprintf(b + i, n - i, format, ap);
        va_end(ap);
    }
    o[5] = i;
    EnterCriticalSection(&xtracesync);
    h = InterlockedExchangePointer(&xtracefhandle, NULL);
    if (IS_VALID_HANDLE(h)) {
        DWORD wr;

        b[i++] = '\r';
        b[i++] = '\n';
        WriteFile(h, b, i, &wr, NULL);
        InterlockedExchangePointer(&xtracefhandle, h);
    }
    LeaveCriticalSection(&xtracesync);
    if (xtraceservice > 1) {
        char s[SVCBATCH_LINE_MAX];

        for (i = 0; i < 6; i++)
            b[o[i]] = CNUL;
        xsnprintf(s, SVCBATCH_LINE_MAX, "%-4s %4s %-5s %s %-22s %s\r\n", b + 1,
                  b + o[0] + 1, b + o[2] + 1,
                  xtracesvcmodes[servicemode], b + o[3] + 1, b + o[4] + 1);
        OutputDebugStringA(s);
    }
}

static void xtraceprints(LPCSTR funcname, int line, int type, LPCSTR string)
{
    if (string == NULL)
        xtraceprintf(funcname, line, type, NULL, NULL);
    else
        xtraceprintf(funcname, line, type, "%s", string);
}
#endif

#if defined(_DEBUG)
static void xiphandler(LPCWSTR e,
                       LPCWSTR w, LPCWSTR f,
                       unsigned int n, uintptr_t r)
{
    DBG_PRINTS("invalid parameter handler called");
}

#endif

static int xwinapierror(LPWSTR buf, int siz, DWORD err)
{
    int n;
    ASSERT_SIZE(siz, SBUFSIZ, 0);
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       err,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       buf,
                       siz - 1,
                       NULL);
    if (n) {
        int i;
        do {
            buf[n--] = WNUL;
        } while ((n > 0) && ((buf[n] == 46) || (buf[n] < 33)));
        i = n++;
        while (i-- > 0) {
            if (buf[i] < 32)
                buf[i] = 32;
        }
    }
    else {
        n = xsnwprintf(buf, siz,
                       L"Unrecognized system error code: %u", err);
    }
    return n;
}

static int setupeventlog(void)
{
    static volatile LONG  ssrv = 0;
    static volatile LONG  eset = 0;
    static const    WCHAR md[] = L"%SystemRoot%\\System32\\netmsg.dll\0";
    DWORD c;
    HKEY  k;

    if (InterlockedIncrement(&eset) > 1)
        return ssrv;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        SYSTEM_SVC_SUBKEY \
                        L"\\EventLog\\Application\\" CPP_WIDEN(SVCBATCH_NAME),
                        0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &k, &c) != ERROR_SUCCESS)
        return 0;
    if (c == REG_CREATED_NEW_KEY) {
        DWORD dw = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
                   EVENTLOG_INFORMATION_TYPE;
        if (RegSetValueExW(k, L"EventMessageFile", 0, REG_EXPAND_SZ,
                          (const BYTE *)md, DSIZEOF(md)) != ERROR_SUCCESS)
            goto finished;
        if (RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD,
                          (const BYTE *)&dw, 4) != ERROR_SUCCESS)
            goto finished;
    }
    InterlockedIncrement(&ssrv);
finished:
    RegCloseKey(k);
    return ssrv;
}



static DWORD svcsyserror(LPCSTR fn, int line, WORD typ, DWORD ern, LPCWSTR err, LPCWSTR eds, LPCWSTR erp)
{
    WCHAR   buf[SVCBATCH_LINE_MAX];
    LPWSTR  dsc;
    LPCWSTR src = CPP_WIDEN(SVCBATCH_NAME);
    LPCWSTR msg[10];
    int     b = 0;
    int     c = 0;
    int     i = 0;
    int     n;
    int     siz;
    int     bsz = SVCBATCH_LINE_MAX - TBUFSIZ;

    if (service->name) {
        if (service->display)
            src = service->display;
        else
            src = service->name;
    }
    buf[0]   = WNUL;
    dsc      = buf;
    msg[i++] = buf;
    if (typ != EVENTLOG_INFORMATION_TYPE) {
        c = xsnwprintf(buf, BBUFSIZ, L"The %s service", src);
        if (typ == EVENTLOG_ERROR_TYPE)
            c = xwcslcat(buf, BBUFSIZ, c, L" reported the following error:\r\n");
        buf[c++] = WNUL;
        msg[i++] = buf + c;
        buf[c++] = L'\r';
        buf[c++] = L'\n';
        dsc = buf + c;
    }
    else {
        if (IS_VALID_WCS(err) || IS_VALID_WCS(eds)) {
        }
        else {
            if (ern == 0)
                c = xwcslcpy(buf, BBUFSIZ, SVCBATCH_MSG(0));
            else
                c = xwcslcpy(buf, BBUFSIZ, SVCBATCH_MSG(1));
            buf[c++] = WNUL;
        }
    }
    n   = 0;
    siz = bsz - c;
    if (IS_VALID_WCS(err)) {
        LPCWSTR pf = xwcschr(err, L'%');
        if (pf) {
            n = xsnwprintf(dsc, siz, err, eds, erp);
            if (xwcschr(pf + 2, L'%'))
                erp = NULL;
        }
        else {
            n = xwcslcat(dsc, siz, n, err);
            n = xwcslcat(dsc, siz, n, eds);
        }
    }
    else {
        if (IS_VALID_WCS(eds))
            n = xwcslcat(dsc, siz, n, eds);
    }
    if (IS_VALID_WCS(erp)) {
        b = n;
        dsc[n++] = L' ';
        msg[i++] = dsc + n;
        dsc[n++] = L':';
        dsc[n++] = L' ';
        n = xwcslcat(dsc, siz, n, erp);
    }
    c += n;
    buf[c++] = WNUL;
    if (c > bsz)
        c = bsz;
    siz = bsz - c;
    if (ern == 0) {
        ern = ERROR_INVALID_PARAMETER;
#if HAVE_DEBUG_TRACE
        if (xtraceservice)
            xtraceprintf(fn, line, typ, "syserror : %S", dsc);
#endif
        if (errorreported)
            return ern;
    }
    else if (siz > BBUFSIZ) {
        msg[i++] = buf + c;
#if HAVE_DEBUG_TRACE
        if (xtraceservice)
            xtraceprintf(fn, line, typ, "syserror : %lu (0x%02X) %S",
                         ern, ern, dsc);
#endif
        if (errorreported)
            return ern;

        buf[c++] = L'\r';
        buf[c++] = L'\n';
        if (typ == EVENTLOG_ERROR_TYPE) {
            c += xsnwprintf(buf + c, bsz - c,
                            L"\r\nSystem error code %lu (0x%02X)", ern, ern);
            buf[c++] = WNUL;
            msg[i++] = buf + c;
            buf[c++] = L'\r';
            buf[c++] = L'\n';

        }
        c += xwinapierror(buf + c, bsz - c, ern);
        if (typ == EVENTLOG_INFORMATION_TYPE)
            c += xsnwprintf(buf + c, bsz - c, L" (%lu)", ern);
        buf[c++] = WNUL;
    }
    if (service->name) {
        HANDLE es = RegisterEventSourceW(NULL, service->name);
        if (IS_VALID_HANDLE(es)) {
            if (b) {
                dsc[b++] = WNUL;
                dsc[b++] = L'\r';
                dsc[b++] = L'\n';
            }
            msg[i] = NULL;
            ReportEventW(es, typ, 0, svceventid + i, NULL, i, 0, msg, NULL);
            DeregisterEventSource(es);
            if (typ == EVENTLOG_ERROR_TYPE)
                InterlockedIncrement(&errorreported);
        }
    }
    return ern;
}

static DWORD xerrvprintf(LPCSTR fn, int line, WORD typ,
                         DWORD err, LPCWSTR eds, LPCWSTR fmt, va_list ap)
{
    HANDLE  es;
    DWORD   rv;
    WCHAR   buf[HBUFSIZ];
    LPWSTR  dsc;
    LPCWSTR src = CPP_WIDEN(SVCBATCH_NAME);
    LPCWSTR msg[10];
    int     c = 0;
    int     i = 0;
    int     bsz = HBUFSIZ - TBUFSIZ;

    buf[0]   = WNUL;
    dsc      = buf;
    msg[i++] = buf;
    if (err == 0)
        rv = ERROR_INVALID_PARAMETER;
    else
        rv = err;
    if (service->name) {
        if (service->display)
            src = service->display;
        else
            src = service->name;
    }

    if (typ != EVENTLOG_INFORMATION_TYPE) {
        c = xsnwprintf(buf, BBUFSIZ, L"The %s service", src);
        if (typ == EVENTLOG_ERROR_TYPE)
            c = xwcslcat(buf, BBUFSIZ, c, L" reported the following error:\r\n");
        buf[c++] = WNUL;
        msg[i++] = buf + c;
        buf[c++] = L'\r';
        buf[c++] = L'\n';
        dsc = buf + c;
    }
    if (IS_EMPTY_WCS(fmt))
        c  = xwcslcat(buf, bsz, c, eds);
    else
        c += xvsnwprintf(dsc, bsz - c, fmt, ap);
#if HAVE_DEBUG_TRACE
    if (xtraceservice) {
        if (err == 0)
            xtraceprintf(fn, line, typ, "syserror : %S", dsc);
        else
            xtraceprintf(fn, line, typ, "syserror : %lu (0x%02X) %S",
                         err, err, dsc);
    }
#endif
    if (errorreported)
        return rv;
    buf[c++] = WNUL;
    if ((err > 0) && (c < (bsz - BBUFSIZ))) {
        msg[i++] = buf + c;
        buf[c++] = L'\r';
        buf[c++] = L'\n';
        if (typ == EVENTLOG_ERROR_TYPE) {
            c += xsnwprintf(buf + c, bsz - c,
                            L"\r\nSystem error code %lu (0x%02X)", err, err);
#if HAVE_DEBUG_TRACE
            if (xtraceservice > 2)
            c += xsnwprintf(buf + c, bsz - c,
                            L" in %S(%d)", fn, line);
#endif
            buf[c++] = WNUL;
            msg[i++] = buf + c;
            buf[c++] = L'\r';
            buf[c++] = L'\n';
        }
        if (IS_VALID_WCS(eds) && IS_VALID_WCS(fmt))
            c  = xwcslcat(buf, bsz, c, eds);
        else
            c += xwinapierror(buf + c, bsz - c, err);
        if (typ == EVENTLOG_INFORMATION_TYPE)
            c += xsnwprintf(buf + c, bsz - c, L" (%lu)", err);
        buf[c++] = WNUL;
    }
    if (service->name) {
        es = RegisterEventSourceW(NULL, service->name);
        if (IS_VALID_HANDLE(es)) {
            msg[i] = NULL;
            ReportEventW(es, typ, 0, svceventid + i, NULL, i, 0, msg, NULL);
            DeregisterEventSource(es);
            if (typ == EVENTLOG_ERROR_TYPE)
                InterlockedIncrement(&errorreported);
        }
    }
    else {
        msg[i++] = CRLFW;
        while (i < 10)
            msg[i++] = NULL;
        if (setupeventlog()) {
            es = RegisterEventSourceW(NULL, src);
            if (IS_VALID_HANDLE(es)) {
                /**
                 * Generic message: '%1 %2 %3 %4 %5 %6 %7 %8 %9'
                 * The event code in netmsg.dll is 3299
                 */
                ReportEventW(es, typ, 0, 3299, NULL, 9, 0, msg, NULL);
                DeregisterEventSource(es);
            }
        }
    }
    return rv;
}

static DWORD xerrsprintf(LPCSTR fn, int line, WORD typ, DWORD err, LPCWSTR eds, LPCWSTR fmt, ...)
{
    if (IS_EMPTY_WCS(fmt)) {
        if (eds == NULL)
            eds = zerostring;
        return xerrvprintf(fn, line, typ, err, eds, NULL, NULL);
    }
    else {
        DWORD   rv;
        va_list ap;

        va_start(ap, fmt);
        rv = xerrvprintf(fn, line, typ, err, eds, fmt, ap);
        va_end(ap);
        return rv;
    }
}

static void closeprocess(LPSVCBATCH_PROCESS p)
{
    InterlockedExchange(&p->state, SVCBATCH_PROCESS_STOPPED);

    DBG_PRINTF("%lu %lu %S", p->pInfo.dwProcessId, p->exitCode, p->application);
    SAFE_CLOSE_HANDLE(p->pInfo.hProcess);
    SAFE_CLOSE_HANDLE(p->pInfo.hThread);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(p->sInfo.hStdError);
    SAFE_MEM_FREE(p->commandLine);
}

static int getproctree(LPSVCBATCH_PROCINFO pa, int siz)
{
    int     i;
    int     n = 1;
    HANDLE  h;
    PROCESSENTRY32W e;

    h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (IS_INVALID_HANDLE(h))
        return n;

    e.dwSize = DSIZEOF(PROCESSENTRY32W);
    if (!Process32FirstW(h, &e)) {
        CloseHandle(h);
        return n;
    }
    do {
        for (i = n - 1; i >= 0; i--) {
            if ((e.th32ParentProcessID == pa[i].i) && !xwcsequals(e.szExeFile, L"conhost.exe")) {
                pa[n].n = 0;
                pa[n].i = e.th32ProcessID;
                pa[n].h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
                                      FALSE, e.th32ProcessID);
                if (pa[n].h) {
                    DBG_PRINTF("kill [%lu] [%lu] %S", e.th32ProcessID, pa[i].i, e.szExeFile);
                    pa[i].n++;
                    if (++n >= siz) {
                        DBG_PRINTS("overflow, stopping...");
                        CloseHandle(h);
                        return n;
                    }
                }
                else {
                    DBG_PRINTF("fail [%lu] %S", e.th32ProcessID, e.szExeFile);
                }
            }
        }
    } while (Process32Next(h, &e));
    CloseHandle(h);
    return n;
}

static int killproctree(HANDLE h, DWORD pid, DWORD rv)
{
    int   i;
    int   n;
    int   c = 0;
    SVCBATCH_PROCINFO pa[MBUFSIZ];

    pa[0].n = 0;
    pa[0].i = pid;
    pa[0].h = h;

    n = getproctree(pa, MBUFSIZ);
    for (i = n - 1; i >= 0; i--) {
        DWORD x = 0;

        if (pa[i].h && pa[i].n) {
            DBG_PRINTF("wait [%lu]", pa[i].i);
            x = WaitForSingleObject(pa[i].h, SVCBATCH_STOP_STEP);
        }
        if (pa[i].h) {
            if (x || !GetExitCodeProcess(pa[i].h, &x))
                x =  STILL_ACTIVE;
            if (x == STILL_ACTIVE) {
                TerminateProcess(pa[i].h, rv);
                c++;
            }
            if (pa[i].h != h) {
                CloseHandle(pa[i].h);
                pa[i].h = NULL;
            }
        }
    }
    return c;
}

static void killprocess(LPSVCBATCH_PROCESS proc, DWORD rv)
{
    DWORD x = 0;
    DBG_PRINTF("proc %lu", proc->pInfo.dwProcessId);

    if (proc->state == SVCBATCH_PROCESS_STOPPED)
        goto finished;
    InterlockedExchange(&proc->state, SVCBATCH_PROCESS_STOPPING);

    if (service->killDepth)
        killproctree(proc->pInfo.hProcess, proc->pInfo.dwProcessId, rv);
    x = WaitForSingleObject(proc->pInfo.hProcess, SVCBATCH_STOP_STEP);
    if (x || !GetExitCodeProcess(proc->pInfo.hProcess, &x))
        x =  STILL_ACTIVE;
    if (x == STILL_ACTIVE) {
        DBG_PRINTF("term %lu", proc->pInfo.dwProcessId);
        TerminateProcess(proc->pInfo.hProcess, rv);
    }
    DBG_PRINTF("kill %lu", proc->pInfo.dwProcessId);
    proc->exitCode = rv;

finished:
    InterlockedExchange(&proc->state, SVCBATCH_PROCESS_STOPPED);
    DBG_PRINTF("done %lu", proc->pInfo.dwProcessId);
}

static void cleanprocess(LPSVCBATCH_PROCESS proc)
{

    DBG_PRINTF("proc %lu", proc->pInfo.dwProcessId);

    if (service->killDepth)
        killproctree(NULL, proc->pInfo.dwProcessId, ERROR_ARENA_TRASHED);
    DBG_PRINTF("done %lu", proc->pInfo.dwProcessId);
}

static DWORD xmdparent(LPWSTR path)
{
    DWORD  rc = 0;
    LPWSTR s;

    s = xwcsrchr(path, L'\\');
    if (s == NULL)
        return ERROR_BAD_PATHNAME;
    *s = WNUL;
    if (!CreateDirectoryW(path, NULL))
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        /**
         * One or more intermediate directories do not exist
         */
        rc = xmdparent(path);
        if (rc == 0) {
            if (!CreateDirectoryW(path, NULL))
                rc = GetLastError();
        }
    }
    *s = L'\\';
    return rc;
}

static DWORD xcreatedir(LPCWSTR path)
{
    DWORD rc = 0;

    if (CreateDirectoryW(path, NULL))
        return 0;
    else
        rc = GetLastError();
    if (rc == ERROR_PATH_NOT_FOUND) {
        WCHAR pp[SVCBATCH_PATH_MAX];
        /**
         * One or more intermediate directories do not exist
         */
        xwcslcpy(pp, SVCBATCH_PATH_SIZ, path);
        xwinpathsep(pp);
        rc = xmdparent(pp);
        if (rc == 0) {
            /**
             * All intermediate paths are created.
             * Create the final directory in the path.
             */
            if (!CreateDirectoryW(path, NULL))
                rc = GetLastError();
        }
    }
    if (rc == ERROR_ALREADY_EXISTS)
        rc = 0;
    return rc;
}

static DWORD WINAPI xrunthread(LPVOID param)
{
    LPSVCBATCH_THREAD p = (LPSVCBATCH_THREAD)param;

    p->duration = GetTickCount64();
    p->exitCode = (*p->startAddress)(p->parameter);
    p->duration = GetTickCount64() - p->duration;
    DBG_PRINTF("%s ended %lu", p->name, p->exitCode);
    InterlockedExchange(&p->started, 0);
#if 0
    ExitThread(p->exitCode);
#endif
    return p->exitCode;
}

static BOOL xcreatethread(SVCBATCH_THREAD_ID id,
                          int suspended,
                          LPTHREAD_START_ROUTINE threadfn,
                          LPVOID param)
{
    if (InterlockedCompareExchange(&threads[id].started, 1, 0)) {
        /**
         * Already started
         */
         SetLastError(ERROR_BUSY);
         return FALSE;
    }
    threads[id].name         = threadnames[id];
    threads[id].id           = id;
    threads[id].startAddress = threadfn;
    threads[id].parameter    = param;
    threads[id].thread       = CreateThread(NULL, 0, xrunthread, &threads[id],
                                            suspended ? CREATE_SUSPENDED : 0, NULL);
    if (threads[id].thread == NULL) {
        threads[id].exitCode = GetLastError();
        InterlockedExchange(&threads[id].started, 0);
        return FALSE;
    }
    return TRUE;
}

static LPCWSTR skipdotslash(LPCWSTR s)
{
    LPCWSTR p = s;

    if (IS_EMPTY_WCS(s))
        return s;
    if (*p == L'.') {
        if (*(++p) == WNUL)
            return p;
        if ((*p == L'\\') || (*p == L'/'))
            return ++p;
    }
    return s;
}

static BOOL isdotslash(LPCWSTR p)
{
    if (IS_EMPTY_WCS(p))
        return FALSE;
    if ((p[0] == L'.') && ((p[1] == L'\\') || (p[1] == L'/')))
        return TRUE;
    else
        return FALSE;
}

static BOOL isnotapath(LPCWSTR p)
{
    if (IS_EMPTY_WCS(p))
        return TRUE;

    while (*p != WNUL) {
        if ((*p == L'\\') || (*p == L'/'))
            return FALSE;
        p++;
    }
    return TRUE;
}

static BOOL isabsolutepath(LPCWSTR p)
{
    if (IS_EMPTY_WCS(p))
        return FALSE;
    if ((p[0] == L'\\') || (xisalpha(p[0]) && (p[1] == L':')))
        return TRUE;
    else
        return FALSE;
}

static BOOL isrelativepath(LPCWSTR p)
{
    if (IS_EMPTY_WCS(p))
        return FALSE;
    if ((p[0] == L'\\') || (xisalpha(p[0]) && (p[1] == L':')))
        return FALSE;
    else
        return TRUE;
}

static LPWSTR xwmakepath(LPCWSTR p, LPCWSTR n, LPCWSTR e)
{
    LPWSTR rs;
    int    lp;
    int    ln;
    int    le;
    int    c;
    int    x = 0;

    ASSERT_WSTR(p, NULL);
    ASSERT_WSTR(n, NULL);
    lp = xwcslen(p);
    ln = xwcslen(n);
    le = xwcslen(e);

    c = lp + ln + le + 2;
    if ((c > MAX_PATH)  && xisalpha(p[0]) &&
        (p[1] == L':' ) &&
        (p[2] == L'\\'))
        x = 4;
    c += x;
    ASSERT_LESS(c, SVCBATCH_PATH_MAX, NULL);
    rs = xwmalloc(c);

    if (x > 0)
        xpprefix(rs);
    wmemcpy(rs + x, p, lp);
    x += lp;
    rs[x++] = L'\\';
    wmemcpy(rs + x, n, ln);
    if (le > 0)
        wmemcpy(rs + x + ln, e, le);

    xwinpathsep(rs);
    return rs;
}

static DWORD xfixmaxpath(LPWSTR buf, DWORD len, int isdir)
{
    if (len > 5) {
        DWORD siz = isdir ? 248 : MAX_PATH;
        if (len < siz) {
            /**
             * Strip leading \\?\ for short paths
             * but not \\?\UNC\* paths
             */
            if (xispprefix(buf) && (buf[5] == L':')) {
                wmemmove(buf, buf + 4, len - 3);
                len -= 4;
            }
        }
        else {
            /**
             * Prepend \\?\ to long paths
             * if starts with X:\
             */
            if (xisalpha(buf[0])  &&
                (buf[1] == L':' ) &&
                (buf[2] == L'\\')) {
                wmemmove(buf + 4, buf, len + 1);
                xpprefix(buf);
                len += 4;
            }
        }
    }
    return len;
}

static LPWSTR xgetfullpath(LPCWSTR path, LPWSTR dst, DWORD siz, int isdir)
{
    DWORD len;

    ASSERT_WSTR(path, NULL);
    len = GetFullPathNameW(path, siz, dst, NULL);
    if (len == 0) {
        return NULL;
    }
    if (len >= siz) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return NULL;
    }
    xwinpathsep(dst);
    xfixmaxpath(dst, len, isdir);
    return dst;
}

static LPWSTR xgetfinalpath(int isdir, LPCWSTR path)
{
    HANDLE fh;
    WCHAR  buf[SVCBATCH_PATH_MAX];
    DWORD  len;
    DWORD  atr = isdir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    DWORD  acc = GENERIC_READ;

    ASSERT_WSTR(path, NULL);
    len = GetFullPathNameW(path, SVCBATCH_PATH_SIZ, buf, NULL);
    if (len == 0)
        return NULL;
    if (len >= SVCBATCH_PATH_SIZ) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return NULL;
    }
    xwinpathsep(buf);
    xfixmaxpath(buf, len, isdir);
    if (isdir > 1)
        acc |= GENERIC_WRITE;
    fh = CreateFileW(buf, acc, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, atr, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, SVCBATCH_PATH_SIZ, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= SVCBATCH_PATH_SIZ)) {
       SetLastError(ERROR_BAD_PATHNAME);
       return NULL;
    }
    len = xfixmaxpath(buf, len, isdir);
    return xwcsndup(buf, len);
}

static LPWSTR xgetdirpath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    HANDLE fh;
    DWORD  len;

    fh = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, dst, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz)) {
        SetLastError(ERROR_BAD_PATHNAME);
        return NULL;
    }

    xfixmaxpath(dst, len, 1);
    return dst;
}

static DWORD xgetfilepath(LPCWSTR path, LPWSTR dst, DWORD siz)
{
    HANDLE fh;
    DWORD  len;

    fh = CreateFileW(path, GENERIC_READ,
                     FILE_SHARE_READ, NULL, OPEN_EXISTING,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (IS_INVALID_HANDLE(fh))
        return 0;
    len = GetFinalPathNameByHandleW(fh, dst, siz, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= siz)) {
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    return xfixmaxpath(dst, len, 0);
}

static LPWSTR xgetenv(LPCWSTR s)
{
    WCHAR  e[BBUFSIZ];
    DWORD  n;
    LPWSTR d = NULL;

    if (IS_EMPTY_WCS(s))
        return NULL;
    n = GetEnvironmentVariableW(s, e, BBUFSIZ);
    if (n == 0)
        return NULL;
    d = xwmalloc(n + 1);
    if (n >= BBUFSIZ)
        GetEnvironmentVariableW(s, d, n);
    else
        wmemcpy(d, e, n);
    return d;
}

static LPWSTR xexpandenvstr(LPCWSTR src, LPCWSTR set)
{
    WCHAR  bb[SVCBATCH_NAME_MAX];
    SVCBATCH_WBUFFER wb;
    LPWSTR           rp;
    LPCWSTR           s = src;

    ASSERT_WSTR(src, NULL);
    if (xwcschr(src, L'$') == NULL)
        return (LPWSTR)src;
    if (xwbsinit(&wb, SVCBATCH_NAME_MAX))
        return NULL;

    while (*s != WNUL) {
        if (*s == L'$') {
            s++;
            if (*s == L'$') {
                xwbsaddwch(&wb, *s++);
            }
            else {
                int     i;
                int     c = 0;
                int     n = 0;
                LPCWSTR v = s;

                if (*v == L'{') {
                    v++;
                    while (*v != WNUL) {
                        if (*v == L'}') {
                            c = 1;
                            break;
                        }
                        v++;
                        n++;
                    }
                    if (c == 0) {
                        xfree(wb.buf);
                        SetLastError(ERROR_INVALID_PARAMETER);
                        return NULL;
                    }
                    if (n > 0)
                        s++;
                }
                else {
                    while (xisvalidvarchar(*v)) {
                        v++;
                        n++;
                    }
                }
                if (n > 0) {
                    LPCWSTR cp = NULL;
                    LPWSTR  ep = NULL;
                    LPWSTR  nb = bb;

                    i = xwcslcpyn(bb, SVCBATCH_NAME_MAX, s, n);
                    if (i >= SVCBATCH_NAME_MAX) {
                        xfree(wb.buf);
                        SetLastError(ERROR_BUFFER_OVERFLOW);
                        return NULL;
                    }
                    if (xisdigit(nb[0]) && (nb[1] == WNUL)) {
                        if (set) {
                            xfree(wb.buf);
                            SetLastError(ERROR_INVALID_NAME);
                            return NULL;
                        }
                        cp = cmdproc->args[nb[0] - L'0'];
                    }
                    else {
                        cp = xgetsysvar(nb, set);
                        if (cp == NULL) {
                            if (set) {
                                xfree(wb.buf);
                                SetLastError(ERROR_INVALID_NAME);
                                return NULL;
                            }
                            ep = xgetenv(nb);
                            cp = ep;
                        }
                    }
                    xwbsaddwcs(&wb, cp, 0);
                    xfree(ep);
                    s += n + c;
                }
                else {
                    xwbsaddwch(&wb, L'$');
                }
            }
        }
        else {
            xwbsaddwch(&wb, *s++);
        }
    }
    rp = xwbsdata(&wb);
    if (IS_EMPTY_WCS(rp)) {
        xfree(wb.buf);
        SetLastError(ERROR_NO_DATA);
        rp = NULL;
    }
    return rp;
}


static DWORD xsetenvvar(LPCWSTR n, LPCWSTR p)
{
    DWORD   r = 0;
    LPWSTR  v;

    ASSERT_WSTR(n, ERROR_BAD_ENVIRONMENT);
    ASSERT_WSTR(p, ERROR_INVALID_PARAMETER);

    v = xexpandenvstr(p, NULL);
    if (v == NULL)
        return GetLastError();
    if (!SetEnvironmentVariableW(n, v))
        r = GetLastError();
    DBG_PRINTF("%S=%S", n, v);
    if (v != p)
        xfree(v);
    return r;
}

static LPCWSTR xgettempdir(void)
{
    LPWSTR p;
    LPWSTR r;

    if (IS_VALID_WCS((LPCWSTR)xwsystempdir))
        return xwsystempdir;

    p = xgetenv(L"TEMP");
    if (p == NULL)
        p = xgetenv(L"TMP");
    if (p == NULL)
        p = xgetenv(L"USERPROFILE");
    if (p == NULL) {
        r = xgetenv(L"SystemRoot");
        if (r != NULL)
            p = xwcsappend(r, L"\\Temp");
    }
    r = xgetfinalpath(2, p);
    xfree(p);
    InterlockedExchangePointer(&xwsystempdir, r);
    return r;
}

static LPWSTR xsearchexe(LPCWSTR name)
{
    WCHAR  buf[SVCBATCH_PATH_MAX];
    DWORD  len;
    HANDLE fh;

    SetSearchPathMode(BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE);
    len = SearchPathW(NULL, name, L".exe", SVCBATCH_PATH_SIZ, buf, NULL);
    if ((len == 0) || (len >= SVCBATCH_PATH_SIZ)) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return NULL;
    }
    fh = CreateFileW(buf, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (IS_INVALID_HANDLE(fh))
        return NULL;
    len = GetFinalPathNameByHandleW(fh, buf, SVCBATCH_PATH_SIZ, VOLUME_NAME_DOS);
    CloseHandle(fh);
    if ((len == 0) || (len >= SVCBATCH_PATH_SIZ)) {
       SetLastError(ERROR_BAD_PATHNAME);
       return NULL;
    }
    len = xfixmaxpath(buf, len, 0);
    return xwcsndup(buf, len);
}

static void setsvcstatusexit(DWORD e)
{
    SVCBATCH_CS_ENTER(service);
    InterlockedExchange(&service->exitCode, e);
    SVCBATCH_CS_LEAVE(service);
}

static void reportsvcstatus(LPCSTR fn, int line, DWORD status, DWORD param)
{
    SVCBATCH_CS_ENTER(service);

    if ((status == SERVICE_STOP_PENDING) && (service->exitCode != 0))
        goto finished;
    if (InterlockedExchange(&service->state, SERVICE_STOPPED) == SERVICE_STOPPED)
        goto finished;

    service->status.dwControlsAccepted        = 0;
    service->status.dwCheckPoint              = 0;
    service->status.dwWaitHint                = 0;
    service->status.dwServiceSpecificExitCode = service->exitCode;

    if (status == SERVICE_RUNNING) {
        service->status.dwControlsAccepted = SERVICE_ACCEPT_STOP |
                                             SERVICE_ACCEPT_SHUTDOWN |
                                             preshutdown;
        service->status.dwWin32ExitCode    = NO_ERROR;
        InterlockedExchange(&service->check, 0);
    }
    else if (status == SERVICE_STOPPED) {
        if (service->status.dwCurrentState != SERVICE_STOP_PENDING) {
            if (param == 0)
                param = service->exitCode;
            if (service->failMode == SVCBATCH_FAIL_EXIT) {
                xerrsprintf(fn, line, EVENTLOG_ERROR_TYPE, param, NULL, SVCBATCH_MSG(1));
                SVCBATCH_CS_LEAVE(service);
                exit(ERROR_INVALID_LEVEL);
            }
            else {
                if ((service->failMode == SVCBATCH_FAIL_NONE) &&
                    (service->status.dwCurrentState == SERVICE_RUNNING)) {
                    xerrsprintf(fn, line, EVENTLOG_INFORMATION_TYPE, param,
                                NULL, param ? SVCBATCH_MSG(1) : SVCBATCH_MSG(0));
                    param = 0;
                    service->status.dwWin32ExitCode = NO_ERROR;
                    service->status.dwServiceSpecificExitCode = 0;
                }
                else {
                    /* SVCBATCH_FAIL_ERROR */
                    if (param == 0) {
                        if (service->status.dwCurrentState == SERVICE_RUNNING)
                            param = ERROR_PROCESS_ABORTED;
                        else
                            param = ERROR_SERVICE_START_HANG;
                    }
                    xerrsprintf(fn, line, EVENTLOG_ERROR_TYPE, param, NULL, SVCBATCH_MSG(1));
                }
            }
        }
        if (param != 0)
            service->status.dwServiceSpecificExitCode  = param;
        if (service->status.dwServiceSpecificExitCode != 0)
            service->status.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
    }
    else {
        service->status.dwCheckPoint = InterlockedIncrement(&service->check);
        service->status.dwWaitHint   = param;
    }
    service->status.dwCurrentState = status;
    InterlockedExchange(&service->state, status);
    if (!SetServiceStatus(service->handle, &service->status)) {
        xsyserror(GetLastError(), L"SetServiceStatus", NULL);
        InterlockedExchange(&service->state, SERVICE_STOPPED);
    }
finished:
    SVCBATCH_CS_LEAVE(service);
}

static BOOL createstdpipe(LPHANDLE rd, LPHANDLE wr, DWORD mode)
{
    DWORD i;
    BYTE  b;
    WCHAR name[WBUFSIZ];
    SECURITY_ATTRIBUTES sa;


    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    b = LOBYTE(uidcounter);
    i = xwcslcat(name, WBUFSIZ, 0, L"\\\\.\\pipe\\");
    name[i++] = hexwchars[b >> 4];
    name[i++] = hexwchars[b & 0x0F];
    name[i++] = L'-';
    if (xuuidstring(name + i, 1, 0) == NULL)
        return FALSE;
    InterlockedIncrement(&uidcounter);

    DBG_PRINTF("%d %S", mode ? 1 : 0, name);

    *rd = CreateNamedPipeW(name,
                           PIPE_ACCESS_INBOUND | mode,
                           PIPE_TYPE_BYTE,
                           1,
                           0,
                           65536,
                           0,
                           &sa);
    if (IS_INVALID_HANDLE(*rd))
        return FALSE;

    *wr = CreateFileW(name,
                      GENERIC_WRITE,
                      0,
                      &sa,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      NULL);
    if (IS_INVALID_HANDLE(*wr))
        return FALSE;
    else
        return TRUE;
}

static BOOL createnulpipe(LPHANDLE ph)
{
    SECURITY_ATTRIBUTES sa;

    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle       = TRUE;

    *ph = CreateFileW(L"NUL",
                      GENERIC_READ | GENERIC_WRITE,
                      0,
                      &sa,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      NULL);
    if (IS_INVALID_HANDLE(*ph))
        return FALSE;
    else
        return TRUE;
}

static DWORD createiopipes(LPSTARTUPINFOW si,
                           LPHANDLE iwrs,
                           LPHANDLE ords,
                           DWORD mode)
{
    HANDLE cp = program->pInfo.hProcess;
    HANDLE rd = NULL;
    HANDLE wr = NULL;

    si->cb      = DSIZEOF(STARTUPINFOW);
    si->dwFlags = STARTF_USESTDHANDLES;

    if (iwrs) {
        /**
         * Create stdin pipe, with write side
         * of the pipe as non inheritable.
         */
        if (!createstdpipe(&rd, &wr, 0))
            return GetLastError();
        if (!DuplicateHandle(cp, wr, cp,
                             iwrs, 0, FALSE,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
            return GetLastError();
    }
    else {
        if (!createnulpipe(&rd))
            return GetLastError();
    }
    si->hStdInput = rd;
    if (ords) {
        /**
         * Create stdout/stderr pipe, with read side
         * of the pipe as non inheritable
         */
        if (!createstdpipe(&rd, &wr, mode))
            return GetLastError();
        if (!DuplicateHandle(cp, rd, cp,
                             ords, 0, FALSE,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS))
            return GetLastError();
    }
    else {
        if (!createnulpipe(&wr))
            return GetLastError();
    }
    si->hStdOutput = wr;
    si->hStdError  = wr;

    return 0;
}

static BOOL canrotatelogs(LPSVCBATCH_LOG log)
{
    BOOL rv = FALSE;

    SVCBATCH_CS_ENTER(log);
    if (log->state) {
        if (log->size) {
            InterlockedExchange(&log->state, 0);
            rv = TRUE;
        }
    }
    SVCBATCH_CS_LEAVE(log);
    return rv;
}

static LPWSTR createsvcdir(LPCWSTR dir)
{
    WCHAR  b[SVCBATCH_PATH_MAX];
    LPWSTR p;

    if (isrelativepath(dir)) {
        int i;

        i = xwcslcat(b, SVCBATCH_PATH_SIZ, 0, service->work);
        b[i++] = L'\\';
        i = xwcslcat(b, SVCBATCH_PATH_SIZ, i, dir);
        if (i >= SVCBATCH_PATH_SIZ) {
            xsyserror(ERROR_BUFFER_OVERFLOW, dir, NULL);
            return NULL;
        }
        xfixmaxpath(b, i, 1);
    }
    else {
        p = xgetfullpath(dir, b, SVCBATCH_PATH_SIZ, 1);
        if (p == NULL) {
            xsyserror(GetLastError(), dir, NULL);
            return NULL;
        }
    }
    p = xgetdirpath(b, b, SVCBATCH_PATH_SIZ);
    if (p == NULL) {
        DWORD rc = GetLastError();

        if (rc > ERROR_PATH_NOT_FOUND) {
            xsyserror(rc, b, NULL);
            return NULL;
        }
        rc = xcreatedir(b);
        if (rc != 0) {
            xsyserror(rc, b, NULL);
            return NULL;
        }
        p = xgetdirpath(b, b, SVCBATCH_PATH_SIZ);
        if (p == NULL) {
            xsyserror(GetLastError(), b, NULL);
            return NULL;
        }
    }
    return xwcsdup(b);
}

static DWORD rotateprevlogs(LPSVCBATCH_LOG log, BOOL ssp)
{
    DWORD rc;
    int   i;
    int   n;
    int   x;
    WCHAR lognn[SVCBATCH_PATH_MAX];
    WCHAR logpn[SVCBATCH_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA ad;

    if (GetFileAttributesExW(log->logFile, GetFileExInfoStandard, &ad)) {
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
            DBG_PRINTF("empty log %S", log->logFile);
            return 0;
        }
    }
    else {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND)
            return xsyserror(rc, log->logFile, NULL);
        else
            return 0;
    }
    x = xwcslcpy(lognn, SVCBATCH_PATH_SIZ, log->logFile);
    x = xwcslcat(lognn, SVCBATCH_PATH_SIZ, x, L".0");
    if (x >= SVCBATCH_PATH_SIZ)
        return xsyserror(ERROR_BAD_PATHNAME, log->logFile, NULL);
    x = xfixmaxpath(lognn, x, 0);
    DBG_PRINTF("0 %S", lognn);
    if (!MoveFileExW(log->logFile, lognn, MOVEFILE_REPLACE_EXISTING))
        return xsyserror(GetLastError(), log->logFile, lognn);
    if (ssp) {
        xsvcstatus(SERVICE_START_PENDING, 0);
    }
    n = log->maxLogs;
    wmemcpy(logpn, lognn, x + 1);
    x--;
    for (i = 1; i < log->maxLogs; i++) {
        lognn[x] = L'0' + i;

        if (!GetFileAttributesExW(lognn, GetFileExInfoStandard, &ad))
            break;
        if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0))
            break;
    }
    n = i;
    /**
     * Rotate previous log files
     */
    for (i = n; i > 0; i--) {
        logpn[x] = L'0' + i - 1;
        lognn[x] = L'0' + i;
        if (GetFileAttributesExW(logpn, GetFileExInfoStandard, &ad)) {
            if ((ad.nFileSizeHigh == 0) && (ad.nFileSizeLow == 0)) {
                DBG_PRINTF("%d skipping empty %S", i, logpn);
            }
            else {
                DBG_PRINTF("%d %S", i, lognn);
                if (!MoveFileExW(logpn, lognn, MOVEFILE_REPLACE_EXISTING))
                    return xsyserror(GetLastError(), logpn, lognn);
                if (ssp) {
                    xsvcstatus(SERVICE_START_PENDING, 0);
                }
            }
        }
        else {
            rc = GetLastError();
            if (rc != ERROR_FILE_NOT_FOUND)
                return xsyserror(rc, logpn, NULL);
        }
    }

    return 0;
}

static DWORD openlogfile(LPSVCBATCH_LOG log, BOOL ssp)
{
    DWORD   rc;
    HANDLE  fh = NULL;
    LPWSTR  nf = NULL;
    LPCWSTR nn = log->logName;

    if (xwcschr(nn, L'@')) {
        nf = xwcsftime(nn);
        if (nf == NULL)
            return xsyserror(GetLastError(), nn, NULL);
        DBG_PRINTF("%S -> %S", nn, nf);
        nn = nf;
    }
    xfree(log->logFile);
    log->logFile = xwmakepath(service->logs, nn, NULL);
    xfree(nf);
    if (log->maxLogs) {
        rc = rotateprevlogs(log, ssp);
        if (rc)
            return rc;
    }
    fh = CreateFileW(log->logFile,
                     GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ, NULL,
                     CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (IS_INVALID_HANDLE(fh))
        return xsyserror(rc, log->logFile, NULL);
    DBG_PRINTF("%S", log->logFile);
    InterlockedExchange64(&log->size, 0);
    InterlockedExchange(&log->state,  0);
    InterlockedExchangePointer(&log->fd, fh);
    return 0;
}

static DWORD rotatelogs(LPSVCBATCH_LOG log)
{
    DWORD  rc = 0;
    HANDLE h;

    ASSERT_NULL(log, 0);
    SVCBATCH_CS_ENTER(log);
    InterlockedExchange(&log->state, 0);

    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL) {
        rc = ERROR_INVALID_HANDLE;
        goto finished;
    }
    if (IS_OPT_SET(SVCBATCH_OPT_TRUNCATE)) {
        LARGE_INTEGER ee = {{ 0, 0 }};

        if (SetFilePointerEx(h, ee, NULL, FILE_BEGIN)) {
            if (SetEndOfFile(h)) {
                DBG_PRINTF("truncated %S", log->logFile);
                InterlockedExchangePointer(&log->fd, h);
                goto finished;
            }
        }
        rc = GetLastError();
        CloseHandle(h);
    }
    else {
        FlushFileBuffers(h);
        CloseHandle(h);
        rc = openlogfile(log, FALSE);
    }

finished:
    InterlockedExchange64(&log->size, 0);
    SVCBATCH_CS_LEAVE(log);
    return rc;
}

static DWORD closelogfile(LPSVCBATCH_LOG log)
{
    HANDLE h;

    if (log == NULL)
        return ERROR_FILE_NOT_FOUND;

    SVCBATCH_CS_ENTER(log);
    DBG_PRINTF("%lu %S", log->state, log->logFile);
    InterlockedExchange(&log->state, 0);

    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    xfree(log->logFile);

    SVCBATCH_CS_LEAVE(log);
    SVCBATCH_CS_CLOSE(log);
    xfree(log);

    return 0;
}

static void resolvetimeout(int hh, int mm, int ss, int od)
{
    SYSTEMTIME     st;
    FILETIME       ft;
    ULARGE_INTEGER si;
    ULARGE_INTEGER ui;

    rotateinterval = od ? ONE_DAY : ONE_HOUR;
    if (IS_OPT_SET(SVCBATCH_OPT_LOCALTIME) && od)
        GetLocalTime(&st);
    else
        GetSystemTime(&st);

    SystemTimeToFileTime(&st, &ft);
    si.HighPart = ft.dwHighDateTime;
    si.LowPart  = ft.dwLowDateTime;
    ui.QuadPart = si.QuadPart + rotateinterval;
    ft.dwHighDateTime = ui.HighPart;
    ft.dwLowDateTime  = ui.LowPart;
    FileTimeToSystemTime(&ft, &st);
    if (od)
        st.wHour = hh;
    st.wMinute = mm;
    st.wSecond = ss;
    SystemTimeToFileTime(&st, &ft);
    rotatetime.HighPart = ft.dwHighDateTime;
    rotatetime.LowPart  = ft.dwLowDateTime;
    DBG_PRINTF("in %llu minutes", (rotatetime.QuadPart - si.QuadPart) / ONE_MINUTE);

    SVCOPT_SET(SVCBATCH_OPT_ROTATE_BY_TIME);
}

static BOOL xarotatetime(LPCWSTR param)
{
    LPWSTR  ep;
    LPCWSTR rp = param;

    if (IS_EMPTY_WCS(param))
        return TRUE;
    if (xiswcschar(param, L'0')) {
        DBG_PRINTS("at midnight");
        resolvetimeout(0, 0, 0, 1);
        return TRUE;
    }
    if (xwcschr(rp, L':')) {
        int hh, mm, ss;

        hh = xwcstoi(rp, &ep);
        if ((hh < 0) || (hh > 23))
            return FALSE;
        if (*ep != L':')
            return FALSE;
        rp = ep + 1;
        mm = xwcstoi(rp, &ep);
        if ((mm < 0) || (mm > 59))
            return FALSE;
        if (*ep != L':')
            return FALSE;
        rp = ep + 1;
        ss = xwcstoi(rp, &ep);
        if ((ss < 0) || (ss > 59))
            return FALSE;

        DBG_PRINTF("at %.2d:%.2d:%.2d",
                   hh, mm, ss);
        resolvetimeout(hh, mm, ss, 1);
        rp = ep;
    }
    if (*rp == WNUL)
        return TRUE;
    DBG_PRINTF("invalid %S", param);
    return FALSE;
}

static void xirotatetime(int mm)
{
    if (mm == 60) {
        DBG_PRINTS("each full hour");
        resolvetimeout(0, 0, 0, 0);
    }
    else {
        rotateinterval = mm * ONE_MINUTE * CPP_INT64_C(-1);
        rotatetime.QuadPart = rotateinterval;
        SVCOPT_SET(SVCBATCH_OPT_ROTATE_BY_TIME);
        DBG_PRINTF("each %d minutes", mm);
    }
}

static DWORD addshmemdata(LPWSTR d, LPDWORD x, const BYTE *s, DWORD n)
{
    DWORD p;
    DWORD i = *x;

    if ((n == 0) || (s == NULL))
        return 0;
    p = MEM_ALIGN_WORD(n) / 2;
    if ((i + p) >= SVCBATCH_DATA_LEN)
        return 0;
    memcpy(d + i, s, n);
    *x = i + p;
    return i;
}

static DWORD addshmemwstr(LPWSTR d, LPDWORD x, LPCWSTR s)
{
    DWORD i = *x;
    DWORD n;

    if (IS_EMPTY_WCS(s))
        return 0;
    n = xwcslen(s) + 1;
    if ((i + n) >= SVCBATCH_DATA_LEN)
        return 0;
    wmemcpy(d + i, s, n);
    *x = i + n;
    return i;
}

static DWORD runshutdown(void)
{
    WCHAR rb[BBUFSIZ];
    DWORD i;
    DWORD rc = 0;
    DWORD x  = 4;
    SECURITY_ATTRIBUTES sa;

    DBG_PRINTS("started");
    sa.nLength              = DSIZEOF(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;
#if HAVE_NAMED_MMAP
    sa.bInheritHandle       = FALSE;
    i = xwcslcat(rb, BBUFSIZ, 0, L"/S:" SVCBATCH_MMAPPFX);
    if (xuuidstring(rb + i, 1, 0) == NULL)
        return GetLastError();
    sharedmmap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                    PAGE_READWRITE, 0,
                                    DSIZEOF(SVCBATCH_IPC), rb + 3);
    if (sharedmmap == NULL)
        return GetLastError();
#else
    sa.bInheritHandle       = TRUE;
    sharedmmap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                    PAGE_READWRITE, 0,
                                    DSIZEOF(SVCBATCH_IPC), NULL);
    if (sharedmmap == NULL)
        return GetLastError();
    i = xwcslcat(rb, BBUFSIZ, 0, L"/S:");
    xxtowcb(sharedmmap, rb + i, 0, 8);
#endif
    sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(sharedmmap,
                                              FILE_MAP_ALL_ACCESS,
                                              0, 0, DSIZEOF(SVCBATCH_IPC));
    if (sharedmem == NULL)
        return GetLastError();
    sharedmem->options   = svcoptions & SVCBATCH_OPT_MASK;
    sharedmem->timeout   = svcstop->timeout;
    sharedmem->killDepth = service->killDepth;
    sharedmem->maxLogs   = stopmaxlogs;
    sharedmem->stdinSize = stdinsize;
    sharedmem->stdinData = addshmemdata(sharedmem->data, &x, stdindata, stdinsize);
    sharedmem->logName   = addshmemwstr(sharedmem->data, &x, stoplogname);
    sharedmem->display   = addshmemwstr(sharedmem->data, &x, service->display);

    sharedmem->name      = addshmemwstr(sharedmem->data, &x, service->name);
    sharedmem->work      = addshmemwstr(sharedmem->data, &x, service->work);
    sharedmem->logs      = addshmemwstr(sharedmem->data, &x, service->logs);
    sharedmem->argc      = svcstop->argc;
    sharedmem->optc      = cmdproc->optc;
    for (i = 0; i < svcstop->argc; i++)
        sharedmem->args[i] = addshmemwstr(sharedmem->data, &x, svcstop->args[i]);
    for (i = 0; i < cmdproc->optc; i++)
        sharedmem->opts[i] = addshmemwstr(sharedmem->data, &x, cmdproc->opts[i]);
    if (x >= SVCBATCH_DATA_LEN)
        return ERROR_OUTOFMEMORY;
    DBG_PRINTF("shared memory size %lu", DSIZEOF(SVCBATCH_IPC));
    rc = createiopipes(&svcstop->sInfo, NULL, NULL, 0);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        return rc;
    }

    svcstop->application = program->application;
    svcstop->commandLine = xappendarg(1, NULL, svcstop->application);
    svcstop->commandLine = xappendarg(0, svcstop->commandLine, rb);
#if HAVE_DEBUG_TRACE
    if (xtraceservice) {
        i = 0;
        rb[i++] = L'/';
        rb[i++] = L'D';
        rb[i++] = L':';
        rb[i++] = L'0' + xtraceservice;
        if (!xtracesvcstop)
            rb[i++] = L'-';
        rb[i++] = WNUL;
        svcstop->commandLine = xappendarg(0, svcstop->commandLine, rb);
    }
    DBG_PRINTF("cmdline %S", svcstop->commandLine);
#endif
    svcstop->sInfo.dwFlags     = STARTF_USESHOWWINDOW;
    svcstop->sInfo.wShowWindow = SW_HIDE;
    if (!CreateProcessW(svcstop->application,
                        svcstop->commandLine,
                        NULL,
                        NULL,
                        TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | CREATE_SUSPENDED,
                        service->environment,
                        NULL,
                        &svcstop->sInfo,
                        &svcstop->pInfo)) {
        rc = GetLastError();
        xsyserror(rc, L"CreateProcess", svcstop->application);
        goto finished;
    }
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(svcstop->sInfo.hStdError);
#if !HAVE_NAMED_MMAP
    SetHandleInformation(sharedmmap, HANDLE_FLAG_INHERIT, 0);
#endif
    ResumeThread(svcstop->pInfo.hThread);
    InterlockedExchange(&svcstop->state, SVCBATCH_PROCESS_RUNNING);
    SAFE_CLOSE_HANDLE(svcstop->pInfo.hThread);

    DBG_PRINTF("waiting %lu ms for shutdown process %lu",
               svcstop->timeout, svcstop->pInfo.dwProcessId);
    rc = WaitForSingleObject(svcstop->pInfo.hProcess, svcstop->timeout);
    if (rc == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(svcstop->pInfo.hProcess, &rc))
            rc = GetLastError();
    }
finished:
    svcstop->exitCode = rc;
    closeprocess(svcstop);
    DBG_PRINTF("done %lu", rc);
    return rc;
}

static DWORD WINAPI stopthread(void *ssp)
{
    DWORD rc = 0;
    DWORD ws = WAIT_OBJECT_1;
    int   ri = cmdproc->timeout;
    ULONGLONG rs;

    ResetEvent(svcstopdone);
    SetEvent(stopstarted);

    if (ssp == NULL) {
        xsvcstatus(SERVICE_STOP_PENDING, service->timeout);
    }
    DBG_PRINTS("started");
    if (outputlog) {
        SVCBATCH_CS_ENTER(outputlog);
        InterlockedExchange(&outputlog->state, 0);
        SVCBATCH_CS_LEAVE(outputlog);
    }
    if (svcstop) {
        rs = GetTickCount64();

        DBG_PRINTS("creating shutdown process");
        rc = runshutdown();
        ri = (int)(GetTickCount64() - rs);
        DBG_PRINTF("shutdown finished with %lu in %d ms", rc, ri);
        xsvcstatus(SERVICE_STOP_PENDING, 0);
        ri = cmdproc->timeout - ri;
        if (ri < SVCBATCH_STOP_SYNC)
            ri = SVCBATCH_STOP_SYNC;
        if (rc == 0) {
            DBG_PRINTF("waiting %d ms for worker", ri);
            ws = WaitForSingleObject(workerended, ri);
            if (ws != WAIT_OBJECT_0) {
                ri = (int)(GetTickCount64() - rs);
                ri = cmdproc->timeout - ri;
                if (ri < SVCBATCH_STOP_SYNC)
                    ri = SVCBATCH_STOP_SYNC;
            }
        }
    }
    if (ws != WAIT_OBJECT_0) {
        xsvcstatus(SERVICE_STOP_PENDING, 0);
        SetConsoleCtrlHandler(NULL, TRUE);
        if (IS_OPT_SET(SVCBATCH_OPT_CTRL_BREAK)) {
            DBG_PRINTS("generating CTRL_BREAK_EVENT");
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cmdproc->pInfo.dwProcessId);
        }
        else {
            DBG_PRINTS("generating CTRL_C_EVENT");
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        }
        DBG_PRINTF("waiting %d ms for worker", ri);
        ws = WaitForSingleObject(workerended, ri);
        SetConsoleCtrlHandler(NULL, FALSE);
    }

    xsvcstatus(SERVICE_STOP_PENDING, 0);
    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(cmdproc, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        cleanprocess(cmdproc);
    }
    xsvcstatus(SERVICE_STOP_PENDING, 0);
    SetEvent(svcstopdone);
    DBG_PRINTS("done");
    return rc;
}

static void createstopthread(DWORD rc)
{
    DBG_PRINTF("status %lu", rc);
    setsvcstatusexit(rc);
    if (servicemode)
        xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, NULL);
}

static void stopshutdown(DWORD rt)
{
    DWORD ws;

    DBG_PRINTS("started");
    SetConsoleCtrlHandler(NULL, TRUE);
    if (IS_OPT_SET(SVCBATCH_OPT_CTRL_BREAK)) {
        DBG_PRINTS("generating CTRL_BREAK_EVENT");
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, cmdproc->pInfo.dwProcessId);
    }
    else {
        DBG_PRINTS("generating CTRL_C_EVENT");
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    }
    ws = WaitForSingleObject(workerended, rt);
    SetConsoleCtrlHandler(NULL, FALSE);
    if (ws != WAIT_OBJECT_0) {
        DBG_PRINTS("worker process is still running ... terminating");
        killprocess(cmdproc, ws);
    }
    else {
        DBG_PRINTS("worker process ended");
        cleanprocess(cmdproc);
    }
    DBG_PRINTS("done");
}

static DWORD logwrdata(LPSVCBATCH_LOG log, BYTE *buf, DWORD len)
{
    DWORD  rc = 0;
    DWORD  wr = 0;
    HANDLE h;

    ASSERT_NULL(log, 0);
    SVCBATCH_CS_ENTER(log);
    h = InterlockedExchangePointer(&log->fd, NULL);
    if (h == NULL) {
        SVCBATCH_CS_LEAVE(log);
        DBG_PRINTS("logfile closed");
        return ERROR_NO_MORE_FILES;
    }
    if (WriteFile(h, buf, len, &wr, NULL) && (wr != 0))
        InterlockedAdd64(&log->size, wr);
    else
        rc = GetLastError();

    InterlockedExchangePointer(&log->fd, h);
    SVCBATCH_CS_LEAVE(log);
    if (rc)
        return xsyserror(rc, L"LogWrite", NULL);
    if (IS_OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIZE)) {
        if (log->size >= rotatesize) {
            if (canrotatelogs(log)) {
                DBG_PRINTS("rotating by size");
                SetEvent(dologrotate);
            }
        }
    }
    return 0;
}

static DWORD WINAPI stdinthread(void *unused)
{
    DWORD  rc = 0;
    DWORD  wr = 0;

    DBG_PRINTS("started");
    if (WriteFile(wrpipehandle, stdindata, stdinsize, &wr, NULL) && (wr != 0)) {
        DBG_PRINTF("wrote %lu bytes", wr);
        if (!FlushFileBuffers(wrpipehandle)) {
            rc = GetLastError();
        }
        else {
            DBG_PRINTF("flush %lu bytes", wr);
        }
    }
    else {
        rc = GetLastError();
    }
    if (rc) {
        if (rc == ERROR_BROKEN_PIPE) {
            DBG_PRINTS("pipe closed");
        }
        else if (rc == ERROR_OPERATION_ABORTED) {
            DBG_PRINTS("aborted");
        }
        else {
            DBG_PRINTF("error %lu", rc);
        }
    }
    DBG_PRINTS("done");
    return rc;
}

static DWORD WINAPI rotatethread(void *wt)
{
    HANDLE wh[4];
    DWORD  rc = 0;
    DWORD  nw = 3;
    DWORD  rw = SVCBATCH_ROTATE_READY;
    BOOL   rr = TRUE;

    wh[0] = workerended;
    wh[1] = stopstarted;
    wh[2] = dologrotate;
    wh[3] = NULL;

    DBG_PRINTF("started");
    if (wt)
        wh[nw++] = wt;

    while (rr) {
        DWORD wc;

        wc = WaitForMultipleObjects(nw, wh, FALSE, rw);
        switch (wc) {
            case WAIT_OBJECT_0:
                rr = FALSE;
                DBG_PRINTS("workerended signaled");
            break;
            case WAIT_OBJECT_1:
                rr = FALSE;
                DBG_PRINTS("stopstarted signaled");
            break;
            case WAIT_OBJECT_2:
                DBG_PRINTS("dologrotate signaled");
                rc = rotatelogs(outputlog);
                if (rc == 0) {
                    if (IS_VALID_HANDLE(wt) && (rotateinterval < 0)) {
                        CancelWaitableTimer(wt);
                        SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE);
                    }
                }
                ResetEvent(dologrotate);
                rw = SVCBATCH_ROTATE_READY;
            break;
            case WAIT_OBJECT_3:
                DBG_PRINTS("rotate timer signaled");
                ResetEvent(dologrotate);
                SVCBATCH_CS_ENTER(outputlog);
                if (rotateinterval > 0)
                    InterlockedExchange(&outputlog->state, 1);
                SVCBATCH_CS_LEAVE(outputlog);
                if (canrotatelogs(outputlog)) {
                    DBG_PRINTS("rotate by time");
                    rc = rotatelogs(outputlog);
                }
                else {
                    DBG_PRINTS("rotate is busy ... canceling timer");
                }
                if (rc == 0) {
                    CancelWaitableTimer(wt);
                    if (rotateinterval > 0)
                        rotatetime.QuadPart += rotateinterval;
                    SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE);
                }
                rw = SVCBATCH_ROTATE_READY;
            break;
            case WAIT_TIMEOUT:
                DBG_PRINTS("rotate ready");
                SVCBATCH_CS_ENTER(outputlog);
                InterlockedExchange(&outputlog->state, 1);
                if (IS_OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIZE)) {
                    if (outputlog->size >= rotatesize) {
                        InterlockedExchange(&outputlog->state, 0);
                        DBG_PRINTS("rotating by size");
                        SetEvent(dologrotate);
                    }
                }
                SVCBATCH_CS_LEAVE(outputlog);
                rw = INFINITE;
            break;
            default:
                rc = wc;
            break;
        }
        if (rc)
            rr = FALSE;
    }
    InterlockedExchange(&outputlog->state, 0);
    if (rc)
        createstopthread(rc);
    if (IS_VALID_HANDLE(wt)) {
        CancelWaitableTimer(wt);
        CloseHandle(wt);
    }
    DBG_PRINTS("done");
    return rc;
}

static DWORD logiodata(LPSVCBATCH_LOG log, LPSVCBATCH_PIPE op)
{
    DWORD rc = 0;

    ASSERT_NULL(log, 0);
    ASSERT_NULL(op,  0);
    if (op->state == ERROR_IO_PENDING) {
        if (!GetOverlappedResult(op->pipe, (LPOVERLAPPED)op,
                                &op->read, FALSE)) {
            op->state = GetLastError();
        }
        else {
            op->state = 0;
            rc = logwrdata(log, op->buffer, op->read);
        }
    }
    else {
        if (ReadFile(op->pipe, op->buffer, SVCBATCH_PIPE_LEN,
                    &op->read, (LPOVERLAPPED)op) && op->read) {
            op->state = 0;
            rc = logwrdata(log, op->buffer, op->read);
            if (rc == 0)
                SetEvent(op->o.hEvent);
        }
        else {
            op->state = GetLastError();
            if (op->state != ERROR_IO_PENDING)
                rc = op->state;
        }
    }
    if (rc) {
        CancelIo(op->pipe);
        ResetEvent(op->o.hEvent);
        if ((rc == ERROR_BROKEN_PIPE) || (rc == ERROR_NO_DATA)) {
            DBG_PRINTS("pipe closed");
        }
        else if (rc == ERROR_NO_MORE_FILES) {
            DBG_PRINTS("log file closed");
        }
        else if (rc == ERROR_OPERATION_ABORTED) {
            DBG_PRINTS("pipe aborted");
        }
        else {
            DBG_PRINTF("error %lu", rc);
        }
    }

    return rc;
}

static DWORD WINAPI workerthread(void *unused)
{
    HANDLE   rd = NULL;
    HANDLE   wr = NULL;
    LPHANDLE rp = NULL;
    LPHANDLE wp = NULL;
    DWORD    rc = 0;
    DWORD    ws = 0;
    DWORD    cf = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
    LPSVCBATCH_PIPE op = NULL;

    DBG_PRINTS("started");
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STARTING);

    if (outputlog)
        rp = &rd;
    if (IS_OPT_SET(SVCBATCH_OPT_WRSTDIN))
        wp = &wr;
    rc = createiopipes(&cmdproc->sInfo, wp, rp, FILE_FLAG_OVERLAPPED);
    if (rc != 0) {
        DBG_PRINTF("createiopipes failed with %lu", rc);
        setsvcstatusexit(rc);
        cmdproc->exitCode = rc;
        goto finished;
    }

    if (outputlog) {
        op = (LPSVCBATCH_PIPE)xmcalloc(sizeof(SVCBATCH_PIPE));
        op->pipe     = rd;
        op->o.hEvent = CreateEventEx(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(op->o.hEvent)) {
            rc = GetLastError();
            setsvcstatusexit(rc);
            xsyserror(rc, L"CreateEvent", NULL);
            cmdproc->exitCode = rc;
            goto finished;
        }
    }
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);
    DBG_PRINTF("cmdline %S", cmdproc->commandLine);
    if (IS_OPT_SET(SVCBATCH_OPT_CTRL_BREAK))
        cf |= CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(cmdproc->application,
                        cmdproc->commandLine,
                        NULL,
                        NULL,
                        TRUE,
                        cf,
                        service->environment,
                        service->work,
                       &cmdproc->sInfo,
                       &cmdproc->pInfo)) {
        rc = GetLastError();
        setsvcstatusexit(rc);
        xsyserror(rc, L"CreateProcess", cmdproc->application);
        cmdproc->exitCode = rc;
        goto finished;
    }
    /**
     * Close our side of the pipes
     */
    SAFE_CLOSE_HANDLE(cmdproc->sInfo.hStdInput);
    SAFE_CLOSE_HANDLE(cmdproc->sInfo.hStdError);

    ResumeThread(cmdproc->pInfo.hThread);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_RUNNING);
    xsvcstatus(SERVICE_RUNNING, 0);

    DBG_PRINTF("running %lu", cmdproc->pInfo.dwProcessId);
    if (IS_OPT_SET(SVCBATCH_OPT_WRSTDIN)) {
        InterlockedExchangePointer(&wrpipehandle, wr);
        ResumeThread(threads[SVCBATCH_STDIN_THREAD].thread);
    }
    if (IS_OPT_SET(SVCBATCH_OPT_ROTATE)) {
        ResumeThread(threads[SVCBATCH_ROTATE_THREAD].thread);
    }
    SAFE_CLOSE_HANDLE(cmdproc->pInfo.hThread);
    if (outputlog) {
        HANDLE wh[2];
        DWORD  nw = 2;

        wh[0] = cmdproc->pInfo.hProcess;
        wh[1] = op->o.hEvent;
        do {
            ws = WaitForMultipleObjects(nw, wh, FALSE, INFINITE);
            switch (ws) {
                case WAIT_OBJECT_0:
                    nw = 0;
                    DBG_PRINTS("process signaled");
                break;
                case WAIT_OBJECT_1:
                    rc = logiodata(outputlog, op);
                    if (rc)
                        nw = 1;
                break;
                default:
                    nw = 0;
                    DBG_PRINTF("wait failed %lu with %lu", ws, GetLastError());
                break;
            }
        } while (nw);
        if (rc == 0) {
            DBG_PRINTS("cancel pipe");
            CancelIo(op->pipe);
        }
    }
    else {
        ws = WaitForSingleObject(cmdproc->pInfo.hProcess, INFINITE);
    }
    DBG_PRINTF("stopping %lu", cmdproc->pInfo.dwProcessId);
    InterlockedExchange(&cmdproc->state, SVCBATCH_PROCESS_STOPPING);
    if (IS_OPT_SET(SVCBATCH_OPT_WRSTDIN) && threads[SVCBATCH_STDIN_THREAD].started) {
        if (WaitForSingleObject(threads[SVCBATCH_STDIN_THREAD].thread, SVCBATCH_STOP_STEP)) {
            DBG_PRINTS("stdinthread is still active ... calling CancelSynchronousIo");
            CancelSynchronousIo(threads[SVCBATCH_STDIN_THREAD].thread);
        }
    }
    if (!GetExitCodeProcess(cmdproc->pInfo.hProcess, &rc))
        rc = GetLastError();
    if (rc) {
        if ((ws != WAIT_OBJECT_0) && (rc == STILL_ACTIVE)) {
            DBG_PRINTF("terminating %lu", cmdproc->pInfo.dwProcessId);
            rc = ERROR_ARENA_TRASHED;
            TerminateProcess(cmdproc->pInfo.hProcess, rc);
        }
        if ((rc != 0x000000FF) && (rc != 0xC000013A)) {
            /**
             * Discard common error codes
             * 255 is exit code when CTRL_C is send to cmd.exe
             */
            cmdproc->exitCode = rc;
        }
    }
    DBG_PRINTF("finished %lu with %lu",
               cmdproc->pInfo.dwProcessId,
               cmdproc->exitCode);

finished:
    if (op != NULL) {
        SAFE_CLOSE_HANDLE(op->pipe);
        SAFE_CLOSE_HANDLE(op->o.hEvent);
        xfree(op);
    }
    closeprocess(cmdproc);
    DBG_PRINTS("done");
    SetEvent(workerended);
    return cmdproc->exitCode;
}

static BOOL WINAPI consolehandler(DWORD ctrl)
{
    BOOL rv = TRUE;

    switch (ctrl) {
        case CTRL_CLOSE_EVENT:
            DBG_PRINTS("signaled CTRL_CLOSE_EVENT");
        break;
        case CTRL_SHUTDOWN_EVENT:
            DBG_PRINTS("signaled CTRL_SHUTDOWN_EVENT");
        break;
        case CTRL_C_EVENT:
            DBG_PRINTS("signaled CTRL_C_EVENT");
        break;
        case CTRL_BREAK_EVENT:
            DBG_PRINTS("signaled CTRL_BREAK_EVENT");
        break;
        case CTRL_LOGOFF_EVENT:
            DBG_PRINTS("signaled CTRL_LOGOFF_EVENT");
        break;
        default:
            DBG_PRINTF("unknown control %lu", ctrl);
            rv = FALSE;
        break;
    }
    return rv;
}

static DWORD WINAPI servicehandler(DWORD ctrl, DWORD _xe, LPVOID _xd, LPVOID _xc)
{
    switch (ctrl) {
        case SERVICE_CONTROL_PRESHUTDOWN:
            /* fall through */
        case SERVICE_CONTROL_SHUTDOWN:
            /* fall through */
            InterlockedExchange(&service->killDepth, 0);
        case SERVICE_CONTROL_STOP:
            DBG_PRINTF("service control %lu", ctrl);
            SVCBATCH_CS_ENTER(service);
            if (service->state == SERVICE_RUNNING) {
                xsvcstatus(SERVICE_STOP_PENDING, service->timeout);
                xcreatethread(SVCBATCH_STOP_THREAD, 0, stopthread, INVALID_HANDLE_VALUE);
            }
            SVCBATCH_CS_LEAVE(service);
        break;
        case SVCBATCH_CTRL_ROTATE:
            if (IS_OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIG)) {
                /**
                 * Signal to rotatethread that
                 * user send custom service control
                 */
                if (canrotatelogs(outputlog)) {
                    DBG_PRINTS("signaling SVCBATCH_CTRL_ROTATE");
                    SetEvent(dologrotate);
                }
                else {
                    DBG_PRINTS("rotatelogs is busy");
                    return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
                }
            }
            else {
                DBG_PRINTS("log rotation is disabled");
                return ERROR_INVALID_SERVICE_CONTROL;
            }
        break;
        case SERVICE_CONTROL_INTERROGATE:
            DBG_PRINTS("SERVICE_CONTROL_INTERROGATE");
        break;
        default:
            if ((ctrl > 127) && (ctrl < 256)) {
                DBG_PRINTF("invalid service control %lu", ctrl);
                return ERROR_INVALID_SERVICE_CONTROL;
            }
            else {
                DBG_PRINTF("unknown control 0x%08X", ctrl);
                return ERROR_CALL_NOT_IMPLEMENTED;
            }
        break;
    }
    return 0;
}

static void threadscleanup(void)
{
    int    i;
    HANDLE h;

    DBG_PRINTS("started");
    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        h = InterlockedExchangePointer(&threads[i].thread, NULL);
        if (h) {
            if (threads[i].started) {
                threads[i].duration = GetTickCount64() - threads[i].duration;
                threads[i].exitCode = ERROR_DISCARDED;
                TerminateThread(h, threads[i].exitCode);
            }
            DBG_PRINTF("%s %lu %llu",
                        threads[i].name,
                        threads[i].exitCode,
                        threads[i].duration);
            CloseHandle(h);
        }
        InterlockedExchange(&threads[i].started, 0);
    }
    DBG_PRINTS("done");
}

static void waitforthreads(DWORD ms)
{
    int i;
    HANDLE wh[SVCBATCH_MAX_THREADS];
    DWORD  nw = 0;

    DBG_PRINTS("started");
    for(i = 0; i < SVCBATCH_MAX_THREADS; i++) {
        if (threads[i].started) {
            DBG_PRINTF("%s", threads[i].name);
            wh[nw++] = threads[i].thread;
        }
    }
    if (nw) {
        DBG_PRINTF("wait for %d threads", nw);
        if (nw > 1)
            WaitForMultipleObjects(nw, wh, TRUE, ms);
        else
            WaitForSingleObject(wh[0], ms);
    }
    DBG_PRINTS("done");
}

static void __cdecl consolecleanup(void)
{
    FreeConsole();
    DBG_PRINTS("done");
}

static void __cdecl objectscleanup(void)
{
    SAFE_CLOSE_HANDLE(workerended);
    SAFE_CLOSE_HANDLE(svcstopdone);
    SAFE_CLOSE_HANDLE(stopstarted);
    SAFE_CLOSE_HANDLE(dologrotate);
    SAFE_CLOSE_HANDLE(svclogmutex);
    if (sharedmem)
        UnmapViewOfFile(sharedmem);
    SAFE_CLOSE_HANDLE(sharedmmap);
    SVCBATCH_CS_CLOSE(service);
    DBG_PRINTS("done");
}

static void xinitvars(void)
{
    svariables = (LPSVCBATCH_VARIABLES)xmcalloc(sizeof(SVCBATCH_VARIABLES));
    svariables->siz = SBUFSIZ;

    xsetsysvar('A', L"APPLICATION", xnopprefix(program->application));
    xsetsysvar('B', L"BASENAME",    program->name);
    xsetsysvar('D', L"DISPLAYNAME", service->display);
    xsetsysvar('N', L"NAME",        service->name);
    xsetsysvar('U', L"UUID",        service->uuid);
    xsetsysvar('V', L"VERSION",     SVCBATCH_VERSION_VER);
    xsetsysvar('R', L"RELEASE",     SVCBATCH_VERSION_REL);
    xsetsysvar('T', L"TEMP",        xgettempdir());

    xsetsysvar('H', L"HOME",        NULL);
    xsetsysvar('L', L"LOGS",        NULL);
    xsetsysvar('W', L"WORK",        NULL);

    xsetsysvar('X', L"PREFIX",      NULL);

    svariables->pos = SYSVARS_COUNT;
}

static void xinitconf(void)
{
    int i;
    svcpparams = (LPSVCBATCH_CONF_VALUE)xmcalloc(SVCBATCH_CFG_MAX * sizeof(SVCBATCH_CONF_VALUE));
    svcsparams = (LPSVCBATCH_CONF_VALUE)xmcalloc(SVCBATCH_SVC_MAX * sizeof(SVCBATCH_CONF_VALUE));

    for (i = 0; i < SVCBATCH_CFG_MAX; i++) {
        svcpparams[i].name = svccparams[i].name;
        svcpparams[i].type = svccparams[i].type;
    }
    for (i = 0; i < SVCBATCH_SVC_MAX; i++) {
        svcsparams[i].name = svcbparams[i].name;
        svcsparams[i].type = svcbparams[i].type;
    }
    svcparams[0] = svcsparams;
    svcparams[1] = svcpparams;
}

static DWORD createevents(void)
{
    workerended = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return GetLastError();
    svcstopdone = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET | CREATE_EVENT_INITIAL_SET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(svcstopdone))
        return GetLastError();
    stopstarted = CreateEventExW(NULL, NULL,
                                 CREATE_EVENT_MANUAL_RESET,
                                 EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(stopstarted))
        return GetLastError();
    if (IS_OPT_SET(SVCBATCH_OPT_ROTATE)) {
        dologrotate = CreateEventExW(NULL, NULL,
                                     CREATE_EVENT_MANUAL_RESET,
                                     EVENT_MODIFY_STATE | SYNCHRONIZE);
        if (IS_INVALID_HANDLE(dologrotate))
            return GetLastError();
    }
    return 0;
}

static int hasconfvar(int p, int i)
{
    return svcparams[p][i].size;
}

static int getconfnum(int p, int i)
{
    if (svcparams[p][i].size)
        return svcparams[p][i].dval;
    else
        return 0;
}

static int getconfval(int p, int i, int d)
{
    if (svcparams[p][i].size)
        return svcparams[p][i].dval;
    else
        return d;
}

static LPCWSTR getconfwcs(int p, int i)
{
    if (svcparams[p][i].size) {
        if (IS_VALID_WCS(svcparams[p][i].sval))
            return svcparams[p][i].sval;
    }
    return NULL;
}

static LPWSTR getconfmsz(int p, int i)
{
    if (svcparams[p][i].size) {
        if (IS_VALID_WCS(svcparams[p][i].sval))
            return (LPWSTR)svcparams[p][i].data;
    }
    return NULL;
}

static LPBYTE getconfbin(int p, int i, int *s)
{
    if (svcparams[p][i].size) {
        *s = svcparams[p][i].size;
        return svcparams[p][i].data;
    }
    *s = 0;
    return NULL;
}

static DWORD getsvcconfig(SC_HANDLE svc, LPQUERY_SERVICE_CONFIGW *lpsc)
{
    DWORD   rv;
    DWORD   nb;
    DWORD   cb = 0;
    LPQUERY_SERVICE_CONFIGW sc = NULL;

    if (!QueryServiceConfigW(svc, NULL, 0, &nb)) {
        rv = GetLastError();
        if (rv == ERROR_INSUFFICIENT_BUFFER) {
            cb = nb;
            sc = (LPQUERY_SERVICE_CONFIGW)xmcalloc(cb);
        }
        else {
            return rv;
        }
    }
    if (!QueryServiceConfigW(svc, sc, cb, &nb)) {
        rv = GetLastError();
        xfree(sc);
        return rv;
    }
    *lpsc = sc;
    return 0;
}

static DWORD getsvcpparams(int id, LPCWSTR *e)
{
    int     i;
    DWORD   t;
    HKEY    k = NULL;
    LPBYTE  b = NULL;
    LSTATUS s;
    WCHAR   name[BBUFSIZ];
    LPSVCBATCH_CONF_VALUE params;

   *e = L"RegOpenKey";
    i = xwcslcat(name, BBUFSIZ, 0, SYSTEM_SVC_SUBKEY);
    i = xwcslcat(name, BBUFSIZ, i, service->name);
    if (id)
    i = xwcslcat(name, BBUFSIZ, i, L"\\" SVCBATCH_PARAMS_KEY);
    if (i >= BBUFSIZ)
        return ERROR_BUFFER_OVERFLOW;
    s = RegOpenKeyExW(HKEY_LOCAL_MACHINE, name, 0, KEY_READ, &k);
    if (s != ERROR_SUCCESS) {
        if (s == ERROR_FILE_NOT_FOUND)
            return 0;
        else
            return s;
    }
    i = 0;
    params = svcparams[id];
    while (params[i].name != NULL) {
        DWORD d = 0;
        DWORD c = 0;
        if (params[i].type == SVCBATCH_REG_TYPE_NUM) {
            c = 4;
            s = RegGetValueW(k, NULL,
                             params[i].name,
                             svcbrrfrtypes[params[i].type],
                             &t,   &d, &c);
        }
        else if (params[i].type == SVCBATCH_REG_TYPE_BOOL) {
            c = 4;
            s = RegGetValueW(k, NULL,
                             params[i].name,
                             svcbrrfrtypes[params[i].type],
                             &t,   &d, &c);
        }
        else {
            c = 0;
            s = RegGetValueW(k, NULL,
                             params[i].name,
                             svcbrrfrtypes[params[i].type],
                             &t, NULL, &c);
        }
        if (s == ERROR_FILE_NOT_FOUND) {
            s = ERROR_SUCCESS;
            i++;
            continue;
        }
        if (s != ERROR_SUCCESS) {
           *e = params[i].name;
            break;
        }
        if (params[i].type == SVCBATCH_REG_TYPE_BOOL) {
            if ((d != 0) && (d != 1)) {
                s = ERROR_INVALID_DATA;
               *e = params[i].name;
                break;
            }
            params[i].dval = d;
        }
        else if (params[i].type == SVCBATCH_REG_TYPE_NUM) {
            params[i].dval = d;
        }
        else {
            if (c) {
                b = (LPBYTE)xmmalloc(c);
                s = RegGetValueW(k, NULL,
                                 params[i].name,
                                 svcbrrfrtypes[params[i].type],
                                 &t, b, &c);
                if (s != ERROR_SUCCESS) {
                    xfree(b);
                   *e = params[i].name;
                    break;
                }
                params[i].data = b;
                params[i].sval = (LPCWSTR)b;
                if ((params[i].type != SVCBATCH_REG_TYPE_BIN) &&
                    IS_EMPTY_WCS(params[i].sval)) {
                    s = ERROR_INVALID_DATA;
                   *e = params[i].name;
                    break;
                }
            }
            else {
                s = ERROR_INVALID_DATA;
               *e = params[i].name;
                break;
            }
        }
        params[i].size = c;
        i++;
    }
    RegCloseKey(k);
    return s;
}

static LPWSTR resolvescript(LPCWSTR p, LPWSTR *d)
{
    LPWSTR n;

    n = xgetfinalpath(0, p);
    if (IS_EMPTY_WCS(n))
        return NULL;
    if (d) {
        LPWSTR s;
        s = xwcsrchr(n, L'\\');
        if (s == NULL) {
            SetLastError(ERROR_BAD_PATHNAME);
            return NULL;
        }
        *d = xwcsndup(n, (size_t)(s - n));
    }
    return n;
}

static int parseoptions(int sargc, LPWSTR *sargv)
{
    DWORD    x;
    DWORD    cx;
    int      i;
    int      opt;
    int      eenvx;
    int      wargc;
    LPCWSTR *wargv;
    WCHAR    eenvp[SVCBATCH_NAME_MAX];

    LPCWSTR  cp;
    LPWSTR   wp;
    LPWSTR   pp;
    LPWSTR   svcscriptdir = NULL;
    LPCWSTR  eexportparam = NULL;
    LPCWSTR  eprefixparam = NULL;
    LPCWSTR  svcmainparam = NULL;
    LPCWSTR  svchomeparam = NULL;
    LPCWSTR  svcworkparam = NULL;
    LPCWSTR  svcstopparam = NULL;
    LPCWSTR  commandparam = NULL;
    LPCWSTR  svclogsparam = NULL;
    LPCWSTR  svclogfname  = NULL;
    LPCWSTR  tempdirparam = NULL;
    LPCWSTR  errmsg       = NULL;
#if HAVE_EXTRA_SVC_PARAMS
    LPCWSTR  sdescription = NULL;
    LPCWSTR  svcimagepath = NULL;
    LPCWSTR  svcusername  = NULL;
#endif
    DBG_PRINTS("started");
    x = getsvcpparams(1, &errmsg);
    if (x != ERROR_SUCCESS)
        return xsyserror(x, SVCBATCH_PARAMS_KEY, errmsg);
    wargc    = svcmainargc;
    wargv    = svcmainargv;
    wargv[0] = service->name;
#if HAVE_EXTRA_SVC_PARAMS
    svcimagepath = getconfwcs(0, SVCBATCH_SVC_IMAGEPATH);
    svcusername  = getconfwcs(0, SVCBATCH_SVC_USERNAME);
    sdescription = getconfwcs(0, SVCBATCH_SVC_DESC);
#endif
    svchomeparam = skipdotslash(getconfwcs(1, SVCBATCH_CFG_HOME));
    tempdirparam = skipdotslash(getconfwcs(1, SVCBATCH_CFG_TEMP));
    svcworkparam = skipdotslash(getconfwcs(1, SVCBATCH_CFG_WORK));

    eexportparam = getconfwcs(1, SVCBATCH_CFG_ENVEXPORT);
    eprefixparam = getconfwcs(1, SVCBATCH_CFG_ENVPREFIX);
    service->killDepth = getconfnum(1, SVCBATCH_CFG_KILLDEPTH);
    if ((service->killDepth < 0) || (service->killDepth > SVCBATCH_MAX_KILLDEPTH))
        return xsyserrno(13, L"KillDepth", xntowcs(service->killDepth));
    if (hasconfvar(1, SVCBATCH_CFG_STOP))
        svcstop = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));
    if (hasconfvar(1, SVCBATCH_CFG_TIMEOUT)) {
        opt = getconfnum(1, SVCBATCH_CFG_TIMEOUT);
        if ((opt < SVCBATCH_STOP_TMIN) || (opt > SVCBATCH_STOP_TMAX))
            return xsyserrno(13, L"StopTimeout", xntowcs(opt));
        cmdproc->timeout = opt;
        service->timeout = opt;
        if (svcstop)
            service->timeout += SVCBATCH_STOP_WAIT;
        else
            service->timeout += SVCBATCH_STOP_SYNC;
    }
    if (hasconfvar(1, SVCBATCH_CFG_STDINDATA)) {
        stdindata = getconfbin(1, SVCBATCH_CFG_STDINDATA, &stdinsize);
        SVCOPT_SET(SVCBATCH_OPT_WRSTDIN);
    }
    if (getconfnum(1, SVCBATCH_CFG_LOCALTIME))
        SVCOPT_SET(SVCBATCH_OPT_LOCALTIME);
    if (getconfnum(1, SVCBATCH_CFG_SENDBREAK))
        SVCOPT_SET(SVCBATCH_OPT_CTRL_BREAK);
    if (getconfnum(1, SVCBATCH_CFG_PRESHUTDOWN)) {
        preshutdown = SERVICE_ACCEPT_PRESHUTDOWN;
        SVCOPT_SET(SVCBATCH_OPT_PRESHUTDOWN);
        if (hasconfvar(0, SVCBATCH_SVC_PSDTIMEOUT)) {
            opt = getconfnum(0, SVCBATCH_SVC_PSDTIMEOUT);
            if ((opt < SVCBATCH_STOP_TDEF) ||
                (opt > SVCBATCH_STOP_TMAX))
                return xsyserrno(13, L"PreshudownTimeout", xntowcs(opt));
            service->timeout = opt;
            if (service->timeout > (cmdproc->timeout + SVCBATCH_STOP_WAIT))
                cmdproc->timeout = service->timeout  - SVCBATCH_STOP_WAIT;
        }
    }
    if (getconfnum(1, SVCBATCH_CFG_NOLOGGING)) {
        SVCOPT_SET(SVCBATCH_OPT_QUIET);
    }
    if (IS_NOT_OPT(SVCBATCH_OPT_QUIET)) {
        svclogsparam = skipdotslash(getconfwcs(1, SVCBATCH_CFG_LOGS));
        svclogfname  = getconfwcs(1, SVCBATCH_CFG_LOGNAME);
        if (svcstop) {
            stoplogname = getconfwcs(1, SVCBATCH_CFG_SLOGNAME);
            if (hasconfvar(1, SVCBATCH_CFG_SMAXLOGS)) {
                stopmaxlogs = getconfnum(1, SVCBATCH_CFG_SMAXLOGS);
                if ((stopmaxlogs < 0) || (stopmaxlogs > SVCBATCH_MAX_LOGS))
                    return xsyserrno(13, L"StopMaxLogs",  xntowcs(stopmaxlogs));
            }
        }
        if (getconfnum(1, SVCBATCH_CFG_TRUNCATE))
            SVCOPT_SET(SVCBATCH_OPT_TRUNCATE);
        if (getconfnum(1, SVCBATCH_CFG_LOGROTATE)) {
            SVCOPT_SET(SVCBATCH_OPT_ROTATE);
            DBG_PRINTS("rotate");
            cx = getconfnum(1, SVCBATCH_CFG_ROTATESIZE);
            if (cx > 0) {
                if (cx < SVCBATCH_MIN_ROTATE_SIZ)
                    return xsyserrno(13, L"LogRotateSize", xntowcs(cx));
                rotatesize = cx;
                SVCOPT_SET(SVCBATCH_OPT_ROTATE_BY_SIZE);
                DBG_PRINTF("size %llu", rotatesize);
            }
            cp = getconfwcs(1, SVCBATCH_CFG_ROTATETIME);
            cx = getconfnum(1, SVCBATCH_CFG_ROTATEINT);
            if (cp && cx)
                return xsyserrno(29, L"LogRotateTime and LogRotateInterval parameters", NULL);
            if (cp) {
                if (!xarotatetime(cp))
                    return xsyserrno(12, L"LogRotateTime", cp);
            }
            if (cx) {
                if ((cx < SVCBATCH_MIN_ROTATE_INT) || (cx > SVCBATCH_MAX_ROTATE_INT))
                    return xsyserrno(13, L"LogRotateInterval", xntowcs(cx));
                xirotatetime(cx);
            }
            if (getconfval(1, SVCBATCH_CFG_ROTATEBYSIG, 1))
                SVCOPT_SET(SVCBATCH_OPT_ROTATE_BY_SIG);
            DBG_PRINTF("ctrl %s", IS_OPT_SET(SVCBATCH_OPT_ROTATE_BY_SIG) ? "Yes" : "No");
        }
    }
    service->failMode = getconfval(1, SVCBATCH_CFG_FAILMODE, SVCBATCH_FAIL_ERROR);
    if (service->failMode > SVCBATCH_FAIL_EXIT) {
        return xsyserrno(13, L"FailMode", xntowcs(service->failMode));
    }

    cp = getconfmsz(1, SVCBATCH_CFG_ENVSET);
    if (cp != NULL) {
        for (; *cp; cp++) {
            pp = xwcsdup(cp);
            wp = xwcschr(pp, L'=');
            if ((wp == NULL) || (wp == pp))
                return xsyserrno(11, L"Environment", cp);
            *(wp++) = WNUL;
            if (IS_EMPTY_WCS(wp))
                return xsyserrno(11, L"Environment", cp);
            if (!xaddsysvar('e', pp, wp))
                return xsyserror(GetLastError(), L"SetUserEnvironment", pp);

            while (*cp)
                cp++;
        }
    }
#if HAVE_DEBUG_TRACE
    if (xtraceservice) {
        DBG_PRINTF("home %S",   svchomeparam);
        DBG_PRINTF("logs %S",   svclogsparam);
        DBG_PRINTF("work %S",   svcworkparam);
        DBG_PRINTF("temp %S",   tempdirparam);
        DBG_PRINTF("export %S", eexportparam);
        DBG_PRINTF("prefix %S", eprefixparam);
        for (i = 0; i < wargc; i++) {
        DBG_PRINTF("argv[%d] %S", i, wargv[i]);
        }
    }
#endif
    xwoptind = 1;
    xwoption = NULL;
    while ((opt = xwgetopt(wargc, wargv, cmdoptions)) != 0) {
        switch (opt) {
            case 'h':
                if (svchomeparam)
                    return xsyserrno(10, L"Home", xwoptarg);
                svchomeparam = skipdotslash(xwoptarg);
            break;
            case 'l':
                if (svclogsparam)
                    return xsyserrno(10, L"Logs", xwoptarg);
                svclogsparam = skipdotslash(xwoptarg);
            break;
            case 'w':
                if (svcworkparam)
                    return xsyserrno(10, L"Work", xwoptarg);
                svcworkparam = skipdotslash(xwoptarg);
            break;
            case 'q':
            case 'd':
            break;
            case ERROR_BAD_LENGTH:
                return xsyserrno(11, xwoption, NULL);
            break;
            default:
                return xsyserrno(22, zerostring, xwoption);
            break;
        }
    }
    wargc -= xwoptind;
    wargv += xwoptind;

    cp = getconfmsz(1, SVCBATCH_CFG_CMD);
    if (cp != NULL) {
        for (; *cp; cp++) {
            if (cmdproc->optc < SVCBATCH_MAX_ARGS)
                cmdproc->opts[cmdproc->optc++] = cp;
            else
                return xsyserrno(16, L"Options", cp);
            while (*cp)
                cp++;
        }
    }
    commandparam = cmdproc->opts[0];

    if (wargc && getconfmsz(1, SVCBATCH_CFG_ARGS))
        return xsyserrno(10, L"Arguments", NULL);
    for (i = 0; i < wargc; i++) {
        /**
         * Add arguments from ImagePath
         */
        if (cmdproc->argc < SVCBATCH_MAX_ARGS)
            cmdproc->args[cmdproc->argc++] = wargv[i];
        else
            return xsyserrno(17, L"Arguments",  wargv[i]);
    }
    cp = getconfmsz(1, SVCBATCH_CFG_ARGS);
    if (cp != NULL) {
        for (; *cp; cp++) {
            if (cmdproc->argc < SVCBATCH_MAX_ARGS)
                cmdproc->args[cmdproc->argc++] = cp;
            else
                return xsyserrno(17, L"Arguments", cp);
            while (*cp)
                cp++;
        }
    }
    if ((cmdproc->argc == 0) && (commandparam == NULL))  {
        /**
         * Use service_name.bat
         */
        cmdproc->args[cmdproc->argc++] = xwmakepath(L".", service->name, L".bat");
    }
    for (i = 1; i < sargc; i++) {
        if (cmdproc->argc < SVCBATCH_MAX_ARGS)
            cmdproc->args[cmdproc->argc++] = sargv[i];
        else
            return xsyserrno(17, L"Arguments",  sargv[i]);
    }
    svcmainparam = cmdproc->args[0];

    if (IS_OPT_SET(SVCBATCH_OPT_QUIET)) {
        /**
         * Discard any log rotate related command options
         * when -q is defined
         */
        InterlockedAnd(&svcoptions, SVCBATCH_OPT_MASK);
        stoplogname  = NULL;
        svclogfname  = NULL;
        svclogsparam = NULL;
        stopmaxlogs  = 0;
    }
    else {
        outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));
        outputlog->logName = svclogfname ? svclogfname : SVCBATCH_LOGNAME;
        if (xwcschr(outputlog->logName, L'@')) {
            if (xchkftime(outputlog->logName))
                return xsyserror(GetLastError(), outputlog->logName, NULL);
        }
        else {
            outputlog->maxLogs = SVCBATCH_DEF_LOGS;
            if (hasconfvar(1, SVCBATCH_CFG_MAXLOGS)) {
                outputlog->maxLogs = getconfnum(1, SVCBATCH_CFG_MAXLOGS);
                if ((outputlog->maxLogs < 0) || (outputlog->maxLogs > SVCBATCH_MAX_LOGS))
                    return xsyserrno(13, L"MaxLogs",  xntowcs(outputlog->maxLogs));
            }
        }
        SVCBATCH_CS_INIT(outputlog);
    }
    if (eprefixparam) {
        if (xwcschr(eprefixparam, L'$')) {
            wp = xexpandenvstr(eprefixparam, namevarset);
            if (wp == NULL)
                return xsyserror(GetLastError(), SVCBATCH_MSG(4), eprefixparam);
            eprefixparam = wp;
        }
    }
    if (eprefixparam == NULL)
        eprefixparam = program->name;
    if (!xisvalidvarname(eprefixparam))
        return xsyserrno(20, SVCBATCH_MSG(4),  eprefixparam);
    /**
     * The size must accommodate trailing
     * underscore character and largest system
     * environment name, eg. '_DISPLAYNAME'
     */
    eenvx = xwcslcpy(eenvp, SVCBATCH_NAME_MAX - 16, eprefixparam);
    if (eenvx >= (SVCBATCH_NAME_MAX - 16))
        return xsyserrno(21, SVCBATCH_MSG(4),  eprefixparam);
    xwcsupper(eenvp);
    SETSYSVAR_VAL('X', xwcsdup(eenvp));

    eenvp[eenvx++] = L'_';
    eenvp[eenvx]   = WNUL;

    /**
     * Find the location of SVCBATCH_SERVICE_HOME
     * all relative paths are resolved against it.
     *
     * 1. If -h is defined and is absolute path
     *    set it as SetCurrentDirectory and use it as
     *    home directory for resolving other relative paths
     *
     * 2. If batch file is defined as absolute path
     *    set it as SetCurrentDirectory and resolve -h parameter
     *    if defined as relative path. If -h was defined and
     *    and is resolved as valid path set it as home directory.
     *    If -h was defined and cannot be resolved fail.
     *
     * 3. Use running svcbatch.exe directory and set it as
     *    SetCurrentDirectory.
     *    If -h parameter was defined, resolve it and set as home
     *    directory or fail.
     *    In case -h was not defined resolve batch file and set its
     *    directory as home directory or fail if it cannot be resolved.
     *
     */
    if (isabsolutepath(svcmainparam)) {
        cmdproc->args[0] = resolvescript(svcmainparam, &svcscriptdir);
        if (IS_EMPTY_WCS(cmdproc->args[0]))
            return xsyserror(ERROR_FILE_NOT_FOUND, svcmainparam, NULL);
        DBG_PRINTF("main %S", svcscriptdir);
    }
    if (isabsolutepath(svchomeparam)) {
        service->home = xgetfinalpath(1, svchomeparam);
        if (IS_EMPTY_WCS(service->home))
            return xsyserror(GetLastError(), svchomeparam, NULL);
    }
    else {
        if (IS_EMPTY_WCS(svcscriptdir))
            svcscriptdir = program->directory;
        if (IS_EMPTY_WCS(svchomeparam)) {
            service->home = svcscriptdir;
            DBG_PRINTF("home %S", service->home);
        }
        else {
            SetCurrentDirectoryW(svcscriptdir);
            service->home = xgetfinalpath(1, svchomeparam);
            if (IS_EMPTY_WCS(service->home))
                return xsyserror(GetLastError(), svchomeparam, NULL);
            DBG_PRINTF("home %S -> %S", svchomeparam, service->home);
        }
    }
    SetCurrentDirectoryW(service->home);
    if (IS_EMPTY_WCS(svcworkparam)) {
        /* Use the same directories for home and work */
        service->work = service->home;
        DBG_PRINTS("work $HOME");
    }
    else {
        service->work = xgetfinalpath(2, svcworkparam);
        if (IS_EMPTY_WCS(service->work))
            return xsyserror(GetLastError(), svcworkparam, NULL);
        DBG_PRINTF("work %S -> %S", svcworkparam, service->work);
    }
    if (isrelativepath(svcmainparam)) {
        if (isnotapath(svcmainparam)) {
            cmdproc->args[0] = xwcsdup(svcmainparam);
            DBG_PRINTF("main %S", cmdproc->args[0]);
        }
        else {
            cp = svcmainparam;
            if (isdotslash(cp))
                cp += 2;
            cmdproc->args[0] = resolvescript(cp, NULL);
            if (IS_EMPTY_WCS(cmdproc->args[0]))
                return xsyserror(ERROR_FILE_NOT_FOUND, svcmainparam, NULL);
            DBG_PRINTF("main %S -> %S", svcmainparam, cmdproc->args[0]);
        }
    }
    if (outputlog) {
        if (IS_EMPTY_WCS(svclogsparam))
            svclogsparam = SVCBATCH_LOGSDIR;
        wp = xexpandenvstr(svclogsparam, namevarset);
        if (wp == NULL)
            return xsyserror(GetLastError(), svclogsparam, NULL);
        service->logs = createsvcdir(wp);
        if (IS_EMPTY_WCS(service->logs))
            return xsyserror(ERROR_BAD_PATHNAME, wp, NULL);
        if (wp != svclogsparam)
            xfree(wp);
#if HAVE_LOGDIR_MUTEX
        pp = xgenresname(xnopprefix(service->logs));
        svclogmutex = CreateMutexW(NULL, FALSE, pp);
        if ((svclogmutex == NULL) || (GetLastError() == ERROR_ALREADY_EXISTS))
            return xsyserror(GetLastError(),
                             L"Cannot create mutex for the following directory",
                             service->logs);
        DBG_PRINTF("logmutex %S", pp);
#else
# if HAVE_LOGDIR_LOCK
        wp = xwmakepath(service->logs, L".lock", NULL);
        svclogmutex = CreateFileW(wp, GENERIC_READ | GENERIC_WRITE,
                                  0, NULL, CREATE_ALWAYS,
                                  FILE_FLAG_DELETE_ON_CLOSE |
                                  FILE_ATTRIBUTE_TEMPORARY  |
                                  FILE_ATTRIBUTE_HIDDEN, NULL);
        if (svclogmutex == INVALID_HANDLE_VALUE)
            return xsyserror(GetLastError(),
                             L"Cannot create lock file in the following directory",
                             service->logs);
        DBG_PRINTF("lockfile : %S", wp);
        xfree(wp);
# endif
#endif
    }
    else {
        /**
         * Use work directory as logs directory
         * for quiet mode.
         */
        service->logs = service->work;
    }

    SETSYSVAR_VAL('H', xnopprefix(service->home));
    SETSYSVAR_VAL('L', xnopprefix(service->logs));
    SETSYSVAR_VAL('W', xnopprefix(service->work));
#if HAVE_DEBUG_TRACE
    if (xtraceservice) {
        DBG_PRINTF("home %S",   service->home);
        DBG_PRINTF("logs %S",   service->logs);
        DBG_PRINTF("work %S",   service->work);
        DBG_PRINTF("prefix %S", eenvp);
    }
#endif
    if (tempdirparam) {
        wp = xexpandenvstr(tempdirparam, NULL);
        if (wp == NULL)
            return xsyserror(GetLastError(), tempdirparam, NULL);
        pp = createsvcdir(wp);
        if (IS_EMPTY_WCS(pp))
            return xsyserror(ERROR_BAD_PATHNAME, wp, NULL);
        if (wp != tempdirparam)
            xfree(wp);
        if (xispprefix(pp))
            return xsyserror(ERROR_BUFFER_OVERFLOW, pp, NULL);
        SetEnvironmentVariableW(L"TMP",  pp);
        SetEnvironmentVariableW(L"TEMP", pp);
        SETSYSVAR_VAL('T', pp);
        DBG_PRINTF("temp %S", pp);
    }
    if (eexportparam) {
        if (xwcstob(eexportparam, &x) == ERROR_SUCCESS) {
            if (x == 0)
                eexportparam = NULL;
            else
                return xsyserror(ERROR_INVALID_NAME, L"Export", eexportparam);
        }
        else {
            cp = eexportparam;
            if (xiswcschar(cp, L'*')) {
                eexportparam = allexportvars;
            }
            else {
                if (*cp == L'+') {
                    eexportparam = xwcsconcat(defexportvars, cp + 1);
                    cp = eexportparam;
                }
                while (*cp) {
                    if (xwcschr(allexportvars, *cp) == NULL)
                        return xsyserrno(12, L"Export", xwctowcs(*cp));
                    cp++;
                }
            }
        }
    }
    else {
        /**
         * Use default export variables
         */
        eexportparam = defexportvars;
    }
    DBG_PRINTF("export %S", eexportparam);
    if (eexportparam) {
        /**
         * Add additional environment variables
         * that are unique to this service instance
         */
        cp = eexportparam;
        while (*cp) {
            if (IS_VALID_WCS(GETSYSVAR_VAL(*cp)) && (GETSYSVAR_KEY(*cp) != NULL)) {
                xwcslcat(eenvp, SVCBATCH_NAME_MAX, eenvx, GETSYSVAR_KEY(*cp));
                if (!SetEnvironmentVariableW(eenvp, GETSYSVAR_VAL(*cp)))
                    return xsyserror(GetLastError(), L"Export", eenvp);
                DBG_PRINTF("%S=%S", eenvp, GETSYSVAR_VAL(*cp));
                eenvp[eenvx] = WNUL;
            }
            cp++;
        }
    }
#if HAVE_DEBUG_TRACE
    if ((xtraceservice > 0) && (svariables->pos > SYSVARS_COUNT)) {
        for (i = SYSVARS_COUNT; i < svariables->pos; i++) {
            if (svariables->var[i].iid == 'e') {
                DBG_PRINTF("%S=%S", svariables->var[i].key, svariables->var[i].val);
            }
        }
    }
#endif
    for (i = SYSVARS_COUNT; i < svariables->pos; i++) {
        if (svariables->var[i].iid == 'e') {
            x = xsetenvvar(svariables->var[i].key, svariables->var[i].val);
            if (x)
                return xsyserror(x, L"Environment", svariables->var[i].key);
        }
    }
    if (commandparam) {
        /**
         * Search the current working folder,
         * and then search the folders that are
         * specified in the system path.
         *
         * This is the system default value.
         */
        wp = xexpandenvstr(commandparam, NULL);
        if (wp == NULL)
            return xsyserror(GetLastError(), commandparam, NULL);
        if (isrelativepath(wp))
            cmdproc->application = xsearchexe(wp);
        else
            cmdproc->application = xgetfinalpath(0, wp);
        if (cmdproc->application == NULL)
            return xsyserror(GetLastError(), wp, NULL);
        if (wp != commandparam)
            xfree(wp);
        cmdproc->opts[0] = cmdproc->application;
    }
    else {
        wp = xgetenv(L"COMSPEC");
        if (wp == NULL)
            return xsyserror(ERROR_BAD_ENVIRONMENT, L"GetEnvironment", L"COMSPEC");

        cmdproc->application = xgetfinalpath(0, wp);
        if (cmdproc->application == NULL)
            return xsyserror(GetLastError(), wp, NULL);
        xfree(wp);
        cmdproc->optc = 0;
        cmdproc->opts[cmdproc->optc++] = cmdproc->application;
        cmdproc->opts[cmdproc->optc++] = SVCBATCH_DEF_OPTS;
        SVCOPT_SET(SVCBATCH_OPT_WRSTDIN);
    }
    for (x = 1; x < cmdproc->argc; x++) {
        if (xwcschr(cmdproc->args[x], L'$')) {
            wp = xexpandenvstr(cmdproc->args[x], NULL);
            if (wp == NULL)
                return xsyserror(GetLastError(), L"ExpandEnvironment", cmdproc->args[x]);
            cmdproc->args[x] = wp;
        }
    }
    if (outputlog) {
        if (xwcschr(outputlog->logName, L'$')) {
            wp = xexpandenvstr(outputlog->logName, namevarset);
            if (wp == NULL)
                return xsyserror(GetLastError(), outputlog->logName, NULL);
            outputlog->logName = wp;
            if (xwcspbrk(outputlog->logName, INVALID_FILENAME_CHARS))
                return xsyserror(ERROR_INVALID_PARAMETER, SVCBATCH_MSG(23), outputlog->logName);
        }
    }
    if (svcstop) {
        cp = getconfmsz(1, SVCBATCH_CFG_STOP);
        if (cp != NULL) {
            for (; *cp; cp++) {
                if (svcstop->argc < SVCBATCH_MAX_ARGS)
                    svcstop->args[svcstop->argc++] = cp;
                else
                    return xsyserrno(16, L"StopArguments", cp);
                while (*cp)
                    cp++;
            }
        }
        if (svcstop->argc == 0)
            return xsyserrno(11, L"StopArguments", NULL);
        svcstopparam = svcstop->args[0];
        if ((svcstopparam[0] == L'$') &&
            (svcstopparam[1] == L'0') &&
            (svcstopparam[2] == WNUL)) {
            /**
             * Use the svcmainparam
             */
            svcstop->args[0] = cmdproc->args[0];
            if (svcstop->argc == 1)
                svcstop->args[svcstop->argc++] = L"stop";
        }
        else {
            wp = NULL;
            if (xwcschr(svcstopparam, L'$')) {
                wp = xexpandenvstr(svcstopparam, namevarset);
                if (wp == NULL)
                    return xsyserror(GetLastError(), svcstopparam, NULL);
                svcstopparam = wp;
            }
            if (isabsolutepath(svcstopparam) || isnotapath(svcstopparam))
                svcstop->args[0] = xwcsdup(svcstopparam);
            else if (isdotslash(svcstopparam))
                svcstop->args[0] = xwcsdup(svcstopparam + 2);
            else
                svcstop->args[0] = xgetfinalpath(0, svcstopparam);
            if (IS_EMPTY_WCS(svcstop->args[0]))
                return xsyserror(GetLastError(), svcstopparam, NULL);
            xfree(wp);
        }
        if (stoplogname) {
            if (xwcschr(stoplogname, L'$')) {
                wp = xexpandenvstr(stoplogname, namevarset);
                if (wp == NULL)
                    return xsyserror(GetLastError(), stoplogname, NULL);
                stoplogname = wp;
            }
            if (xwcspbrk(stoplogname, INVALID_FILENAME_CHARS))
                return xsyserror(ERROR_INVALID_PARAMETER, SVCBATCH_MSG(23), stoplogname);
        }
        else {
            if (stopmaxlogs > 0)
                stoplogname = SVCBATCH_LOGSTOP;
        }
        if (stoplogname) {
            if (xwcsequals(stoplogname, outputlog->logName))
                return xsyserrno(31, L"LogName and StopLogName", stoplogname);
            if (xwcschr(stoplogname, L'@')) {
                if (xchkftime(stoplogname))
                    return xsyserror(GetLastError(), stoplogname, NULL);
                stopmaxlogs = 0;
            }
        }
        for (x = 1; x < svcstop->argc; x++) {
            if (xwcschr(svcstop->args[x], L'$')) {
                wp = xexpandenvstr(svcstop->args[x], NULL);
                if (wp == NULL)
                    return xsyserror(GetLastError(), L"ExpandEnvironment", svcstop->args[x]);
                svcstop->args[x] = wp;
            }
        }
        svcstop->timeout = cmdproc->timeout;
    }
#if HAVE_DEBUG_TRACE
    if (xtraceservice) {
        DBG_PRINTF("cmd %S", cmdproc->application);
        for (x = 1; x < cmdproc->optc; x++)
        DBG_PRINTF("%d %S", x, cmdproc->opts[x]);
        for (x = 0; x < cmdproc->argc; x++)
        DBG_PRINTF("%d %S", x, cmdproc->args[x]);
        if (outputlog) {
        DBG_PRINTF("log %S", outputlog->logName);
        DBG_PRINTF("max %d", outputlog->maxLogs);
        }
        if (svcstop) {
        for (x = 0; x < svcstop->argc; x++)
        DBG_PRINTF("stop %d %S", x, svcstop->args[x]);
        if (stoplogname) {
        DBG_PRINTF("stoplog %S", stoplogname);
        DBG_PRINTF("stopmax %d", stopmaxlogs);
        }
        }

        DBG_PRINTF("options %08X",  svcoptions);
        DBG_PRINTF("timeout %d",    cmdproc->timeout);
        pp = xgetenv(L"PATH");
        DBG_PRINTF("PATH %S",       pp);
        xfree(pp);
        DBG_PRINTF("name %S",       service->name);
        DBG_PRINTF("display %S",    service->display);
#if HAVE_EXTRA_SVC_PARAMS
        DBG_PRINTF("desc %S",       sdescription);
        DBG_PRINTF("imagepath %S",  svcimagepath);
        DBG_PRINTF("username %S",   svcusername);
#endif
        DBG_PRINTF("failmode %d",   service->failMode);
        DBG_PRINTF("killdepth %d",  service->killDepth);
        DBG_PRINTF("timeout %d ms", service->timeout);
        DBG_PRINTS("done");
    }
#endif
    return 0;
}

static void WINAPI servicemain(DWORD argc, LPWSTR *argv)
{
    DWORD   i;
    DWORD   ec;
    DWORD   rv = 0;
    LPWSTR  es;
    LPWSTR  ep;
    LPWSTR  en;
    LPCWSTR em = NULL;

    DBG_PRINTS("started");
    service->status.dwServiceType  = SERVICE_WIN32_OWN_PROCESS;
    service->status.dwCurrentState = SERVICE_START_PENDING;

    if (argc > 0)
        service->name = argv[0];
    if (IS_EMPTY_WCS(service->name)) {
        xsyserror(ERROR_INVALID_PARAMETER, L"InvalidServiceName", NULL);
        exit(1);
    }
    service->handle = RegisterServiceCtrlHandlerExW(service->name, servicehandler, NULL);
    if (IS_INVALID_HANDLE(service->handle)) {
        xsyserror(GetLastError(), L"RegisterServiceCtrlHandlerEx", service->name);
        exit(1);
    }
    DBG_PRINTF("%S", service->name);
    xsvcstatus(SERVICE_START_PENDING, SVCBATCH_START_HINT);

    xinitconf();
    rv = getsvcpparams(0, &em);
    if (rv != ERROR_SUCCESS) {
        xsyserror(rv, L"Service Parameters", em);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    em = getconfwcs(0, SVCBATCH_SVC_DISPLAY);
    if (IS_VALID_WCS(em))
        service->display = em;
    service->uuid = xuuidstring(NULL, 1, 0);
    if (IS_EMPTY_WCS(service->uuid)) {
        rv = xsyserror(GetLastError(), L"SVCBATCH_SERVICE_UUID", NULL);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    xinitvars();
    rv = parseoptions(argc, argv);
    if (rv) {
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    es = GetEnvironmentStringsW();
    if (es == NULL) {
        rv = xsyserror(GetLastError(), L"GetEnvironmentStrings", NULL);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    ec = xmszlen(es);
    en = xwmalloc(ec + 2);
    wmemcpy(en, es, ec);
    FreeEnvironmentStringsW(es);
    for (ep = en; *ep; ep++) {
        i = 0;
        while (ucenvvars[i]) {
            em = xwcsbegins(ep, ucenvvars[i]);
            if ((em != NULL) && (*em == L'=')) {
                wmemcpy(ep, ucenvvars[i], xwcslen(ucenvvars[i]));
                break;
            }
            i++;
        }
        while (*ep) {
            ep++;
        }
    }
    service->environment = en;

    rv = createevents();
    if (rv) {
        xsyserror(rv, L"CreateEvent", NULL);
        xsvcstatus(SERVICE_STOPPED, rv);
        return;
    }
    xsvcstatus(SERVICE_START_PENDING, 0);
    if (outputlog) {
        rv = openlogfile(outputlog, TRUE);
        if (rv) {
            xsvcstatus(SERVICE_STOPPED, rv);
            return;
        }
        xsvcstatus(SERVICE_START_PENDING, 0);
    }
    cmdproc->commandLine = xappendarg(1, NULL, cmdproc->application);
    for (i = 1; i < cmdproc->optc; i++)
    cmdproc->commandLine = xappendarg(0, cmdproc->commandLine, cmdproc->opts[i]);
    for (i = 0; i < cmdproc->argc; i++)
    cmdproc->commandLine = xappendarg(1, cmdproc->commandLine, cmdproc->args[i]);

    if (IS_OPT_SET(SVCBATCH_OPT_WRSTDIN)) {
        if (!xcreatethread(SVCBATCH_STDIN_THREAD,
                           1, stdinthread, NULL)) {
            rv = GetLastError();
            xsyserror(rv, L"StdinThread", NULL);
            goto finished;
        }
    }
    if (IS_OPT_SET(SVCBATCH_OPT_ROTATE)) {
        HANDLE wt = NULL;
        if (IS_OPT_SET(SVCBATCH_OPT_ROTATE_BY_TIME)) {
            wt = CreateWaitableTimer(NULL, TRUE, NULL);
            if (IS_INVALID_HANDLE(wt)) {
                rv = GetLastError();
                xsyserror(rv, L"CreateWaitableTimer", NULL);
                goto finished;
            }
            if (!SetWaitableTimer(wt, &rotatetime, 0, NULL, NULL, FALSE)) {
                rv = GetLastError();
                xsyserror(rv, L"SetWaitableTimer", NULL);
                goto finished;
            }
        }
        if (!xcreatethread(SVCBATCH_ROTATE_THREAD,
                           1, rotatethread, wt)) {
            rv = GetLastError();
            xsyserror(rv, L"RotateThread", NULL);
            goto finished;
        }
    }
    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rv = xsyserror(GetLastError(), L"WorkerThread", NULL);
        goto finished;
    }
    WaitForSingleObject(threads[SVCBATCH_WORKER_THREAD].thread, INFINITE);
    SVCBATCH_CS_ENTER(service);
    if (InterlockedExchange(&service->state, SERVICE_STOP_PENDING) != SERVICE_STOP_PENDING) {
        /**
         * Service ended without stop signal
         */
        DBG_PRINTS("ended without SERVICE_CONTROL_STOP");
        rv = cmdproc->exitCode;
    }
    SVCBATCH_CS_LEAVE(service);
    DBG_PRINTS("waiting for stop to finish");
    WaitForSingleObject(svcstopdone, cmdproc->timeout);
    waitforthreads(SVCBATCH_STOP_STEP);

    DBG_PRINTS("closing");
finished:
    closelogfile(outputlog);
    threadscleanup();
    xsvcstatus(SERVICE_STOPPED, rv);
    DBG_PRINTS("done");
}

static DWORD svcstopmain(void)
{
    DWORD i;
    DWORD rc;

    DBG_PRINTS("started");
    workerended = CreateEventEx(NULL, NULL,
                                CREATE_EVENT_MANUAL_RESET,
                                EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (IS_INVALID_HANDLE(workerended))
        return GetLastError();
    DBG_PRINTF("%S 0x%08X", service->name, svcoptions);
    if (outputlog) {
        rc = openlogfile(outputlog, FALSE);
        if (rc)
            return rc;
    }
    cmdproc->commandLine = xappendarg(1, NULL, cmdproc->application);
    for (i = 1; i < cmdproc->optc; i++)
    cmdproc->commandLine = xappendarg(0, cmdproc->commandLine, cmdproc->opts[i]);
    for (i = 0; i < cmdproc->argc; i++)
    cmdproc->commandLine = xappendarg(1, cmdproc->commandLine, cmdproc->args[i]);

    if (IS_OPT_SET(SVCBATCH_OPT_WRSTDIN)) {
        if (!xcreatethread(SVCBATCH_STDIN_THREAD,
                           1, stdinthread, NULL)) {
            rc = xsyserror(GetLastError(), L"StdinThread", NULL);
            setsvcstatusexit(rc);
            goto finished;        }
    }
    if (!xcreatethread(SVCBATCH_WORKER_THREAD,
                       0, workerthread, NULL)) {
        rc = xsyserror(GetLastError(), L"WorkerThread", NULL);
        setsvcstatusexit(rc);
        goto finished;
    }
    DBG_PRINTS("waiting for worker thread to finish");
    rc = WaitForSingleObject(threads[SVCBATCH_WORKER_THREAD].thread, cmdproc->timeout);
    if (rc != WAIT_OBJECT_0) {
        DBG_PRINTS("stop timeout");
        stopshutdown(SVCBATCH_STOP_SYNC);
        setsvcstatusexit(rc);
    }
    waitforthreads(SVCBATCH_STOP_WAIT);

    DBG_PRINTS("closing");
finished:
    closelogfile(outputlog);
    threadscleanup();
    DBG_PRINTS("done");
    return service->exitCode;
}

static int setsvcparams(LPSVCBATCH_SCM_PARAMS opts)
{
    int         i;
    int         e;
    DWORD       n;
    HKEY        k = NULL;
    LPBYTE      b = NULL;
    LSTATUS     s;
    WCHAR       name[BBUFSIZ];

    i = xwcslcat(name, BBUFSIZ, 0, SYSTEM_SVC_SUBKEY);
    i = xwcslcat(name, BBUFSIZ, i, service->name);
    i = xwcslcat(name, BBUFSIZ, i, L"\\" SVCBATCH_PARAMS_KEY);
    if (i >= BBUFSIZ) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return __LINE__;
    }
    e = __LINE__;
    s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, name, 0, NULL,
                        0, KEY_CREATE_SUB_KEY | KEY_READ | KEY_WRITE,
                        NULL, &k, NULL);
    if (s != ERROR_SUCCESS)
        goto finished;

    for (i = 0; i < opts->pos; i++) {
        if (opts->p[i].type == SVCBATCH_REG_TYPE_SZ) {
            e = __LINE__;
            if (IS_EMPTY_WCS(opts->p[i].val)) {
                s = ERROR_INVALID_DATA;
                goto finished;
            }
            n = xwcslen(opts->p[i].val) + 1;
            e = __LINE__;
            s = RegSetValueExW(k, opts->p[i].key, 0,
                               svcbregrtypes[opts->p[i].type],
                               (const BYTE *)opts->p[i].val, n * 2);
        }
        else if (opts->p[i].type == SVCBATCH_REG_TYPE_MSZ) {
            if (opts->p[i].argc) {
                e = __LINE__;
                b = (LPBYTE)xargvtomsz(opts->p[i].argc, opts->p[i].args, &n);
            }
            else {
                e = __LINE__;
                if (IS_EMPTY_WCS(opts->p[i].val)) {
                    s = ERROR_INVALID_DATA;
                    goto finished;
                }
                n = xwcslen(opts->p[i].val) * 2;
                b = xmcalloc(n + 4);
                memcpy(b, opts->p[i].val, n);
                n += 4;
            }
            if (b == NULL) {
                s = GetLastError();
                goto finished;
            }
            e = __LINE__;
            s = RegSetValueExW(k, opts->p[i].key, 0,
                               svcbregrtypes[opts->p[i].type], b, n);
            xfree(b);
        }
        else if (opts->p[i].type == SVCBATCH_REG_TYPE_BIN) {
            e = __LINE__;
            s = xcsbtob(opts->p[i].val, &b, &n);
            if (s != ERROR_SUCCESS)
                goto finished;
            e = __LINE__;
            s = RegSetValueExW(k, opts->p[i].key, 0,
                               svcbregrtypes[opts->p[i].type],
                               b, n);
            xfree(b);
        }
        else if (opts->p[i].type == SVCBATCH_REG_TYPE_NUM) {
            e = __LINE__;
            s = xwcstod(opts->p[i].val, &n);
            if (s != ERROR_SUCCESS)
                goto finished;
            e = __LINE__;
            s = RegSetValueExW(k, opts->p[i].key, 0,
                               svcbregrtypes[opts->p[i].type],
                               (const BYTE *)&n, 4);
        }
        else if (opts->p[i].type == SVCBATCH_REG_TYPE_BOOL) {
            e = __LINE__;
            s = xwcstob(opts->p[i].val, &n);
            if (s != ERROR_SUCCESS)
                goto finished;
            e = __LINE__;
            s = RegSetValueExW(k, opts->p[i].key, 0,
                               svcbregrtypes[opts->p[i].type],
                               (const BYTE *)&n, 4);
        }
        else {
            e = __LINE__;
            s = ERROR_NOT_SUPPORTED;
            goto finished;
        }
        if (s != ERROR_SUCCESS)
            goto finished;
    }

finished:
    if (k)
        RegCloseKey(k);
    if (s != ERROR_SUCCESS) {
        SetLastError(s);
        return e + 1;
    }
    return 0;
}

static int setsvcentlog(LPCWSTR name, LPCWSTR msgs, int r)
{
    WCHAR   b[BBUFSIZ];
    DWORD   c;
    HKEY    k = NULL;
    int     i;
    int     e;

    LSTATUS s;

    ASSERT_WSTR(name, __LINE__);
    ASSERT_WSTR(msgs, __LINE__);

    i = xwcslcat(b, BBUFSIZ, 0, SYSTEM_SVC_SUBKEY L"EventLog\\Application\\");
    i = xwcslcat(b, BBUFSIZ, i, name);
    e = __LINE__;
    s = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        b, 0, NULL, 0,
                        KEY_QUERY_VALUE | KEY_READ | KEY_WRITE,
                        NULL, &k, &c);
    if (s != ERROR_SUCCESS)
        goto finished;

    if ((r == 0) && (c == REG_OPENED_EXISTING_KEY)) {
        e = __LINE__;
        s = ERROR_ALREADY_REGISTERED;
        goto finished;
    }
    e = __LINE__;
    s = RegSetValueExW(k, L"EventMessageFile", 0, REG_EXPAND_SZ,
                          (const BYTE *)msgs, (xwcslen(msgs) + 1) * 2);
    if (s != ERROR_SUCCESS)
        goto finished;

    c = EVENTLOG_ERROR_TYPE | EVENTLOG_WARNING_TYPE |
        EVENTLOG_INFORMATION_TYPE;
    e = __LINE__;
    s = RegSetValueExW(k, L"TypesSupported", 0, REG_DWORD,
                       (const BYTE *)&c, 4);

finished:
    if (k)
        RegCloseKey(k);
    if (s != ERROR_SUCCESS) {
        SetLastError(s);
        return e + 1;
    }
    return 0;
}

static int xscmcommand(LPCWSTR ncmd)
{
    int i;
    for (i = 0; i < SVCBATCH_SCM_MAX; i++) {
        if (xwcsequals(ncmd, scmcommands[i]))
            return i;
    }
    return -1;
}

static int xscmhelp(LPCWSTR cmd)
{
    int r = 0;

    if (IS_EMPTY_STR(cmd)) {
        fputs(xgenerichelp, stdout);
    }
    else {
        int i = xscmcommand(cmd);
        if (i < 0) {
            fprintf(stdout,
                    "\nError:\n  Unknown command: %S\n",
                    cmd);
            r = ERROR_INVALID_NAME;
        }
        else {
            fputs(xcommandhelp[i], stdout);
        }
    }
    return r;
}

static int xscmexecute(int cmd, int argc, LPCWSTR *argv)
{
    PSERVICE_CONTROL_STATUS_REASON_PARAMSW ssr;
    LPSERVICE_STATUS_PROCESS ssp;
    WCHAR     cb[SVCBATCH_PATH_MAX];
    DWORD     bneed;
    LSTATUS   s;
    int       i;
    int       opt;
    int       rv = 0;
    int       rw = 0;
    int       ec = 0;
    int       ep = 0;
    int       en = 0;
    int       cmdverbose    = 1;

    int       eventsource   = 1;
    int       cleanupall    = 1;
    int       ooverwrite    = 1;

    int       currpc        = 0;
    int       wtime         = 0;
    int       startdelayed  = 0;
    ULONGLONG wtmstart      = 0;
    ULONGLONG wtimeout      = 0;
    LPCWSTR   ed            = NULL;
    LPCWSTR   ex            = NULL;
    LPWSTR    pp            = NULL;
    LPWSTR    sdepends      = NULL;
    LPWSTR    reqprivs      = NULL;
    LPWSTR    binarypath    = NULL;
    LPCWSTR   description   = NULL;
    LPCWSTR   displayname   = NULL;
    LPCWSTR   username      = NULL;
    LPCWSTR   password      = NULL;
    DWORD     psdwntime     = 0;
    DWORD     starttype     = SERVICE_NO_CHANGE;
    DWORD     servicetype   = SERVICE_NO_CHANGE;
    DWORD     srmajor       = SERVICE_STOP_REASON_MAJOR_NONE;
    DWORD     srminor       = SERVICE_STOP_REASON_MINOR_NONE;
    DWORD     srflag        = SERVICE_STOP_REASON_FLAG_PLANNED;
    SC_HANDLE mgr           = NULL;
    SC_HANDLE svc           = NULL;

    LPSVCBATCH_CONF_PARAM   param = NULL;

    if (cmd == SVCBATCH_SCM_VERSION) {
        fputs(cnamestamp, stdout);
        return 0;
    }
    if (cmd == SVCBATCH_SCM_HELP)
        return xscmhelp(argv[0]);
    if (IS_EMPTY_WCS(argv[0])) {
        fprintf(stdout, "\nError:\n  Missing service name");
        xscmhelp(NULL);
        return ERROR_INVALID_PARAMETER;
    }

    ssr = (PSERVICE_CONTROL_STATUS_REASON_PARAMSW)xmcalloc(sizeof(SERVICE_CONTROL_STATUS_REASON_PARAMSW));
    ssp = &ssr->ServiceStatus;
    service->name = argv[0];

    wtmstart      = GetTickCount64();
    DBG_PRINTF("%S %S", scmcommands[cmd], service->name);
    if (!xisalnum(*service->name)) {
        rv = ERROR_INVALID_SERVICENAME;
        ec = __LINE__;
        ex = SVCBATCH_MSG(27);
        goto finished;
    }
    if (xwcspbrk(service->name, INVALID_FILENAME_CHARS)) {
        rv = ERROR_INVALID_SERVICENAME;
        ec = __LINE__;
        ex = SVCBATCH_MSG(23);
        goto finished;
    }
    if (xwcslen(service->name) > SVCBATCH_NAME_MAX) {
        rv = ERROR_INVALID_SERVICENAME;
        ec = __LINE__;
        ex = SVCBATCH_MSG(24);
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CREATE) {
        starttype     = SERVICE_DEMAND_START;
        servicetype   = SERVICE_WIN32_OWN_PROCESS;
    }
    if ((cmd == SVCBATCH_SCM_CREATE)  || (cmd == SVCBATCH_SCM_CONFIG)) {
        xinitconf();
        scmpparams = (LPSVCBATCH_SCM_PARAMS)xmcalloc(sizeof(SVCBATCH_SCM_PARAMS));
        scmpparams->siz = SBUFSIZ;
    }
    if ((cmd == SVCBATCH_SCM_START)  || (cmd == SVCBATCH_SCM_STOP))
        wtime = SVCBATCH_SCM_WAIT_DEF;

    while ((opt = xlongopt(argc, argv, scmcoptions, scmallowed[cmd])) != 0) {
        switch (opt) {
            case '[':
                xwoptend = 1;
            break;
            case ']':
                xwoptend = 0;
                xwoptarr = 0;
            break;
            case 'A':
                if (param->argc < SBUFSIZ) {
                    param->args[param->argc++] = xwoptarg;
                }
                else {
                    rv = ERROR_BUFFER_OVERFLOW;
                    ec = __LINE__;
                    ed = L"Argument array";
                    goto finished;
                }
            break;
            case 'b':
                xfree(binarypath);
                pp = xgetfinalpath(0, skipdotslash(xwoptarg));
                if (pp == NULL) {
                    rv = GetLastError();
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
                binarypath = xappendarg(1, NULL, pp);
                xfree(pp);
            break;
            case 'D':
                xfree(sdepends);
                sdepends = xstrtomsz(xwoptarg, L'/', NULL);
            break;
            case 'P':
                xfree(reqprivs);
                reqprivs = xstrtomsz(xwoptarg, L'/', NULL);
            break;
            case 'd':
                description = xwoptarg;
            break;
            case 's':
                if (scmpparams->pos == scmpparams->siz) {
                    rv = ERROR_BUFFER_OVERFLOW;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
                param      = &scmpparams->p[scmpparams->pos];
                param->key = xwoptarg;
                xwoptarg   = argv[xwoptind++];
                if (xwoptind > argc) {
                    /* Missing argument */
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ex = SVCBATCH_MSG(22);
                    ed = xwoption;
                    goto finished;
                }
                while (xisblank(*xwoptarg))
                    xwoptarg++;
                if (*xwoptarg == WNUL) {
                    rv = ERROR_BAD_LENGTH;
                    ec = __LINE__;
                    ed = param->key;
                    goto finished;
                }
                if (xiswcschar(xwoptarg, L'[')) {
                    xwoptarr   = 'A';
                    xwoptend   =  1;
                }
                else {
                    xwoptarr   =  0;
                    xwoptend   =  0;
                    param->val =  xwoptarg;
                }
                scmpparams->pos++;
            break;
            case 'n':
                displayname = xwoptarg;
            break;
            case 'p':
                password    = xwoptarg;
            break;
            case 'q':
                cmdverbose  = 0;
            break;
            case 't':
                starttype   = xnamemap(xwoptarg, starttypemap, &startdelayed, SERVICE_NO_CHANGE);
                if (starttype == SERVICE_NO_CHANGE) {
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ed = xwoptarg;
                    goto finished;
                }
            break;
            case 'r':
                ec = __LINE__;
                rv  = xwcstod(xwoptarg, &psdwntime);
                if (rv) {
                    ed = xwoptarg;
                    goto finished;
                }
                if ((psdwntime < SVCBATCH_STOP_TDEF) ||
                    (psdwntime > SVCBATCH_STOP_TMAX)) {
                    rv = ERROR_INVALID_PARAMETER;
                    ec = __LINE__;
                    ex = SVCBATCH_MSG(26);
                    ed = L"[3000 - 180000]";
                    en = psdwntime;
                    goto finished;

                }
            break;
            case 'u':
                if ((xwoptarg[0] > 47) && (xwoptarg[0] < 51) && (xwoptarg[1] == WNUL))
                    username = scmsvcaccounts[xwoptarg[0] - 48];
                else
                    username = xwoptarg;
            break;
            case 'w':
                if (xwoptarg) {
                    wtime = xwcstoi(xwoptarg, NULL);
                    if ((wtime < 0) || (wtime > SVCBATCH_WAIT_TMAX)) {
                        rv = ERROR_INVALID_PARAMETER;
                        ec = __LINE__;
                        ex = SVCBATCH_MSG(26);
                        ed = L"[0 - 180]";
                        en = wtime;
                        goto finished;
                    }
                }
                else {
                    /* Use maximum  wait time */
                    wtime = SVCBATCH_WAIT_TMAX;
                }
            break;
            case ERROR_BAD_LENGTH:
                rv = ERROR_BAD_LENGTH;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
            case ERROR_INVALID_DATA:
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ex = SVCBATCH_MSG(22);
                ed = xwoption;
                goto finished;
            break;
            default:
                rv = ERROR_NOT_SUPPORTED;
                ec = __LINE__;
                ed = xwoption;
                goto finished;
            break;
        }
    }
    if (xwoptarr && xwoptend) {
        rv = ERROR_INVALID_PARAMETER;
        ec = __LINE__;
        goto finished;
    }
    if (displayname) {
        if (xwcspbrk(displayname, INVALID_PATHNAME_CHARS)) {
            rv = ERROR_INVALID_SERVICENAME;
            ec = __LINE__;
            ed = displayname;
            ex = SVCBATCH_MSG(23);
            goto finished;
        }
        if (xwcslen(displayname) > SVCBATCH_NAME_MAX) {
            rv = ERROR_INVALID_SERVICENAME;
            ec = __LINE__;
            ed = displayname;
            ex = SVCBATCH_MSG(24);
            goto finished;
        }
    }

    if ((cmd == SVCBATCH_SCM_CREATE) || (cmd == SVCBATCH_SCM_CONFIG)) {
        int  x;
        int  m;
        char u[SVCBATCH_CFG_MAX];

        memset(u, 0, SVCBATCH_CFG_MAX);
        for (i = 0; i < scmpparams->pos; i++) {
            x = 0;
            m = 0;
            while (svcpparams[x].name != NULL) {
                if (xwcsequals(scmpparams->p[i].key, svccparams[x].name)) {
                    if (u[x]++) {
                        rv = ERROR_ALREADY_ASSIGNED;
                        ec = __LINE__;
                        ed = scmpparams->p[i].key;
                        goto finished;
                    }
                    scmpparams->p[i].key  = svccparams[x].name;
                    scmpparams->p[i].type = svccparams[x].type;
                    m = 1;
                    break;
                }
                x++;
            }
            if (m == 0) {
                rv = ERROR_INVALID_NAME;
                ec = __LINE__;
                ed = scmpparams->p[i].key;
                goto finished;
            }
        }
    }

    argc -= xwoptind;
    argv += xwoptind;
    if (wtime)
        wtimeout = wtime * ONE_SECOND;
    mgr = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (mgr == NULL) {
        rv = GetLastError();
        ec = __LINE__;
        goto finished;
    }
    if (cmdverbose && wtime) {
        fprintf(stdout, "Service Name : %S\n", service->name);
        fprintf(stdout, "     Command : %S\n", scmcommands[cmd]);
        fprintf(stdout, "             :  ");
        cmdverbose++;
    }
    if (cmd == SVCBATCH_SCM_CREATE) {
        if (binarypath == NULL)
            binarypath = xappendarg(1, NULL, program->application);
        for (i = 0; i < argc; i++)
            binarypath = xappendarg(1, binarypath, argv[i]);
        svc = CreateServiceW(mgr,
                             service->name,
                             displayname,
                             SERVICE_ALL_ACCESS,
                             servicetype,
                             starttype,
                             SERVICE_ERROR_NORMAL,
                             binarypath,
                             NULL,
                             NULL,
                             sdepends,
                             username,
                             password);
        if (svc != NULL) {
            if (scmpparams->pos) {
                ep = setsvcparams(scmpparams);
                if (ep) {
                    rv = GetLastError();
                    DeleteService(svc);
                    ec = __LINE__;
                    ed = SVCBATCH_PARAMS_KEY;
                    goto finished;
                }
            }
            if (eventsource) {
                ep = setsvcentlog(service->name, program->application, ooverwrite);
                if (ep) {
                    rw = GetLastError();
                    if (rw == ERROR_ALREADY_REGISTERED) {
                        ec = __LINE__;
                        ed = L"Reusing existing EventLog";
                    }
                    else {
                        DeleteService(svc);
                        rv = rw;
                        ec = __LINE__;
                    }
                    goto finished;
                }
            }
        }
    }
    else {
        svc = OpenServiceW(mgr,
                           service->name,
                           SERVICE_ALL_ACCESS);
    }
    if (svc == NULL) {
        rv = GetLastError();
        ec = __LINE__;
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CONFIG) {
        LPQUERY_SERVICE_CONFIGW sc = NULL;

        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
            ed = SVCBATCH_MSG(35);
            goto finished;
        }
        rv = getsvcconfig(svc, &sc);
        if (rv) {
            ec = __LINE__;
            goto finished;
        }
        if (argc) {
            if (binarypath == NULL)
                binarypath = xappendarg(0, NULL, sc->lpBinaryPathName);
            for (i = 0; i < argc; i++)
                binarypath = xappendarg(1, binarypath, argv[i]);
        }
        xfree(sc);
        if (!ChangeServiceConfigW(svc,
                                  servicetype,
                                  starttype,
                                  SERVICE_NO_CHANGE,
                                  binarypath,
                                  NULL,
                                  NULL,
                                  sdepends,
                                  username,
                                  password,
                                  displayname)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (scmpparams->pos) {
            ep = setsvcparams(scmpparams);
            if (ep) {
                rv = GetLastError();
                ec = __LINE__;
                ed = SVCBATCH_PARAMS_KEY;
                goto finished;
            }
        }
    }

    if ((cmd == SVCBATCH_SCM_CREATE) || (cmd == SVCBATCH_SCM_CONFIG)) {
        if (description) {
            SERVICE_DESCRIPTIONW sc;
            sc.lpDescription = (LPWSTR)description;
            if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &sc)) {
                rv = GetLastError();
                if (cmd == SVCBATCH_SCM_CREATE)
                    DeleteService(svc);
                ec = __LINE__;
                goto finished;
            }
        }
        if (reqprivs) {
            SERVICE_REQUIRED_PRIVILEGES_INFOW sc;
            sc.pmszRequiredPrivileges = reqprivs;
            if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &sc)) {
                rv = GetLastError();
                if (cmd == SVCBATCH_SCM_CREATE)
                    DeleteService(svc);
                ec = __LINE__;
                goto finished;
            }
        }
        if (startdelayed) {
            SERVICE_DELAYED_AUTO_START_INFO sc;
            sc.fDelayedAutostart = TRUE;
            if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &sc)) {
                rv = GetLastError();
                if (cmd == SVCBATCH_SCM_CREATE)
                    DeleteService(svc);
                ec = __LINE__;
                goto finished;
            }
        }
        if (psdwntime) {
            SERVICE_PRESHUTDOWN_INFO sc;
            sc.dwPreshutdownTimeout = psdwntime;
            if (!ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &sc)) {
                rv = GetLastError();
                if (cmd == SVCBATCH_SCM_CREATE)
                    DeleteService(svc);
                ec = __LINE__;
                goto finished;
            }
        }
    }
    if (cmd == SVCBATCH_SCM_DELETE) {
        LPQUERY_SERVICE_CONFIGW sc = NULL;

        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
            ed = SVCBATCH_MSG(34);
            goto finished;
        }
        if (!DeleteService(svc)) {
            rv = GetLastError();
            ec = __LINE__;
        }
        if (eventsource && cleanupall) {
            i = xwcslcat(cb, BBUFSIZ, 0, SYSTEM_SVC_SUBKEY L"EventLog\\Application\\");
            xwcslcat(cb, BBUFSIZ, i, service->name);
            s = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, cb, KEY_WOW64_64KEY, 0);
            if (s != ERROR_SUCCESS) {
                rw = s;
                ec = __LINE__;
                ex = L"Cannot Delete EventLog Application Registry key";
                ed = cb;
            }
        }
        xfree(sc);
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_START) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_STOPPED) {
            rv = ERROR_SERVICE_ALREADY_RUNNING;
            ec = __LINE__;
            goto finished;
        }
        if (!StartServiceW(svc, argc, argv)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (wtime) {
            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            while (ssp->dwCurrentState == SERVICE_START_PENDING) {
                do {
                    Sleep(100);
                    if (cmdverbose) {
                        fprintf(stdout, "\b%c", xpropeller[currpc % PROPELLER_SIZE]);
                        fflush(stdout);
                    }
                } while (++currpc % 10);

                if (!QueryServiceStatusEx(svc,
                                          SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                          SZ_STATUS_PROCESS_INFO, &bneed)) {
                    rv = GetLastError();
                    ec = __LINE__;
                    goto finished;
                }
                if (GetTickCount64() - wtmstart > wtimeout) {
                    /** Timeout */
                    rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                    ec = __LINE__;
                    goto finished;
                }
            }
            if (ssp->dwCurrentState != SERVICE_RUNNING) {
                rv = ERROR_SERVICE_START_HANG;
                ec = __LINE__;
            }
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_STOP) {
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState == SERVICE_STOPPED) {
            rv = ERROR_SERVICE_NOT_ACTIVE;
            ec = __LINE__;
            goto finished;
        }
        while (ssp->dwCurrentState == SERVICE_STOP_PENDING) {
            if (wtime == 0)
                goto finished;
            do {
                Sleep(100);
                if (cmdverbose) {
                    fprintf(stdout, "\b%c", xpropeller[currpc % PROPELLER_SIZE]);
                    fflush(stdout);
                }
            } while (++currpc % 10);

            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            if (ssp->dwCurrentState == SERVICE_STOPPED)
                goto finished;
            if (GetTickCount64() - wtmstart > wtimeout) {
                /** Timeout */
                rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                ec = __LINE__;
                goto finished;
            }
        }
        if (argc == 0) {
            if (!ControlService(svc, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)ssp)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
        }
        else {
            DWORD   sv;
            LPWSTR  sp;
            LPCWSTR rp = argv[0];

            sv = xwcstoi(rp, &sp);
            if ((sv < 1) || (sv > 4)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ex = SVCBATCH_MSG(26);
                ed = L"[1 - 4]";
                en = sv;
                goto finished;
            }
            srflag = (sv << 28);
            sv = 0;
            if (*sp) {
                rp = sp + 1;
                sv = xwcstoi(rp, &sp);
            }
            if ((sv < 1) || (sv > 255)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ex = SVCBATCH_MSG(26);
                ed = L"[1 - 255]";
                en = sv;
                goto finished;
            }
            srmajor = (sv << 16);
            sv = 0;
            if (*sp) {
                rp = sp + 1;
                sv = xwcstoi(rp, &sp);
            }
            if ((sv < 1) || (sv > 65535)) {
                rv = ERROR_INVALID_PARAMETER;
                ec = __LINE__;
                ex = SVCBATCH_MSG(26);
                ed = L"[1 - 65535]";
                en = sv;
                goto finished;
            }
            srminor = sv;
            if (argc > 1) {
                /* Comment is limited to 128 chars */
                xwcslcpy(cb, 128, argv[1]);
                ssr->pszComment = cb;
            }
            ssr->dwReason = srflag | srmajor | srminor;
            if (!ControlServiceExW(svc, SERVICE_CONTROL_STOP,
                                   SERVICE_CONTROL_STATUS_REASON_INFO, (LPBYTE)ssr)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
        }
        while (ssp->dwCurrentState != SERVICE_STOPPED) {
            if (wtime == 0)
                break;
            do {
                Sleep(100);
                if (cmdverbose) {
                    fprintf(stdout, "\b%c", xpropeller[currpc % PROPELLER_SIZE]);
                    fflush(stdout);
                }
            } while (++currpc % 10);

            if (!QueryServiceStatusEx(svc,
                                      SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                      SZ_STATUS_PROCESS_INFO, &bneed)) {
                rv = GetLastError();
                ec = __LINE__;
                goto finished;
            }
            if (ssp->dwCurrentState == SERVICE_STOPPED)
                break;
            if (GetTickCount64() - wtmstart > wtimeout) {
                /** Timeout */
                rv = ERROR_SERVICE_REQUEST_TIMEOUT;
                ec = __LINE__;
                goto finished;
            }
        }
        goto finished;
    }
    if (cmd == SVCBATCH_SCM_CONTROL) {
        DWORD sv;
        if (argc == 0) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            ed = SVCBATCH_MSG(33);
            goto finished;
        }
        if (!QueryServiceStatusEx(svc,
                                  SC_STATUS_PROCESS_INFO, (LPBYTE)ssp,
                                  SZ_STATUS_PROCESS_INFO, &bneed)) {
            rv = GetLastError();
            ec = __LINE__;
            goto finished;
        }
        if (ssp->dwCurrentState != SERVICE_RUNNING) {
            rv = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
            ec = __LINE__;
            ed = SVCBATCH_MSG(2);
            goto finished;
        }
        sv = xwcstoi(argv[0], NULL);
        if ((sv < 128) || (sv > 255)) {
            rv = ERROR_INVALID_PARAMETER;
            ec = __LINE__;
            ex = SVCBATCH_MSG(26);
            ed = L"[128 - 255]";
            en = sv;
            goto finished;
        }
        if (!ControlServiceExW(svc, sv,
                               SERVICE_CONTROL_STATUS_REASON_INFO, (LPBYTE)ssr)) {
            rv = GetLastError();
            ec = __LINE__;
            ed = argv[0];
        }
        goto finished;
    }

finished:
    if (svc != NULL)
        CloseServiceHandle(svc);
    if (mgr != NULL)
        CloseServiceHandle(mgr);
    if (cmdverbose) {
        wchar_t eb[SVCBATCH_LINE_MAX];
        if (cmdverbose == 1) {
            fprintf(stdout, "Service Name : %S\n", service->name);
            fprintf(stdout, "     Command : %S\n", scmcommands[cmd]);
        }
        else {
            fputc('\r', stdout);
        }
        if (rv) {
            if (ex == NULL) {
            xwinapierror(eb, SVCBATCH_LINE_MAX, rv);
            ex = eb;
            }
            fprintf(stdout, "             : FAILED\n");
            if ((wtime > 0) && (rv == ERROR_SERVICE_REQUEST_TIMEOUT))
            fprintf(stdout, "               %llu ms\n", GetTickCount64() - wtmstart);
            if (ec) {
            if (ep)
            fprintf(stdout, "        LINE : %d (%d)\n", ec, ep);
            else
            fprintf(stdout, "        LINE : %d\n", ec);
            }
            fprintf(stdout, "       ERROR : %d (0x%x)\n", rv,  rv);
            fprintf(stdout, "               %S\n", ex);
            if (ed != NULL) {
            fprintf(stdout, "               %S", ed);
            if (en)
            fprintf(stdout, " (%d)", en);
            fputc('\n', stdout);
            }
        }
        else {
            fprintf(stdout, "             : SUCCESS\n");
            if (wtime)
            fprintf(stdout, "               %llu ms\n", GetTickCount64() - wtmstart);
            if (cmd == SVCBATCH_SCM_CONTROL)
            fprintf(stdout, "               %S\n", argv[0]);
            if (cmd == SVCBATCH_SCM_CREATE) {
            if (startdelayed)
            fprintf(stdout, "     STARTUP : Automatic (Delayed Start)\n");
            else
            fprintf(stdout, "     STARTUP : %S (%lu)\n", xcodemap(starttypemap, starttype), starttype);
            }
            if (cmd == SVCBATCH_SCM_START && wtime)
            fprintf(stdout, "         PID : %lu\n",  ssp->dwProcessId);
            if (cmd == SVCBATCH_SCM_STOP  && wtime)
            fprintf(stdout, "    EXITCODE : %lu (0x%lx)\n", ssp->dwServiceSpecificExitCode, ssp->dwServiceSpecificExitCode);
            if (rw) {
            if (ex == NULL) {
                xwinapierror(eb, SVCBATCH_LINE_MAX, rw);
                ex = eb;
            }
            xwinapierror(eb, SVCBATCH_LINE_MAX, rw);
            fprintf(stdout, "        LINE : %d\n", ec);
            fprintf(stdout, "     WARNING : %d (0x%x)\n", rw,  rw);
            fprintf(stdout, "               %S\n", ex);
            if (ed != NULL)
            fprintf(stdout, "               %S\n", ed);
            }

        }
        fputc('\n', stdout);
    }
    DBG_PRINTS("done");
    return rv;
}

#if HAVE_DEBUG_TRACE
static void __cdecl xtracecleanup(void)
{
    HANDLE h;

    DBG_PRINTS("done");
    h = InterlockedExchangePointer(&xtracefhandle, NULL);
    if (IS_VALID_HANDLE(h)) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    DeleteCriticalSection(&xtracesync);
}

static void xtracefopen(void)
{
    HANDLE  fh;
    DWORD   rc;
    LPWSTR  fn;
    LPCWSTR td;

    if (xtraceservice == 0)
        return;

    td = xgettempdir();
    if (IS_EMPTY_WCS(td)) {
        xsyswarn(ERROR_PATH_NOT_FOUND, 0, L"Temporary directory");
        return;
    }
    if (servicemode)
        fn = xwmakepath(td, program->name, XTRACE_SERVICE_EXT);
    else
        fn = xwmakepath(td, program->name, XTRACE_STOPSVC_EXT);
    if (!DeleteFileW(fn)) {
        rc = GetLastError();
        if (rc != ERROR_FILE_NOT_FOUND) {
            xsyswarn(rc, 0, L"Cannot delete %s trace file", fn);
            xfree(fn);
            return;
        }
    }
    fh = CreateFileW(fn, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                     NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    rc = GetLastError();
    if (fh != INVALID_HANDLE_VALUE) {
        if (rc == ERROR_ALREADY_EXISTS)
            rc = 0;
        if (rc == 0)
            InterlockedExchangePointer(&xtracefhandle, fh);
    }
    if (rc)
        xsyswarn(rc, 0, L"Cannot create %s trace file", fn);
    xfree(fn);
}
#endif

static int xwmaininit(void)
{
    WCHAR  bb[SVCBATCH_PATH_MAX];
    LPWSTR pp;
    DWORD  nn;

    processheap = GetProcessHeap();
    if (IS_INVALID_HANDLE(processheap))
        return SVCBATCH_EEXIT("GetProcessHeap failed", GetLastError());
    GetSystemInfo(&ssysteminfo);
#if HAVE_DEBUG_TRACE
    InitializeCriticalSection(&xtracesync);
    atexit(xtracecleanup);
#endif
    threads = (LPSVCBATCH_THREAD )xmcalloc(SVCBATCH_MAX_THREADS * sizeof(SVCBATCH_THREAD));
    service = (LPSVCBATCH_SERVICE)xmcalloc(sizeof(SVCBATCH_SERVICE));
    program = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));
    cmdproc = (LPSVCBATCH_PROCESS)xmcalloc(sizeof(SVCBATCH_PROCESS));

    cmdproc->timeout           = SVCBATCH_STOP_TIMEOUT;
    service->timeout           = cmdproc->timeout + SVCBATCH_STOP_SYNC;
    program->timeout           = cmdproc->timeout;
    program->state             = SVCBATCH_PROCESS_RUNNING;
    program->pInfo.hProcess    = GetCurrentProcess();
    program->pInfo.dwProcessId = GetCurrentProcessId();
    program->pInfo.dwThreadId  = GetCurrentThreadId();
    program->sInfo.cb          = DSIZEOF(STARTUPINFOW);
    GetStartupInfoW(&program->sInfo);

    InterlockedExchange(&uidcounter, program->pInfo.dwProcessId);
    nn = GetModuleFileNameW(NULL, bb, SVCBATCH_PATH_SIZ);
    if (nn == 0)
        return SVCBATCH_EEXIT("GetModuleFileName failed", GetLastError());
    if (nn >= SVCBATCH_PATH_SIZ)
        return SVCBATCH_EEXIT("Application name is too large", ERROR_BUFFER_OVERFLOW);
    nn = xgetfilepath(bb, bb, SVCBATCH_PATH_SIZ);
    if (nn < 9)
        return SVCBATCH_EEXIT("Application name is too short", ERROR_INVALID_NAME);
    if (!xwcsequals(bb + nn - 4, L".exe"))
        return SVCBATCH_EEXIT("Application name is missing .exe extension", ERROR_BAD_FORMAT);
    program->application = xwcsndup(bb, nn);
    program->directory   = xwcsndup(bb, nn - 4);

    pp = program->directory;
    pp = xwcsrchr(pp, L'\\');
    if (pp == NULL)
        return SVCBATCH_EEXIT("Cannot find program directory", ERROR_BAD_FORMAT);
    *pp = WNUL;

    program->name = pp + 1;
    if (!xisvalidvarname(program->name))
        return SVCBATCH_EEXIT("Invalid program name", ERROR_BAD_FORMAT);
    xwcslower(program->name);

    return 0;
}

/**
 * Main program entry
 *
 * 1. If invoked from SCM
 *    argc and argv are constructed
 *    by SCM, using CommandLineToArgvW Win32 API
 *    where the lpCmdLine is value of the registry key
 *    HKLM\SYSTEM\CurrentControlSet\Services\service\ImagePath
 *
 * 2. Invoked as Console application
 *    argc and argv are standard program
 *    arguments
 *
 * 3. Invoked as Stop process by service
 *    when it SCM sends a STOP control signal
 *    to the service, and a service was configured
 *    with stop process feature
 *
 */
int wmain(int argc, LPCWSTR *argv)
{
    DWORD   x;
    int     opt;
    int     rv;
    HANDLE  h;
    LPCWSTR p = zerostring;
#if HAVE_DEBUG_TRACE
    LPCWSTR xsvcdbgparam = NULL;
#endif
    LPCWSTR svcmmapparam = NULL;
    SERVICE_TABLE_ENTRYW se[2];

#if defined(_DEBUG)
# if defined(_MSC_VER) && (_MSC_VER < 1900)
    /* Not supported */
# else
    _set_invalid_parameter_handler(xiphandler);
# endif
   _CrtSetReportMode(_CRT_ASSERT, 0);
#endif
    /**
     * Make sure child processes are kept quiet.
     */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);

    rv = xwmaininit();
    if (rv != 0)
        return rv;
    if (argc > 1)
        p = argv[1];
    if (xisalpha(*p)) {
        /**
         * Check if this is a Service Manager command
         */
        int cmd = xscmcommand(p);
        if (cmd >= 0) {
            argc  -= 2;
            argv  += 2;
#if HAVE_DEBUG_TRACE
            xtraceservice = 0;
#endif
            return xscmexecute(cmd, argc, argv);
        }
    }
    SVCBATCH_CS_INIT(service);
    atexit(objectscleanup);

    while ((opt = xwgetopt(argc, argv, cmdoptions)) != 0) {
        switch (opt) {
            case 'q':
                SVCOPT_SET(SVCBATCH_OPT_QUIET);
            break;
            case 's':
                svcmmapparam = xwoptarg;
            break;
            case 'd':
#if HAVE_DEBUG_TRACE
                if (xwoptarg)
                    xsvcdbgparam = xwoptarg;
                else
                    xtraceservice++;
#endif
            break;
            case ERROR_BAD_LENGTH:
            case ERROR_INVALID_DATA:
            case ERROR_BAD_FORMAT:
            case ERROR_INVALID_FUNCTION:
                rv = SVCBATCH_EEXIT("Invalid command line option(s)", ERROR_INVALID_PARAMETER);
                goto finished;
            break;
            default:
            break;
        }
    }
#if HAVE_DEBUG_TRACE
    if (xsvcdbgparam) {
        if (xisdigit(*xsvcdbgparam))
            xtraceservice = *(xsvcdbgparam++) - L'0';
        if (*xsvcdbgparam == L'-')
            xtracesvcstop = 0;
    }
#endif
    /**
     * Check if running as child stop process.
     */
    if (svcmmapparam) {
        LPWSTR dp;

        cnamestamp = SHUTDOWN_APPNAME " " SVCBATCH_VERSION_TXT;
#if HAVE_DEBUG_TRACE
        if (xtracesvcstop)
            xtracefopen();
        DBG_PRINTS(cnamestamp);
#endif
#if HAVE_NAMED_MMAP
        sharedmmap = OpenFileMappingW(FILE_MAP_READ, FALSE, svcmmapparam);
        if (sharedmmap == NULL) {
            rv = xsyserror(GetLastError(), L"OpenFileMapping", svcmmapparam);
            goto finished;
        }
#else
        xwcxtoq(svcmmapparam, &h);
        if (!DuplicateHandle(program->pInfo.hProcess,
                             h,
                             program->pInfo.hProcess,
                             &sharedmmap, 0, FALSE,
                             DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) {
            rv = xsyserror(GetLastError(), L"DuplicateHandle", svcmmapparam);
            goto finished;
        }
#endif
        sharedmem = (LPSVCBATCH_IPC)MapViewOfFile(
                                        sharedmmap,
                                        FILE_MAP_READ,
                                        0, 0, DSIZEOF(SVCBATCH_IPC));
        if (sharedmem == NULL) {
            rv = xsyserror(GetLastError(), L"MapViewOfFile", svcmmapparam);
            CloseHandle(sharedmmap);
            goto finished;
        }
        dp = sharedmem->data;
        svcoptions           = sharedmem->options;
        cmdproc->timeout     = sharedmem->timeout;
        service->killDepth   = sharedmem->killDepth;
        if (sharedmem->stdinSize) {
            stdinsize = sharedmem->stdinSize;
            stdindata = (LPBYTE)(dp + sharedmem->stdinData);
        }
        if (sharedmem->display)
            service->display = dp + sharedmem->display;
        service->name    = dp + sharedmem->name;
        service->work    = dp + sharedmem->work;
        service->logs    = dp + sharedmem->logs;
        if (sharedmem->logName && IS_NOT_OPT(SVCBATCH_OPT_QUIET)) {
            outputlog = (LPSVCBATCH_LOG)xmcalloc(sizeof(SVCBATCH_LOG));
            outputlog->logName = dp + sharedmem->logName;
            outputlog->maxLogs = sharedmem->maxLogs;
            SVCBATCH_CS_INIT(outputlog);
        }
        else {
            SVCOPT_SET(SVCBATCH_OPT_QUIET);
        }
        cmdproc->argc = sharedmem->argc;
        cmdproc->optc = sharedmem->optc;
        for (x = 0; x < cmdproc->argc; x++)
            cmdproc->args[x] = dp + sharedmem->args[x];
        for (x = 0; x < cmdproc->optc; x++)
            cmdproc->opts[x] = dp + sharedmem->opts[x];
        cmdproc->application = (LPWSTR)cmdproc->opts[0];
        rv = svcstopmain();
        goto finished;
    }
    servicemode = 1;
#if HAVE_DEBUG_TRACE
    xtracefopen();
    DBG_PRINTS(cnamestamp);
#endif
    /**
     * Presume we are started from SCM
     *
     * Create invisible console and
     * start to run as service
     */
    h = GetStdHandle(STD_INPUT_HANDLE);
    if (IS_INVALID_HANDLE(h)) {
        if (AllocConsole()) {
            /**
             * AllocConsole should create new set of
             * standard i/o handles
             */
            atexit(consolecleanup);
            h = GetStdHandle(STD_INPUT_HANDLE);
            ASSERT_HANDLE(h, ERROR_DEV_NOT_EXIST);
        }
        else {
            rv = GetLastError();
            goto finished;
        }
    }
    program->sInfo.hStdInput  = h;
    program->sInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    program->sInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consolehandler, TRUE);

    svcmainargc = argc;
    svcmainargv = argv;
    se[0].lpServiceName = zerostring;
    se[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)servicemain;
    se[1].lpServiceName = NULL;
    se[1].lpServiceProc = NULL;
    if (!StartServiceCtrlDispatcherW(se))
        rv = GetLastError();

finished:
    DBG_PRINTS("done");
    return rv;
}
