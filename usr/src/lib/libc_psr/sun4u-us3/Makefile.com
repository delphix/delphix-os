#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"%Z%%M%	%I%	%E% SMI"
#

#
#	Create default so empty rules don't
#	confuse make
#

LIBRARY		= libc_psr.a
VERS		= .1

include $(SRC)/lib/Makefile.lib
include $(SRC)/Makefile.psm

#
# Since libc_psr is strictly assembly, deactivate the CTF build logic.
#
CTFCONVERT_POST	= :
CTFMERGE_LIB	= :

LIBS		= $(DYNLIB)
IFLAGS		= -I$(SRC)/lib/libc/inc -I$(SRC)/uts/sun4u \
		  -I$(ROOT)/usr/platform/sun4u/include
CPPFLAGS	= -D_REENTRANT -D$(MACH) $(IFLAGS) $(CPPFLAGS.master)
ASDEFS		= -D__STDC__ -D_ASM $(CPPFLAGS)
ASFLAGS		= -P $(ASDEFS)

#
# Used when building links in /platform/$(PLATFORM)/lib
#
LINKED_PLATFORMS	= SUNW,Sun-Blade-1000
LINKED_PLATFORMS	+= SUNW,Sun-Blade-1500
LINKED_PLATFORMS	+= SUNW,Sun-Blade-2500
LINKED_PLATFORMS	+= SUNW,A70
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V445
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V215
LINKED_PLATFORMS	+= SUNW,Netra-CP3010
LINKED_PLATFORMS	+= SUNW,Sun-Fire
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V240
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V250
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V440
LINKED_PLATFORMS	+= SUNW,Sun-Fire-280R
LINKED_PLATFORMS	+= SUNW,Sun-Fire-15000
LINKED_PLATFORMS	+= SUNW,Sun-Fire-880
LINKED_PLATFORMS	+= SUNW,Sun-Fire-480R
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V890
LINKED_PLATFORMS	+= SUNW,Sun-Fire-V490
LINKED_PLATFORMS	+= SUNW,Netra-T12
LINKED_PLATFORMS	+= SUNW,Netra-T4

#
# install rule
#
$(ROOT_PSM_LIB_DIR)/%: % $(ROOT_PSM_LIB_DIR)
	$(INS.file)

#
# build rules
#
pics/%.o: ../../$(PLATFORM)/common/%.s
	$(AS) $(ASFLAGS) $< -o $@
	$(POST_PROCESS_O)

pics/%.o: ../../$(COMPAT_PLAT)/common/%.s
	$(AS) $(ASFLAGS) $< -o $@
	$(POST_PROCESS_O)
