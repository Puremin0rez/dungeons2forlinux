/* Regression tests for xgameruntime.dll; run by tests/run.sh under Wine. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct XAsyncBlock {
    void *queue;
    void *context;
    void (__stdcall *callback)(struct XAsyncBlock *);
    unsigned char internal[32];
} XAsyncBlock;
typedef struct { XAsyncBlock *async; SIZE_T bufferSize; void *buffer; void *context; } PD;
typedef HRESULT (__stdcall *provider_fn)(UINT32, const PD *);
typedef void (__stdcall *qcb_fn)(void *, BOOLEAN);

enum { T_STATUS = 3, T_RESULTSIZE, T_CANCEL, T_RUN, T_BEGIN, T_PAD, T_SCHEDULE, T_COMPLETE, T_RESULT,
       T_QCREATE, T_QCOMPOSITE, T_QPORT, T_QDUP, T_QDISPATCH, T_QCLOSE, T_QSUBMIT, T_QSUBMITDELAYED,
       T_QREGWAITER, T_QUNREGWAITER, T_QTERMINATE, T_QREGMON, T_QUNREGMON, T_QGETPROCESS, T_QSETPROCESS };

static void **vt, *thr, **uvt, *uobj, **nvt, *nobj;
static int failures;
static volatile LONG stage;
static const char *current = "";

#define CHECK(c) do { if (!(c)) { printf("  FAIL %s: %s (line %d)\n", current, #c, __LINE__); failures++; } } while (0)

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        printf("  FAIL %s: access violation at %p (stage %ld)\n", current,
               e->ExceptionRecord->ExceptionAddress, (long)stage);
        fflush(stdout);
        ExitProcess(3);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#define CALL(type, idx, ...) ((type)vt[idx])(thr, __VA_ARGS__)
static HRESULT status(XAsyncBlock *b, BOOLEAN wait) { return CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, BOOLEAN), T_STATUS, b, wait); }
static HRESULT begin(XAsyncBlock *b, void *ctx, provider_fn p) { return CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, void *, const void *, const char *, void *), T_BEGIN, b, ctx, (void *)p, "test", (void *)p); }
static HRESULT sched(XAsyncBlock *b, UINT32 d) { return CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, UINT32), T_SCHEDULE, b, d); }
static void complete(XAsyncBlock *b, HRESULT hr, SIZE_T n) { CALL(void (__stdcall *)(void *, XAsyncBlock *, HRESULT, SIZE_T), T_COMPLETE, b, hr, n); }
static HRESULT result(XAsyncBlock *b, SIZE_T n, void *buf, SIZE_T *used) { return CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, const void *, SIZE_T, void *, SIZE_T *), T_RESULT, b, NULL, n, buf, used); }
static void cancel(XAsyncBlock *b) { CALL(void (__stdcall *)(void *, XAsyncBlock *), T_CANCEL, b); }
static void *qcreate(UINT32 w, UINT32 c) { void *q = NULL; CALL(HRESULT (__stdcall *)(void *, UINT32, UINT32, void **), T_QCREATE, w, c, &q); return q; }
static void qclose(void *q) { CALL(void (__stdcall *)(void *, void *), T_QCLOSE, q); }
static void *qport(void *q, UINT32 p) { void *o = NULL; CALL(HRESULT (__stdcall *)(void *, void *, UINT32, void **), T_QPORT, q, p, &o); return o; }
static void *qcomposite(void *w, void *c) { void *o = NULL; CALL(HRESULT (__stdcall *)(void *, void *, void *, void **), T_QCOMPOSITE, w, c, &o); return o; }
static void *qdup(void *q) { void *o = NULL; CALL(HRESULT (__stdcall *)(void *, void *, void **), T_QDUP, q, &o); return o; }
static HRESULT qsubmit(void *q, UINT32 port, UINT32 delay, void *ctx, qcb_fn cb) { return CALL(HRESULT (__stdcall *)(void *, void *, UINT32, UINT32, void *, qcb_fn), T_QSUBMITDELAYED, q, port, delay, ctx, cb); }
static BOOLEAN qdispatch(void *q, UINT32 port, UINT32 t) { return CALL(BOOLEAN (__stdcall *)(void *, void *, UINT32, UINT32), T_QDISPATCH, q, port, t); }
static HRESULT qterminate(void *q, BOOLEAN wait, void *ctx, void (__stdcall *cb)(void *)) { return CALL(HRESULT (__stdcall *)(void *, void *, BOOLEAN, void *, void (__stdcall *)(void *)), T_QTERMINATE, q, wait, ctx, cb); }

static void wait_until(volatile LONG *flag, LONG want, DWORD ms)
{
    DWORD i;
    for (i = 0; i < ms / 5 && *flag < want; i++) Sleep(5);
}

static volatile LONG cleanups;
static HRESULT __stdcall payload_provider(UINT32 op, const PD *d)
{
    if (op == 0) return sched(d->async, 0);
    if (op == 1) complete(d->async, S_OK, 8);
    if (op == 2 && d->buffer) memcpy(d->buffer, "payload", 8);
    if (op == 4) InterlockedIncrement(&cleanups);
    return S_OK;
}

static volatile LONG cb_done;
static char got[8];
static void __stdcall cb_getresult(XAsyncBlock *b)
{
    SIZE_T used = 0;
    CHECK(result(b, sizeof got, got, &used) == S_OK && used == 8);
    InterlockedIncrement(&cb_done);
}
static void test_getresult_in_callback(void)
{
    XAsyncBlock b;
    current = "GetResult inside completion callback";
    memset(&b, 0, sizeof b);
    b.callback = cb_getresult;
    cb_done = 0;
    cleanups = 0;
    CHECK(begin(&b, NULL, payload_provider) == S_OK);
    wait_until(&cb_done, 1, 2000);
    Sleep(50);
    CHECK(cb_done == 1);
    CHECK(!strcmp(got, "payload"));
    CHECK(cleanups == 1);
    CHECK(status(&b, FALSE) == S_OK);
}

static HRESULT __stdcall delayed_provider(UINT32 op, const PD *d)
{
    if (op == 0) return sched(d->async, 150);
    if (op == 1) complete(d->async, S_OK, 0);
    if (op == 3) complete(d->async, E_ABORT, 0);
    return S_OK;
}
static void __stdcall cb_count(XAsyncBlock *b) { (void)b; InterlockedIncrement(&cb_done); }
static void test_cancel_with_delayed_work(void)
{
    XAsyncBlock b;
    current = "cancel with delayed work queued";
    memset(&b, 0, sizeof b);
    b.callback = cb_count;
    cb_done = 0;
    CHECK(begin(&b, NULL, delayed_provider) == S_OK);
    cancel(&b);
    wait_until(&cb_done, 1, 2000);
    Sleep(300);
    CHECK(cb_done == 1);
    CHECK(status(&b, FALSE) == E_ABORT);
}

static void test_composite_refcount(void)
{
    void *base, *comp, *dup;
    current = "composite duplicate and double close";
    base = qcreate(1, 1);
    comp = qcomposite(qport(base, 0), qport(base, 0));
    dup = qdup(comp);
    CHECK(comp && dup == comp);
    qclose(comp);
    qclose(dup);
    qclose(base);
}

static void *self_q;
static void __stdcall close_self(void *ctx, BOOLEAN canceled)
{
    (void)ctx; (void)canceled;
    qclose(self_q);
    InterlockedIncrement(&cb_done);
}
static void test_callback_closes_own_queue(void)
{
    current = "callback closes its own queue";
    cb_done = 0;
    self_q = qcreate(1, 1);
    CHECK(qsubmit(self_q, 0, 0, NULL, close_self) == S_OK);
    wait_until(&cb_done, 1, 2000);
    Sleep(200);
    CHECK(cb_done == 1);
}

static void __stdcall cb_free_block(XAsyncBlock *b)
{
    InterlockedIncrement(&cb_done);
    free(b);
}
static HRESULT __stdcall empty_provider(UINT32 op, const PD *d)
{
    if (op == 0) return sched(d->async, 0);
    if (op == 1) complete(d->async, S_OK, 0);
    return S_OK;
}
static void test_callback_frees_block(void)
{
    XAsyncBlock *b = calloc(1, sizeof *b);
    current = "callback frees its block";
    cb_done = 0;
    b->callback = cb_free_block;
    CHECK(begin(b, NULL, empty_provider) == S_OK);
    wait_until(&cb_done, 1, 2000);
    Sleep(100);
    CHECK(cb_done == 1);
}

static volatile LONG reuse_left;
static void __stdcall cb_reuse(XAsyncBlock *b)
{
    SIZE_T used;
    char buf[8];
    CHECK(result(b, sizeof buf, buf, &used) == S_OK);
    if (InterlockedDecrement(&reuse_left) > 0) CHECK(begin(b, NULL, payload_provider) == S_OK);
    InterlockedIncrement(&cb_done);
}
static void test_block_reuse_in_callback(void)
{
    static XAsyncBlock b;
    current = "block reused from its own callback";
    memset(&b, 0, sizeof b);
    b.callback = cb_reuse;
    cb_done = 0;
    reuse_left = 5;
    CHECK(begin(&b, NULL, payload_provider) == S_OK);
    wait_until(&cb_done, 5, 3000);
    Sleep(50);
    CHECK(cb_done == 5);
}

static volatile LONG term_calls, canceled_calls, plain_calls;
static void __stdcall on_term(void *ctx) { (void)ctx; InterlockedIncrement(&term_calls); }
static void __stdcall count_cb(void *ctx, BOOLEAN canceled)
{
    (void)ctx;
    if (canceled) InterlockedIncrement(&canceled_calls);
    else InterlockedIncrement(&plain_calls);
}
static void __stdcall monitor_cb(void *ctx, void *queue, UINT32 port) { (void)ctx; (void)queue; (void)port; }

static void test_termination(void)
{
    void *q, *manual, *comp;
    current = "termination";
    term_calls = canceled_calls = plain_calls = 0;
    q = qcreate(1, 1);
    CHECK(qterminate(q, FALSE, NULL, on_term) == S_OK);
    wait_until(&term_calls, 1, 2000);
    CHECK(term_calls == 1);
    CHECK(qsubmit(q, 0, 0, NULL, count_cb) != S_OK);
    {
        XAsyncBlock b;
        memset(&b, 0, sizeof b);
        b.queue = q;
        CHECK(begin(&b, NULL, payload_provider) == E_ABORT);
    }
    qclose(q);

    manual = qcreate(0, 0);
    CALL(HRESULT (__stdcall *)(void *, void *, void *, void *, UINT64 *), T_QREGMON, manual, NULL, (void *)monitor_cb, NULL);
    CHECK(qsubmit(manual, 1, 0, NULL, count_cb) == S_OK);
    qclose(manual);
    wait_until(&canceled_calls, 1, 2000);
    CHECK(canceled_calls == 1);

    q = qcreate(1, 1);
    comp = qcomposite(qport(q, 0), qport(q, 1));
    CHECK(qterminate(comp, TRUE, NULL, on_term) == S_OK);
    plain_calls = 0;
    CHECK(qsubmit(q, 0, 0, NULL, count_cb) == S_OK);
    wait_until(&plain_calls, 1, 2000);
    CHECK(plain_calls == 1);
    qclose(comp);
    qclose(q);
}

static volatile LONG order_step, term_seen_at, done_seen_at;
static void __stdcall on_term_ordered(void *ctx) { (void)ctx; term_seen_at = InterlockedIncrement(&order_step); }
static void __stdcall cb_ordered(XAsyncBlock *b) { (void)b; done_seen_at = InterlockedIncrement(&order_step); }
static void test_termination_waits_for_calls(void)
{
    XAsyncBlock b;
    void *q;
    int wait;
    current = "termination waits for in-flight calls";
    for (wait = 0; wait <= 1; wait++) {
        order_step = term_seen_at = done_seen_at = 0;
        q = qcreate(1, 1);
        memset(&b, 0, sizeof b);
        b.queue = q;
        b.callback = cb_ordered;
        CHECK(begin(&b, NULL, delayed_provider) == S_OK);
        CHECK(qterminate(q, (BOOLEAN)wait, NULL, on_term_ordered) == S_OK);
        wait_until(&term_seen_at, 1, 3000);
        CHECK(done_seen_at == 1 && term_seen_at == 2);
        qclose(q);
    }
}

static HRESULT __stdcall forever_provider(UINT32 op, const PD *d)
{
    if (op == 0 || op == 1) return sched(d->async, 30);
    return S_OK;
}
static volatile LONG forever_done;
static void __stdcall cb_forever(XAsyncBlock *b) { (void)b; InterlockedIncrement(&forever_done); }
static void test_termination_aborts_endless_calls(void)
{
    XAsyncBlock b;
    void *q;
    current = "termination aborts endless calls";
    forever_done = term_calls = 0;
    q = qcreate(1, 1);
    memset(&b, 0, sizeof b);
    b.queue = q;
    b.callback = cb_forever;
    CHECK(begin(&b, NULL, forever_provider) == S_OK);
    Sleep(100);
    CHECK(qterminate(q, FALSE, NULL, on_term) == S_OK);
    wait_until(&term_calls, 1, 3000);
    CHECK(forever_done == 1);
    CHECK(term_calls == 1);
    CHECK(status(&b, FALSE) == E_ABORT);
    qclose(q);
}

static const PD *saved_pd;
static volatile LONG pd_saved, clobbered;
static HRESULT __stdcall saving_provider(UINT32 op, const PD *d)
{
    if (op == 0) return sched(d->async, 0);
    if (op == 1 && !saved_pd) { saved_pd = d; InterlockedExchange(&pd_saved, 1); }
    return S_OK;
}
static void __stdcall clobber_stack(void *ctx, BOOLEAN canceled)
{
    volatile char junk[4096];
    (void)ctx; (void)canceled;
    memset((void *)junk, 0xAB, sizeof junk);
    InterlockedExchange(&clobbered, 1);
}
static void test_provider_data_outlives_call(void)
{
    XAsyncBlock b;
    void *q;
    int marker = 42;
    current = "provider data outlives the provider call";
    saved_pd = NULL;
    pd_saved = clobbered = 0;
    q = qcreate(1, 1);
    memset(&b, 0, sizeof b);
    b.queue = q;
    CHECK(begin(&b, &marker, saving_provider) == S_OK);
    wait_until(&pd_saved, 1, 2000);
    CHECK(qsubmit(q, 0, 0, NULL, clobber_stack) == S_OK);
    wait_until(&clobbered, 1, 2000);
    CHECK(saved_pd && saved_pd->context == &marker && saved_pd->async && saved_pd->async->queue == q);
    if (saved_pd && saved_pd->context == &marker) complete(saved_pd->async, S_OK, 0);
    CHECK(status(&b, TRUE) == S_OK);
    qclose(q);
}

static HRESULT __stdcall run_work(XAsyncBlock *b) { (void)b; return S_OK; }
static void test_manual_queue(void)
{
    XAsyncBlock b;
    void *q;
    current = "manual queue dispatch";
    q = qcreate(0, 0);
    CALL(HRESULT (__stdcall *)(void *, void *, void *, void *, UINT64 *), T_QREGMON, q, NULL, (void *)monitor_cb, NULL);
    memset(&b, 0, sizeof b);
    b.queue = q;
    b.callback = cb_count;
    cb_done = 0;
    CHECK(CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, void *), T_RUN, &b, (void *)run_work) == S_OK);
    CHECK(status(&b, FALSE) == E_PENDING);
    CHECK(qdispatch(q, 0, 100));
    CHECK(qdispatch(q, 1, 100));
    CHECK(cb_done == 1);
    CHECK(status(&b, FALSE) == S_OK);

    current = "dispatch rejects an invalid port";
    plain_calls = 0;
    CHECK(qsubmit(q, 1, 0, NULL, count_cb) == S_OK);
    CHECK(!qdispatch(q, 7, 0));
    CHECK(plain_calls == 0);
    CHECK(qdispatch(q, 1, 0));
    CHECK(plain_calls == 1);
    qclose(q);
}

static void *affinity_q;
static volatile LONG affinity_stop, affinity_ran, affinity_wrong;
static DWORD affinity_tid;
static void __stdcall affinity_cb(void *ctx, BOOLEAN canceled)
{
    (void)ctx; (void)canceled;
    if (GetCurrentThreadId() != affinity_tid) InterlockedIncrement(&affinity_wrong);
    InterlockedIncrement(&affinity_ran);
}
static DWORD WINAPI affinity_dispatcher(void *arg)
{
    (void)arg;
    affinity_tid = GetCurrentThreadId();
    while (!affinity_stop) {
        qdispatch(affinity_q, 0, 20);
        qdispatch(affinity_q, 1, 0);
    }
    return 0;
}
static void test_manual_queue_thread_affinity(void)
{
    HANDLE t;
    int i;
    current = "manual queue runs only on its dispatcher";
    affinity_stop = affinity_ran = affinity_wrong = 0;
    affinity_q = qcreate(0, 0);
    t = CreateThread(NULL, 0, affinity_dispatcher, NULL, 0, NULL);
    Sleep(50);
    for (i = 0; i < 400; i++) {
        CHECK(qsubmit(affinity_q, (UINT32)(i & 1), 0, NULL, affinity_cb) == S_OK);
        if ((i % 50) == 0) Sleep(30);
    }
    Sleep(1500);
    affinity_stop = 1;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(affinity_ran == 400);
    CHECK(affinity_wrong == 0);
    qclose(affinity_q);

    current = "undispatched manual queue is rescued";
    cb_done = 0;
    affinity_q = qcreate(0, 0);
    plain_calls = 0;
    CHECK(qsubmit(affinity_q, 1, 0, NULL, count_cb) == S_OK);
    wait_until(&plain_calls, 1, 3000);
    CHECK(plain_calls >= 1);
    qclose(affinity_q);
}

static HRESULT __stdcall slow_work(XAsyncBlock *b) { (void)b; Sleep(100); return 0x12345678 & 0; }
static void test_wait(void)
{
    XAsyncBlock b;
    current = "blocking wait";
    memset(&b, 0, sizeof b);
    CHECK(CALL(HRESULT (__stdcall *)(void *, XAsyncBlock *, void *), T_RUN, &b, (void *)slow_work) == S_OK);
    CHECK(status(&b, TRUE) == S_OK);
}

static volatile LONG stress_done;
static void __stdcall cb_stress(XAsyncBlock *b)
{
    SIZE_T used;
    char buf[8];
    result(b, sizeof buf, buf, &used);
    free(b);
    InterlockedIncrement(&stress_done);
}
static DWORD WINAPI stress_thread(void *arg)
{
    int i;
    void *q = arg;
    for (i = 0; i < 1500; i++) {
        XAsyncBlock *b = calloc(1, sizeof *b);
        b->queue = (i & 1) ? q : NULL;
        b->callback = cb_stress;
        if (begin(b, NULL, (i % 3) ? payload_provider : delayed_provider) != S_OK) {
            free(b);
            InterlockedIncrement(&stress_done);
            continue;
        }
        if ((i % 7) == 0) cancel(b);
    }
    return 0;
}
static void test_stress(void)
{
    HANDLE t[6];
    void *q;
    int i;
    current = "concurrent stress";
    stress_done = 0;
    q = qcreate(1, 1);
    for (i = 0; i < 6; i++) t[i] = CreateThread(NULL, 0, stress_thread, q, 0, NULL);
    WaitForMultipleObjects(6, t, TRUE, INFINITE);
    for (i = 0; i < 6; i++) CloseHandle(t[i]);
    wait_until(&stress_done, 6 * 1500, 30000);
    CHECK(stress_done == 6 * 1500);
    qclose(q);
}

typedef struct { SIZE_T tokenSize, signatureSize; const char *token, *signature; } token_blob;
static const char *token_for(const char *url, char *out, size_t cap)
{
    XAsyncBlock b;
    SIZE_T size = 0, used = 0;
    void *buf, *ptr = NULL;
    memset(&b, 0, sizeof b);
    out[0] = 0;
    if (((HRESULT (__stdcall *)(void *, void *, UINT32, const char *, const char *, SIZE_T, const void *, SIZE_T, const void *, XAsyncBlock *))uvt[23])(
            uobj, uobj, 0, "GET", url, 0, NULL, 0, NULL, &b) != S_OK) return out;
    if (status(&b, TRUE) != S_OK) return out;
    ((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T *))uvt[24])(uobj, &b, &size);
    buf = malloc(size);
    if (((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T, void *, void *, SIZE_T *))uvt[25])(uobj, &b, size, buf, &ptr, &used) == S_OK)
        snprintf(out, cap, "%s", ((token_blob *)ptr)->token);
    CHECK(((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T, void *, void *, SIZE_T *))uvt[25])(uobj, &b, size, buf, &ptr, &used) != S_OK);
    free(buf);
    return out;
}
static void test_signin(int expect_ok)
{
    char t[128];
    DWORD start = GetTickCount();
    current = expect_ok ? "sign-in through the helper" : "failed sign-in reports promptly";
    token_for("https://playfabapi.com/", t, sizeof t);
    if (expect_ok) CHECK(!strcmp(t, "XBL3.0 x=1;fresh-pf-token"));
    else CHECK(t[0] == 0 && GetTickCount() - start < 20000);
}

static void test_user_add_signs_in(int expect_ok)
{
    XAsyncBlock b;
    void *user = NULL;
    UINT64 id = 0;
    current = expect_ok ? "user add waits for sign-in" : "user add fails when sign-in fails";
    memset(&b, 0, sizeof b);
    CHECK(((HRESULT (__stdcall *)(void *, UINT32, XAsyncBlock *))uvt[7])(uobj, 1, &b) == S_OK);
    if (expect_ok) {
        CHECK(status(&b, TRUE) == S_OK);
        CHECK(((HRESULT (__stdcall *)(void *, XAsyncBlock *, void **))uvt[8])(uobj, &b, &user) == S_OK);
        CHECK(((HRESULT (__stdcall *)(void *, void *, UINT64 *))uvt[11])(uobj, user, &id) == S_OK);
        CHECK(id == 2533274800000002ull);
    } else {
        CHECK(status(&b, TRUE) != S_OK);
    }
}

static void test_user_add_survives_early_cancel(void)
{
    XAsyncBlock b;
    void *q = qcreate(0, 0);
    current = "signed-in user add survives an early cancel";
    CALL(HRESULT (__stdcall *)(void *, void *, void *, void *, UINT64 *), T_QREGMON, q, NULL, (void *)monitor_cb, NULL);
    memset(&b, 0, sizeof b);
    b.queue = q;
    CHECK(((HRESULT (__stdcall *)(void *, UINT32, XAsyncBlock *))uvt[7])(uobj, 0, &b) == S_OK);
    cancel(&b);
    CHECK(status(&b, FALSE) == S_OK);
    while (qdispatch(q, 1, 0) || qdispatch(q, 0, 0)) ;
    qclose(q);
}

static volatile LONG race_ok, race_bad;
static void __stdcall cb_collect_token(XAsyncBlock *b)
{
    SIZE_T size = 0, used = 0;
    void *ptr = NULL;
    char buf[256];
    if (((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T *))uvt[24])(uobj, b, &size) == S_OK && size <= sizeof buf &&
        ((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T, void *, void *, SIZE_T *))uvt[25])(uobj, b, sizeof buf, buf, &ptr, &used) == S_OK)
        InterlockedIncrement(&race_ok);
    free(b);
}
static void test_token_begin_race(void)
{
    int i;
    void *q = qcreate(1, 3);
    current = "token answered at Begin reports success";
    race_ok = race_bad = 0;
    for (i = 0; i < 200; i++) {
        XAsyncBlock *b = calloc(1, sizeof *b);
        b->queue = q;
        b->callback = cb_collect_token;
        if (((HRESULT (__stdcall *)(void *, void *, UINT32, const char *, const char *, SIZE_T, const void *, SIZE_T, const void *, XAsyncBlock *))uvt[23])(
                uobj, uobj, 0, "GET", "https://playfabapi.com/", 0, NULL, 0, NULL, b) != S_OK)
            InterlockedIncrement(&race_bad);
    }
    Sleep(200);
    CHECK(race_bad == 0);
    CHECK(race_ok == 200);
    qclose(q);
}

static void test_tokens(void)
{
    char t[128], url[1200];
    current = "token routing";
    CHECK(!strcmp(token_for("https://playfabapi.com/", t, sizeof t), "XBL3.0 x=1;pf-token"));
    CHECK(!strcmp(token_for("https://api.minecraftservices.com", t, sizeof t), "XBL3.0 x=1;mc-token"));
    CHECK(!strcmp(token_for("https://peoplehub.xboxlive.com/users/me", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("https://6B9DE.PlayFabAPI.com:443/Client/Login", t, sizeof t), "XBL3.0 x=1;pf-token"));
    CHECK(!strcmp(token_for("https://user@6b9de.playfabapi.com?x=1", t, sizeof t), "XBL3.0 x=1;pf-token"));
    CHECK(!strcmp(token_for("https://example.com/?next=playfabapi.com", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("https://example.com/minecraftservices.com/", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("https://playfabapi.com.example.net/", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("https://notplayfabapi.com/", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    CHECK(!strcmp(token_for("https://peoplehub.xboxlive.com/users/me?QUERYSECRET=1", t, sizeof t), "XBL3.0 x=1;xbox-token"));
    snprintf(url, sizeof url, "https://peoplehub.xboxlive.com/%0900d", 0);
    CHECK(!strcmp(token_for(url, t, sizeof t), "XBL3.0 x=1;xbox-token"));
}

static void net_security(void)
{
    XAsyncBlock b;
    SIZE_T used = 0;
    void *info = NULL;
    char buf[64];
    memset(&b, 0, sizeof b);
    CHECK(((HRESULT (__stdcall *)(void *, const char *, XAsyncBlock *))nvt[8])(nobj, "https://example.invalid/?QUERYSECRET=1", &b) == S_OK);
    CHECK(status(&b, TRUE) == S_OK);
    CHECK(((HRESULT (__stdcall *)(void *, XAsyncBlock *, SIZE_T, SIZE_T *, void *, void *))nvt[10])(nobj, &b, sizeof buf, &used, buf, &info) == S_OK);
    CHECK(info == buf && (*(UINT32 *)buf & 0x800));
}

static void test_hook_behaviour(HMODULE mod)
{
    void *(*fx_open)(void) = (void *)GetProcAddress(mod, "fx_open");
    BOOL (*fx_close)(void *) = (void *)GetProcAddress(mod, "fx_close");
    BOOL (*fx_set_option)(void *, DWORD, void *, DWORD) = (void *)GetProcAddress(mod, "fx_set_option");
    void *(*fx_connect)(void *, const WCHAR *, unsigned short) = (void *)GetProcAddress(mod, "fx_connect");
    void *(*fx_open_request)(void *, const WCHAR *) = (void *)GetProcAddress(mod, "fx_open_request");
    BOOL (*fx_send)(void *) = (void *)GetProcAddress(mod, "fx_send");
    BOOL (*fx_receive)(void *) = (void *)GetProcAddress(mod, "fx_receive");
    HMODULE wh = GetModuleHandleA("winhttp.dll");
    BOOL (__stdcall *send)(void *, const WCHAR *, DWORD, void *, DWORD, DWORD, DWORD_PTR) = (void *)GetProcAddress(wh, "WinHttpSendRequest");
    BOOL (__stdcall *receive)(void *, void *) = (void *)GetProcAddress(wh, "WinHttpReceiveResponse");
    void *s, *c, *r;
    DWORD v = 1, want, got;
    BOOL ok;
    if (!fx_open) return;
    current = "XCurl hooks keep WinHTTP's behaviour";
    s = fx_open();
    CHECK(s != NULL);
    CHECK(fx_set_option(s, 140 , &v, sizeof v));
    c = fx_connect(s, L"example.invalid", 443);
    r = c ? fx_open_request(c, L"/path?QUERYSECRET=1") : NULL;
    CHECK(c && r);

    SetLastError(0);
    CHECK(!send(NULL, NULL, 0, NULL, 0, 0, 0));
    want = GetLastError();
    SetLastError(0);
    ok = fx_send(NULL);
    got = GetLastError();
    CHECK(!ok && want && got == want);

    SetLastError(0);
    CHECK(!receive(r, NULL));
    want = GetLastError();
    SetLastError(0);
    ok = fx_receive(r);
    got = GetLastError();
    CHECK(!ok && want && got == want);

    if (r) fx_close(r);
    if (c) fx_close(c);
    if (s) fx_close(s);
}

static void test_hooks(const char *xcurl)
{
    HMODULE mod, self;
    unsigned char *base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int hooked = 0;
    current = "XCurl hooks";
    net_security();
    mod = LoadLibraryA(xcurl);
    if (!mod) { printf("  skip %s: cannot load %s (%lu)\n", current, xcurl, GetLastError()); return; }
    net_security();
    self = GetModuleHandleA("xgameruntime.dll");
    base = (unsigned char *)mod;
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA64 *n, *s;
        if (_stricmp((char *)base + imp->Name, "winhttp.dll")) continue;
        n = (IMAGE_THUNK_DATA64 *)(base + imp->OriginalFirstThunk);
        s = (IMAGE_THUNK_DATA64 *)(base + imp->FirstThunk);
        for (; n->u1.AddressOfData; n++, s++) {
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + n->u1.AddressOfData))->Name;
            MEMORY_BASIC_INFORMATION mbi;
            int ours = VirtualQuery((void *)s->u1.Function, &mbi, sizeof mbi) && mbi.AllocationBase == (void *)self;
            if (!strcmp(name, "WinHttpSetOption")) { CHECK(ours); hooked += ours; }
            if (!strcmp(name, "WinHttpReadData")) CHECK(!ours);
        }
    }
    CHECK(hooked == 1);
    test_hook_behaviour(mod);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 && !strncmp(argv[1], "--", 2) ? argv[1] : "";
    const char *xcurl = argc > 1 && !mode[0] ? argv[1] : NULL;
    GUID tid = { 0x073b7dcb, 0x1fcf, 0x4030, { 0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28 } };
    GUID uid = { 0x01acd177, 0x91f9, 0x4763, { 0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0 } };
    GUID nid = { 0x37e56907, 0x2f10, 0x41e8, { 0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a } };
    HMODULE m;
    HRESULT (__stdcall *init)(uint64_t, uint64_t, uint32_t, const void *);
    HRESULT (__stdcall *query)(const GUID *, const GUID *, void **);

    setvbuf(stdout, NULL, _IONBF, 0);
    AddVectoredExceptionHandler(1, on_fault);
    m = LoadLibraryA("xgameruntime.dll");
    if (!m) { printf("cannot load xgameruntime.dll\n"); return 2; }
    init = (void *)GetProcAddress(m, "InitializeApiImplEx2");
    query = (void *)GetProcAddress(m, "QueryApiImpl");
    if (!init || !query) { printf("exports missing\n"); return 2; }
    init(0, 0, 0, NULL);
    if (query(&tid, &tid, &thr) || query(&uid, &uid, &uobj) || query(&nid, &nid, &nobj)) { printf("query failed\n"); return 2; }
    vt = *(void ***)thr;
    uvt = *(void ***)uobj;
    nvt = *(void ***)nobj;

    if (!strcmp(mode, "--signin-ok") || !strcmp(mode, "--signin-fail")) {
        test_user_add_signs_in(!strcmp(mode, "--signin-ok"));
        test_signin(!strcmp(mode, "--signin-ok"));
        printf(failures ? "FAILED: %d\n" : "all tests passed\n", failures);
        return failures ? 1 : 0;
    }
    test_getresult_in_callback();
    test_cancel_with_delayed_work();
    test_composite_refcount();
    test_callback_closes_own_queue();
    test_callback_frees_block();
    test_block_reuse_in_callback();
    test_termination();
    test_termination_waits_for_calls();
    test_termination_aborts_endless_calls();
    test_provider_data_outlives_call();
    test_manual_queue();
    test_manual_queue_thread_affinity();
    test_wait();
    test_stress();
    test_tokens();
    test_user_add_survives_early_cancel();
    test_token_begin_race();
    if (xcurl) test_hooks(xcurl);
    printf(failures ? "FAILED: %d\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
