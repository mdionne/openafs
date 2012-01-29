#include <afsconfig.h>
#include <afs/param.h>
#include <afs/stds.h>

#include <roken.h>

#include <afs/afsrepl.h>
#include <afs/vlserver.h>
#include <afs/ihandle.h>
#include "viced.h"
#include "viced_prototypes.h"
#include <afs/vnode.h>
#include <afs/volume.h>
#include "rw_replication.h"

#if defined(AFS_PTHREAD_ENV)
#include <pthread.h>
pthread_key_t fs_update;
pthread_mutex_t update_list_mutex;
#endif

struct updateItem *update_list_head = NULL;
struct updateItem *update_list_tail = NULL;

/*
 * Add an update to the pending list of updates for RW replication.
 *
 * The parameters vary depending on the type of update; any unused
 * parameter should be set to NULL by the caller.
 *
 * Space for the item and any referenced strings and ACLs are allocated here.
 * Space for the StoreData buffer is assumed to be pre-allocated.
 *
 * Return the address of the new item.
 */
struct updateItem *
stashUpdate(afs_int32 rpcId, struct AFSFid *fid1, struct AFSFid *fid2, char *name1,
	char *name2, struct AFSStoreStatus *status, struct AFSOpaque *acl,
	afs_uint64 pos, afs_uint64 length, afs_uint64 fileLen,
	afs_int32 clientViceId, char *buf, afs_int32 volid,
	AFSStoreVolumeStatus *volStatus, struct AFSFid *delFid)
{
    struct updateItem *item;

    item = malloc(sizeof(struct updateItem));
    if (!item)
	return NULL;
    memset(item, 0, sizeof(struct updateItem));

    item->rpcId = rpcId;
    item->clientViceId = clientViceId;

    if (fid1)
	memcpy(&item->fid1, fid1, sizeof(struct AFSFid));

    if (fid2)
	memcpy(&item->fid2, fid2, sizeof(struct AFSFid));

    if (delFid)
	memcpy(&item->delFid, delFid, sizeof(struct AFSFid));

    if (name1) {
	item->name1 = malloc(sizeof(char)*AFSNAMEMAX);
	if (!item->name1)
	    return NULL;
	strncpy(item->name1, name1, AFSNAMEMAX);
    }

    if (name2) {
	if (rpcId == RPC_SetVolumeStatus) {
	    item->name2 = malloc(sizeof(char)*AFSOPAQUEMAX);
	    if (!item->name2)
		return NULL;
	    strncpy(item->name2, name2, AFSOPAQUEMAX);
	} else {
	    item->name2 = malloc(sizeof(char)*AFSNAMEMAX);
	    if (!item->name2)
		return NULL;
	    strncpy(item->name2, name2, AFSNAMEMAX);
	}
    }

    if (status)
	memcpy(&item->status, status, sizeof(struct AFSStoreStatus));

    if (acl) {
	item->acl.AFSOpaque_len = acl->AFSOpaque_len;
	item->acl.AFSOpaque_val = malloc(AFSOPAQUEMAX);
	if (!item->acl.AFSOpaque_val)
	    return NULL;
	strncpy(item->acl.AFSOpaque_val, acl->AFSOpaque_val, AFSOPAQUEMAX);
    }

    if (volStatus)
	memcpy(&item->volStatus, volStatus, sizeof(struct AFSStoreVolumeStatus));

    item->pos = pos;
    item->length = length;
    item->fileLength = fileLen;
    item->storeBuf = buf;
    item->volid = volid;

#if defined(AFS_PTHREAD_ENV)
    /* Initialize synchronization bits and insert item in update list */
    CV_INIT(&item->item_cv, "update item cv", CV_DEFAULT, 0);
    MUTEX_INIT(&item->item_lock, "update item lock", MUTEX_DEFAULT, 0);
    item->ref_count = 1;
    item->deleted = 0;
    UPDATE_LIST_LOCK;
    if (update_list_head == NULL && update_list_tail == NULL) {
	update_list_tail = update_list_head = item;
    } else {
	update_list_tail->next = item;
	update_list_tail = item;
    }
    UPDATE_LIST_UNLOCK;
#endif

    return item;
}

afs_int32
repl_init(afs_uint32 rx_bindhost, struct rx_securityClass **securityClasses,
	afs_int32 numClasses)
{
    struct rx_service *service;

    service = rx_NewServiceHost(rx_bindhost, 0, REPL_SERVICE_ID,
                                 "REPL", securityClasses, numClasses,
                                 REPL_ExecuteRequest);
    if (!service) {
        ViceLog(0,
                ("Failed to initialize REPL, probably two servers running.\n"));
        return -1;
    }
    rx_SetMinProcs(service, 2);
    rx_SetMaxProcs(service, 10);
    rx_SetCheckReach(service, 1);
    return 0;
}

