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

#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <roken.h>

#include <afs/afsint.h>
#include <afs/vlserver.h>
#include <afs/ihandle.h>
#include "viced.h"
#include <afs/vnode.h>
#include <afs/volume.h>
#include "rw_replication.h"

#if defined(AFS_PTHREAD_ENV)
#include <pthread.h>
#endif

afs_uint32 queryDbserver;
#define MustBeDIR	2
#define MustNOTBeDIR	1

afs_int32 CheckVnodeWithCall(AFSFid *fid, Volume **volptr, struct VCallByVol *cbv,
                   Vnode **vptr, int lock);

#if defined(AFS_PTHREAD_ENV)
pthread_key_t fs_update;
#endif

/* Get all the RWSL servers for the Volume */
/* TODO: we want to cache this info somewhere */
int
GetSlaveServersForVolume(struct AFSFid *Fid,
	struct vldbentry *entry)
{
    static struct rx_connection *vlConn = 0;
    static int down = 0;
    static afs_int32 lastDownTime = 0;
    struct rx_securityClass *vlSec;
    register afs_int32 code;

    ViceLog(0,("The VLDB is [%d]\n", queryDbserver));

    if (!vlConn) {
	vlSec = rxnull_NewClientSecurityObject();
	vlConn =
	    rx_NewConnection(queryDbserver, htons(7003), 52, vlSec, 0);
	rx_SetConnDeadTime(vlConn, 15); /* don't wait long */
    }
    if (down && (FT_ApproxTime() < lastDownTime + 180)) {
	ViceLog(0,("Some kind of Failure\n")); /**/
    }

    code = VL_GetEntryByID(vlConn, Fid->Volume, RWVOL, entry);
    if (code >= 0){
	down = 0;               /* call worked */
    }
    if (code) {
	if (code < 0) {
	    lastDownTime = FT_ApproxTime();     /* last time we tried an RPC */
	    down = 1;
	}
	ViceLog(0,("Another kind of Failure\n")); /**/
	return 1;
    }

    return 0;
}

/* Make dummy COnnection to make fileserver->fileserver RPC call*/
struct rx_connection *MakeDummyConnection(afs_int32 serverIp)
{
    struct rx_connection *tcon;
    struct rx_securityClass *sc;
    afs_uint32 ip = htonl(serverIp);

    /* Make a new connection */

    ViceLog(0,("Initial [%d], Later [%d]\n",serverIp,ip));

    sc = rxnull_NewClientSecurityObject();
    /*server2_ip = inet_addr(f_ip);*/

    tcon = rx_NewConnection(ip,htons(7000),1,sc,0);

    return tcon;
}

static afs_int32
GetReplicaVolumePackage(struct AFSFid *Fid, Volume **volptr,
	Vnode **targetptr, int chkforDir, int locktype)
{
    int errorCode = 0;         /* return code to caller */
    if ((errorCode = CheckVnodeWithCall(Fid, volptr, NULL, targetptr, locktype)))
        return (errorCode);
    if (chkforDir) {
        if (chkforDir == MustNOTBeDIR
                && ((*targetptr)->disk.type == vDirectory))
            return (EISDIR);
        else if (chkforDir == MustBeDIR
                && ((*targetptr)->disk.type != vDirectory))
            return (ENOTDIR);
    }
    return errorCode;
}

static void
PutReplicaVolumePackage(struct Vnode *targetptr, struct Volume *volptr)
{
    Error fileCode = 0;

    if (targetptr) {
	VPutVnode(&fileCode,targetptr);
    }
    if (volptr) {
	VPutVolume(volptr);
    }
    return;
}

afs_int32
SRXAFS_RStoreACL(struct rx_call *acall, struct AFSFid *Fid,
	struct AFSOpaque *AccessList, struct AFSVolSync *Sync)
{
    Vnode *vptr = 0;
    Volume *volptr = 0;

    ViceLog(0, ("Got RStoreACL call. Return success, no operation done.\n"));
    GetReplicaVolumePackage(Fid, &volptr, &vptr, MustBeDIR, WRITE_LOCK);
    PutReplicaVolumePackage(vptr, volptr);

