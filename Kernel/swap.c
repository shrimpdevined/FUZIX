/*
 *	For small machines it's cheaper to simply allocate banks in the size
 *	needed for the largest swapout of the application as that'll be under
 *	64K. For split I/D we can allocate pairs of swap banks.
 *
 *	It's possible to be a lot smarter about this and for 32bit systems
 *	it becomes a necessity not to use this simple swap logic.
 */

#include <kernel.h>
#include <kdata.h>
#include <printf.h>

#undef DEBUG

#ifdef SWAPDEV


uint16_t swappage;			/* Target page */

/* Linux-style slot table: one free list, one owner per slot, one release path.
 *
 *  swap_map[]     free-slot stack  (Linux: swap_info_struct.swap_map)
 *  nr_swap_free   depth of the free stack (Linux: nr_swap_pages)
 *  swap_state[]   SWAP_FREE / SWAP_USED, the idempotence guard
 *  swap_owner[]   the ptptr that currently owns a used slot (NULL = none)
 *
 * Every success or error path returns a slot through swap_free(); there is
 * exactly one release function in the whole tree. */
static uint8_t swap_map[MAX_SWAPS];
static uint_fast8_t nr_swap_free;
static uint8_t swap_state[MAX_SWAPS];
static ptptr swap_owner[MAX_SWAPS];

#define SWAP_FREE 0u
#define SWAP_USED 1u

static char maxswap[] = PANIC_MAXSWAP;

/* Linux: mm/swapfile.c swap_free().  Idempotent: a second free of the same
 * entry is a no-op, so any teardown path may call it safely. */
void swap_free(swp_entry_t entry)
{
	if (entry >= MAX_SWAPS || swap_state[entry] == SWAP_FREE)
		return;
	swap_state[entry] = SWAP_FREE;
	swap_owner[entry] = NULL;
	if (nr_swap_free == MAX_SWAPS)
		panic(maxswap);
	swap_map[nr_swap_free++] = (uint8_t)entry;
	sysinfo.swapusedk -= SWAP_SIZE / 2;
}

/* Linux: mm/swapfile.c folio_alloc_swap().  Returns SWP_ENTRY_NONE on
 * exhaustion, never 0 (slot 0 is a perfectly valid slot). */
swp_entry_t swap_alloc(void)
{
	if (nr_swap_free == 0)
		return SWP_ENTRY_NONE;
	swp_entry_t entry = swap_map[--nr_swap_free];
	swap_state[entry] = SWAP_USED;
	swap_owner[entry] = NULL;
	sysinfo.swapusedk += SWAP_SIZE / 2;
	return entry;
}

/* Record the process that owns a freshly allocated slot. */
void swap_hold(swp_entry_t entry, ptptr p)
{
	if (entry < MAX_SWAPS)
		swap_owner[entry] = p;
}

/* Linux: mm/swapfile.c put_swap_page().  Drop the slot held by p, if any. */
void put_swap_page(ptptr p)
{
	for (unsigned i = 0; i < MAX_SWAPS; i++) {
		if (swap_owner[i] == p) {
			swap_free((swp_entry_t)i);
			return;
		}
	}
}

void swap_init(swp_entry_t entry)
{
	if (entry >= MAX_SWAPS || swap_state[entry] != SWAP_FREE)
		panic(maxswap);
	/* Remove from the free stack if present, then re-add once. */
	for (uint_fast8_t i = 0; i < nr_swap_free; i++) {
		if (swap_map[i] == entry) {
			swap_map[i] = swap_map[--nr_swap_free];
			break;
		}
	}
	swap_map[nr_swap_free++] = (uint8_t)entry;
	sysinfo.swapk += SWAP_SIZE / 2;
}

/* We can re-use udata.u_block and friends as we can never be swapped while
   we are in the middle of an I/O (at least for now). If we rework the kernel
   for sleepable I/O this will change */

int swapread(uint16_t dev, blkno_t blkno, register usize_t nbytes,
                    uaddr_t buf, uint16_t page)
{
	int res;

	udata.u_dptr = swap_map(buf);
	udata.u_block = blkno;
	if (nbytes & BLKMASK)
		panic("swprd");
	udata.u_nblock = nbytes >> BLKSHIFT;
	swappage = page;
#ifdef DEBUG
	kprintf("SR | L %p M %p Bytes %u Page %u Block %u\n",
		buf, udata.u_dptr, nbytes, page, udata.u_block);
#endif
	res = ((*dev_tab[major(dev)].dev_read) (minor(dev), 2, 0));

#ifdef CONFIG_TRIM
	while (nbytes != 0)
	{
		d_ioctl(dev, HDIO_TRIM, (void*)&blkno);
		blkno++;
		nbytes -= 1<<BLKSHIFT;
	}
#endif

	return res;
}


