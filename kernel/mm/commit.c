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

#define CAP_RESERVE_DIVISOR   32  // The part of the commit limit that only a CAP_VM_NORESERVE holder may use
#define CAP_RESERVE_MIN_PAGES 256 // The least that it is: 1 MiB

size_t commit_limit_pages = 0U;
size_t commit_charged_pages = 0U;

static size_t cap_reserve_pages = 0U;

static int charge(size_t pages, size_t credit, bool use_reserve)
{
    size_t limit = commit_limit_pages;

    if(!use_reserve) {
        limit -= cap_reserve_pages;
    }

    size_t charged = atomic_load(&commit_charged_pages, ATOMIC_RELAXED);

    do {
        // The credit lets the total pass the limit for a moment. The caller
        // gives that many pages back right after, so only a replacement has one
        if(charged > limit + credit || pages > limit + credit - charged) {
            return -ENOMEM;
        }
    } while(!atomic_cmpxchg_weak(&commit_charged_pages, &charged, charged + pages, ATOMIC_RELAXED, ATOMIC_RELAXED));

    return 0;
}

int commit_charge(size_t pages, bool use_reserve)
{
    return charge(pages, 0U, use_reserve);
}

int commit_charge_replacing(size_t pages, size_t replaced, bool use_reserve)
{
    return charge(pages, replaced, use_reserve);
}

void commit_uncharge(size_t pages)
{
    size_t charged = atomic_fetch_sub(&commit_charged_pages, pages, ATOMIC_RELAXED);

    kassert(charged >= pages);
}

static void init_commit(void)
{
    size_t reserve = pslab_numpages / RESERVE_DIVISOR;

    if(reserve < RESERVE_MIN_PAGES) {
        reserve = RESERVE_MIN_PAGES;
    }

    if(pslab_numpages > reserve) {
        commit_limit_pages = pslab_numpages - reserve;
    }
    else {
        pr_warning("commit: %zu pages are less than the reserve, so nothing can be mapped", pslab_numpages);
    }

    cap_reserve_pages = commit_limit_pages / CAP_RESERVE_DIVISOR;

    if(cap_reserve_pages < CAP_RESERVE_MIN_PAGES) {
        cap_reserve_pages = CAP_RESERVE_MIN_PAGES;
    }

    if(cap_reserve_pages > commit_limit_pages) {
        cap_reserve_pages = commit_limit_pages;
    }

    char hrsize_buffer[128];
    format_size_iec(hrsize_buffer, sizeof(hrsize_buffer), commit_limit_pages * PAGE_SIZE);
    pr_inform("commit: limit is %s in %zu pages", hrsize_buffer, commit_limit_pages);
}

initcall_bsp_pmm(init_commit, INIT_LATE);
