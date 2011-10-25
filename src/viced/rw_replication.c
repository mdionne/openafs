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
#define REPL_PROTOTYPES
#include "viced_prototypes.h"

#if defined(AFS_PTHREAD_ENV)
#include <pthread.h>
#endif

afs_uint32 queryDbserver;
#define MustBeDIR	2
#define MustNOTBeDIR	1

#if defined(AFS_PTHREAD_ENV)
pthread_key_t fs_update;
pthread_mutex_t remote_update_mutex;
pthread_mutex_t update_list_mutex;
#endif

struct AFSUpdateListItem *update_list_head = NULL;
struct AFSUpdateListItem *update_list_tail = NULL;

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

void
get_item(struct AFSUpdateListItem *item) {
#if defined(AFS_PTHREAD_ENV)
    pthread_mutex_lock(&item->item_lock);
    item->ref_count++;
    pthread_mutex_unlock(&item->item_lock);
#endif
}

/* Make dummy COnnection to make fileserver->fileserver RPC call*/
struct rx_connection *
MakeDummyConnection(afs_int32 serverIp)
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

afs_int32
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

void
PutReplicaVolumePackage(struct Vnode *targetptr, struct Vnode *parentptr, struct Volume *volptr)
{
    Error fileCode = 0;

    if (targetptr) {
	VPutVnode(&fileCode, targetptr);
    }
    if (parentptr) {
	VPutVnode(&fileCode, parentptr);
    }
    if (volptr) {
	VPutVolume(volptr);
    }
    return;
}

afs_int32
SRXAFS_RCreateFile(struct rx_call *acall, IN  AFSFid *DirFid, char *Name,
		AFSStoreStatus *InStatus, AFSFid *InFid, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSCallBack CallBack;
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RCreateFile call\n"));
    return SAFSS_CreateFile(acall, DirFid, Name, InStatus, InFid, &OutFidStatus,
	    &OutDirStatus, &CallBack, &Sync, REMOTE_RPC, clientViceId);
}

afs_int32
SRXAFS_RRemoveFile(struct rx_call *acall, IN  AFSFid *DirFid, char *Name,
	afs_int32 clientViceId)
{
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;
    struct AFSFid FileFid;

    ViceLog(0, ("Processing RRemoveFile call\n"));
    return SAFSS_RemoveFile(acall, DirFid, Name, &OutDirStatus, &Sync,
	    REMOTE_RPC, clientViceId, &FileFid);
}

afs_int32
SRXAFS_RRename(struct rx_call *acall, AFSFid *OldDirFid, char *OldName,
	AFSFid *NewDirFid, char *NewName, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutNewDirStatus;
    struct AFSFetchStatus OutOldDirStatus;
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RRename call\n"));
    return SAFSS_Rename(acall, OldDirFid, OldName, NewDirFid, NewName,
	    &OutOldDirStatus, &OutNewDirStatus, &Sync, REMOTE_RPC, clientViceId);
}

afs_int32
SRXAFS_RStoreData64(struct rx_call *acall, struct AFSFid *Fid,
	struct AFSStoreStatus *InStatus, afs_uint64 Pos, afs_uint64 Length,
	afs_uint64 FileLength, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutStatus;
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RStoreData64 call\n"));
    return SAFSS_StoreData64(acall, Fid, InStatus, Pos, Length, FileLength,
	    &OutStatus, &Sync, REMOTE_RPC, clientViceId);
}

afs_int32
SRXAFS_RRemoveDir(struct rx_call *acall, struct AFSFid *DirFid, char *Name, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;
    struct AFSFid RDirFid;

    ViceLog(0, ("Processing RRemoveDir call\n"));
    return SAFSS_RemoveDir(acall, DirFid, Name, &OutDirStatus, &Sync, REMOTE_RPC, clientViceId, &RDirFid);

}

afs_int32
SRXAFS_RMakeDir(struct rx_call *acall, struct AFSFid *DirFid, char *Name,
	struct AFSStoreStatus *InStatus, struct AFSFid *InFid, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSCallBack CallBack;
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RMakeDir call, calling SAFS_StoreACL\n"));

    return SAFSS_MakeDir(acall, DirFid, Name, InStatus, InFid, &OutFidStatus,
	    &OutDirStatus, &CallBack, &Sync, REMOTE_RPC, clientViceId);
}

