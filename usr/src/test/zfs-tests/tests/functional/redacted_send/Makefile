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

#
# Copyright (c) 2016 by Delphix. All rights reserved.
#

include $(SRC)/Makefile.master

ROOTOPTPKG = $(ROOT)/opt/zfs-tests
TESTDIR = $(ROOTOPTPKG)/tests/functional/redacted_send

PROGS = cleanup \
	redacted_compressed \
	redacted_contents  \
	redacted_disabled_feature \
	redacted_embedded \
	redacted_holes \
	redacted_incrementals \
	redacted_largeblocks \
	redacted_many_clones \
	redacted_mixed_recsize \
	redacted_mounts \
	redacted_negative \
	redacted_origin \
	redacted_resume \
	redacted_volume \
	setup

FILES = redacted.cfg \
	redacted.kshlib

CMDS = $(PROGS:%=$(TESTDIR)/%) $(FILES:%=$(TESTDIR)/%)
$(CMDS) := FILEMODE = 0555

all lint clean clobber:

install: $(CMDS)

$(CMDS): $(TESTDIR)

$(TESTDIR):
	$(INS.dir)

$(TESTDIR)/%: %.ksh
	$(INS.rename)

$(TESTDIR)/%: %
	$(INS.file)
