// LD_PRELOAD allocation counting shim. Counts malloc/calloc/realloc/free (+memalign family),
// tracks live bytes with malloc_usable_size, prints totals + mallinfo2 at exit to stderr.
#define _GNU_SOURCE
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);
extern void *__libc_memalign(size_t, size_t);

static atomic_ulong n_malloc, n_calloc, n_realloc, n_free, n_memalign, n_free_null;
static atomic_ulong req_bytes;          // sum of requested bytes (malloc/calloc/realloc new size/memalign)
static atomic_long live, peak;          // live usable bytes
static atomic_ulong peak_count_live, live_count; // live block count / peak
static atomic_long live_cnt_l, peak_cnt_l;

static void add_live(void *p) {
  if (!p) return;
  long u = (long)malloc_usable_size(p);
  long l = atomic_fetch_add(&live, u) + u;
  long pk = atomic_load(&peak);
  while (l > pk && !atomic_compare_exchange_weak(&peak, &pk, l)) {}
  long c = atomic_fetch_add(&live_cnt_l, 1) + 1;
  long pc = atomic_load(&peak_cnt_l);
  while (c > pc && !atomic_compare_exchange_weak(&peak_cnt_l, &pc, c)) {}
}
static void sub_live(void *p) {
  if (!p) return;
  atomic_fetch_sub(&live, (long)malloc_usable_size(p));
  atomic_fetch_sub(&live_cnt_l, 1);
}

void *malloc(size_t n) {
  void *p = __libc_malloc(n);
  atomic_fetch_add(&n_malloc, 1); atomic_fetch_add(&req_bytes, n);
  add_live(p); return p;
}
void *calloc(size_t a, size_t b) {
  void *p = __libc_calloc(a, b);
  atomic_fetch_add(&n_calloc, 1); atomic_fetch_add(&req_bytes, a * b);
  add_live(p); return p;
}
void *realloc(void *o, size_t n) {
  atomic_fetch_add(&n_realloc, 1); atomic_fetch_add(&req_bytes, n);
  if (o) sub_live(o);
  void *p = __libc_realloc(o, n);
  if (p) add_live(p); else if (o && n) add_live(o); /* failed: old block stays live */
  else if (o && n == 0) { /* realloc(p,0) frees */ }
  return p;
}
void free(void *p) {
  if (!p) { atomic_fetch_add(&n_free_null, 1); return; }
  atomic_fetch_add(&n_free, 1);
  sub_live(p);
  __libc_free(p);
}
void *memalign(size_t al, size_t n) {
  void *p = __libc_memalign(al, n);
  atomic_fetch_add(&n_memalign, 1); atomic_fetch_add(&req_bytes, n);
  add_live(p); return p;
}
int posix_memalign(void **out, size_t al, size_t n) {
  void *p = __libc_memalign(al, n);
  if (!p) return 12;
  atomic_fetch_add(&n_memalign, 1); atomic_fetch_add(&req_bytes, n);
  add_live(p); *out = p; return 0;
}
void *aligned_alloc(size_t al, size_t n) { return memalign(al, n); }

__attribute__((destructor)) static void report(void) {
  struct mallinfo2 m = mallinfo2();
  unsigned long a = atomic_load(&n_malloc), c = atomic_load(&n_calloc), r = atomic_load(&n_realloc),
                f = atomic_load(&n_free), ma = atomic_load(&n_memalign);
  // allocs = malloc+calloc+memalign (new blocks; realloc(NULL,n) counted in realloc); realloc reported separately
  fprintf(stderr,
    "SHIM malloc=%lu calloc=%lu realloc=%lu memalign=%lu free=%lu free_null=%lu\n"
    "SHIM new_block_allocs(malloc+calloc+memalign)=%lu  allocs_minus_frees=%ld\n"
    "SHIM live_blocks_at_exit=%ld live_usable_bytes_at_exit=%ld peak_live_usable_bytes=%ld peak_live_blocks=%ld requested_bytes_total=%lu\n"
    "SHIM mallinfo2 arena=%zu uordblks=%zu fordblks=%zu hblks=%zu hblkhd=%zu keepcost=%zu\n",
    a, c, r, ma, f, atomic_load(&n_free_null),
    a + c + ma, (long)(a + c + ma) - (long)f,
    atomic_load(&live_cnt_l), atomic_load(&live), atomic_load(&peak), atomic_load(&peak_cnt_l), atomic_load(&req_bytes),
    m.arena, m.uordblks, m.fordblks, m.hblks, m.hblkhd, m.keepcost);
}
