/*
 * Copyright (c) 2011 Marc Dionne. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _AFS_VICED_RW_REPLICATION_H
#define _AFS_VICED_RW_REPLICATION_H

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <roken.h>

afs_int32 DelayedPopFromUpdateList(void);
afs_int32 PushIntoUpdateList(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2,
	struct AFSStoreStatus *pInStatus, struct AFSVolSync *pSync,
	struct AFSOpaque *pAccessList, afs_uint64 pPos,
	afs_uint64 pLength, afs_uint64 pFileLength, char *storebuf,
	afs_int32 clientViceId);

struct AFSUpdateListItem {
    afs_int32 RPCCall;
    struct AFSFid InFid1;
    struct AFSFid InFid2;
    char *Name1;
    char *Name2;
    AFSStoreStatus InStatus;
    AFSVolSync Sync;
    AFSOpaque AccessList;
    afs_uint64 Pos;
    afs_uint64 Length;
    afs_uint64 FileLength;
    afs_int32 ClientViceId;
    struct AFSUpdateListItem *NextItem;
    char *StoreBuffer;
};

#endif /* _AFS_VICED_RW_REPLICATION_H */
