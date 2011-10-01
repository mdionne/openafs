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

afs_int32 DelayedPopFromUpdateList(void);
afs_int32 PushIntoUpdateList(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2,
	struct AFSStoreStatus *pInStatus, struct AFSVolSync *pSync,
	struct AFSOpaque *pAccessList, afs_uint64 pPos,
	afs_uint64 pLength, afs_uint64 pFileLength, char *storebuf,
	afs_int32 clientViceId);
int GetSlaveServersForVolume(struct AFSFid *Fid, struct vldbentry *entry);
struct rx_connection *MakeDummyConnection(afs_int32 serverIp);
void FS_PostProc(afs_int32 code);
struct AFSUpdateListItem *StashUpdate(afs_int32 pRPCCall, struct AFSFid *pInFid1,
        struct AFSFid *pInFid2, char *pName1, char *pName2, struct AFSStoreStatus *pInStatus,
        struct AFSVolSync *pSync, struct AFSOpaque *pAccessList,
        afs_uint64 pPos, afs_uint64 pLength, afs_uint64 pFileLength, afs_int32 pClientViceId);
afs_int32 GetReplicaVolumePackage(struct AFSFid *Fid, Volume **volptr,
	    Vnode **targetptr, int chkforDir, int locktype);
void PutReplicaVolumePackage(struct Vnode *targetptr, struct Volume *volptr);

#if defined(AFS_PTHREAD_ENV)
extern pthread_key_t fs_update;
#endif

#define RPC_StoreData 133
#define RPC_StoreACL 134
#define RPC_StoreStatus 135;
#define RPC_RemoveFile 136;
#define RPC_CreateFile 137;
#define RPC_Rename 138;
#define RPC_Symlink 139;
#define RPC_Link 140;
#define RPC_MakeDir 141;
#define RPC_RemoveDir 142;
#define RPC_StoreData64 65538;

#define LOCAL_RPC 0
#define REMOTE_RPC 1

#endif /* _AFS_VICED_RW_REPLICATION_H */
