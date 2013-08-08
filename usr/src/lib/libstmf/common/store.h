/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2009, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 */
#ifndef	_STORE_H
#define	_STORE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <libnvpair.h>

/* size of ascii hex 16 byte guid with NULL */
#define	GUID_STR_MIN_SIZE 33

int psAddHostGroupMember(char *groupName, char *memberName);
int psAddTargetGroupMember(char *groupName, char *memberName);
int psAddViewEntry(stmfGuid *lu, stmfViewEntry *viewEntry);
int psCreateHostGroup(char *groupName);
int psDeleteHostGroup(char *groupName);
int psCreateTargetGroup(char *groupName);
int psDeleteTargetGroup(char *groupName);
int psGetViewEntry(stmfGuid *lu, uint32_t viewEntryIndex, stmfViewEntry *ve);
int psGetLogicalUnitList(stmfGuidList **guidList);
int psRemoveHostGroupMember(char *groupName, char *memberName);
int psRemoveTargetGroupMember(char *groupName, char *memberName);
int psRemoveViewEntry(stmfGuid *lu, uint32_t viewEntryIndex);
int psGetHostGroupList(stmfGroupList **groupList);
int psGetTargetGroupList(stmfGroupList **groupList);
int psGetHostGroupMemberList(char *groupName, stmfGroupProperties **groupList);
int psGetTargetGroupMemberList(char *groupName,
    stmfGroupProperties **groupList);
int psGetViewEntryList(stmfGuid *lu, stmfViewEntryList **viewEntryList);
int psCheckService();
int psSetProviderData(char *providerName, nvlist_t *nvl, int providerType,
    uint64_t *setHandle);
int psGetProviderData(char *providerName, nvlist_t **nvl, int providerType,
    uint64_t *setHandle);
int psGetProviderDataList(stmfProviderList **providerList);
int psClearProviderData(char *providerName, int providerType);
int psSetServicePersist(uint8_t persistType);
int psGetServicePersist(uint8_t *persistType);
int psSetStmfProp(int propType, char *propVal);
int psGetStmfProp(int propType, char *propVal);
int psFormatGuid(stmfGuid *guid, char *guidAsciiBuf, size_t guidAsciiBufSize);

#ifdef	__cplusplus
}
#endif

#endif	/* _STORE_H */