    return 0;
}

void
FS_PostProc(afs_int32 code)
{
#if defined(AFS_PTHREAD_ENV)
    struct AFSUpdateListItem *item;
#endif

    ViceLog(0, ("FS_PostProc: we got called successfully.  RPC returned code %d\n", code));
#if defined(AFS_PTHREAD_ENV)
    item = pthread_getspecific(fs_update);
    if (item) {
	ViceLog(0, ("FS_PostProc: got update item to process, OP is %d\n", item->RPCCall));
    } else {
	ViceLog(0, ("FS_PostProc: no items to process\n"));
    }
    pthread_setspecific(fs_update, NULL);
    free(item);
#endif
}

struct AFSUpdateListItem *
StashUpdate(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2, struct AFSStoreStatus *pInStatus,
	struct AFSVolSync *pSync, struct AFSOpaque *pAccessList,
	afs_uint64 pPos, afs_uint64 pLength, afs_uint64 pFileLength, afs_int32 pClientViceId)
{
    struct AFSUpdateListItem *item;

    item = malloc(sizeof(struct AFSUpdateListItem));

    item->RPCCall = pRPCCall;
    item->ClientViceId = pClientViceId;
    item->NextItem = NULL;

    if (pInFid1) {
	item->InFid1.Volume = pInFid1->Volume;
	item->InFid1.Vnode = pInFid1->Vnode;
	item->InFid1.Unique = pInFid1->Unique;
    }
    if (pInFid2) {
	item->InFid2.Volume = pInFid2->Volume;
	item->InFid2.Vnode = pInFid2->Vnode;
	item->InFid2.Unique = pInFid2->Unique;
    }
    if (pName1) {
	if (strlen(pName1) > 0) {
	    item->Name1 = (char *)malloc(sizeof(char)*AFSNAMEMAX);
	    if (!item->Name1) {
		ViceLog(0,("Name1 allocate memory failed\n"));
		return NULL;
	    }
	    strcpy(item->Name1,pName1);
	}
    } else {
	item->Name1 = NULL;
    }
    if (pName2) {
	if (strlen(pName2) > 0) {
	    if (pRPCCall == 6) { /* Dont use constants !! */
		item->Name2 = (char *)malloc(sizeof(char)*AFSPATHMAX);
	    } else {
		item->Name2 = (char *)malloc(sizeof(char)*AFSNAMEMAX);
	    }
	    if (!item->Name2) {
		ViceLog(0,("Name2 allocate memory failed\n"));
                return NULL;
	    }
	    strcpy(item->Name2,pName2);
	}
    } else {
	item->Name2 = NULL;
    }

    if (pInStatus) {
	item->InStatus.Mask = pInStatus->Mask;
	item->InStatus.ClientModTime = pInStatus->ClientModTime;
	item->InStatus.Owner = pInStatus->Owner;
	item->InStatus.Group = pInStatus->Group;
	item->InStatus.UnixModeBits = pInStatus->UnixModeBits;
	item->InStatus.SegSize = pInStatus->SegSize;
    }
    if (pSync) {
	item->Sync.spare1 = pSync->spare1;
	item->Sync.spare2 = pSync->spare2;
	item->Sync.spare3 = pSync->spare3;
	item->Sync.spare4 = pSync->spare4;
	item->Sync.spare5 = pSync->spare5;
	item->Sync.spare6 = pSync->spare6;
    }
    if (pAccessList) {
	item->AccessList.AFSOpaque_len = pAccessList->AFSOpaque_len;
	/* Here we need to copy a string*/
	item->AccessList.AFSOpaque_val = malloc(AFSOPAQUEMAX);
	if (!item->AccessList.AFSOpaque_val) {
	    ViceLog(0,("AFSOpaque_val allocate memory failed\n"));
	    return NULL;
	}
	strcpy(item->AccessList.AFSOpaque_val,
	pAccessList->AFSOpaque_val);
    } else {
	item->AccessList.AFSOpaque_val = NULL;
    }

    item->Pos = pPos;
    item->Length = pLength;
    item->FileLength = pFileLength;

    return item;
}