afs_int32
SRXAFS_RStoreACL(struct rx_call *acall, struct AFSFid *Fid,
	struct AFSOpaque *AccessList)
{
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RStoreACL call, calling SAFSS_StoreACL\n"));

    return SAFSS_StoreACL(acall, Fid, AccessList, NULL, &Sync, REMOTE_RPC);
}

afs_int32
SRXAFS_RStoreStatus(struct rx_call *acall, struct AFSFid *Fid, struct AFSStoreStatus *InStatus,
	afs_int32 clientViceId)
{
    struct AFSFetchStatus OutStatus;
    struct AFSVolSync Sync;

    ViceLog(0, ("Processing RStoreStatus call, calling SAFSS_StoreStatus\n"));

    return SAFSS_StoreStatus(acall, Fid, InStatus, &OutStatus, &Sync, REMOTE_RPC, clientViceId);
}

afs_int32
SRXAFS_RSetVolumeStatus(struct rx_call *acall, afs_int32 avolid,
	AFSStoreVolumeStatus *StoreVolStatus, char *Name, char *OfflineMsg,
	afs_int32 clientViceId)
{
    ViceLog(0, ("Processing RSetVolumeStatus call, calling SAFSS_SetVolumeStatus\n"));

    return SAFSS_SetVolumeStatus(acall, avolid, StoreVolStatus, Name,
            OfflineMsg, NULL, REMOTE_RPC, clientViceId);

}

#if defined (AFS_PTHREAD_ENV)
static afs_int32
rw_StoreData64(struct rx_connection *rcon, struct AFSFid *Fid,
        struct AFSStoreStatus *InStatus, afs_uint64 Pos, afs_uint64 Length,
        afs_uint64 FileLength, afs_int32 clientViceId, char *StoreBuffer)
{
    struct rx_call *call;
    unsigned long bytes;
    char *pos;
    int code;

    /*
     * This is more complex than the other remote calls, since we
     * need to replay the rx_Write calls to the remote server.
     */
    ViceLog(0, ("Processing rw_StoreData64\n"));
    ViceLog(0, ("Calling StartRXAFS_RStoreData64\n"));
    call = rx_NewCall(rcon);
    code = StartRXAFS_RStoreData64(call, Fid, InStatus, Pos, Length, FileLength, clientViceId);
    /* Loop, sending data with rx_Write */
    ViceLog(0, ("Looping over rx_Write to send data.  Start StoreData64 returned %d\n", code));
    pos = StoreBuffer;
    while (Length > 0) {
	bytes = rx_Write(call, pos, Length);
	if (bytes != Length) {
	    code = rx_Error(call);
	    break;
	}
	Length -= bytes;
	pos += bytes;
    }
    ViceLog(0, ("Calling EndRXAFS_RStoreData64\n"));
    code = EndRXAFS_RStoreData64(call);
    ViceLog(0, ("End of rw_RStoreData64 returned %d\n", code));
    code = rx_EndCall(call, code);
    ViceLog(0, ("End of rx_EndCall returned %d\n", code));
    return code;
}
#endif

