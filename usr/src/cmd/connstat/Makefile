#
# CDDL HEADER START
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#
# CDDL HEADER END
#
#
# Copyright (c) 2015 by Delphix. All rights reserved.
#

PROG=	connstat
OBJS=	connstat_main.o connstat_mib.o connstat_tcp.o
SRCS=	$(OBJS:%.o=%.c)
POFILES= connstat_main.po connstat_tcp.po connstat_mib.po
POFILE=	connstat.po

include ../Makefile.cmd

CSTD=	$(CSTD_GNU99)
LDLIBS += -lsocket -lnsl -linetutil -lumem
XGETFLAGS += -a -x $(PROG).xcl

#
# Instrument connstat with CTF data to ease debugging.
#
CTFCONVERT_HOOK = && $(CTFCONVERT_O)
CTFMERGE_HOOK = && $(CTFMERGE) -L VERSION -o $@ $(OBJS)
$(OBJS) := CFLAGS += $(CTF_FLAGS)

.KEEP_STATE:

all: $(PROG)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS) $(CTFMERGE_HOOK)
	$(POST_PROCESS)

$(POFILE): $(POFILES)
	$(RM) $@
	cat $(POFILES) > $@

install: all $(ROOTPROG)

clean:
	$(RM) $(OBJS)

lint:   lint_SRCS

include ../Makefile.targ
