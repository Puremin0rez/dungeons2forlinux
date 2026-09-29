/* Test allocator: each allocation gets its own pages, and free() makes them inaccessible. */
#include <windows.h>
#include <string.h>
#include <stdlib.h>
static void *guard_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (sz && total / sz != n) return NULL;
    return VirtualAlloc(NULL, total ? total : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}
static void *guard_malloc(size_t sz) { return guard_calloc(1, sz); }
static void guard_free(void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;
    if (p && VirtualQuery(p, &mbi, sizeof mbi)) VirtualProtect(mbi.AllocationBase, mbi.RegionSize, PAGE_NOACCESS, &old);
}
#define calloc guard_calloc
#define malloc guard_malloc
#define free guard_free