void
FS_PostProc(afs_int32 code)
{
#if defined(AFS_PTHREAD_ENV)
    struct vldbentry entry;
    int i;
    struct rx_connection *rcon;
    struct AFSUpdateListItem *item;
    struct AFSUpdateListItem *it, *prev;
    int dowait = 0;
#endif

#if defined(AFS_PTHREAD_ENV)
    item = pthread_getspecific(fs_update);
    if (item) {
	/* Need to wait for any earlier related update */
restart:
	UPDATE_LIST_LOCK;
	dowait = 0;
	for (it = update_list_head; it != NULL && it != item; it = it->NextItem) {
	    if (it->InFid1.Volume) {
		if (item->InFid1.Volume &&
			item->InFid1.Volume == it->InFid1.Volume &&
			item->InFid1.Vnode == it->InFid1.Vnode)
		    dowait = 1;
		if (item->InFid2.Volume &&
			item->InFid2.Volume == it->InFid1.Volume &&
			item->InFid2.Vnode == it->InFid1.Vnode)
		    dowait = 1;
	    }
	    if (it->InFid2.Volume) {
		if (item->InFid1.Volume &&
			item->InFid1.Volume == it->InFid2.Volume &&
			item->InFid1.Vnode == it->InFid2.Vnode)
		    dowait = 1;
		if (item->InFid2.Volume &&
			item->InFid2.Volume == it->InFid2.Volume &&
			item->InFid2.Vnode == it->InFid2.Vnode)
		    dowait = 1;
	    }
	    if (dowait) {
		/* Get a ref so it doesn't go away after we unlock */
		get_item(it);
		UPDATE_LIST_UNLOCK;
		pthread_mutex_lock(&it->item_lock);
		/* If we're the only remaining ref, the item has been removed.  move on */
		if (it->deleted) {
		    it->ref_count--;
		    if (it->ref_count == 0)
			free(it);
		    else
			pthread_mutex_unlock(&it->item_lock);
		    goto restart;
		}
		pthread_cond_wait(&it->item_cv, &it->item_lock);
		it->ref_count--;
		if (it->ref_count == 0)
		    free(it);
		else
		    pthread_mutex_unlock(&it->item_lock);
		goto restart;
	    }
	}
	UPDATE_LIST_UNLOCK;
	/* If no FID provided, use root vnode - for SetVolumeStatus */
	if (!item->InFid1.Volume) {
	    item->InFid1.Volume = item->Volid;
	    item->InFid1.Vnode = ROOTVNODE;
	    item->InFid1.Unique = 1;
	}
	GetSlaveServersForVolume(&item->InFid1, &entry);
	for (i = 0; i < entry.nServers; i++) {
	    if (entry.serverFlags[i] & 0x10) {
		/* make connections for each Slave */
		ViceLog(0, ("%p Calling remote\n", item));
		rcon = MakeDummyConnection(entry.serverNumber[i]);
		switch(item->RPCCall) {
		    case RPC_CreateFile:
			ViceLog(0, ("Calling remote CreateFile\n"));
			RXAFS_RCreateFile(rcon, &item->InFid1, item->Name1, &item->InStatus,
				&item->InFid2, item->ClientViceId);
			break;
		    case RPC_RemoveFile:
			ViceLog(0, ("Calling remote RemoveFile\n"));
			RXAFS_RRemoveFile(rcon, &item->InFid1, item->Name1, item->ClientViceId);
			break;
		    case RPC_Rename:
			ViceLog(0, ("Calling remote Rename\n"));
			RXAFS_RRename(rcon, &item->InFid1, item->Name1, &item->InFid2, item->Name2,
				item->ClientViceId);
			break;
		    case RPC_StoreData64:
			ViceLog(0, ("Calling remote StoreData64\n"));
			rw_StoreData64(rcon, &item->InFid1, &item->InStatus, item->Pos,
				item->Length, item->FileLength, item->ClientViceId,
				item->StoreBuffer);
			break;
		    case RPC_RemoveDir:
			ViceLog(0, ("Calling remote RemoveDir\n"));
			RXAFS_RRemoveDir(rcon, &item->InFid1, item->Name1, item->ClientViceId);
			break;
		    case RPC_MakeDir:
			ViceLog(0, ("Calling remote MakeDir\n"));
			RXAFS_RMakeDir(rcon, &item->InFid1, item->Name1, &item->InStatus,
				&item->InFid2, item->ClientViceId);
			break;
		    case RPC_StoreACL:
			ViceLog(0, ("Calling remote StoreACL\n"));
			RXAFS_RStoreACL(rcon, &item->InFid1, &item->AccessList);
			break;
		    case RPC_StoreStatus:
			ViceLog(0, ("Calling remote StoreStatus\n"));
			RXAFS_RStoreStatus(rcon, &item->InFid1, &item->InStatus, item->ClientViceId);
			break;
		    case RPC_SetVolumeStatus:
			ViceLog(0, ("Calling remote SetVolumeStatus\n"));
			RXAFS_RSetVolumeStatus(rcon, item->Volid, &item->StoreVolStatus,
				item->Name1, item->Name2, item->ClientViceId);
			break;
		    default:
			ViceLog(0, ("Warning: unhandled stashed RPC, op: %d\n", item->RPCCall));
		}
	    }
	}
	if (item->RPCCall == RPC_StoreData64 && item->StoreBuffer)
	    free(item->StoreBuffer);
	/* Remove item from list */
	UPDATE_LIST_LOCK;
	if (item == update_list_head) {
	    update_list_head = item->NextItem;
	    if (update_list_head == NULL)
		update_list_tail = NULL;
	} else {
	    prev = update_list_head;
	    for (it = update_list_head; it != NULL && it != item; it = it->NextItem)
		prev = it;
		ViceLog(0, ("Removing %p from list, prev: %p\n", item, prev));
	    prev->NextItem = item->NextItem;
	    if (item == update_list_tail)
		update_list_tail = prev;
	}
	pthread_mutex_lock(&item->item_lock);
	item->ref_count--;
	item->deleted = 1;
	pthread_cond_broadcast(&item->item_cv);
    ViceLog(0, ("*** %p DONE ***, UNLOCKING\n", item));
	if (item->ref_count == 0)
	    free(item);
	else
	    pthread_mutex_unlock(&item->item_lock);
	UPDATE_LIST_UNLOCK;
    } else {
    }
    pthread_setspecific(fs_update, NULL);
#endif
}

