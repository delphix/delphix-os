/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2017 by Delphix. All rights reserved.
 */

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/fs/zfs.h>

#include <libzfs.h>
libzfs_handle_t *g_zfs;

char *myopts[] = {
#define	CONSOLE 0
	"console",
#define	FSTYPE 1
	"fstype",
	NULL
};

void
usage()
{
	(void) fprintf(stderr, gettext("bootcfg <pool>\n"
	    "bootcfg [-R] [-d <filesystem>] [-c <maxboot>] [-o <opts>] <pool>\n"
	    "bootcfg -C <pool>\n"));
	exit(1);
}

int
main(int argc, char **argv)
{
	int error = 0;
	char c;
	boolean_t reset = B_FALSE;
	boolean_t clear = B_FALSE;
	char *nextboot = NULL;
	boolean_t set_maxboot = B_FALSE;
	unsigned long maxboot = 0;
	nvlist_t *opts = NULL;
	zpool_handle_t *zhp;

	g_zfs = libzfs_init();

	while ((c = getopt(argc, argv, ":RCd:c:o:")) != -1) {
		switch (c) {
		case 'C':
			clear = B_TRUE;
			break;
		case 'R':
			reset = B_TRUE;
			break;
		case 'd':
			nextboot = optarg;
			break;
		case 'c': {
			char *tail;
			errno = 0;
			maxboot = strtoul(optarg, &tail, 10);
			if (tail == optarg || *tail != '\0' ||
			    errno == ERANGE) {
				(void) fprintf(stderr,
				    gettext("invalid maxboot count \"%s\"\n"),
				    optarg);
				usage();
			}
			set_maxboot = B_TRUE;
			break;
		}
		case 'o':
			opts = fnvlist_alloc();
			while (*optarg != '\0') {
				char *value;
				int opt = getsubopt(&optarg, myopts, &value);
				switch (opt) {
				case CONSOLE:
				case FSTYPE: {
					const char *option = myopts[opt];
					if (value == NULL) {
						(void) fprintf(stderr,
						    gettext("no value provided "
						    "for %s\n"), option);
						usage();
					}

					fnvlist_add_string(opts, option, value);
					break;
				}
				default:
					(void) fprintf(stderr, gettext("bad "
					    "option token: %s\n"), value);
					usage();
				}
			}
			if (fnvlist_num_pairs(opts) == 0) {
				(void) fprintf(stderr, gettext("empty option "
				    "string\n"));
				usage();
			}
			break;
		default:
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc != 1) {
		(void) fprintf(stderr, gettext("incorrect number of"
		    " arguments\n"));
		usage();
	}

	if (clear && (reset || set_maxboot || opts != NULL ||
	    nextboot != NULL)) {
		(void) fprintf(stderr, gettext("cannot clear and set in same"
		    " operation\n"));
		usage();
	}
	if ((zhp = zpool_open_canfail(g_zfs, argv[0])) == NULL) {
		(void) fprintf(stderr, gettext("could not open pool\n"));
		usage();
	}
	libzfs_print_on_error(g_zfs, B_TRUE);

	if (clear) {
		error = zpool_set_nextboot(zhp, "", NULL, NULL);
		return (error);
	}

	if (reset && !set_maxboot && nextboot == NULL && opts == NULL) {
		error = zpool_reset_bootcount(zhp);
		return (error);
	}

	if (!set_maxboot && nextboot == NULL && opts == NULL) {
		char *cur_nextboot;
		nvlist_t *cur_opts;
		unsigned cur_maxboot;
		unsigned cur_bootcount;
		error = zpool_get_nextboot(zhp, &cur_nextboot, &cur_opts,
		    &cur_maxboot, &cur_bootcount);
		if (error != 0) {
			(void) fprintf(stderr, gettext("could not query state: "
			    "%s\n"), strerror(error));
			return (error);
		}
		(void) printf("nextboot dataset: %s\n", cur_nextboot);
		(void) printf("boot environment variables:[");
		for (nvpair_t *nvp = nvlist_next_nvpair(cur_opts, NULL);
		    NULL != nvp; nvp = nvlist_next_nvpair(cur_opts, nvp)) {
			(void) printf("\n\t%s: %s", nvpair_name(nvp),
			    fnvpair_value_string(nvp));
		}
		(void) printf("]\nmax bootcount: %d\n", cur_maxboot);
		(void) printf("current bootcount: %d\n", cur_bootcount);

		return (error);
	}

	/*
	 * Update nextboot config; for any parameters the user didn't provide,
	 * use the current values.
	 */
	char *cur_nextboot;
	nvlist_t *cur_opts;
	unsigned cur_maxboot, ignored;
	error = zpool_get_nextboot(zhp, &cur_nextboot, &cur_opts, &cur_maxboot,
	    &ignored);
	if (error != 0) {
		(void) fprintf(stderr, gettext("could not query state: %s\n"),
		    strerror(error));
		return (error);
	}
	if (nextboot == NULL)
		nextboot = cur_nextboot;
	if (opts == NULL)
		opts = cur_opts;
	if (!set_maxboot)
		maxboot = cur_maxboot;
	error = zpool_set_nextboot(zhp, nextboot, opts, maxboot);

	if (opts != NULL && opts != cur_opts)
		nvlist_free(opts);
	libzfs_fini(g_zfs);
	return (error);
}
