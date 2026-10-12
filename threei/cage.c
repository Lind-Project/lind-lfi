#define _GNU_SOURCE
// Cage table and lifetime management.
#include "cage.h"

#include "lock.h"

#include <stdlib.h>
#include <string.h>

struct Engine3i *g_eng;
_Thread_local struct Frame *tl_frames;
_Thread_local sigjmp_buf *tl_copy_jb;

// Cage structures are never returned to the system: lock-free lookups
// (cage_get) may touch a structure that is concurrently being recycled, and
// validate it afterwards.
static struct Cage *
pool_take(struct Engine3i *e)
{
    struct Cage *c = e->free_head;
    if (c) {
        e->free_head = c->pool_next;
        return c;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    pthread_rwlock_init(&c->memlk, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_mutex_init(&c->g_lk, NULL);
    pthread_mutex_init(&c->g_spawn_lk, NULL);
    pthread_mutex_init(&c->siglk, NULL);
    return c;
}

struct Cage *
cage_alloc(uint32_t ppid, struct LFILinuxProc *proc)
{
    struct Engine3i *e = g_eng;
    pthread_mutex_lock(&e->lk);
    uint32_t id = 0;
    for (size_t i = 0; i < e->table_n; i++) {
        uint32_t cand = e->next_id;
        e->next_id = e->next_id + 1 >= e->table_n ? 1 : e->next_id + 1;
        if (!e->id_used[cand]) {
            id = cand;
            break;
        }
    }
    if (!id) {
        pthread_mutex_unlock(&e->lk);
        return NULL;
    }
    struct Cage *c = pool_take(e);
    if (!c) {
        pthread_mutex_unlock(&e->lk);
        return NULL;
    }
    e->id_used[id] = 1;
    e->live++;
    pthread_mutex_unlock(&e->lk);

    c->id = id;
    c->gen++;
    c->ppid = (int) ppid;
    c->proc = proc;
    c->kt = NULL;
    c->nchildren = 0;
    c->exit_status = 0;
    c->killsig = 0;
    c->host_thread_valid = false;
    c->next_thread = NULL;
    c->umask = 022;
    c->nice = 0;
    c->g_entry = c->g_spawn = 0;
    c->g_all = c->g_free = c->g_cloner = NULL;
    atomic_store(&c->inflight, 0);
    atomic_store(&c->is_grate, false);
    sig_cage_init(c);
    for (size_t i = 0, n = ht_limit(c); i < n; i++)
        atomic_store_explicit(&c->htab[i], 0, memory_order_relaxed);
    atomic_store_explicit(&c->alt_hw, 0, memory_order_release);
    atomic_store(&c->state, CAGE_RUNNING);
    // The owner reference: held by the table, then by the parent's child list.
    atomic_store(&c->refs, 1);
    if (proc) {
        proc->cage = c;
        atomic_store_explicit(&e->bases[id], proc->box_info.base,
            memory_order_release);
    }
    return c;
}

void
cage_publish(struct Cage *c)
{
    atomic_store_explicit(&g_eng->table[c->id], c, memory_order_release);
}

void
cage_unpublish(struct Cage *c)
{
    atomic_store_explicit(&g_eng->table[c->id], NULL, memory_order_release);
}

struct Cage *
cage_get(uint32_t id)
{
    struct Engine3i *e = g_eng;
    if (id == 0 || id >= e->table_n)
        return NULL;
    struct Cage *c = atomic_load_explicit(&e->table[id], memory_order_acquire);
    if (!c)
        return NULL;
    int r = atomic_load_explicit(&c->refs, memory_order_relaxed);
    do {
        if (r <= 0)
            return NULL;
    } while (!atomic_compare_exchange_weak_explicit(&c->refs, &r, r + 1,
        memory_order_acquire, memory_order_relaxed));
    if (c->id != id || atomic_load(&c->state) == CAGE_FREE) {
        cage_put(c);
        return NULL;
    }
    return c;
}

static void
cage_destroy(struct Cage *c)
{
    struct Engine3i *e = g_eng;
    atomic_store(&c->state, CAGE_FREE);
    pthread_mutex_lock(&e->lk);
    e->id_used[c->id] = 0;
    c->pool_next = e->free_head;
    e->free_head = c;
    pthread_mutex_unlock(&e->lk);
}

void
cage_put(struct Cage *c)
{
    if (atomic_fetch_sub_explicit(&c->refs, 1, memory_order_acq_rel) == 1)
        cage_destroy(c);
}

void
cage_add_child(struct Cage *parent, struct Cage *child)
{
    LOCK_WITH_DEFER(&g_eng->tree_lk, lk);
    if (parent->nchildren == parent->cchildren) {
        size_t n = parent->cchildren ? parent->cchildren * 2 : 8;
        struct Cage **p = realloc(parent->children, n * sizeof(*p));
        if (!p)
            abort();
        parent->children = p;
        parent->cchildren = n;
    }
    parent->children[parent->nchildren++] = child;
}

bool
cage_is_descendant(struct Cage *c, struct Cage *ancestor)
{
    // Walk up the parent chain (bounded: ids are finite).
    uint32_t id = c->id;
    for (int depth = 0; depth < 4096; depth++) {
        struct Cage *x = cage_get(id);
        if (!x)
            return false;
        int pp = x->ppid;
        cage_put(x);
        if (pp <= 0)
            return false;
        if ((uint32_t) pp == ancestor->id)
            return true;
        id = pp;
    }
    return false;
}
