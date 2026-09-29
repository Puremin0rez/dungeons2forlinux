/* xgameruntime.dll replacement for Minecraft Dungeons II under Proton: the XAsync and
 * XTaskQueue runtime, XUser sign-in backed by signin.py, and the smaller GDK interfaces. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define E_NOTIMPL_      ((HRESULT)0x80004001)
#define E_NOINTERFACE_  ((HRESULT)0x80004002)
#define E_POINTER_      ((HRESULT)0x80004003)
#define E_ABORT_        ((HRESULT)0x80004004)
#define E_FAIL_         ((HRESULT)0x80004005)
#define E_PENDING_      ((HRESULT)0x8000000A)
#define E_OUTOFMEMORY_  ((HRESULT)0x8007000E)
#define E_INVALIDARG_   ((HRESULT)0x80070057)
#define E_INSUFFICIENT_ ((HRESULT)0x8007007A)

#ifndef D2FL_VERSION
#define D2FL_VERSION "dev"
#endif

#define TITLE_ID 0x6B9DE498u
#define ASYNC_MAGIC 0x31524758u
#define ASYNC_DONE  0x32454758u
#define MODE_MANUAL 0
#define MODE_THREADPOOL 1
#define MODE_SERIAL 2
#define MODE_IMMEDIATE 3
#define PORT_WORK 0
#define PORT_COMP 1
#define OP_BEGIN 0
#define OP_DOWORK 1
#define OP_GETRESULT 2
#define OP_CANCEL 3
#define OP_CLEANUP 4

/* Longest time a blocking wait or termination waits for a call to finish. */
#define WAIT_GIVEUP_MS 30000u
#define LOG_MAX_BYTES (8u << 20)

typedef struct queue_obj queue_obj;

typedef struct XAsyncBlock {
    queue_obj *queue;
    void *context;
    void (__stdcall *callback)(struct XAsyncBlock *);
    unsigned char internal[32];
} XAsyncBlock;

_Static_assert(sizeof(XAsyncBlock) == 56, "XAsyncBlock must match the GDK x64 layout");

typedef struct XAsyncProviderData {
    XAsyncBlock *async;
    SIZE_T bufferSize;
    void *buffer;
    void *context;
} XAsyncProviderData;

typedef HRESULT (__stdcall *XAsyncProvider)(UINT32 op, const XAsyncProviderData *data);
typedef HRESULT (__stdcall *XAsyncWork)(XAsyncBlock *async);
typedef void (__stdcall *XTaskQueueCallback)(void *context, BOOLEAN canceled);
typedef void (__stdcall *XAsyncCompletion)(XAsyncBlock *async);

typedef struct async_state async_state;

typedef struct task_item {
    struct task_item *next;
    ULONGLONG ready;
    int kind;
    void *context;
    XTaskQueueCallback callback;
    async_state *state;
    queue_obj *via;
    int via_ref;
} task_item;

#define ITEM_CALLBACK 0
#define ITEM_DOWORK 1
#define ITEM_COMPLETION 2

typedef struct port_obj {
    queue_obj *q;
    int which;
} port_obj;

#define QUEUE_MAGIC 0x51554555u

/* refs counts game handles, the process-queue slot, composites built on it,
 * in-flight calls and running Dispatch/Terminate calls. At zero the queue is
 * retired; a queue with a worker thread is freed by that thread. */
struct queue_obj {
    UINT32 magic;
    LONG refs;
    int work_mode;
    int comp_mode;
    int terminated;
    int closing;
    int composite;
    queue_obj *work_delegate;
    queue_obj *comp_delegate;
    int work_which;
    int comp_which;
    task_item *work_head, *work_tail;
    task_item *comp_head, *comp_tail;
    port_obj work_port;
    port_obj comp_port;
    CRITICAL_SECTION lock;
    HANDLE work_event;
    HANDLE comp_event;
    HANDLE wake_event;
    HANDLE thread;
    DWORD thread_id;
    int nmon;
    void *mon_ctx[4];
    void (__stdcall *mon_cb[4])(void *ctx, struct queue_obj *queue, unsigned port);
    UINT64 mon_token[4];
    LONG suspends;  /* in-flight calls; termination callbacks wait until this is zero */
    struct term_note *pending_terms;
    int dispatched;
};

struct term_note {
    void (__stdcall *cb)(void *);
    void *ctx;
    struct term_note *next;
};

/* refs counts the operation until it is detached from the caller's block,
 * each queued item, and each state_acquire caller. */
struct async_state {
    UINT32 magic;
    LONG refs;
    XAsyncBlock *block;
    XAsyncCompletion callback;
    queue_obj *queue;
    void *context;
    const void *identity;
    XAsyncProvider provider;
    HRESULT result;
    SIZE_T required;
    int complete;
    void *payload;
    SIZE_T payload_len;
    int payload_secret;
    int traced;
    char name[48];
};

static CRITICAL_SECTION g_lock;
static CONDITION_VARIABLE g_done_cv = CONDITION_VARIABLE_INIT;
static int g_lock_ready;
static queue_obj *g_process_queue;
static DWORD g_tls = TLS_OUT_OF_INDEXES;
static LONG g_inited;

#define STATE_DIR_NAME L"dungeons2forlinux"
#define LOG_NAME L"dungeons2forlinux.log"

static WCHAR g_state_dir[MAX_PATH];
static WCHAR g_log_path[MAX_PATH + 32];
static INIT_ONCE g_state_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_state_dir(INIT_ONCE *once, void *param, void **ctx)
{
    WCHAR base[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD n;
    (void)once; (void)param; (void)ctx;
    /* DUNGEONS2FORLINUX_DIR (a Windows path) overrides the state folder. */
    n = GetEnvironmentVariableW(L"DUNGEONS2FORLINUX_DIR", g_state_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) wcscpy(base, L"C:\\users\\steamuser\\AppData\\Local");
        _snwprintf(g_state_dir, MAX_PATH - 1, L"%ls\\%ls", base, STATE_DIR_NAME);
    }
    CreateDirectoryW(g_state_dir, NULL);
    _snwprintf(g_log_path, MAX_PATH + 31, L"%ls\\%ls", g_state_dir, LOG_NAME);
    if (GetFileAttributesExW(g_log_path, GetFileExInfoStandard, &fa) &&
        (fa.nFileSizeHigh || fa.nFileSizeLow > LOG_MAX_BYTES)) {
        WCHAR old[MAX_PATH + 40];
        _snwprintf(old, MAX_PATH + 39, L"%ls.old", g_log_path);
        MoveFileExW(g_log_path, old, MOVEFILE_REPLACE_EXISTING);
    }
    return TRUE;
}

static const WCHAR *state_dir(void)
{
    InitOnceExecuteOnce(&g_state_once, init_state_dir, NULL, NULL);
    return g_state_dir;
}

/* Appends a line to the log, cutting long lines. Preserves GetLastError. */
static void xlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void xlog(const char *fmt, ...)
{
    char buf[640];
    va_list ap;
    DWORD wrote, err = GetLastError();
    SYSTEMTIME t;
    HANDLE h;
    int n, cap, m;

    state_dir();
    GetLocalTime(&t);
    n = snprintf(buf, sizeof buf, "%02u:%02u:%02u.%03u %5lu ", (unsigned)t.wHour, (unsigned)t.wMinute,
                 (unsigned)t.wSecond, (unsigned)t.wMilliseconds, (unsigned long)GetCurrentThreadId());
    if (n < 0 || n >= (int)sizeof buf / 2) n = 0;
    cap = (int)sizeof buf - 1 - n;
    va_start(ap, fmt);
    m = vsnprintf(buf + n, (size_t)cap, fmt, ap);
    va_end(ap);
    if (m < 0 || m > cap - 1) m = cap - 1;
    n += m;
    if (buf[n - 1] != '\n') buf[n++] = '\n';
    h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, buf, (DWORD)n, &wrote, NULL);
        CloseHandle(h);
    }
    SetLastError(err);
}

static void xlog_url(const char *what, const char *url)
{
    if (!url) url = "";
    xlog("%s %.*s", what, (int)strcspn(url, "?#"), url);
}

static const char *g_seen[160];
static int g_seen_n;

static void log_once(const char *s)
{
    int i;
    if (!g_lock_ready) { xlog("%s", s); return; }
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_seen_n; i++) {
        if (g_seen[i] == s) { LeaveCriticalSection(&g_lock); return; }
    }
    if (g_seen_n < (int)(sizeof(g_seen) / sizeof(g_seen[0])))
        g_seen[g_seen_n++] = s;
    LeaveCriticalSection(&g_lock);
    xlog("%s", s);
}

