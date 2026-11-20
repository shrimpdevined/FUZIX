/****************************************************
 * FUZIX kernel assertion framework (MASTER_PLAN M8)
 ****************************************************/

#ifndef __FUZIX__BUG_DOT_H__
#define __FUZIX__BUG_DOT_H__

#include <printf.h>		/* kprintf() */

/*
 * System-wide assertion macros.  Both are safe to use from kernel context:
 * they only go through kprintf() (the console choke point) and panic().
 *
 *   BUG_ON(cond)  - a "can't happen" invariant: panic() with file, line and
 *                   the failed expression.
 *   WARN_ON(cond) - a suspicious but recoverable condition: print a warning
 *                   with file and line, then carry on.
 *
 * cond is evaluated exactly once.  Do not put side effects in it.
 */

#define __BUG_STR1(x)	#x
#define __BUG_STR(x)	__BUG_STR1(x)

#define BUG_ON(cond)							   \
	do {								   \
		if (cond)						   \
			panic("BUG_ON " __FILE__ ":" __BUG_STR(__LINE__) \
			      " (" #cond ")");			   \
	} while (0)

#define WARN_ON(cond)							   \
	do {								   \
		if (cond)						   \
			kprintf("WARNING: %s:%d: WARN_ON(" #cond ")\n",    \
				__FILE__, __LINE__);			   \
	} while (0)

#endif /* __FUZIX__BUG_DOT_H__ */
