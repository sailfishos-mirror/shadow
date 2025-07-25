/*
 * SPDX-FileCopyrightText: 1990 - 1994, Julianne Frances Haugh
 * SPDX-FileCopyrightText: 1996 - 2000, Marek Michałkiewicz
 * SPDX-FileCopyrightText: 2002 - 2005, Tomasz Kłoczko
 * SPDX-FileCopyrightText: 2007 - 2008, Nicolas François
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "config.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "defines.h"
#include "prototypes.h"
#include "string/strcmp/streq.h"
#include "string/strcmp/strprefix.h"
#include "string/strspn/stpspn.h"
#include "string/strspn/stprspn.h"
#include "string/strtok/stpsep.h"


#ifndef SUAUTHFILE
#define SUAUTHFILE "/etc/suauth"
#endif

#define	NOACTION	0
#define	NOPWORD		1
#define	DENY		-1
#define	OWNPWORD	2

#ifdef SU_ACCESS

/* Really, I could do with a few const char's here defining all the
 * strings output to the user or the syslog. -- chris
 */
static int applies (const char *, char *);

static int isgrp (const char *, const char *);

static intmax_t lines = 0;


int
check_su_auth(const char *actual_id, const char *wanted_id, bool su_to_root)
{
	FILE *authfile_fd;
	char temp[1024];
	char *to_users;
	char *from_users;
	char *action;

	if (!(authfile_fd = fopen (SUAUTHFILE, "r"))) {
		int err = errno;
		/*
		 * If the file doesn't exist - default to the standard su
		 * behaviour (no access control).  If open fails for some
		 * other reason - maybe someone is trying to fool us with
		 * file descriptors limit etc., so deny access.  --marekm
		 */
		if (ENOENT == err) {
			return NOACTION;
		}
		SYSLOG ((LOG_ERR,
		         "could not open/read config file '%s': %s\n",
		         SUAUTHFILE, strerror (err)));
		return DENY;
	}

	while (fgets (temp, sizeof (temp), authfile_fd) != NULL) {
		char  *p;

		lines++;

		if (stpsep(temp, "\n") == NULL) {
			SYSLOG ((LOG_ERR,
				 "%s, line %jd: line too long or missing newline",
				 SUAUTHFILE, lines));
			continue;
		}

		stpcpy(stprspn(temp, " \t"), "");

		p = stpspn(temp, " \t");
		if (strprefix(p, "#") || streq(p, ""))
			continue;

		to_users = strsep(&p, ":");
		from_users = strsep(&p, ":");
		action = strsep(&p, ":");
		if (action == NULL || p != NULL) {
			SYSLOG ((LOG_ERR,
				 "%s, line %jd. Bad number of fields.\n",
				 SUAUTHFILE, lines));
			continue;
		}

		if (!applies (wanted_id, to_users))
			continue;
		if (!applies (actual_id, from_users))
			continue;
		if (streq(action, "DENY")) {
			SYSLOG ((su_to_root ? LOG_WARN : LOG_NOTICE,
				 "DENIED su from '%s' to '%s' (%s)\n",
				 actual_id, wanted_id, SUAUTHFILE));
			fputs (_("Access to su to that account DENIED.\n"),
			       stderr);
			fclose (authfile_fd);
			return DENY;
		} else if (streq(action, "NOPASS")) {
			SYSLOG ((su_to_root ? LOG_NOTICE : LOG_INFO,
				 "NO password asked for su from '%s' to '%s' (%s)\n",
				 actual_id, wanted_id, SUAUTHFILE));
			fputs (_("Password authentication bypassed.\n"),stderr);
			fclose (authfile_fd);
			return NOPWORD;
		} else if (streq(action, "OWNPASS")) {
			SYSLOG ((su_to_root ? LOG_NOTICE : LOG_INFO,
				 "su from '%s' to '%s': asking for user's own password (%s)\n",
				 actual_id, wanted_id, SUAUTHFILE));
			fputs (_("Please enter your OWN password as authentication.\n"),
			       stderr);
			fclose (authfile_fd);
			return OWNPWORD;
		} else {
			SYSLOG ((LOG_ERR,
				 "%s, line %jd: unrecognized action!\n",
				 SUAUTHFILE, lines));
		}
	}
	fclose (authfile_fd);
	return NOACTION;
}

static int
applies(const char *single, char *list)
{
	char *tok;

	int state = 0;

	while (NULL != (tok = strsep(&list, ", "))) {

		if (streq(tok, "ALL")) {
			if (state != 0) {
				SYSLOG ((LOG_ERR,
					 "%s, line %jd: ALL in bad place\n",
					 SUAUTHFILE, lines));
				return 0;
			}
			state = 1;
		} else if (streq(tok, "EXCEPT")) {
			if (state != 1) {
				SYSLOG ((LOG_ERR,
					 "%s, line %jd: EXCEPT in bas place\n",
					 SUAUTHFILE, lines));
				return 0;
			}
			state = 2;
		} else if (streq(tok, "GROUP")) {
			if ((state != 0) && (state != 2)) {
				SYSLOG ((LOG_ERR,
					 "%s, line %jd: GROUP in bad place\n",
					 SUAUTHFILE, lines));
				return 0;
			}
			state = (state == 0) ? 3 : 4;
		} else {
			switch (state) {
			case 0:	/* No control words yet */
				if (streq(tok, single))
					return 1;
				break;
			case 1:	/* An all */
				SYSLOG ((LOG_ERR,
					 "%s, line %jd: expect another token after ALL\n",
					 SUAUTHFILE, lines));
				return 0;
			case 2:	/* All except */
				if (streq(tok, single))
					return 0;
				break;
			case 3:	/* Group */
				if (isgrp (single, tok))
					return 1;
				break;
			case 4:	/* All except group */
				if (isgrp (single, tok))
					return 0;
				/* FALL THRU */
			}
		}
	}
	if ((state != 0) && (state != 3))
		return 1;
	return 0;
}

static int isgrp (const char *name, const char *group)
{
	struct group *grp;

	grp = getgrnam (group); /* local, no need for xgetgrnam */

	if (!grp || !grp->gr_mem)
		return 0;

	return is_on_list (grp->gr_mem, name);
}
#endif				/* SU_ACCESS */
