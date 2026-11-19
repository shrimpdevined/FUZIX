/*
 * shadow.h - shadow password database
 *
 * This file is part of FUZIX.
 */

#ifndef _SHADOW_H
#define _SHADOW_H

#ifndef __TYPES_H
#include <types.h>
#endif
#include <stdio.h>

struct spwd {
	char		*sp_namp;	/* login name */
	char		*sp_pwdp;	/* encrypted password */
	long		sp_lstchg;	/* last change, days since epoch */
	long		sp_min;		/* minimum days between changes */
	long		sp_max;		/* maximum days between changes */
	long		sp_warn;	/* days of warning before expiry */
	long		sp_inact;	/* days after expiry until disabled */
	long		sp_expire;	/* account expiry, days since epoch */
	unsigned long	sp_flag;	/* reserved */
};

extern char *_path_shadow;

extern void setspent(void);
extern void endspent(void);
extern struct spwd *getspent(void);
extern struct spwd *fgetspent(FILE *__file);
extern struct spwd *getspnam(const char *__name);
extern int putspent(const struct spwd *__sp, FILE *__f);

#endif /* _SHADOW_H */