struct AFSUpdateListItem *
StashUpdate(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2, struct AFSStoreStatus *pInStatus,
	struct AFSOpaque *pAccessList, afs_uint64 pPos, afs_uint64 pLength,
	afs_uint64 pFileLength, afs_int32 pClientViceId, char *buf,
	afs_int32 volid, AFSStoreVolumeStatus *pStoreVolStatus)
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
    } else {
	item->InFid1.Volume = 0;
	item->InFid1.Vnode = 0;
	item->InFid1.Unique = 0;
    }
    if (pInFid2) {
	item->InFid2.Volume = pInFid2->Volume;
	item->InFid2.Vnode = pInFid2->Vnode;
	item->InFid2.Unique = pInFid2->Unique;
    } else {
	item->InFid2.Volume = 0;
	item->InFid2.Vnode = 0;
	item->InFid2.Unique = 0;
    }
    if (pName1) {
	item->Name1 = (char *)malloc(sizeof(char)*AFSNAMEMAX);
	if (!item->Name1) {
	    ViceLog(0,("Name1 allocate memory failed\n"));
	    return NULL;
	}
	strcpy(item->Name1,pName1);
    } else {
	item->Name1 = NULL;
    }
    if (pName2) {
	if (pRPCCall == RPC_SetVolumeStatus) {
	    item->Name2 = malloc(sizeof(char)*AFSOPAQUEMAX);
	}
	if (!item->Name2) {
	    ViceLog(0,("Name2 allocate memory failed\n"));
	    return NULL;
	}
	strcpy(item->Name2,pName2);
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
    if (pStoreVolStatus) {
	item->StoreVolStatus.Mask = pStoreVolStatus->Mask;
	item->StoreVolStatus.MinQuota = pStoreVolStatus->MinQuota;
	item->StoreVolStatus.MaxQuota = pStoreVolStatus->MaxQuota;
    } else {
	item->StoreVolStatus.Mask = 0;
	item->StoreVolStatus.MinQuota = 0;
	item->StoreVolStatus.MaxQuota = 0;
    }

    item->Pos = pPos;
    item->Length = pLength;
    item->FileLength = pFileLength;
    item->StoreBuffer = buf;
    item->Volid = volid;
#if defined(AFS_PTHREAD_ENV)
    /* Insert item at end of pending update list */
ViceLog(0,("%p : inserting in list\n", item));
    CV_INIT(&item->item_cv, "update item cv", CV_DEFAULT, 0);
    pthread_mutex_init(&item->item_lock, NULL);
    item->ref_count = 1;
    item->deleted = 0;
    UPDATE_LIST_LOCK;
    if (update_list_head == NULL && update_list_tail == NULL) {
	update_list_tail = update_list_head = item;
    } else {
	update_list_tail->NextItem = item;
	update_list_tail = item;
    }
    UPDATE_LIST_UNLOCK;
#endif

    return item;
}
