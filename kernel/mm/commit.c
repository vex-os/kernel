#include <vex/assert.h>
#include <vex/atomic.h>
#include <vex/commit.h>
#include <vex/errno.h>
#include <vex/hrsize.h>
#include <vex/initcall.h>
#include <vex/kprintf.h>
#include <vex/page.h>
#include <vex/pslab.h>

#define RESERVE_DIVISOR 8 // The kernel keeps this part of the pages for itself

#define RESERVE_MIN_PAGES 4096 // The least that it keeps: the slab, stacks and page tables are not charged

static size_t limit_pages = 0U;
static size_t charged_pages = 0U;

int commit_charge(size_t pages)
{
    size_t charged = atomic_load(&charged_pages, ATOMIC_RELAXED);

    do {
        // The loop only ever adds what fits
        // so charged never passes the limit
        if(pages > limit_pages - charged) {
            return -ENOMEM;
        }
    } while(!atomic_cmpxchg_weak(&charged_pages, &charged, charged + pages, ATOMIC_RELAXED, ATOMIC_RELAXED));

    return 0;
}

void commit_uncharge(size_t pages)
{
    size_t charged = atomic_fetch_sub(&charged_pages, pages, ATOMIC_RELAXED);

    kassert(charged >= pages);
}

static void init_commit(void)
{
    size_t reserve = pslab_numpages / RESERVE_DIVISOR;

    if(reserve < RESERVE_MIN_PAGES) {
        reserve = RESERVE_MIN_PAGES;
    }

    if(pslab_numpages > reserve) {
        limit_pages = pslab_numpages - reserve;
    }
    else {
        pr_warning("commit: %zu pages are less than the reserve, so nothing can be mapped", pslab_numpages);
    }

    char hrsize_buffer[128];
    format_size_iec(hrsize_buffer, sizeof(hrsize_buffer), limit_pages * PAGE_SIZE);
    pr_inform("commit: limit is %s in %zu pages", hrsize_buffer, limit_pages);
}

initcall_bsp_pmm(init_commit, INIT_LATE);