static int guid_eq(const GUID *a, const GUID *b)
{
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static void log_guid(const char *tag, const GUID *g)
{
    xlog("%s %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
         tag, g->Data1, g->Data2, g->Data3,
         g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
         g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

static char *dup_string(const char *s)
{
    size_t n;
    char *d;
    if (!s) return NULL;
    n = strlen(s) + 1;
    d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void free_secret(void *p, SIZE_T len)
{
    if (!p) return;
    SecureZeroMemory(p, len);
    free(p);
}

#define G(name, d1, d2, d3, a, b, c, d, e, f, g, h) \
    static const GUID name = { d1, d2, d3, { a, b, c, d, e, f, g, h } };

G(IID_IUnknown_, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
G(IID_Threading, 0x073b7dcb, 0x1fcf, 0x4030, 0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28)
G(IID_Feature,   0x8836fe87, 0xedb9, 0x4fe3, 0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40)
G(IID_User,      0x01acd177, 0x91f9, 0x4763, 0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0)
G(IID_User2,     0xeb9bf948, 0x18dc, 0x4d82, 0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0)
G(IID_User3,     0x1bf2f8c5, 0xd507, 0x4e52, 0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61)
G(IID_User4,     0x079415e3, 0x6727, 0x437f, 0x8e, 0x9d, 0x8f, 0x8f, 0x9b, 0x24, 0x39, 0xf7)
G(IID_User5,     0x26f3c674, 0xa2fe, 0x44fa, 0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53)
G(IID_User6,     0x5131d685, 0x4394, 0x4ee6, 0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff)
G(IID_Gamertag,  0xcef4fac0, 0x7676, 0x4a94, 0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74)
G(IID_Game,      0x973a344e, 0x24bf, 0x4d0f, 0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29)
G(IID_Game2,     0x50849859, 0x0ad8, 0x4f81, 0x80, 0xe4, 0x5b, 0xc7, 0x86, 0x26, 0xf8, 0x52)
G(IID_Game3,     0x2549f142, 0x6419, 0x4a06, 0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34)
G(IID_System,    0xe349bd1a, 0xfc20, 0x4e40, 0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f)
G(IID_System2,   0x6fd71f09, 0x7513, 0x49f0, 0x89, 0xbc, 0xbf, 0xaf, 0x5d, 0xf6, 0xf8, 0x52)
G(IID_System3,   0x67ce4bfc, 0xb1d1, 0x4ac7, 0xbc, 0x3a, 0xcb, 0x92, 0x19, 0xa9, 0x7a, 0x85)
G(IID_System4,   0xdadc2895, 0x34b0, 0x4ef5, 0xa8, 0x3e, 0x45, 0x11, 0x4d, 0x62, 0x9b, 0x80)
G(IID_System5,   0x1861cf2e, 0xe18b, 0x4834, 0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf)
G(IID_Analytics, 0xb884675d, 0xb738, 0x4a9c, 0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b)
G(IID_PLS,       0xf4faf4d4, 0x2d04, 0x4fce, 0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84)
G(IID_PLS2,      0xd29411df, 0x0794, 0x4553, 0x8b, 0x27, 0x95, 0xfc, 0x02, 0xd0, 0xf7, 0x5d)
G(IID_PLS3,      0x41a4e10c, 0x5a7e, 0x41d9, 0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6)
G(IID_Error,     0x8ca467f7, 0x22e8, 0x4096, 0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8)
G(IID_Protocol,  0x026b010c, 0x06c3, 0x4cdd, 0xbb, 0xcb, 0x43, 0xf2, 0x29, 0xdb, 0x1c, 0xff)
G(CLSID_Protocol,0x95fd18d2, 0x74dd, 0x4d7c, 0xaa, 0x1b, 0x0b, 0x51, 0x82, 0x76, 0x65, 0xd6)
G(IID_Net,       0x37e56907, 0x2f10, 0x41e8, 0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a)
G(IID_Net2,      0xbf2346b2, 0x39af, 0x4658, 0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3)

typedef struct com_obj { const void *vtbl; } com_obj;

static HRESULT WINAPI gen_qi(com_obj *self, const GUID *iid, void **out, const GUID *const *ok, int n)
{
    int i;
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!iid) return E_INVALIDARG_;
    if (guid_eq(iid, &IID_IUnknown_)) { *out = self; return S_OK; }
    for (i = 0; i < n; i++) {
        if (guid_eq(iid, ok[i])) { *out = self; return S_OK; }
    }
    return E_NOINTERFACE_;
}

static ULONG WINAPI gen_addref(com_obj *self) { (void)self; return 2; }
static ULONG WINAPI gen_release(com_obj *self) { (void)self; return 1; }

static HRESULT WINAPI stub_notimpl(void *self)
{
    (void)self;
    log_once("stub_notimpl");
    return E_NOTIMPL_;
}

/* pblock is the copy of the caller's block handed to the provider. pdata is
 * passed to every provider call and lives as long as the operation. */
struct async_ext {
    XAsyncBlock pblock;
    XAsyncProviderData pdata;
    int extracted;
    int wait_done;
    int provider_cleaned;
    int holds_termination;
};

typedef struct async_full {
    async_state st;
    struct async_ext x;
} async_full;

#define EXT(s) (&((async_full *)(s))->x)

static void state_release(async_state *st);
static void queue_release(queue_obj *q);
static void queue_resume_termination(queue_obj *q);
static void run_item(task_item *it, int canceled);
static void complete_async(XAsyncBlock *block, HRESULT result, SIZE_T required);

/* While a call is bound, internal[] holds the state pointer and ASYNC_MAGIC.
 * Once it is finished it holds ASYNC_DONE, the result and the result size. */
static async_state *state_of(XAsyncBlock *b)
{
    async_state *s;
    UINT32 magic;
    if (!b) return NULL;
    memcpy(&s, b->internal, sizeof(s));
    memcpy(&magic, b->internal + sizeof(s), sizeof(magic));
    if (magic != ASYNC_MAGIC || !s || s->magic != ASYNC_MAGIC)
        return NULL;
    if (s->block != b && &EXT(s)->pblock != b)
        return NULL;
    return s;
}

static void state_bind(XAsyncBlock *b, async_state *s)
{
    UINT32 magic = ASYNC_MAGIC;
    memset(b->internal, 0, sizeof(b->internal));
    memcpy(b->internal, &s, sizeof(s));
    memcpy(b->internal + sizeof(s), &magic, sizeof(magic));
}

static void state_mark_done(XAsyncBlock *b, HRESULT hr, SIZE_T required)
{
    UINT32 magic = ASYNC_DONE;
    memset(b->internal, 0, sizeof(b->internal));
    memcpy(b->internal + 8, &magic, sizeof(magic));
    memcpy(b->internal + 12, &hr, sizeof(hr));
    memcpy(b->internal + 16, &required, sizeof(required));
}

static int read_done(XAsyncBlock *b, HRESULT *hr, SIZE_T *required)
{
    UINT32 magic;
    if (!b) return 0;
    memcpy(&magic, b->internal + 8, sizeof(magic));
    if (magic != ASYNC_DONE) return 0;
    if (hr) memcpy(hr, b->internal + 12, sizeof(*hr));
    if (required) memcpy(required, b->internal + 16, sizeof(*required));
    return 1;
}

static async_state *state_acquire(XAsyncBlock *b)
{
    async_state *s;
    if (!b || !g_lock_ready) return NULL;
    EnterCriticalSection(&g_lock);
    s = state_of(b);
    if (s) s->refs++;
    LeaveCriticalSection(&g_lock);
    return s;
}

static void state_addref(async_state *s)
{
    EnterCriticalSection(&g_lock);
    s->refs++;
    LeaveCriticalSection(&g_lock);
}

/* Detaches the call from the caller's block. Call with g_lock held; returns 1
 * if it detached, and the caller then drops the operation's reference. */
static int state_extract_locked(async_state *s, HRESULT final_hr, int mark_done)
{
    struct async_ext *x = EXT(s);
    if (x->extracted) return 0;
    x->extracted = 1;
    if (s->block && state_of(s->block) == s) {
        if (mark_done) state_mark_done(s->block, final_hr, 0);
        else memset(s->block->internal, 0, sizeof(s->block->internal));
    }
    return 1;
}

static void state_extract(async_state *s, HRESULT final_hr, int mark_done)
{
    int detached;
    EnterCriticalSection(&g_lock);
    detached = state_extract_locked(s, final_hr, mark_done);
    LeaveCriticalSection(&g_lock);
    WakeAllConditionVariable(&g_done_cv);
    if (detached) state_release(s);
}

static void state_release(async_state *s)
{
    struct async_ext *x;
    LONG left;
    if (!s) return;
    EnterCriticalSection(&g_lock);
    left = --s->refs;
    LeaveCriticalSection(&g_lock);
    if (left > 0) return;
    x = EXT(s);
    if (s->provider && !x->provider_cleaned) {
        x->provider_cleaned = 1;
        s->provider(OP_CLEANUP, &x->pdata);
    }
    if (s->payload_secret) free_secret(s->payload, s->payload_len);
    else free(s->payload);
    queue_release(s->queue);
    s->magic = 0;
    free(s);
}

static task_item *item_new(int kind, async_state *st, XTaskQueueCallback cb, void *ctx, UINT32 delay)
{
    task_item *it = calloc(1, sizeof(*it));
    if (!it) return NULL;
    it->kind = kind;
    it->callback = cb;
    it->context = ctx;
    it->ready = GetTickCount64() + delay;
    if (st) {
        state_addref(st);
        it->state = st;
    }
    return it;
}

static void item_free(task_item *it)
{
    if (!it) return;
    if (it->state) state_release(it->state);
    if (it->via_ref) queue_release(it->via);
    free(it);
}

static int queue_live(queue_obj *q)
{
    return q && q->magic == QUEUE_MAGIC && !q->terminated;
}

/* The real queue and port that hold items for a port of q; *port is updated. */
static queue_obj *target_queue(queue_obj *q, int *port)
{
    if (!q) return NULL;
    if (q->composite) {
        if (*port == PORT_WORK) { *port = q->work_which; return q->work_delegate; }
        *port = q->comp_which;
        return q->comp_delegate;
    }
    return q;
}

static int mode_of(queue_obj *q, int port)
{
    queue_obj *t = target_queue(q, &port);
    if (!t) return MODE_THREADPOOL;
    return port == PORT_WORK ? t->work_mode : t->comp_mode;
}

static int port_is_auto(int mode)
{
    return mode != MODE_MANUAL;
}

static int queue_acquire(queue_obj *q)
{
    int ok;
    if (!q || !g_lock_ready) return 0;
    EnterCriticalSection(&g_lock);
    ok = queue_live(q);
    if (ok) q->refs++;
    LeaveCriticalSection(&g_lock);
    return ok;
}

static task_item *pop_ready(task_item **head, task_item **tail, ULONGLONG cutoff)
{
    task_item *prev = NULL, *it = *head;
    while (it) {
        if (it->ready <= cutoff) {
            if (prev) prev->next = it->next;
            else *head = it->next;
            if (*tail == it) *tail = prev;
            it->next = NULL;
            return it;
        }
        prev = it;
        it = it->next;
    }
    return NULL;
}

static DWORD next_due_ms(task_item *it, DWORD cap)
{
    ULONGLONG now = GetTickCount64();
    DWORD best = cap;
    for (; it; it = it->next) {
        if (it->ready <= now) return 0;
        if (it->ready - now < best) best = (DWORD)(it->ready - now);
    }
    return best;
}

static int dispatch_due(queue_obj *q, int port, ULONGLONG cutoff)
{
    task_item *it;
    queue_obj *t = target_queue(q, &port);
    if (!t) return 0;
    EnterCriticalSection(&t->lock);
    it = port == PORT_WORK ? pop_ready(&t->work_head, &t->work_tail, cutoff)
                           : pop_ready(&t->comp_head, &t->comp_tail, cutoff);
    LeaveCriticalSection(&t->lock);
    if (!it) return 0;
    /* Items submitted through a terminating queue run canceled. */
    run_item(it, it->via && it->via->closing);
    return 1;
}

static int dispatch_one(queue_obj *q, int port)
{
    return dispatch_due(q, port, GetTickCount64());
}

static void fire_monitors(queue_obj *q, unsigned port)
{
    void (__stdcall *cbs[4])(void *, queue_obj *, unsigned);
    void *ctxs[4];
    int n = 0, i;
    if (!q || !g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    if (q->magic == QUEUE_MAGIC) {
        n = q->nmon;
        if (n > 4) n = 4;
        for (i = 0; i < n; i++) { cbs[i] = q->mon_cb[i]; ctxs[i] = q->mon_ctx[i]; }
    }
    LeaveCriticalSection(&g_lock);
    for (i = 0; i < n; i++) if (cbs[i]) cbs[i](ctxs[i], q, port);
}

static HRESULT enqueue(queue_obj *q, int port, task_item *it)
{
    queue_obj *t;
    int manual;
    if (!it) return E_OUTOFMEMORY_;
    if (!g_lock_ready) { item_free(it); return E_FAIL_; }
    EnterCriticalSection(&g_lock);
    t = target_queue(q, &port);
    if (!queue_live(t)) {
        LeaveCriticalSection(&g_lock);
        item_free(it);
        return E_ABORT_;
    }
    it->via = q;
    if (q != t) {
        q->refs++;
        it->via_ref = 1;
    }
    EnterCriticalSection(&t->lock);
    LeaveCriticalSection(&g_lock);
    it->next = NULL;
    if (port == PORT_WORK) {
        if (t->work_tail) t->work_tail->next = it;
        else t->work_head = it;
        t->work_tail = it;
        SetEvent(t->work_event);
    } else {
        if (t->comp_tail) t->comp_tail->next = it;
        else t->comp_head = it;
        t->comp_tail = it;
        SetEvent(t->comp_event);
    }
    manual = (port == PORT_WORK ? t->work_mode : t->comp_mode) == MODE_MANUAL;
    LeaveCriticalSection(&t->lock);
    if (manual) fire_monitors(t, (unsigned)port);
    return S_OK;
}

/* Runs a retired queue's remaining items canceled, then frees it. */
static void queue_teardown(queue_obj *q)
{
    task_item *it;
    for (;;) {
        EnterCriticalSection(&q->lock);
        it = q->work_head ? q->work_head : q->comp_head;
        if (it) {
            if (it == q->work_head) {
                q->work_head = it->next;
                if (!q->work_head) q->work_tail = NULL;
            } else {
                q->comp_head = it->next;
                if (!q->comp_head) q->comp_tail = NULL;
            }
        }
        LeaveCriticalSection(&q->lock);
        if (!it) break;
        run_item(it, 1);
    }
    if (q->work_event) CloseHandle(q->work_event);
    if (q->comp_event) CloseHandle(q->comp_event);
    if (q->wake_event) CloseHandle(q->wake_event);
    if (q->thread) CloseHandle(q->thread);
    DeleteCriticalSection(&q->lock);
    free(q);
}

/* On a manual queue nobody dispatches or monitors, the worker runs items
 * that have waited this long. */
#define RESCUE_AFTER_MS 500

static DWORD WINAPI queue_thread(LPVOID param)
{
    queue_obj *q = param;
    for (;;) {
        HANDLE waits[3];
        DWORD n = 0, timeout = INFINITE;
        int did, auto_work, auto_comp, rescue;
        do {
            ULONGLONG now = GetTickCount64();
            auto_work = port_is_auto(q->work_mode);
            auto_comp = port_is_auto(q->comp_mode);
            rescue = !auto_work && !auto_comp && q->nmon == 0 && !q->dispatched;
            did = 0;
            if (auto_work) did |= dispatch_due(q, PORT_WORK, now);
            if (auto_comp) did |= dispatch_due(q, PORT_COMP, now);
            if (rescue && now > RESCUE_AFTER_MS) {
                did |= dispatch_due(q, PORT_WORK, now - RESCUE_AFTER_MS);
                did |= dispatch_due(q, PORT_COMP, now - RESCUE_AFTER_MS);
            }
        } while (did && !q->terminated);
        if (q->terminated) break;
        EnterCriticalSection(&q->lock);
        if (auto_work) timeout = next_due_ms(q->work_head, timeout);
        if (auto_comp) timeout = next_due_ms(q->comp_head, timeout);
        if (rescue && (q->work_head || q->comp_head) && timeout > 100) timeout = 100;
        LeaveCriticalSection(&q->lock);
        if (auto_work) waits[n++] = q->work_event;
        if (auto_comp) waits[n++] = q->comp_event;
        waits[n++] = q->wake_event;
        if (timeout > 1000) timeout = 1000;
        WaitForMultipleObjects(n, waits, FALSE, timeout);
    }
    queue_teardown(q);
    return 0;
}

static queue_obj *queue_new(int work_mode, int comp_mode)
{
    queue_obj *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->refs = 1;
    q->work_mode = work_mode;
    q->comp_mode = comp_mode;
    q->work_port.q = q;
    q->work_port.which = PORT_WORK;
    q->comp_port.q = q;
    q->comp_port.which = PORT_COMP;
    InitializeCriticalSection(&q->lock);
    q->work_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    q->comp_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    q->wake_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!q->work_event || !q->comp_event || !q->wake_event) {
        queue_teardown(q);
        return NULL;
    }
    q->magic = QUEUE_MAGIC;
    q->thread = CreateThread(NULL, 0, queue_thread, q, 0, &q->thread_id);
    if (!q->thread) {
        q->magic = 0;
        queue_teardown(q);
        return NULL;
    }
    return q;
}

static void queue_release(queue_obj *q)
{
    LONG left;
    queue_obj *w = NULL, *c = NULL;
    int composite;
    if (!q || !g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    if (q->magic != QUEUE_MAGIC) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    left = --q->refs;
    if (left > 0) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    q->terminated = 1;
    q->magic = 0;
    composite = q->composite;
    if (composite) {
        w = q->work_delegate;
        c = q->comp_delegate;
    }
    LeaveCriticalSection(&g_lock);
    if (composite) {
        DeleteCriticalSection(&q->lock);
        free(q);
        queue_release(w);
        queue_release(c);
        return;
    }
    SetEvent(q->wake_event);
}

static queue_obj *process_queue(void)
{
    queue_obj *q;
    if (!g_lock_ready) return NULL;
    EnterCriticalSection(&g_lock);
    q = g_process_queue;
    LeaveCriticalSection(&g_lock);
    if (q) return q;
    q = queue_new(MODE_THREADPOOL, MODE_THREADPOOL);
    if (!q) return NULL;
    EnterCriticalSection(&g_lock);
    if (!g_process_queue) {
        g_process_queue = q;
        q = NULL;
    }
    LeaveCriticalSection(&g_lock);
    if (q) queue_release(q);
    return g_process_queue;
}

static int queue_closing(queue_obj *q)
{
    int closing;
    EnterCriticalSection(&g_lock);
    closing = q->closing ||
              (q->composite && (q->work_delegate->closing || q->comp_delegate->closing));
    LeaveCriticalSection(&g_lock);
    return closing;
}

static queue_obj *queue_or_process(queue_obj *q)
{
    if (!q) {
        if (!process_queue()) return NULL;
        EnterCriticalSection(&g_lock);
        q = g_process_queue;
        if (q) q->refs++;
        LeaveCriticalSection(&g_lock);
        return q;
    }
    return queue_acquire(q) ? q : NULL;
}

static void signal_wait(async_state *st)
{
    int resume;
    EnterCriticalSection(&g_lock);
    EXT(st)->wait_done = 1;
    resume = EXT(st)->holds_termination;
    EXT(st)->holds_termination = 0;
    LeaveCriticalSection(&g_lock);
    WakeAllConditionVariable(&g_done_cv);
    if (resume) queue_resume_termination(st->queue);
}

static void run_dowork(async_state *st)
{
    struct async_ext *x = EXT(st);
    if (st->traced) xlog("%s work running", st->name);
    EnterCriticalSection(&g_lock);
    if (x->extracted || st->complete || !st->provider) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    LeaveCriticalSection(&g_lock);
    st->provider(OP_DOWORK, &x->pdata);
}

static void run_completion(async_state *st)
{
    if (st->traced) xlog("%s completion callback", st->name);
    if (st->callback) st->callback(st->block);
    signal_wait(st);
}

static void cancel_call(async_state *st)
{
    if (!st->complete && st->provider) st->provider(OP_CANCEL, &EXT(st)->pdata);
    if (!st->complete) complete_async(&EXT(st)->pblock, E_ABORT_, 0);
}

static void run_item(task_item *it, int canceled)
{
    if (it->kind == ITEM_CALLBACK) {
        if (it->callback) it->callback(it->context, canceled ? TRUE : FALSE);
    } else if (it->kind == ITEM_DOWORK) {
        if (canceled) cancel_call(it->state);
        else run_dowork(it->state);
    } else if (it->kind == ITEM_COMPLETION) {
        run_completion(it->state);
    }
    item_free(it);
}

/* A call with no result data is detached at once; otherwise GetResult detaches it. */
static void complete_async(XAsyncBlock *block, HRESULT result, SIZE_T required)
{
    async_state *st;
    int detach, mode;
    task_item *it;
    static LONG logs;

    if (result == E_PENDING_) return;
    if (!block || !g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    st = state_of(block);
    if (!st || st->complete) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    st->complete = 1;
    st->result = result;
    if (FAILED(result)) required = 0;
    st->required = required;
    EXT(st)->pdata.bufferSize = required;
    st->refs++;
    detach = required == 0 && state_extract_locked(st, result, 1);
    LeaveCriticalSection(&g_lock);
    WakeAllConditionVariable(&g_done_cv);
    if (st->traced || InterlockedIncrement(&logs) <= 48)
        xlog("complete %s hr=%08lX callback=%d", st->name, (unsigned long)result, st->callback != NULL);

    if (st->callback) {
        mode = mode_of(st->queue, PORT_COMP);
        if (mode == MODE_IMMEDIATE) {
            run_completion(st);
        } else {
            it = item_new(ITEM_COMPLETION, st, NULL, NULL, 0);
            if (FAILED(enqueue(st->queue, PORT_COMP, it))) run_completion(st);
        }
    } else {
        signal_wait(st);
    }
    if (detach) state_release(st);
    state_release(st);
}

static HRESULT schedule_async(XAsyncBlock *block, UINT32 delay)
{
    async_state *st;
    task_item *it;
    HRESULT hr;
    static LONG logs;

    if (!block || !g_lock_ready) return E_INVALIDARG_;
    EnterCriticalSection(&g_lock);
    st = state_of(block);
    if (!st || EXT(st)->extracted) {
        LeaveCriticalSection(&g_lock);
        return E_INVALIDARG_;
    }
    if (st->complete) {
        LeaveCriticalSection(&g_lock);
        return S_OK;
    }
    st->refs++;
    LeaveCriticalSection(&g_lock);
    if (InterlockedIncrement(&logs) <= 48)
        xlog("schedule %s delay=%lu mode=%d", st->name, (unsigned long)delay, mode_of(st->queue, PORT_WORK));
    if (mode_of(st->queue, PORT_WORK) == MODE_IMMEDIATE && delay == 0) {
        run_dowork(st);
        hr = S_OK;
    } else {
        it = item_new(ITEM_DOWORK, st, NULL, NULL, delay);
        hr = enqueue(st->queue, PORT_WORK, it);
    }
    state_release(st);
    return hr;
}

/* builtin=1 also runs the provider's Cleanup op when the call cannot start. */
static HRESULT begin_async_ex(XAsyncBlock *async, void *context, const void *identity,
                              const char *name, XAsyncProvider provider, int builtin)
{
    async_full *f;
    async_state *st;
    queue_obj *q;
    XAsyncProviderData data;
    HRESULT hr;

    hr = S_OK;
    q = NULL;
    f = NULL;
    if (!async || !provider || !g_lock_ready) hr = E_INVALIDARG_;
    if (SUCCEEDED(hr)) {
        EnterCriticalSection(&g_lock);
        st = state_of(async);
        LeaveCriticalSection(&g_lock);
        if (st) hr = E_INVALIDARG_;
    }
    if (SUCCEEDED(hr) && !(q = queue_or_process(async->queue))) hr = E_ABORT_;
    if (SUCCEEDED(hr) && queue_closing(q)) {
        hr = E_ABORT_;
        xlog("begin rejected on terminating queue: %s", name ? name : "?");
    }
    if (SUCCEEDED(hr) && !(f = calloc(1, sizeof(*f)))) hr = E_OUTOFMEMORY_;
    if (FAILED(hr)) {
        queue_release(q);
        if (builtin && provider) {
            memset(&data, 0, sizeof(data));
            data.context = context;
            provider(OP_CLEANUP, &data);
        }
        return hr;
    }
    st = &f->st;
    st->magic = ASYNC_MAGIC;
    st->refs = 1;
    st->block = async;
    st->callback = async->callback;
    st->queue = q;
    st->context = context;
    st->identity = identity;
    st->provider = provider;
    snprintf(st->name, sizeof(st->name), "%s", name ? name : "?");
    st->traced = !strncmp(st->name, "XUserAdd", 8);
    if (st->traced) {
        int wp = PORT_WORK, cp = PORT_COMP;
        queue_obj *tw, *tc;
        EnterCriticalSection(&g_lock);
        tw = target_queue(q, &wp);
        tc = target_queue(q, &cp);
        xlog("%s begin block=%p queue=%p%s work=%d/%s comp=%d/%s dispatched=%d/%d monitors=%d/%d",
             st->name, (void *)async, (void *)q, q->composite ? " (composite)" : "",
             tw->work_mode, wp == PORT_WORK ? "work" : "comp", tc->comp_mode, cp == PORT_WORK ? "work" : "comp",
             tw->dispatched, tc->dispatched, tw->nmon, tc->nmon);
        LeaveCriticalSection(&g_lock);
    }
    f->x.pblock = *async;
    f->x.pblock.queue = q;
    f->x.pdata.async = &f->x.pblock;
    f->x.pdata.context = context;
    EnterCriticalSection(&g_lock);
    state_bind(async, st);
    state_bind(&f->x.pblock, st);
    q->suspends++;
    f->x.holds_termination = 1;
    LeaveCriticalSection(&g_lock);

    hr = provider(OP_BEGIN, &f->x.pdata);
    if (FAILED(hr)) {
        xlog("begin failed %s %08lX", st->name, (unsigned long)hr);
        signal_wait(st);
        state_extract(st, hr, 0);
        return hr;
    }
    return S_OK;
}

static HRESULT begin_async(XAsyncBlock *async, void *context, const void *identity,
                           const char *name, XAsyncProvider provider)
{
    return begin_async_ex(async, context, identity, name, provider, 1);
}

static void finish_result(async_state *st)
{
    state_extract(st, st->result, 1);
}

static HRESULT builtin_common(UINT32 op, const XAsyncProviderData *data, int owns_context)
{
    if (op == OP_BEGIN) return schedule_async(data->async, 0);
    if (op == OP_CANCEL) complete_async(data->async, E_ABORT_, 0);
    if (op == OP_CLEANUP && owns_context) free(data->context);
    return S_OK;
}

static void complete_with_payload(XAsyncBlock *block, const char *text, int secret, SIZE_T required_extra)
{
    async_state *st = state_acquire(block);
    char *copy;
    SIZE_T len;
    if (!st) return;
    len = strlen(text) + 1;
    copy = dup_string(text);
    if (!copy) {
        state_release(st);
        complete_async(block, E_OUTOFMEMORY_, 0);
        return;
    }
    EnterCriticalSection(&g_lock);
    if (st->complete || st->payload) {
        LeaveCriticalSection(&g_lock);
        if (secret) free_secret(copy, len);
        else free(copy);
        state_release(st);
        return;
    }
    st->payload = copy;
    st->payload_len = len;
    st->payload_secret = secret;
    LeaveCriticalSection(&g_lock);
    state_release(st);
    complete_async(block, S_OK, len + required_extra);
}

/* With wait set, blocks until the completion callback has run; never dispatches. */
static HRESULT WINAPI thr_GetStatus(void *self, XAsyncBlock *async, BOOLEAN wait)
{
    async_state *st;
    HRESULT hr;
    ULONGLONG start;
    static LONG calls;
    (void)self;
    if (!async || !g_lock_ready) return E_INVALIDARG_;
    EnterCriticalSection(&g_lock);
    st = state_of(async);
    if (!st) {
        LeaveCriticalSection(&g_lock);
        if (read_done(async, &hr, NULL)) return hr;
        return wait ? E_INVALIDARG_ : E_PENDING_;
    }
    if ((InterlockedIncrement(&calls) % 200000) == 1)
        xlog("GetStatus polled %s complete=%d wait=%u", st->name, st->complete, (unsigned)wait);
    if (!wait) {
        hr = st->complete ? st->result : E_PENDING_;
        LeaveCriticalSection(&g_lock);
        return hr;
    }
    st->refs++;
    start = GetTickCount64();
    while (!EXT(st)->wait_done) {
        if (GetTickCount64() - start > WAIT_GIVEUP_MS) {
            xlog("GetStatus wait gave up on %s after %u ms", st->name, WAIT_GIVEUP_MS);
            break;
        }
        SleepConditionVariableCS(&g_done_cv, &g_lock, 1000);
    }
    hr = st->complete ? st->result : E_PENDING_;
    LeaveCriticalSection(&g_lock);
    state_release(st);
    return hr;
}

static HRESULT WINAPI thr_GetResultSize(void *self, XAsyncBlock *async, SIZE_T *bufferSize)
{
    async_state *st;
    HRESULT hr;
    (void)self;
    if (!bufferSize) return E_POINTER_;
    *bufferSize = 0;
    if (!async || !g_lock_ready) return E_INVALIDARG_;
    EnterCriticalSection(&g_lock);
    st = state_of(async);
    if (!st) {
        LeaveCriticalSection(&g_lock);
        return read_done(async, &hr, NULL) ? hr : E_PENDING_;
    }
    hr = st->complete ? st->result : E_PENDING_;
    if (st->complete) *bufferSize = st->required;
    LeaveCriticalSection(&g_lock);
    return hr;
}

static void WINAPI thr_Cancel(void *self, XAsyncBlock *async)
{
    async_state *st = state_acquire(async);
    (void)self;
    if (!st) return;
    if (!st->complete && st->provider) st->provider(OP_CANCEL, &EXT(st)->pdata);
    state_release(st);
}

static HRESULT WINAPI run_provider(UINT32 op, const XAsyncProviderData *data)
{
    XAsyncWork work;
    HRESULT hr;
    if (op == OP_BEGIN) return schedule_async(data->async, 0);
    if (op != OP_DOWORK) return S_OK;
    work = (XAsyncWork)data->context;
    hr = work ? work(data->async) : E_FAIL_;
    complete_async(data->async, hr, 0);
    return S_OK;
}

static HRESULT WINAPI thr_Run(void *self, XAsyncBlock *async, XAsyncWork work)
{
    (void)self;
    if (!async || !work) return E_INVALIDARG_;
    return begin_async_ex(async, (void *)work, (const void *)run_provider, "XAsyncRun", run_provider, 0);
}

static HRESULT WINAPI thr_Begin(void *self, XAsyncBlock *async, void *context, const void *identity,
                                const char *identityName, XAsyncProvider provider)
{
    (void)self;
    log_once(identityName ? identityName : "XAsyncBegin");
    return begin_async_ex(async, context, identity, identityName, provider, 0);
}

static HRESULT WINAPI thr_Pad(void *self)
{
    (void)self;
    log_once("threading padding");
    return E_NOTIMPL_;
}

static HRESULT WINAPI thr_Schedule(void *self, XAsyncBlock *async, UINT32 delay)
{
    (void)self;
    return schedule_async(async, delay);
}

static void WINAPI thr_Complete(void *self, XAsyncBlock *async, HRESULT result, SIZE_T required)
{
    (void)self;
    complete_async(async, result, required);
}

static HRESULT WINAPI thr_GetResult(void *self, XAsyncBlock *async, const void *identity,
                                    SIZE_T bufferSize, void *buffer, SIZE_T *bufferUsed)
{
    async_state *st;
    HRESULT hr;
    (void)self;
    if (bufferUsed) *bufferUsed = 0;
    if (!async || !g_lock_ready) return E_INVALIDARG_;
    EnterCriticalSection(&g_lock);
    st = state_of(async);
    if (!st) {
        LeaveCriticalSection(&g_lock);
        return read_done(async, &hr, NULL) ? hr : E_PENDING_;
    }
    if (!st->complete) {
        LeaveCriticalSection(&g_lock);
        return E_PENDING_;
    }
    if (FAILED(st->result)) {
        hr = st->result;
        LeaveCriticalSection(&g_lock);
        return hr;
    }
    if (identity && st->identity && identity != st->identity) {
        LeaveCriticalSection(&g_lock);
        return E_INVALIDARG_;
    }
    if (bufferSize < st->required || (st->required && !buffer)) {
        LeaveCriticalSection(&g_lock);
        return E_INSUFFICIENT_;
    }
    st->refs++;
    LeaveCriticalSection(&g_lock);
    EXT(st)->pdata.bufferSize = st->required;
    EXT(st)->pdata.buffer = buffer;
    hr = st->provider ? st->provider(OP_GETRESULT, &EXT(st)->pdata) : S_OK;
    EXT(st)->pdata.buffer = NULL;
    if (bufferUsed && SUCCEEDED(hr)) *bufferUsed = st->required;
    finish_result(st);
    state_release(st);
    return hr;
}

static HRESULT WINAPI thr_QueueCreate(void *self, UINT32 workMode, UINT32 compMode, queue_obj **out)
{
    (void)self;
    if (!out) return E_POINTER_;
    if (workMode > MODE_IMMEDIATE || compMode > MODE_IMMEDIATE) return E_INVALIDARG_;
    *out = queue_new((int)workMode, (int)compMode);
    xlog("QueueCreate %p work=%lu comp=%lu", (void *)*out, (unsigned long)workMode, (unsigned long)compMode);
    return *out ? S_OK : E_FAIL_;
}

static HRESULT WINAPI thr_QueueCreateComposite(void *self, port_obj *work, port_obj *comp, queue_obj **out)
{
    queue_obj *q;
    (void)self;
    log_once("QueueCreateComposite");
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!work || !comp || !g_lock_ready) return E_INVALIDARG_;
    q = calloc(1, sizeof(*q));
    if (!q) return E_OUTOFMEMORY_;
    EnterCriticalSection(&g_lock);
    if (!queue_live(work->q) || !queue_live(comp->q)) {
        LeaveCriticalSection(&g_lock);
        free(q);
        return E_INVALIDARG_;
    }
    q->work_which = work->which;
    q->comp_which = comp->which;
    q->work_delegate = target_queue(work->q, &q->work_which);
    q->comp_delegate = target_queue(comp->q, &q->comp_which);
    q->work_delegate->refs++;
    q->comp_delegate->refs++;
    LeaveCriticalSection(&g_lock);
    q->refs = 1;
    q->composite = 1;
    q->work_port.q = q;
    q->work_port.which = PORT_WORK;
    q->comp_port.q = q;
    q->comp_port.which = PORT_COMP;
    InitializeCriticalSection(&q->lock);
    q->magic = QUEUE_MAGIC;
    *out = q;
    return S_OK;
}

static HRESULT WINAPI thr_GetPort(void *self, queue_obj *q, UINT32 port, port_obj **out)
{
    (void)self;
    if (!out) return E_POINTER_;
    if (!q || port > PORT_COMP) return E_INVALIDARG_;
    *out = (port == PORT_COMP) ? &q->comp_port : &q->work_port;
    return S_OK;
}

static HRESULT WINAPI thr_Duplicate(void *self, queue_obj *q, queue_obj **out)
{
    (void)self;
    if (!out) return E_POINTER_;
    if (!queue_acquire(q)) return E_INVALIDARG_;
    *out = q;
    return S_OK;
}

static void WINAPI thr_Close(void *self, queue_obj *q)
{
    (void)self;
    queue_release(q);
}

static BOOLEAN WINAPI thr_Dispatch(void *self, queue_obj *q, UINT32 port, UINT32 timeout)
{
    ULONGLONG start;
    BOOLEAN hit = FALSE;
    static LONG disp_logs;
    (void)self;
    if (port > PORT_COMP) return FALSE;
    q = queue_or_process(q);
    if (!q) return FALSE;
    {
        int wp = PORT_WORK, cp = PORT_COMP;
        EnterCriticalSection(&g_lock);
        q->dispatched = 1;
        target_queue(q, &wp)->dispatched = 1;
        target_queue(q, &cp)->dispatched = 1;
        LeaveCriticalSection(&g_lock);
    }
    if (timeout != 0 && InterlockedIncrement(&disp_logs) <= 40)
        xlog("Dispatch q=%p port=%lu timeout=%lu", (void *)q, (unsigned long)port, (unsigned long)timeout);
    start = GetTickCount64();
    for (;;) {
        queue_obj *t;
        DWORD slice = 15;
        if (dispatch_one(q, (int)port)) { hit = TRUE; break; }
        if (timeout == 0 || q->terminated) break;
        if (timeout != 0xFFFFFFFFu) {
            ULONGLONG spent = GetTickCount64() - start;
            if (spent >= timeout) break;
            if (timeout - spent < slice) slice = (DWORD)(timeout - spent);
        }
        {
            int p = (int)port;
            t = target_queue(q, &p);
            WaitForSingleObject(p == PORT_COMP ? t->comp_event : t->work_event, slice);
        }
    }
    queue_release(q);
    return hit;
}

static HRESULT submit(queue_obj *q, UINT32 port, UINT32 delay, void *context, XTaskQueueCallback cb)
{
    HRESULT hr;
    int closing;
    if (!cb || port > PORT_COMP) return E_INVALIDARG_;
    q = queue_or_process(q);
    if (!q) return E_INVALIDARG_;
    closing = queue_closing(q);
    if (closing) {
        hr = E_ABORT_;
    } else if (delay == 0 && mode_of(q, (int)port) == MODE_IMMEDIATE) {
        cb(context, FALSE);
        hr = S_OK;
    } else {
        hr = enqueue(q, (int)port, item_new(ITEM_CALLBACK, NULL, cb, context, delay));
    }
    queue_release(q);
    return hr;
}

static HRESULT WINAPI thr_Submit(void *self, queue_obj *q, UINT32 port, void *context, XTaskQueueCallback cb)
{
    (void)self;
    return submit(q, port, 0, context, cb);
}

static HRESULT WINAPI thr_SubmitDelayed(void *self, queue_obj *q, UINT32 port, UINT32 delay, void *context, XTaskQueueCallback cb)
{
    (void)self;
    return submit(q, port, delay, context, cb);
}

static HRESULT WINAPI thr_RegWaiter(void *self, queue_obj *q, UINT32 port, HANDLE h, void *ctx, XTaskQueueCallback cb, void *token)
{
    (void)self; (void)q; (void)port; (void)h; (void)ctx; (void)cb; (void)token;
    log_once("XTaskQueueRegisterWaiter");
    return E_NOTIMPL_;
}
static void WINAPI thr_UnregWaiter(void *self, queue_obj *q, UINT64 token)
{
    (void)self; (void)q; (void)token;
}

static void __stdcall term_marker(void *ctx, BOOLEAN canceled)
{
    struct term_note *n = ctx;
    (void)canceled;
    if (n->cb) n->cb(n->ctx);
    free(n);
}

static void deliver_termination(queue_obj *q, struct term_note *n)
{
    if (FAILED(enqueue(q, PORT_COMP, item_new(ITEM_CALLBACK, NULL, term_marker, n, 0)))) {
        if (n->cb) n->cb(n->ctx);
        free(n);
    }
}

static void queue_resume_termination(queue_obj *q)
{
    struct term_note *list = NULL, *n;
    EnterCriticalSection(&g_lock);
    if (--q->suspends == 0) {
        list = q->pending_terms;
        q->pending_terms = NULL;
    }
    LeaveCriticalSection(&g_lock);
    while ((n = list)) {
        list = n->next;
        deliver_termination(q, n);
    }
}

static void promote_items(queue_obj *q)
{
    int port;
    for (port = PORT_WORK; port <= PORT_COMP; port++) {
        int p = port;
        queue_obj *t = target_queue(q, &p);
        task_item *it;
        EnterCriticalSection(&t->lock);
        for (it = p == PORT_WORK ? t->work_head : t->comp_head; it; it = it->next)
            if (it->via == q) it->ready = 0;
        SetEvent(p == PORT_WORK ? t->work_event : t->comp_event);
        LeaveCriticalSection(&t->lock);
        if ((p == PORT_WORK ? t->work_mode : t->comp_mode) == MODE_MANUAL) fire_monitors(t, (unsigned)p);
    }
}

static HRESULT WINAPI thr_Terminate(void *self, queue_obj *q, BOOLEAN wait, void *ctx, void (__stdcall *cb)(void *))
{
    struct term_note *n;
    int deferred = 0;
    (void)self;
    xlog("XTaskQueueTerminate q=%p wait=%u", (void *)q, (unsigned)wait);
    if (!q || !queue_acquire(q)) {
        if (cb) cb(ctx);
        return S_OK;
    }
    if (wait) {
        ULONGLONG start = GetTickCount64();
        EnterCriticalSection(&g_lock);
        q->closing = 1;
        LeaveCriticalSection(&g_lock);
        promote_items(q);
        for (;;) {
            LONG busy;
            if (dispatch_one(q, PORT_WORK) || dispatch_one(q, PORT_COMP)) continue;
            EnterCriticalSection(&g_lock);
            busy = q->suspends;
            LeaveCriticalSection(&g_lock);
            if (!busy) break;
            if (GetTickCount64() - start > WAIT_GIVEUP_MS) {
                xlog("XTaskQueueTerminate gave up waiting for %ld calls", (long)busy);
                break;
            }
            Sleep(1);
        }
        if (cb) cb(ctx);
    } else {
        n = calloc(1, sizeof(*n));
        if (!n) {
            if (cb) cb(ctx);
        } else {
            n->cb = cb;
            n->ctx = ctx;
            EnterCriticalSection(&g_lock);
            q->closing = 1;
            if (q->suspends > 0) {
                n->next = q->pending_terms;
                q->pending_terms = n;
                deferred = 1;
            }
            LeaveCriticalSection(&g_lock);
            promote_items(q);
            if (!deferred) deliver_termination(q, n);
        }
    }
    queue_release(q);
    return S_OK;
}

static HRESULT WINAPI thr_RegMon(void *self, queue_obj *q, void *ctx, void *cb, UINT64 *token)
{
    static UINT64 next_token = 1;
    HRESULT hr = S_OK;
    (void)self;
    xlog("XTaskQueueRegisterMonitor q=%p", (void *)q);
    if (!cb || !g_lock_ready) return E_INVALIDARG_;
    q = queue_or_process(q);
    if (!q) return E_INVALIDARG_;
    EnterCriticalSection(&g_lock);
    if (q->nmon >= 4) {
        hr = E_FAIL_;
    } else {
        q->mon_ctx[q->nmon] = ctx;
        q->mon_cb[q->nmon] = cb;
        q->mon_token[q->nmon] = next_token++;
        if (token) *token = q->mon_token[q->nmon];
        q->nmon++;
    }
    LeaveCriticalSection(&g_lock);
    queue_release(q);
    return hr;
}

static void WINAPI thr_UnregMon(void *self, queue_obj *q, UINT64 token)
{
    int i;
    (void)self;
    q = queue_or_process(q);
    if (!q) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < q->nmon; i++) {
        if (q->mon_token[i] != token) continue;
        q->nmon--;
        q->mon_ctx[i] = q->mon_ctx[q->nmon];
        q->mon_cb[i] = q->mon_cb[q->nmon];
        q->mon_token[i] = q->mon_token[q->nmon];
        break;
    }
    LeaveCriticalSection(&g_lock);
    queue_release(q);
}

static BOOLEAN WINAPI thr_GetProcessQueue(void *self, queue_obj **out)
{
    (void)self;
    if (!out) return FALSE;
    *out = queue_or_process(NULL);
    return *out ? TRUE : FALSE;
}

static void WINAPI thr_SetProcessQueue(void *self, queue_obj *q)
{
    queue_obj *old;
    (void)self;
    log_once("SetProcessQueue");
    if (!q || !queue_acquire(q)) return;
    EnterCriticalSection(&g_lock);
    old = g_process_queue;
    g_process_queue = q;
    LeaveCriticalSection(&g_lock);
    queue_release(old);
}

static HRESULT WINAPI thr_SetTimeSens(void *self, BOOLEAN on)
{
    (void)self;
    if (g_tls != TLS_OUT_OF_INDEXES) TlsSetValue(g_tls, (void *)(uintptr_t)(on ? 1 : 0));
    return S_OK;
}
static HRESULT WINAPI thr_Pad2(void *self) { (void)self; return E_NOTIMPL_; }
static void WINAPI thr_AssertNot(void *self) { (void)self; }
static BOOLEAN WINAPI thr_IsTimeSens(void *self)
{
    (void)self;
    if (g_tls == TLS_OUT_OF_INDEXES) return FALSE;
    return TlsGetValue(g_tls) ? TRUE : FALSE;
}

static void *threading_vtbl[] = {
    gen_qi, gen_addref, gen_release,
    thr_GetStatus, thr_GetResultSize, thr_Cancel, thr_Run, thr_Begin, thr_Pad,
    thr_Schedule, thr_Complete, thr_GetResult,
    thr_QueueCreate, thr_QueueCreateComposite, thr_GetPort, thr_Duplicate, thr_Dispatch, thr_Close,
    thr_Submit, thr_SubmitDelayed, thr_RegWaiter, thr_UnregWaiter, thr_Terminate,
    thr_RegMon, thr_UnregMon, thr_GetProcessQueue, thr_SetProcessQueue,
    thr_SetTimeSens, thr_Pad2, thr_AssertNot, thr_IsTimeSens
};
static com_obj threading_obj = { threading_vtbl };

static HRESULT WINAPI threading_qi(com_obj *self, const GUID *iid, void **out)
{
    const GUID *ok[] = { &IID_Threading };
    return gen_qi(self, iid, out, ok, 1);
}

static HRESULT copy_text(const char *src, SIZE_T cap, char *dst, SIZE_T *used)
{
    SIZE_T n = strlen(src) + 1;
    if (used) *used = n;
    if (!dst || cap < n) return E_INSUFFICIENT_;
    memcpy(dst, src, n);
    return S_OK;
}

#define DEFINE_QI(fn, ...)                                                   \
    static HRESULT WINAPI fn(com_obj *self, const GUID *iid, void **out)     \
    {                                                                        \
        const GUID *ok[] = { __VA_ARGS__ };                                  \
        return gen_qi(self, iid, out, ok, (int)(sizeof ok / sizeof ok[0]));  \
    }

static BOOLEAN WINAPI feature_available(void *self, UINT32 feature)
{
    static const UINT32 supported[] = {
        2,  /* XAsync */
        3,  /* XAsyncProvider */
        5,  /* XGame */
        10, /* XNetworking */
        12, /* XPersistentLocalStorage */
        15, /* XSystem */
        16, /* XTaskQueue */
        17, /* XThread */
        18, /* XUser */
        19, /* XError */
    };
    static LONG64 reported;
    size_t i;
    BOOLEAN yes = FALSE;
    (void)self;
    for (i = 0; i < sizeof supported / sizeof supported[0]; i++)
        if (supported[i] == feature) yes = TRUE;
    if (feature < 64 && !(InterlockedOr64(&reported, (LONG64)(1ull << feature)) & (LONG64)(1ull << feature)))
        xlog("feature %lu available=%u", (unsigned long)feature, (unsigned)yes);
    return yes;
}
static void *feature_vtbl[] = { gen_qi, gen_addref, gen_release, feature_available };
static com_obj feature_obj = { feature_vtbl };
DEFINE_QI(feature_qi, &IID_Feature)

static HRESULT WINAPI game_get_title_id(void *self, UINT32 *title_id)
{
    (void)self;
    if (!title_id) return E_POINTER_;
    *title_id = TITLE_ID;
    return S_OK;
}
static void WINAPI game_launch_new(void *self, const char *exe, const char *args, void *user)
{
    (void)self; (void)user;
    xlog("XLaunchNewGame ignored: %s %s", exe ? exe : "", args ? args : "");
}
static HRESULT WINAPI game_restart_on_crash(void *self, const char *args, UINT32 reserved)
{
    (void)self; (void)args; (void)reserved;
    return S_OK;
}
static void *game_vtbl[] = { gen_qi, gen_addref, gen_release, game_get_title_id, game_launch_new, game_restart_on_crash };
static com_obj game_obj = { game_vtbl };
DEFINE_QI(game_qi, &IID_Game, &IID_Game2, &IID_Game3)

#define CONSOLE_ID "0000000000000001"
#define SANDBOX_ID "RETAIL"
#define APP_DEVICE_ID "d2d2d2d2-d2d2-4d2d-8d2d-d2d2d2d2d2d2"

static HRESULT copy_id(const char *src, INT32 cap, char *dst, SIZE_T *used)
{
    return copy_text(src, cap < 0 ? 0 : (SIZE_T)cap, dst, used);
}
static HRESULT WINAPI system_console_id(void *self, INT32 cap, char *dst, SIZE_T *used)
{
    (void)self;
    return copy_id(CONSOLE_ID, cap, dst, used);
}
static HRESULT WINAPI system_sandbox_id(void *self, INT32 cap, char *dst, SIZE_T *used)
{
    (void)self;
    return copy_id(SANDBOX_ID, cap, dst, used);
}
static HRESULT WINAPI system_device_id(void *self, INT32 cap, char *dst, SIZE_T *used)
{
    (void)self;
    return copy_id(APP_DEVICE_ID, cap, dst, used);
}
static HRESULT WINAPI system_handle_track(void *self, void *cb, void *ctx)
{
    (void)self; (void)cb; (void)ctx;
    return S_OK;
}
static BOOLEAN WINAPI system_handle_valid(void *self, void *handle)
{
    (void)self;
    return handle != NULL;
}
static void WINAPI system_set_bandwidth(void *self, BOOLEAN enable)
{
    (void)self; (void)enable;
}
static void *system_vtbl[] = {
    gen_qi, gen_addref, gen_release,
    system_console_id, system_sandbox_id, system_device_id,
    system_handle_track, system_handle_valid, system_set_bandwidth
};
static com_obj system_obj = { system_vtbl };
DEFINE_QI(system_qi, &IID_System, &IID_System2, &IID_System3, &IID_System4, &IID_System5)

typedef struct version_quad { UINT16 major, minor, build, revision; } version_quad;
typedef struct analytics_info {
    version_quad os;
    version_quad host;
    char family[64];
    char form[64];
} analytics_info;

static analytics_info *WINAPI analytics_get(void *self, analytics_info *out)
{
    static const analytics_info windows11 = { { 10, 0, 26100, 0 }, { 10, 0, 26100, 0 }, "Windows.Desktop", "Desktop" };
    (void)self;
    if (out) *out = windows11;
    return out;
}
static void *analytics_vtbl[] = { gen_qi, gen_addref, gen_release, analytics_get };
static com_obj analytics_obj = { analytics_vtbl };
DEFINE_QI(analytics_qi, &IID_Analytics)

#define PLS_PARENT "C:\\users\\steamuser\\AppData\\Local\\Dungeons2"
#define PLS_DIR PLS_PARENT "\\PLS"

static HRESULT WINAPI pls_path_size(void *self, SIZE_T *size)
{
    (void)self;
    if (!size) return E_POINTER_;
    *size = sizeof PLS_DIR;
    return S_OK;
}
static HRESULT WINAPI pls_path(void *self, SIZE_T cap, char *path, SIZE_T *used)
{
    (void)self;
    CreateDirectoryA(PLS_PARENT, NULL);
    CreateDirectoryA(PLS_DIR, NULL);
    return copy_text(PLS_DIR, cap, path, used);
}
static HRESULT WINAPI pls_space(void *self, UINT64 *info)
{
    (void)self;
    if (!info) return E_POINTER_;
    info[0] = 32ull << 30;  /* available */
    info[1] = 32ull << 30;  /* total quota */
    info[2] = 0;
    info[3] = 64ull << 30;
    return S_OK;
}
static HRESULT WINAPI pls_prompt_provider(UINT32 op, const XAsyncProviderData *data)
{
    if (op == OP_DOWORK) complete_async(data->async, S_OK, 0);
    return builtin_common(op, data, 0);
}
static HRESULT WINAPI pls_prompt(void *self, UINT64 bytes, XAsyncBlock *async)
{
    (void)self; (void)bytes;
    return begin_async(async, NULL, (const void *)pls_prompt_provider, "PLSPrompt", pls_prompt_provider);
}
static HRESULT WINAPI pls_prompt_result(void *self, XAsyncBlock *async)
{
    (void)self;
    return thr_GetStatus(NULL, async, FALSE);
}
static HRESULT WINAPI pls_mount(void *self, const char *package, void **mount)
{
    (void)self; (void)mount;
    xlog("PLS mount not supported: %s", package ? package : "");
    return E_NOTIMPL_;
}
static void *pls_vtbl[] = {
    gen_qi, gen_addref, gen_release,
    pls_path_size, pls_path, pls_space, pls_prompt, pls_prompt_result, pls_mount
};
static com_obj pls_obj = { pls_vtbl };
DEFINE_QI(pls_qi, &IID_PLS, &IID_PLS2, &IID_PLS3)

typedef void (__stdcall *error_callback)(HRESULT hr, const char *msg, void *ctx);
static error_callback g_err_cb;
static void *g_err_ctx;

static HRESULT WINAPI error_reserved(void *self) { (void)self; return E_NOTIMPL_; }
static void WINAPI error_set_callback(void *self, error_callback cb, void *ctx)
{
    (void)self;
    g_err_ctx = ctx;
    g_err_cb = cb;
}
static void WINAPI error_set_options(void *self, UINT32 debugger, UINT32 telemetry)
{
    (void)self;
    xlog("XErrorSetOptions %lu %lu", (unsigned long)debugger, (unsigned long)telemetry);
}
static void *error_vtbl[] = { gen_qi, gen_addref, gen_release, error_reserved, error_set_callback, error_set_options };
static com_obj error_obj = { error_vtbl };
DEFINE_QI(error_qi, &IID_Error)

#define SIGNIN_TIMEOUT_S 240
#define SIGNIN_POLL_MS 200

/* Tokens are "XBL3.0 x=<uhs>;<token>": xbox for *.xboxlive.com, mc for
 * minecraftservices.com, pf (with a device claim) for playfabapi.com. */
typedef struct auth_data {
    char gamertag[96];
    char xbox[12000];
    char mc[12000];
    char pf[12000];
    char msa[8000];
    unsigned long long xuid;
    long long exp;
} auth_data;

static SRWLOCK g_auth_lock = SRWLOCK_INIT;
static auth_data g_auth;
static FILETIME g_auth_stamp;
static ULONGLONG g_auth_size;
static int g_auth_valid;
static CRITICAL_SECTION g_signin_lock;

static WCHAR g_token_w[MAX_PATH + 32];
static WCHAR g_code_w[MAX_PATH + 32];
static WCHAR g_err_w[MAX_PATH + 32];
static WCHAR g_helper_w[MAX_PATH + 32];
static INIT_ONCE g_paths_once = INIT_ONCE_STATIC_INIT;

/* signin.py, embedded at build time and NUL-terminated. */
__asm__(".section .rdata,\"dr\"\n"
        ".global signin_py\n"
        "signin_py:\n"
        ".incbin \"src/signin.py\"\n"
        ".byte 0\n"
        ".text\n");
extern const char signin_py[];

static BOOL CALLBACK init_paths(INIT_ONCE *once, void *param, void **ctx)
{
    const WCHAR *dir = state_dir();
    (void)once; (void)param; (void)ctx;
    _snwprintf(g_token_w, MAX_PATH + 31, L"%ls\\tokens.txt", dir);
    _snwprintf(g_code_w, MAX_PATH + 31, L"%ls\\login-code.txt", dir);
    _snwprintf(g_err_w, MAX_PATH + 31, L"%ls\\login-error.txt", dir);
    _snwprintf(g_helper_w, MAX_PATH + 31, L"%ls\\signin.py", dir);
    return TRUE;
}

static int paths_ready(void)
{
    InitOnceExecuteOnce(&g_paths_once, init_paths, NULL, NULL);
    return g_token_w[0] != 0;
}

/* Linux path for a Windows path, from Wine. Free with HeapFree; NULL outside Wine. */
static char *unix_path(const WCHAR *path)
{
    typedef char *(CDECL *unix_name_fn)(const WCHAR *);
    unix_name_fn fn = (unix_name_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    return fn ? fn(path) : NULL;
}

/* Writes the embedded signin.py to the state folder unless an identical copy exists. */
static int write_helper(void)
{
    size_t len = strlen(signin_py);
    char *have;
    FILE *f;
    int same = 0;
    have = malloc(len + 1);
    if (have && (f = _wfopen(g_helper_w, L"rb"))) {
        same = fread(have, 1, len + 1, f) == len && !memcmp(have, signin_py, len);
        fclose(f);
    }
    free(have);
    if (same) return 1;
    f = _wfopen(g_helper_w, L"wb");
    if (!f) return 0;
    same = fwrite(signin_py, 1, len, f) == len;
    return fclose(f) == 0 && same;
}

static void auth_apply_line(auth_data *a, char *line)
{
    char *eq = strchr(line, '=');
    char *val;
    if (!eq) return;
    *eq = 0;
    val = eq + 1;
    if (!strcmp(line, "exp")) a->exp = atoll(val);
    else if (!strcmp(line, "xuid")) a->xuid = strtoull(val, NULL, 10);
    else if (!strcmp(line, "gamertag")) snprintf(a->gamertag, sizeof a->gamertag, "%s", val);
    else if (!strcmp(line, "xbox")) snprintf(a->xbox, sizeof a->xbox, "%s", val);
    else if (!strcmp(line, "mc")) snprintf(a->mc, sizeof a->mc, "%s", val);
    else if (!strcmp(line, "pf")) snprintf(a->pf, sizeof a->pf, "%s", val);
    else if (!strcmp(line, "msa")) snprintf(a->msa, sizeof a->msa, "%s", val);
}

/* Reloads tokens.txt if it changed. Returns 1 if the tokens are valid for 30 more seconds. */
static int auth_load(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    ULONGLONG size;
    auth_data *a;
    FILE *f;
    char *line;
    int valid;

    if (!paths_ready()) return 0;
    if (!GetFileAttributesExW(g_token_w, GetFileExInfoStandard, &fa)) {
        AcquireSRWLockExclusive(&g_auth_lock);
        if (g_auth_valid) SecureZeroMemory(&g_auth, sizeof g_auth);
        g_auth_valid = 0;
        memset(&g_auth_stamp, 0, sizeof g_auth_stamp);
        ReleaseSRWLockExclusive(&g_auth_lock);
        log_once("token file missing");
        return 0;
    }
    size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    AcquireSRWLockShared(&g_auth_lock);
    if (CompareFileTime(&fa.ftLastWriteTime, &g_auth_stamp) == 0 && size == g_auth_size) {
        valid = g_auth_valid && g_auth.exp > (long long)time(NULL) + 30;
        ReleaseSRWLockShared(&g_auth_lock);
        return valid;
    }
    ReleaseSRWLockShared(&g_auth_lock);

    a = calloc(1, sizeof(*a));
    line = malloc(16384);
    f = (a && line) ? _wfopen(g_token_w, L"r") : NULL;
    if (f) {
        while (fgets(line, 16384, f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            auth_apply_line(a, line);
        }
        fclose(f);
    }
    if (line) free_secret(line, 16384);
    if (!f) {
        if (a) free_secret(a, sizeof(*a));
        return 0;
    }
    AcquireSRWLockExclusive(&g_auth_lock);
    g_auth = *a;
    g_auth_valid = g_auth.xbox[0] != 0;
    g_auth_stamp = fa.ftLastWriteTime;
    g_auth_size = size;
    valid = g_auth_valid && g_auth.exp > (long long)time(NULL) + 30;
    ReleaseSRWLockExclusive(&g_auth_lock);
    free_secret(a, sizeof(*a));
    log_once(valid ? "token file loaded" : "token file expired or incomplete");
    return valid;
}

static DWORD WINAPI signin_prompt(void *unused)
{
    FILE *f;
    char url[256], code[64], msg[400];
    (void)unused;
    url[0] = code[0] = 0;
    f = _wfopen(g_code_w, L"r");
    if (f) {
        if (!fgets(url, sizeof url, f)) url[0] = 0;
        if (!fgets(code, sizeof code, f)) code[0] = 0;
        fclose(f);
    }
    snprintf(msg, sizeof msg, "Sign in with your Microsoft account.\n\n%s\nCode: %s", url, code);
    MessageBoxA(NULL, msg, "Minecraft Dungeons II sign-in", MB_OK | MB_SETFOREGROUND);
    return 0;
}

/* Runs signin.py as a Linux process through start.exe /unix, passing the state
 * folder as a Linux path. DUNGEONS2FORLINUX_SIGNIN (a Linux path) runs another script. */
static int start_helper(STARTUPINFOW *si, PROCESS_INFORMATION *pi)
{
    WCHAR cmd[2048], script[1024], dir_w[1024];
    char *dir_u, *script_u = NULL;
    int ok = 0;
    DWORD n = GetEnvironmentVariableW(L"DUNGEONS2FORLINUX_SIGNIN", script, 1024);
    if (n == 0 || n >= 1024) {
        if (!write_helper()) {
            xlog("cannot write the sign-in helper");
            return 0;
        }
        script_u = unix_path(g_helper_w);
        if (!script_u || !MultiByteToWideChar(CP_UTF8, 0, script_u, -1, script, 1024)) script[0] = 0;
    }
    dir_u = unix_path(state_dir());
    if (script[0] && dir_u && MultiByteToWideChar(CP_UTF8, 0, dir_u, -1, dir_w, 1024) &&
        !wcschr(script, L'"') && !wcschr(dir_w, L'"')) {
        int len = _snwprintf(cmd, 2047, L"C:\\windows\\system32\\start.exe /unix /usr/bin/python3 \"%ls\" --state \"%ls\"",
                             script, dir_w);
        cmd[2047] = 0;
        if (len < 0 || len >= 2047) {
            xlog("sign-in helper paths are too long");
        } else {
            ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, si, pi);
            if (!ok) xlog("sign-in helper failed to start: %lu", (unsigned long)GetLastError());
        }
    } else {
        xlog("cannot map the sign-in helper to a Linux path (not running under Wine?)");
    }
    if (script_u) HeapFree(GetProcessHeap(), 0, script_u);
    if (dir_u) HeapFree(GetProcessHeap(), 0, dir_u);
    return ok;
}

/* Returns 1 once valid tokens exist, running the helper and waiting for
 * tokens.txt or login-error.txt. One thread signs in at a time. */
static int auth_ensure(void)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ULONGLONG deadline;
    int prompted = 0, ok = 0, timed_out = 0;

    if (auth_load()) return 1;
    if (!paths_ready()) return 0;
    EnterCriticalSection(&g_signin_lock);
    if (auth_load()) {
        LeaveCriticalSection(&g_signin_lock);
        return 1;
    }
    DeleteFileW(g_err_w);
    DeleteFileW(g_code_w);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    xlog("starting Microsoft sign-in");
    if (!start_helper(&si, &pi)) {
        LeaveCriticalSection(&g_signin_lock);
        return 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    deadline = GetTickCount64() + SIGNIN_TIMEOUT_S * 1000ull;
    for (;;) {
        FILE *err;
        if (auth_load()) { ok = 1; break; }
        err = _wfopen(g_err_w, L"r");
        if (err) {
            char buf[300];
            if (!fgets(buf, sizeof buf, err)) buf[0] = 0;
            fclose(err);
            xlog("sign-in failed: %s", buf);
            break;
        }
        if (!prompted && GetFileAttributesW(g_code_w) != INVALID_FILE_ATTRIBUTES) {
            HANDLE t;
            prompted = 1;
            t = CreateThread(NULL, 0, signin_prompt, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
        if (GetTickCount64() >= deadline) {
            timed_out = 1;
            break;
        }
        Sleep(SIGNIN_POLL_MS);
    }
    if (timed_out) xlog("sign-in timed out");
    else if (ok) xlog("sign-in ok");
    LeaveCriticalSection(&g_signin_lock);
    return ok;
}

/* True if the URL's host is domain or a subdomain, ignoring case, user info and port. */
static int url_host_is(const char *url, const char *domain)
{
    const char *host, *end, *p;
    size_t n, d = strlen(domain);
    if (!url) return 0;
    host = url + strcspn(url, ":/?#");
    host = (host[0] == ':' && host[1] == '/' && host[2] == '/') ? host + 3 : url;
    end = host + strcspn(host, "/?#");
    for (p = host; p < end; p++) if (*p == '@') host = p + 1;
    for (p = host; p < end && *p != ':'; p++) ;
    n = (size_t)(p - host);
    if (n < d || _strnicmp(p - d, domain, d) != 0) return 0;
    return n == d || p[-(ptrdiff_t)d - 1] == '.';
}

/* Copy of the token for a URL's service; free with free_secret. */
static char *auth_token_copy(const char *url)
{
    const char *src;
    char *out;
    AcquireSRWLockShared(&g_auth_lock);
    src = g_auth.xbox;
    if (url_host_is(url, "playfabapi.com") && g_auth.pf[0]) src = g_auth.pf;
    else if (url_host_is(url, "minecraftservices.com") && g_auth.mc[0]) src = g_auth.mc;
    out = dup_string(src);
    ReleaseSRWLockShared(&g_auth_lock);
    return out;
}

static char *auth_msa_copy(void)
{
    char *out;
    AcquireSRWLockShared(&g_auth_lock);
    out = dup_string(g_auth.msa);
    ReleaseSRWLockShared(&g_auth_lock);
    return out;
}

static unsigned long long auth_xuid(void)
{
    unsigned long long x;
    auth_load();
    AcquireSRWLockShared(&g_auth_lock);
    x = g_auth.xuid;
    ReleaseSRWLockShared(&g_auth_lock);
    return x;
}

static com_obj user_obj;
static com_obj gamertag_obj;

#define LOCAL_USER_ID 1ull

static async_state *completed_state(XAsyncBlock *async, HRESULT *hr)
{
    async_state *st = state_acquire(async);
    if (!st) {
        if (!read_done(async, hr, NULL)) *hr = E_PENDING_;
        else if (SUCCEEDED(*hr)) *hr = E_INVALIDARG_;
        return NULL;
    }
    if (!st->complete || FAILED(st->result)) {
        *hr = st->complete ? st->result : E_PENDING_;
        state_release(st);
        return NULL;
    }
    *hr = S_OK;
    return st;
}

static HRESULT WINAPI user_dup(void *self, void *user, void **out)
{
    (void)self;
    if (!out) return E_POINTER_;
    *out = user ? user : (void *)&user_obj;
    return S_OK;
}
static void WINAPI user_close(void *self, void *user) { (void)self; (void)user; }
static INT32 WINAPI user_cmp(void *self, void *a, void *b)
{
    (void)self;
    if (a == b) return 0;
    return (a < b) ? -1 : 1;
}
static HRESULT WINAPI user_max(void *self, UINT32 *max)
{
    (void)self;
    if (!max) return E_POINTER_;
    *max = 4;
    return S_OK;
}

/* Completes at Begin when tokens are cached; otherwise signs in on the work queue. */
static HRESULT WINAPI user_add_provider(UINT32 op, const XAsyncProviderData *data)
{
    if (op == OP_BEGIN && auth_load()) {
        complete_async(data->async, S_OK, sizeof(void *));
        return S_OK;
    }
    if (op == OP_DOWORK) {
        if (auth_ensure()) complete_async(data->async, S_OK, sizeof(void *));
        else complete_async(data->async, E_FAIL_, 0);
    }
    else if (op == OP_GETRESULT && data->buffer && data->bufferSize >= sizeof(void *))
        *(void **)data->buffer = &user_obj;
    return builtin_common(op, data, 0);
}
static HRESULT WINAPI user_add_async(void *self, UINT32 options, XAsyncBlock *async)
{
    (void)self;
    xlog("XUserAddAsync opts=%lu", (unsigned long)options);
    return begin_async(async, NULL, (const void *)user_add_provider, "XUserAdd", user_add_provider);
}
static HRESULT WINAPI user_add_result(void *self, XAsyncBlock *async, void **newUser)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    if (!newUser) return E_POINTER_;
    st = completed_state(async, &hr);
    if (!st) return hr;
    *newUser = &user_obj;
    finish_result(st);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI user_local_id(void *self, void *user, UINT64 *id)
{
    (void)self; (void)user;
    if (!id) return E_POINTER_;
    *id = LOCAL_USER_ID;
    return S_OK;
}
static HRESULT WINAPI user_find_local(void *self, UINT64 id, void **handle)
{
    (void)self;
    if (!handle) return E_POINTER_;
    if (id != LOCAL_USER_ID) return E_FAIL_;
    *handle = &user_obj;
    return S_OK;
}
static HRESULT WINAPI user_get_id(void *self, void *user, UINT64 *id)
{
    unsigned long long x;
    (void)self; (void)user;
    if (!id) return E_POINTER_;
    x = auth_xuid();
    *id = x ? x : LOCAL_USER_ID;
    log_once("XUserGetId");
    return S_OK;
}
static HRESULT WINAPI user_find_id(void *self, UINT64 id, void **handle)
{
    (void)self;
    if (!handle) return E_POINTER_;
    if (id != LOCAL_USER_ID && id != auth_xuid()) {
        xlog("XUserFindUserById miss %llu", (unsigned long long)id);
        return E_FAIL_;
    }
    *handle = &user_obj;
    return S_OK;
}
static HRESULT WINAPI user_guest(void *self, void *user, BOOLEAN *guest)
{
    (void)self; (void)user;
    if (!guest) return E_POINTER_;
    *guest = FALSE;
    return S_OK;
}
static HRESULT WINAPI user_state(void *self, void *user, UINT32 *state)
{
    (void)self; (void)user;
    if (!state) return E_POINTER_;
    *state = 0;  /* SignedIn */
    return S_OK;
}
static HRESULT WINAPI user_pic_async(void *self, void *user, UINT32 size, XAsyncBlock *async)
{
    (void)self; (void)user; (void)size; (void)async;
    log_once("XUserGetGamerPictureAsync");
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_pic_size(void *self, XAsyncBlock *async, SIZE_T *sz)
{
    (void)self; (void)async; (void)sz;
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_pic_result(void *self, XAsyncBlock *async, SIZE_T sz, void *buf, SIZE_T *used)
{
    (void)self; (void)async; (void)sz; (void)buf; (void)used;
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_age(void *self, void *user, UINT32 *age)
{
    (void)self; (void)user;
    if (!age) return E_POINTER_;
    *age = 3;  /* Adult */
    return S_OK;
}
static HRESULT WINAPI user_priv(void *self, void *user, UINT32 opts, UINT32 priv, BOOLEAN *has, UINT32 *reason)
{
    (void)self; (void)user; (void)opts; (void)priv;
    if (has) *has = TRUE;
    if (reason) *reason = 0;
    return S_OK;
}
static HRESULT WINAPI user_resolve_priv_async(void *self, void *user, UINT32 opts, UINT32 priv, XAsyncBlock *async)
{
    (void)self; (void)user; (void)opts; (void)priv; (void)async;
    log_once("XUserResolvePrivilegeWithUiAsync");
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_resolve_priv_result(void *self, XAsyncBlock *async)
{
    (void)self; (void)async;
    return E_NOTIMPL_;
}

/* XUserGetTokenAndSignature result: this header, the token, then an empty signature. */
typedef struct token_blob {
    SIZE_T tokenSize;
    SIZE_T signatureSize;
    const char *token;
    const char *signature;
} token_blob;

static SIZE_T token_blob_size(const async_state *st)
{
    return sizeof(token_blob) + st->payload_len + 1;
}

static void token_blob_fill(const async_state *st, void *buf)
{
    token_blob *blob = buf;
    char *dst = (char *)(blob + 1);
    memcpy(dst, st->payload, st->payload_len);
    dst[st->payload_len] = 0;
    blob->tokenSize = st->payload_len;
    blob->signatureSize = 1;
    blob->token = dst;
    blob->signature = dst + st->payload_len;
}

/* Answers at Begin when tokens are cached; otherwise signs in on the work queue. */
static HRESULT WINAPI token_provider(UINT32 op, const XAsyncProviderData *data)
{
    if (op == OP_BEGIN || op == OP_DOWORK) {
        char *tok;
        if (op == OP_BEGIN && !auth_load()) return builtin_common(op, data, 1);
        if (!auth_ensure()) {
            complete_async(data->async, E_FAIL_, 0);
            return S_OK;
        }
        tok = auth_token_copy((const char *)data->context);
        if (!tok) {
            complete_async(data->async, E_OUTOFMEMORY_, 0);
            return S_OK;
        }
        complete_with_payload(data->async, tok, 1, sizeof(token_blob) + 1);
        free_secret(tok, strlen(tok) + 1);
        return S_OK;
    } else if (op == OP_GETRESULT && data->buffer) {
        async_state *st = state_acquire(data->async);
        if (st && st->payload && data->bufferSize >= token_blob_size(st)) token_blob_fill(st, data->buffer);
        state_release(st);
    }
    return builtin_common(op, data, 1);
}

static HRESULT start_token_async(XAsyncBlock *async, const char *url, const char *name)
{
    char *ctx = NULL;
    HRESULT hr;
    if (url && url[0] && !(ctx = dup_string(url))) return E_OUTOFMEMORY_;
    hr = begin_async(async, ctx, (const void *)token_provider, name, token_provider);
    return hr;
}

static HRESULT WINAPI user_token_async(void *self, void *user, UINT32 opts, const char *method, const char *url,
                                       SIZE_T headerCount, const void *headers, SIZE_T bodySize, const void *body, XAsyncBlock *async)
{
    (void)self; (void)user; (void)opts; (void)headerCount; (void)headers; (void)bodySize; (void)body;
    if (!url) url = "";
    xlog("token %s %.*s", method ? method : "?", (int)strcspn(url, "?#"), url);
    return start_token_async(async, url, "XUserToken");
}
static HRESULT WINAPI user_token_size(void *self, XAsyncBlock *async, SIZE_T *sz)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    if (!sz) return E_POINTER_;
    st = completed_state(async, &hr);
    if (!st) return hr;
    *sz = token_blob_size(st);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI user_token_result(void *self, XAsyncBlock *async, SIZE_T sz, void *buf, void *ptr, SIZE_T *used)
{
    HRESULT hr;
    async_state *st;
    SIZE_T need;
    (void)self;
    st = completed_state(async, &hr);
    if (!st) return hr;
    need = token_blob_size(st);
    if (used) *used = need;
    if (!buf || sz < need) {
        state_release(st);
        return E_INSUFFICIENT_;
    }
    token_blob_fill(st, buf);
    if (ptr) *(void **)ptr = buf;
    finish_result(st);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI user_token16_async(void *self, void *user, UINT32 opts, const WCHAR *method, const WCHAR *url,
                                         SIZE_T headerCount, const void *headers, SIZE_T bodySize, const void *body, XAsyncBlock *async)
{
    char url8[2048];
    (void)self; (void)user; (void)opts; (void)method; (void)headerCount; (void)headers; (void)bodySize; (void)body;
    url8[0] = 0;
    if (url && !WideCharToMultiByte(CP_UTF8, 0, url, -1, url8, sizeof url8, NULL, NULL)) url8[0] = 0;
    xlog_url("token utf16", url8);
    return start_token_async(async, url8, "XUserToken16");
}
static HRESULT WINAPI user_issue_async(void *self, void *user, const char *url, XAsyncBlock *async)
{
    (void)self; (void)user; (void)async;
    xlog_url("resolve issue", url);
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_issue_result(void *self, XAsyncBlock *async)
{
    (void)self; (void)async;
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_issue16_async(void *self, void *user, const WCHAR *url, XAsyncBlock *async)
{
    (void)self; (void)user; (void)url; (void)async;
    log_once("resolve issue utf16");
    return E_NOTIMPL_;
}

typedef void (__stdcall *user_change_fn)(void *context, UINT64 localId, UINT32 event);
typedef struct change_reg { user_change_fn cb; void *ctx; } change_reg;
static change_reg g_change;

static void __stdcall deliver_user_change(void *ctx, BOOLEAN canceled)
{
    change_reg *r = ctx;
    if (!canceled && r->cb) r->cb(r->ctx, LOCAL_USER_ID, 0 );
}
static HRESULT WINAPI user_reg_change(void *self, void *q, void *ctx, void *cb, UINT64 *token)
{
    (void)self;
    if (!cb) return E_INVALIDARG_;
    g_change.cb = (user_change_fn)cb;
    g_change.ctx = ctx;
    xlog("XUserRegisterForChangeEvent q=%p", q);
    if (token) *token = 1;
    return submit((queue_obj *)q, PORT_COMP, 0, &g_change, deliver_user_change);
}
static BOOLEAN WINAPI user_unreg_change(void *self, UINT64 token, BOOLEAN wait)
{
    (void)self; (void)token; (void)wait;
    g_change.cb = NULL;
    return TRUE;
}
static HRESULT WINAPI user_deferral(void *self, void **out)
{
    (void)self;
    if (!out) return E_POINTER_;
    *out = &user_obj;
    return S_OK;
}
static void WINAPI user_close_deferral(void *self, void *d) { (void)self; (void)d; }
static HRESULT WINAPI user_add_by_id(void *self, UINT64 id, XAsyncBlock *async)
{
    xlog("XUserAddByIdWithUiAsync id=%llu", (unsigned long long)id);
    return user_add_async(self, 0, async);
}
static HRESULT WINAPI user_add_by_id_result(void *self, XAsyncBlock *async, void **user)
{
    return user_add_result(self, async, user);
}

static HRESULT WINAPI msa_provider(UINT32 op, const XAsyncProviderData *data)
{
    if (op == OP_DOWORK) {
        char *tok;
        if (!auth_ensure()) {
            complete_async(data->async, E_FAIL_, 0);
            return S_OK;
        }
        tok = auth_msa_copy();
        if (!tok) {
            complete_async(data->async, E_OUTOFMEMORY_, 0);
            return S_OK;
        }
        complete_with_payload(data->async, tok, 1, 0);
        free_secret(tok, strlen(tok) + 1);
    } else if (op == OP_GETRESULT && data->buffer) {
        async_state *st = state_acquire(data->async);
        if (st && st->payload && data->bufferSize >= st->payload_len) memcpy(data->buffer, st->payload, st->payload_len);
        state_release(st);
    }
    return builtin_common(op, data, 0);
}
static HRESULT WINAPI user_msa_async(void *self, void *user, UINT32 opts, const char *scope, XAsyncBlock *async)
{
    (void)self; (void)user; (void)opts;
    xlog("msa token %s", scope ? scope : "");
    return begin_async(async, NULL, (const void *)msa_provider, "XUserMsa", msa_provider);
}
static HRESULT WINAPI user_msa_result(void *self, XAsyncBlock *async, SIZE_T cap, char *tok, SIZE_T *used)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    st = completed_state(async, &hr);
    if (!st) return hr;
    if (used) *used = st->payload_len;
    if (!tok || cap < st->payload_len) {
        state_release(st);
        return E_INSUFFICIENT_;
    }
    memcpy(tok, st->payload, st->payload_len);
    finish_result(st);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI user_msa_size(void *self, XAsyncBlock *async, SIZE_T *sz)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    if (!sz) return E_POINTER_;
    st = completed_state(async, &hr);
    if (!st) return hr;
    *sz = st->payload_len;
    state_release(st);
    return S_OK;
}
static BOOLEAN WINAPI user_is_store(void *self, void *user)
{
    (void)self; (void)user;
    return FALSE;
}
static HRESULT WINAPI user_remote_set(void *self, void *q, void *handlers)
{
    (void)self; (void)q; (void)handlers;
    log_once("remote connect handlers");
    return S_OK;
}
static HRESULT WINAPI user_remote_cancel(void *self, void *op)
{
    (void)self; (void)op;
    return S_OK;
}
static HRESULT WINAPI user_spop_set(void *self, void *q, void *handler, void *ctx)
{
    (void)self; (void)q; (void)handler; (void)ctx;
    log_once("spop handlers");
    return S_OK;
}
static HRESULT WINAPI user_spop_complete(void *self, void *op, UINT32 result)
{
    (void)self; (void)op; (void)result;
    return S_OK;
}
static BOOLEAN WINAPI user_signout_present(void *self)
{
    (void)self;
    return FALSE;
}
static HRESULT WINAPI user_signout_async(void *self, void *user, XAsyncBlock *async)
{
    (void)self; (void)user; (void)async;
    log_once("XUserSignOutAsync");
    return E_NOTIMPL_;
}
static HRESULT WINAPI user_signout_result(void *self, XAsyncBlock *async)
{
    (void)self; (void)async;
    return E_NOTIMPL_;
}

static void *user_vtbl[] = {
    gen_qi, gen_addref, gen_release,
    user_dup, user_close, user_cmp, user_max, user_add_async, user_add_result,
    user_local_id, user_find_local, user_get_id, user_find_id, user_guest, user_state,
    stub_notimpl,
    user_pic_async, user_pic_size, user_pic_result, user_age, user_priv,
    user_resolve_priv_async, user_resolve_priv_result,
    user_token_async, user_token_size, user_token_result,
    user_token16_async, user_token_size, user_token_result,
    user_issue_async, user_issue_result, user_issue16_async, user_issue_result,
    user_reg_change, user_unreg_change, user_deferral, user_close_deferral,
    user_add_by_id, user_add_by_id_result,
    user_msa_async, user_msa_result, user_msa_size,
    user_is_store,
    user_remote_set, user_remote_cancel, user_spop_set, user_spop_complete,
    user_signout_present, user_signout_async, user_signout_result
};
static HRESULT WINAPI user_qi(com_obj *self, const GUID *iid, void **out)
{
    const GUID *ok[] = { &IID_User, &IID_User2, &IID_User3, &IID_User4, &IID_User5, &IID_User6 };
    if (iid && guid_eq(iid, &IID_Gamertag)) {
        if (!out) return E_POINTER_;
        *out = &gamertag_obj;
        return S_OK;
    }
    return gen_qi(self, iid, out, ok, 6);
}

static HRESULT WINAPI tag_get(void *self, void *user, UINT32 component, SIZE_T cap, char *buf, SIZE_T *used)
{
    char tag[96];
    (void)self; (void)user;
    log_once("XUserGetGamertag");
    if (component == 2) return copy_text("", cap, buf, used);
    auth_load();
    AcquireSRWLockShared(&g_auth_lock);
    snprintf(tag, sizeof tag, "%s", g_auth.gamertag[0] ? g_auth.gamertag : "Player");
    ReleaseSRWLockShared(&g_auth_lock);
    return copy_text(tag, cap, buf, used);
}
static void *gamertag_vtbl[] = { gen_qi, gen_addref, gen_release, tag_get };
static HRESULT WINAPI gamertag_qi(com_obj *self, const GUID *iid, void **out)
{
    const GUID *ok[] = { &IID_Gamertag };
    if (iid && (guid_eq(iid, &IID_User) || guid_eq(iid, &IID_User6))) {
        if (!out) return E_POINTER_;
        *out = &user_obj;
        return S_OK;
    }
    return gen_qi(self, iid, out, ok, 1);
}

static HRESULT WINAPI proto_reg(void *self, void *queue, void *context, void *callback, UINT64 *token)
{
    (void)self; (void)queue; (void)context; (void)callback;
    log_once("XGameProtocolRegisterForActivation");
    if (token) *token = 1;
    return S_OK;
}
static BOOLEAN WINAPI proto_unreg(void *self, UINT64 token, BOOLEAN wait)
{
    (void)self; (void)token; (void)wait;
    return TRUE;
}
static HRESULT WINAPI net_port(void *self, UINT16 *port)
{
    (void)self;
    if (!port) return E_POINTER_;
    *port = 3074;
    return S_OK;
}
static HRESULT WINAPI net_port_async(void *self, XAsyncBlock *async)
{
    (void)self; (void)async;
    log_once("udp port async");
    return E_NOTIMPL_;
}
static HRESULT WINAPI net_port_result(void *self, XAsyncBlock *async, UINT16 *port)
{
    (void)self; (void)async;
    if (!port) return E_POINTER_;
    *port = 3074;
    return S_OK;
}
static HRESULT WINAPI net_reg_port(void *self, void *q, void *ctx, void *cb, UINT64 *token)
{
    (void)self; (void)q; (void)ctx; (void)cb;
    if (token) *token = 1;
    return S_OK;
}
static BOOLEAN WINAPI net_unreg_port(void *self, UINT64 token, BOOLEAN wait)
{
    (void)self; (void)token; (void)wait;
    return TRUE;
}

/* WinHTTP protocol bits, used by XCurl for WINHTTP_OPTION_SECURE_PROTOCOLS. */
#define WINHTTP_TLS12 0x800u
#define WINHTTP_TLS13 0x2000u
#define WINHTTP_OPT_PROTOCOLS 84u
#define WINHTTP_OPT_IPV6_FAST_FALLBACK 140u
#define WINHTTP_QUERY_STATUS_NUMBER 0x20000013u
#define HTTP_LOG_LIMIT 400

static BOOL (WINAPI *real_set_option)(void *, DWORD, void *, DWORD);
static void *(WINAPI *real_connect)(void *, const WCHAR *, unsigned short, DWORD);
static void *(WINAPI *real_open_request)(void *, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR **, DWORD);
static BOOL (WINAPI *real_send)(void *, const WCHAR *, DWORD, void *, DWORD, DWORD, DWORD_PTR);
static BOOL (WINAPI *real_recv)(void *, void *);
static BOOL (WINAPI *real_query)(void *, DWORD, const WCHAR *, void *, DWORD *, DWORD *);
static LONG g_http_logs;

static int http_log_ok(void)
{
    return InterlockedIncrement(&g_http_logs) <= HTTP_LOG_LIMIT;
}

/* Accepts the IPv6 fast-fallback option without passing it on, and replaces a
 * protocol mask without TLS 1.2 with TLS 1.2 and 1.3. */
static BOOL WINAPI hook_set_option(void *handle, DWORD option, void *buffer, DWORD length)
{
    DWORD fixed;
    if (option == WINHTTP_OPT_IPV6_FAST_FALLBACK)
        return TRUE;
    if (option == WINHTTP_OPT_PROTOCOLS && buffer && length >= sizeof(DWORD) &&
        (*(DWORD *)buffer & WINHTTP_TLS12) == 0) {
        fixed = WINHTTP_TLS12 | WINHTTP_TLS13;
        return real_set_option(handle, option, &fixed, sizeof fixed);
    }
    return real_set_option(handle, option, buffer, length);
}
static void *WINAPI hook_connect(void *session, const WCHAR *host, unsigned short port, DWORD reserved)
{
    char host8[256];
    host8[0] = 0;
    if (host && !WideCharToMultiByte(CP_UTF8, 0, host, -1, host8, sizeof host8, NULL, NULL)) host8[0] = 0;
    if (!strstr(host8, "events.data.microsoft.com") && http_log_ok())
        xlog("http connect %s:%u", host8, (unsigned)port);
    return real_connect(session, host, port, reserved);
}
static void *WINAPI hook_open_request(void *connect, const WCHAR *verb, const WCHAR *object, const WCHAR *version,
                                      const WCHAR *referrer, const WCHAR **accept, DWORD flags)
{
    char path[256];
    path[0] = 0;
    if (object && !WideCharToMultiByte(CP_UTF8, 0, object, -1, path, sizeof path, NULL, NULL)) path[0] = 0;
    path[strcspn(path, "?")] = 0;
    if (path[0] && !strstr(path, "OneCollector") && http_log_ok())
        xlog("http %s", path);
    return real_open_request(connect, verb, object, version, referrer, accept, flags);
}
static BOOL WINAPI hook_send(void *request, const WCHAR *headers, DWORD headers_len, void *optional,
                             DWORD optional_len, DWORD total, DWORD_PTR ctx)
{
    BOOL ok = real_send(request, headers, headers_len, optional, optional_len, total, ctx);
    if (!ok) xlog("http send failed %lu", (unsigned long)GetLastError());
    return ok;
}
static BOOL WINAPI hook_recv(void *request, void *reserved)
{
    DWORD code = 0, sz = sizeof code, err;
    BOOL ok = real_recv(request, reserved);
    if (!ok) {
        xlog("http receive failed %lu", (unsigned long)GetLastError());
        return ok;
    }
    err = GetLastError();
    if (real_query && real_query(request, WINHTTP_QUERY_STATUS_NUMBER, NULL, &code, &sz, NULL) && code >= 400)
        xlog("http status %lu", (unsigned long)code);
    SetLastError(err);
    return ok;
}

/* The import address table slot for dll!func in mod, looked up by name, or NULL. */
static void **find_import_slot(HMODULE mod, const char *dll, const char *func)
{
    unsigned char *base = (unsigned char *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD size;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return NULL;
    size = nt->OptionalHeader.SizeOfImage;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.VirtualAddress >= size) return NULL;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        IMAGE_THUNK_DATA64 *names, *slots;
        if (imp->Name >= size || _stricmp((const char *)base + imp->Name, dll) != 0) continue;
        if (imp->FirstThunk >= size) return NULL;
        names = (IMAGE_THUNK_DATA64 *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        slots = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            IMAGE_IMPORT_BY_NAME *by;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            if (names->u1.AddressOfData >= size) return NULL;
            by = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (!strcmp((const char *)by->Name, func)) return (void **)&slots->u1.Function;
        }
        return NULL;
    }
    return NULL;
}

static int patch_import(HMODULE mod, const char *func, void *hook, void **saved)
{
    void **slot = find_import_slot(mod, "winhttp.dll", func);
    DWORD old;
    if (!slot) {
        xlog("XCurl import %s not found; not hooked", func);
        return 0;
    }
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *saved = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static INIT_ONCE g_hook_once = INIT_ONCE_STATIC_INIT;

/* Hooks XCurl's WinHTTP imports. Returns FALSE until XCurl is loaded, so a later call retries. */
static BOOL CALLBACK install_hooks(INIT_ONCE *once, void *param, void **ctx)
{
    HMODULE mod = GetModuleHandleW(L"XCurl.dll");
    void **query;
    (void)once; (void)param; (void)ctx;
    if (!mod) {
        log_once("XCurl.dll not loaded yet; WinHTTP not hooked");
        return FALSE;
    }
    patch_import(mod, "WinHttpSetOption", (void *)hook_set_option, (void **)&real_set_option);
    patch_import(mod, "WinHttpConnect", (void *)hook_connect, (void **)&real_connect);
    patch_import(mod, "WinHttpOpenRequest", (void *)hook_open_request, (void **)&real_open_request);
    patch_import(mod, "WinHttpSendRequest", (void *)hook_send, (void **)&real_send);
    patch_import(mod, "WinHttpReceiveResponse", (void *)hook_recv, (void **)&real_recv);
    query = find_import_slot(mod, "winhttp.dll", "WinHttpQueryHeaders");
    if (query) real_query = *query;
    xlog("hooked XCurl WinHTTP");
    return TRUE;
}

typedef struct net_sec_info {
    UINT32 flags;
    SIZE_T count;
    void *prints;
} net_sec_info;

static void fill_sec(net_sec_info *out)
{
    out->flags = WINHTTP_TLS12 | WINHTTP_TLS13;
    out->count = 0;
    out->prints = NULL;
}
static HRESULT WINAPI sec_provider(UINT32 op, const XAsyncProviderData *data)
{
    if (op == OP_DOWORK) complete_async(data->async, S_OK, sizeof(net_sec_info));
    else if (op == OP_GETRESULT && data->buffer && data->bufferSize >= sizeof(net_sec_info))
        fill_sec(data->buffer);
    return builtin_common(op, data, 0);
}
static HRESULT start_sec_async(XAsyncBlock *async, const char *url)
{
    static LONG logs;
    if (InterlockedIncrement(&logs) <= HTTP_LOG_LIMIT) xlog_url("security for", url);
    InitOnceExecuteOnce(&g_hook_once, install_hooks, NULL, NULL);
    return begin_async(async, NULL, (const void *)sec_provider, "XNetSecurity", sec_provider);
}
static HRESULT WINAPI net_sec_async(void *self, const char *url, XAsyncBlock *async)
{
    (void)self;
    return start_sec_async(async, url);
}
static HRESULT WINAPI net_sec_size(void *self, XAsyncBlock *async, SIZE_T *n)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    if (!n) return E_POINTER_;
    st = completed_state(async, &hr);
    if (!st) return hr;
    *n = sizeof(net_sec_info);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI net_sec_result(void *self, XAsyncBlock *async, SIZE_T cap, SIZE_T *used, void *buf, void *info)
{
    HRESULT hr;
    async_state *st;
    (void)self;
    st = completed_state(async, &hr);
    if (!st) return hr;
    if (used) *used = sizeof(net_sec_info);
    if (!buf || cap < sizeof(net_sec_info)) {
        state_release(st);
        return E_INSUFFICIENT_;
    }
    fill_sec(buf);
    if (info) *(void **)info = buf;
    finish_result(st);
    state_release(st);
    return S_OK;
}
static HRESULT WINAPI net_sec16_async(void *self, const WCHAR *url, XAsyncBlock *async)
{
    char url8[2048];
    (void)self;
    url8[0] = 0;
    if (url && !WideCharToMultiByte(CP_UTF8, 0, url, -1, url8, sizeof url8, NULL, NULL)) url8[0] = 0;
    return start_sec_async(async, url8);
}
static HRESULT WINAPI net_verify(void *self, void *req, const void *info)
{
    (void)self; (void)req; (void)info;
    log_once("verify cert");
    return S_OK;
}

typedef struct connectivity_hint {
    UINT32 connectivity_level;  /* 3 = InternetAccess */
    UINT32 connectivity_cost;  /* 1 = Unrestricted */
    UINT32 interface_type;  /* 6 = ethernet */
    BOOLEAN network_initialized;
    unsigned char pad[3];
} connectivity_hint;
_Static_assert(sizeof(connectivity_hint) == 16, "connectivity hint is 16 bytes");

static void fill_hint(void *p)
{
    connectivity_hint h;
    memset(&h, 0, sizeof h);
    h.connectivity_level = 3;
    h.connectivity_cost = 1;
    h.interface_type = 6;
    h.network_initialized = 1;
    memcpy(p, &h, sizeof h);
}
static HRESULT WINAPI net_hint(void *self, void *hint)
{
    (void)self;
    if (!hint) return E_POINTER_;
    fill_hint(hint);
    return S_OK;
}

typedef void (__stdcall *net_change_fn)(void *context, const void *hint);
typedef struct net_reg { net_change_fn cb; void *ctx; } net_reg;
static net_reg g_net;

static void __stdcall deliver_net(void *ctx, BOOLEAN canceled)
{
    net_reg *r = ctx;
    connectivity_hint hint;
    if (canceled || !r->cb) return;
    fill_hint(&hint);
    r->cb(r->ctx, &hint);
}
static HRESULT WINAPI net_reg_hint(void *self, void *q, void *ctx, void *cb, UINT64 *token)
{
    (void)self;
    if (!cb) return E_INVALIDARG_;
    g_net.cb = (net_change_fn)cb;
    g_net.ctx = ctx;
    if (token) *token = 1;
    return submit((queue_obj *)q, PORT_COMP, 0, &g_net, deliver_net);
}
static BOOLEAN WINAPI net_unreg_hint(void *self, UINT64 token, BOOLEAN wait)
{
    (void)self; (void)token; (void)wait;
    g_net.cb = NULL;
    return TRUE;
}
static HRESULT WINAPI net_get_cfg(void *self, UINT32 setting, UINT64 *value)
{
    (void)self;
    xlog("net cfg %lu", (unsigned long)setting);
    if (!value) return E_POINTER_;
    *value = 4ull << 20;
    return S_OK;
}
static HRESULT WINAPI net_set_cfg(void *self, UINT32 setting, UINT64 value)
{
    (void)self;
    xlog("net set cfg %lu %llu", (unsigned long)setting, (unsigned long long)value);
    return S_OK;
}
static HRESULT WINAPI net_stats(void *self, UINT32 type, void *buf)
{
    (void)self; (void)type;
    if (!buf) return E_POINTER_;
    memset(buf, 0, 40);
    return S_OK;
}

static void *net_vtbl[] = {
    gen_qi, gen_addref, gen_release,
    net_port, net_port_async, net_port_result, net_reg_port, net_unreg_port,
    net_sec_async, net_sec_size, net_sec_result,
    net_sec16_async, net_sec_size, net_sec_result,
    net_verify, net_hint, net_reg_hint, net_unreg_hint,
    net_get_cfg, net_set_cfg, net_stats
};
static com_obj net_obj = { net_vtbl };
static HRESULT WINAPI net_qi(com_obj *self, const GUID *iid, void **out)
{
    const GUID *ok[] = { &IID_Net, &IID_Net2 };
    return gen_qi(self, iid, out, ok, 2);
}

static void *protocol_vtbl[] = { gen_qi, gen_addref, gen_release, proto_reg, proto_unreg };
static com_obj protocol_obj = { protocol_vtbl };
static HRESULT WINAPI protocol_qi(com_obj *self, const GUID *iid, void **out)
{
    const GUID *ok[] = { &IID_Protocol };
    return gen_qi(self, iid, out, ok, 1);
}

/* Every vtable follows the GDK ABI slot order. Slot 0 is replaced here with
 * the interface's QI. */
static void fix_vtbls(void)
{
    ((void **)threading_vtbl)[0] = threading_qi;
    ((void **)feature_vtbl)[0] = feature_qi;
    ((void **)game_vtbl)[0] = game_qi;
    ((void **)system_vtbl)[0] = system_qi;
    ((void **)analytics_vtbl)[0] = analytics_qi;
    ((void **)pls_vtbl)[0] = pls_qi;
    ((void **)error_vtbl)[0] = error_qi;
    ((void **)protocol_vtbl)[0] = protocol_qi;
    ((void **)net_vtbl)[0] = net_qi;
    ((void **)user_vtbl)[0] = user_qi;
    ((void **)gamertag_vtbl)[0] = gamertag_qi;
    user_obj.vtbl = user_vtbl;
    gamertag_obj.vtbl = gamertag_vtbl;
}

/* Returns 1 when id is not an interface implemented here. */
static HRESULT known_and_qi(const GUID *id, const GUID *iid, void **out)
{
    if (guid_eq(id, &IID_Threading)) return threading_qi(&threading_obj, iid, out);
    if (guid_eq(id, &IID_Feature)) return feature_qi(&feature_obj, iid, out);
    if (guid_eq(id, &IID_User) || guid_eq(id, &IID_User2) || guid_eq(id, &IID_User3) ||
        guid_eq(id, &IID_User4) || guid_eq(id, &IID_User5) || guid_eq(id, &IID_User6) ||
        guid_eq(id, &IID_Gamertag))
        return user_qi(&user_obj, iid, out);
    if (guid_eq(id, &IID_Game) || guid_eq(id, &IID_Game2) || guid_eq(id, &IID_Game3))
        return game_qi(&game_obj, iid, out);
    if (guid_eq(id, &IID_System) || guid_eq(id, &IID_System2) || guid_eq(id, &IID_System3) ||
        guid_eq(id, &IID_System4) || guid_eq(id, &IID_System5))
        return system_qi(&system_obj, iid, out);
    if (guid_eq(id, &IID_Analytics)) return analytics_qi(&analytics_obj, iid, out);
    if (guid_eq(id, &IID_PLS) || guid_eq(id, &IID_PLS2) || guid_eq(id, &IID_PLS3))
        return pls_qi(&pls_obj, iid, out);
    if (guid_eq(id, &IID_Error)) return error_qi(&error_obj, iid, out);
    if (guid_eq(id, &IID_Protocol) || guid_eq(id, &CLSID_Protocol))
        return protocol_qi(&protocol_obj, iid, out);
    if (guid_eq(id, &IID_Net) || guid_eq(id, &IID_Net2))
        return net_qi(&net_obj, iid, out);
    return 1;
}

static BOOL CALLBACK init_runtime(INIT_ONCE *once, void *param, void **ctx)
{
    char exe[MAX_PATH];
    (void)once; (void)param; (void)ctx;
    fix_vtbls();
    process_queue();
    if (!GetModuleFileNameA(NULL, exe, sizeof exe)) exe[0] = 0;
    xlog("dungeons2forlinux %s, exe=%s", D2FL_VERSION, exe);
    return TRUE;
}
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;

__declspec(dllexport) HRESULT WINAPI InitializeApiImplEx2(UINT64 gdk, UINT64 gs, UINT32 mode, const void *options)
{
    (void)gdk; (void)gs; (void)mode; (void)options;
    InitOnceExecuteOnce(&g_init_once, init_runtime, NULL, NULL);
    InterlockedExchange(&g_inited, 1);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI InitializeApiImplEx(UINT64 gdk, UINT64 gs, UINT32 mode)
{
    return InitializeApiImplEx2(gdk, gs, mode, NULL);
}

__declspec(dllexport) HRESULT WINAPI InitializeApiImpl(UINT64 gdk, UINT64 gs)
{
    return InitializeApiImplEx2(gdk, gs, 0, NULL);
}

__declspec(dllexport) HRESULT WINAPI UninitializeApiImpl(void)
{
    log_once("uninit");
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI XErrorReport(HRESULT hr, const char *msg)
{
    xlog("XErrorReport %08lX %s", (unsigned long)hr, msg ? msg : "");
    if (g_err_cb) g_err_cb(hr, msg, g_err_ctx);
    return S_OK;
}

static GUID g_unknown[48];
static int g_unknown_n;

__declspec(dllexport) HRESULT WINAPI QueryApiImpl(const GUID *clsid, const GUID *iid, void **out)
{
    HRESULT rc;
    int i, seen = 0;
    if (!out) return E_POINTER_;
    *out = NULL;
    if (!clsid) return E_INVALIDARG_;
    InitializeApiImplEx2(0, 0, 0, NULL);
    rc = known_and_qi(clsid, iid ? iid : clsid, out);
    if (rc != 1) return rc;
    if (iid) {
        rc = known_and_qi(iid, iid, out);
        if (rc != 1) return rc;
    }
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_unknown_n; i++) if (guid_eq(&g_unknown[i], clsid)) seen = 1;
    if (!seen && g_unknown_n < (int)(sizeof(g_unknown) / sizeof(g_unknown[0])))
        g_unknown[g_unknown_n++] = *clsid;
    LeaveCriticalSection(&g_lock);
    if (!seen) {
        log_guid("unknown interface", clsid);
        if (iid && !guid_eq(iid, clsid)) log_guid(" requested iid", iid);
    }
    return E_NOINTERFACE_;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_tls = TlsAlloc();
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_signin_lock);
        g_lock_ready = 1;
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (g_tls != TLS_OUT_OF_INDEXES) TlsFree(g_tls);
    }
    return TRUE;
}
