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
# Copyright (c) 2013 by Delphix. All rights reserved.
#

MODULE = mdb_vmss.so
MDBTGT = kvm
MODULE_BUILD_TYPE = mdb

MODSRCS = mdb_vmss.c

include ../../../../../Makefile.cmd
include ../../../../../Makefile.cmd.64
include ../../../../intel/Makefile.amd64
include ../../../../Makefile.module

CSTD=		$(CSTD_GNU99)
C99LMODE=       -Xc99=%all

CPPFLAGS += -DMP -D_MACHDEP -D_MDB
CPPFLAGS += -U_KERNEL -D_KMEMUSER
CPPFLAGS += -I../../../../common
CPPFLAGS += -I$(SRC)/uts/common
CPPFLAGS += -I$(SRC)/uts/i86pc
CPPFLAGS += -I$(SRC)/uts/intel