afs_int32
SREPL_CreateFile(struct rx_call *acall, IN  AFSFid *DirFid, char *Name,
		AFSStoreStatus *InStatus, AFSFid *InFid, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSCallBack CallBack;
    struct AFSVolSync Sync;

    return SAFSS_CreateFile(acall, DirFid, Name, InStatus, InFid, &OutFidStatus,
	    &OutDirStatus, &CallBack, &Sync, 1, clientViceId);
}

afs_int32
SREPL_RemoveFile(struct rx_call *acall, IN  AFSFid *DirFid, char *Name,
	afs_int32 clientViceId)
{
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;

    return SAFSS_RemoveFile(acall, DirFid, Name, &OutDirStatus, &Sync,
	    1, clientViceId);
}

afs_int32
SREPL_Rename(struct rx_call *acall, AFSFid *OldDirFid, char *OldName,
	AFSFid *NewDirFid, char *NewName, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutNewDirStatus;
    struct AFSFetchStatus OutOldDirStatus;
    struct AFSVolSync Sync;

    return SAFSS_Rename(acall, OldDirFid, OldName, NewDirFid, NewName,
	    &OutOldDirStatus, &OutNewDirStatus, &Sync, 1, clientViceId);
}

afs_int32
SREPL_StoreData64(struct rx_call *acall, struct AFSFid *Fid,
	struct AFSStoreStatus *InStatus, afs_uint64 Pos, afs_uint64 Length,
	afs_uint64 FileLength, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutStatus;
    struct AFSVolSync Sync;

    return SAFSS_StoreData64(acall, Fid, InStatus, Pos, Length, FileLength,
	    &OutStatus, &Sync, 1, clientViceId);
}

afs_int32
SREPL_RemoveDir(struct rx_call *acall, struct AFSFid *DirFid, char *Name, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;
    struct AFSFid RDirFid;

    return SAFSS_RemoveDir(acall, DirFid, Name, &OutDirStatus, &Sync, 1, clientViceId, &RDirFid);
}

afs_int32
SREPL_MakeDir(struct rx_call *acall, struct AFSFid *DirFid, char *Name,
	struct AFSStoreStatus *InStatus, struct AFSFid *InFid,
	afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSCallBack CallBack;
    struct AFSVolSync Sync;

    return SAFSS_MakeDir(acall, DirFid, Name, InStatus, InFid, &OutFidStatus,
	    &OutDirStatus, &CallBack, &Sync, 1, clientViceId);
}

afs_int32
SREPL_StoreACL(struct rx_call *call, struct AFSFid *Fid,
	struct AFSOpaque *AccessList)
{
    struct AFSVolSync Sync;

    return SAFSS_StoreACL(call, Fid, AccessList, NULL, &Sync, 1);
}

afs_int32
SREPL_StoreStatus(struct rx_call *acall, struct AFSFid *Fid,
	struct AFSStoreStatus *InStatus, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutStatus;
    struct AFSVolSync Sync;

    return SAFSS_StoreStatus(acall, Fid, InStatus, &OutStatus, &Sync, 1,
	    clientViceId);
}

afs_int32
SREPL_SetVolumeStatus(struct rx_call *acall, afs_int32 avolid,
	AFSStoreVolumeStatus *StoreVolStatus, char *Name, char *OfflineMsg,
	afs_int32 clientViceId)
{
    return SAFSS_SetVolumeStatus(acall, avolid, StoreVolStatus, Name,
	    OfflineMsg, NULL, 1, clientViceId);
}

afs_int32
SREPL_Symlink(struct rx_call *acall, struct AFSFid *DirFid, char *Name,
	char *Link, struct AFSStoreStatus *InStatus, struct AFSFid *InFid,
	afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;

    return SAFSS_Symlink(acall, DirFid, Name, Link, InStatus, InFid,
	    &OutFidStatus, &OutDirStatus, &Sync, 1, clientViceId);
}

afs_int32
SREPL_Link(struct rx_call *acall, struct AFSFid *DirFid, char *Name,
	struct AFSFid *TargetFid, afs_int32 clientViceId)
{
    struct AFSFetchStatus OutFidStatus;
    struct AFSFetchStatus OutDirStatus;
    struct AFSVolSync Sync;