int swapwrite(uint16_t dev, blkno_t blkno, usize_t nbytes,
		     uaddr_t buf, uint16_t page)
{
	/* FIXME: duplication here */
	udata.u_dptr = swap_map(buf);
	udata.u_block = blkno;
	if (nbytes & BLKMASK)
		panic("swpwr");
	udata.u_nblock = nbytes >> BLKSHIFT;
	swappage = page;
#ifdef DEBUG
	kprintf("SW | L %p M %p Bytes %u Page %u Block %u\n",
		buf, udata.u_dptr, nbytes, page, udata.u_block);
#endif
	return ((*dev_tab[major(dev)].dev_write) (minor(dev), 2, 0));
}


/*
 *	Swap out process. As we have them all the same size we ignore
 *	p for now. For a single memory image system most of this can go!
 */
static ptptr swapvictim(ptptr p, int notself)
{
#ifdef CONFIG_MULTI
	register ptptr c;
	register ptptr r = NULL;
	register ptptr f = NULL;
	register uint16_t sc = 0;
	uint16_t s;

	extern ptptr getproc_nextp;	/* Eww.. */

	c = getproc_nextp;

	/* FIXME: with the punishment flag we ought to also consider that */
	do {
		if (c->p_page && c != udata.u_ptab) {	/* No point swapping someone in swap! */
			/* We swapped it in but have not run it yet. Avoid thrashing */
			if (c->p_flags & PFL_SWAPIN)
				continue;
			/* Find the last entry before us */
			if (c->p_status == P_READY)
				r = c;
			if (c->p_status > P_READY
			    && c->p_status <= P_FORKING) {
				/* relative position in order of waits, bigger is longer, can wrap but
				   shouldn't really matter to us much if it does */
				s = (waitno - c->p_waitno);
				if (s >= sc) {
					sc = s;
					f = c;
				}
			}
		}
		c++;
		if (c == ptab_end)
			c = ptab;
	}
	while (c != getproc_nextp);

	/* Oldest waiter gets the boot */
	if (f) {
#ifdef DEBUG
		kprintf("swapvictim %x (page %d, state %d)\n", f,
			f->p_page, f->p_status);
#endif
		return f;
	}
	/* No waiters.. the scheduler cycles so we will be the last to run
	   again, failing that the one before us that was ready */
	if (notself == 0)
		return udata.u_ptab;
	return r;
#else
	used(p);
	if (notself)
		panic(PANIC_NOTSELF);
	return udata.u_ptab;
#endif
}

ptptr swapneeded(ptptr p, int notself)
{
	ptptr n = swapvictim(p, notself);
	if (n) {
		inswap = 1;
#ifdef CONFIG_PLATFORM_UDMA
		plt_udma_sync(n);
#endif
		if (swapout(n))
			n = NULL;
		inswap = 0;
	}
	return n;
}

void swapper2(register ptptr p, uint16_t map)
{
#ifdef DEBUG
	kprintf("Swapping in %p (page %d), udata.ptab %p\n", p, p->p_page,
		udata.u_ptab);
#endif
	/* inswap = 1/0 around the swapin to sort preempt ? */
	swapin(p, map);
	swap_free(map);
#ifdef DEBUG
	kprintf("Swapped in %p (page %d), udata.ptab %p\n",
		p, p->p_page, udata.u_ptab);
#endif
	udata.u_page = p->p_page;
	udata.u_page2 = p->p_page2;
	/* We booted it out onto disk, don't then not run it */
	p->p_flags &= ~PFL_BATCH;
}

/*
 *	Called from switchin when we discover that we want to run
 *	a swapped process. We let pagemap_alloc cause any needed swap
 *	out of idle processes.
 */
void swapper(register ptptr p)
{
        uint16_t map = p->p_page2;
	pagemap_alloc(p);	/* May cause a swapout. May also destroy
                                   the old value of p->page2 */
	swapper2(p, map);
}

#endif