    return SAFSS_Link(acall, DirFid, Name, TargetFid, &OutFidStatus,
	    &OutDirStatus, &Sync, 1, clientViceId);
}

static int
repl_fidmatch(struct AFSFid fid1, struct AFSFid fid2) {
    if (fid1.Vnode == 0 || fid2.Vnode == 0)
	return 0;
    if (fid1.Vnode == fid2.Vnode && fid1.Volume == fid2.Volume)
	return 1;
    else
	return 0;
}

/*
 * Determine if there's an ordering dependency between two updates
 */
int
repl_depends_on(struct updateItem *it1, struct updateItem *it2) {
    switch (it1->rpcId) {
	case RPC_SetVolumeStatus:
		/* No conflict with anything */
		return 0;
	case RPC_StoreACL:
	case RPC_StoreData64:
	case RPC_StoreStatus:
		/* Wait for operations specific to this vnode */
		if (repl_fidmatch(it1->fid1, it2->fid1) &&
			(it2->rpcId == RPC_StoreACL ||
			it2->rpcId == RPC_StoreData64 ||
			it2->rpcId == RPC_StoreStatus))
		    return 1;
		if (repl_fidmatch(it1->fid1, it2->fid2) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		if (repl_fidmatch(it1->fid1, it2->delFid) &&
			it2->rpcId == RPC_Rename)
		    return 1;
		break;
	case RPC_Symlink:
	case RPC_CreateFile:
	case RPC_RemoveFile:
	case RPC_MakeDir:
		/* Wait for operations specific to this vnode */
		if (repl_fidmatch(it1->fid2, it2->fid1) &&
			(it2->rpcId == RPC_StoreACL ||
			it2->rpcId == RPC_StoreData64 ||
			it2->rpcId == RPC_StoreStatus))
		    return 1;
		if (repl_fidmatch(it1->fid2, it2->fid2) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		if (repl_fidmatch(it1->fid2, it2->delFid) &&
			it2->rpcId == RPC_Rename)
		    return 1;
		/* Wait for operations in same directory, but
		 * only if the names match */
		if (repl_fidmatch(it1->fid1, it2->fid1) &&
			!strcmp(it1->name1, it2->name1) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		/* Wait for major operations on parent directory */
		if (repl_fidmatch(it1->fid1, it2->fid2) &&
			(it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		/* Wait for rename that targets this file */
		if (repl_fidmatch(it1->fid1, it2->fid1) &&
			it2->rpcId == RPC_Rename &&
			!strcmp(it1->name1, it2->name1))
		    return 1;
		if (repl_fidmatch(it1->fid1, it2->fid2) &&
			it2->rpcId == RPC_Rename &&
			!strcmp(it1->name1, it2->name2))
		    return 1;
		break;
	case RPC_RemoveDir:
		/* Wait for operations specific to this vnode */
		if (repl_fidmatch(it1->fid2, it2->fid1) &&
			(it2->rpcId == RPC_StoreACL ||
			it2->rpcId == RPC_StoreData64 ||
			it2->rpcId == RPC_StoreStatus))
		    return 1;
		if (repl_fidmatch(it1->fid2, it2->fid2) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		/* Wait for operations specific to the directory */
		if (repl_fidmatch(it1->fid2, it2->fid1) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		break;
	case RPC_Rename:
		/* Wait for operations specific to the deleted file (if any) */
		if (repl_fidmatch(it1->delFid, it2->fid2) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		if (repl_fidmatch(it1->delFid, it2->fid1) &&
			(it2->rpcId == RPC_StoreACL ||
			it2->rpcId == RPC_StoreData64 ||
			it2->rpcId == RPC_StoreStatus))
		    return 1;
		/* Wait for operations specific to the directories */
		if ((repl_fidmatch(it1->fid1, it2->fid1) ||
			repl_fidmatch(it1->fid2, it2->fid1)) &&
			(it2->rpcId == RPC_Symlink ||
			it2->rpcId == RPC_CreateFile ||
			it2->rpcId == RPC_RemoveFile ||
			it2->rpcId == RPC_RemoveDir ||
			it2->rpcId == RPC_MakeDir))
		    return 1;
		/* Wait for other renames, same fid or same directories */
		if (it2->rpcId == RPC_Rename &&
			(repl_fidmatch(it1->fid1, it2->fid1) ||
			repl_fidmatch(it1->fid1, it2->fid2) ||
			repl_fidmatch(it1->fid2, it2->fid1) ||
			repl_fidmatch(it1->fid2, it2->fid2) ||
			repl_fidmatch(it1->delFid, it2->delFid)))
		    return 1;
		break;
    }
    return 0;
}
