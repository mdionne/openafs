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
#include <afs/cellconfig.h>
#include "rw_replication.h"

#include <pthread.h>
pthread_key_t fs_update;
pthread_mutex_t update_list_mutex;

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
    ViceLog(0, ("Stashing update of type %d\n", rpcId));
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

    return item;
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

/*
 * Get RW replica information from the VLDB
 * Store the information in the volume structure.
 * It is not stored in the volume header on disk.
 *
 * The volume must be locked by the caller.
 */
int
repl_getVldb(Volume *vptr)
{
    afs_int32 code;
    int i;
    int have_rw = 0;
    struct repl_server *r_server, *n_server, *prev;
    struct vldbentry entry;
    char host[16];

    ViceLog(0, ("Getting VLDB entry for volume %u\n", vptr->header->diskstuff.id));
    code = ubik_VL_GetEntryByID(cstruct, 0, vptr->header->diskstuff.id,
	    RWVOL, &entry);
    if (code)
	return code;
    ViceLog(0, ("GetEntryByID returned %u\n", code));

    /* Wipe current info */
    for (r_server = n_server = vptr-> repl_servers; n_server; r_server = n_server) {
	n_server = r_server->next;
	free(r_server);
    }
    prev = NULL;
    for (i = 0; i < entry.nServers; i++) {
	ViceLog(0, ("site %d, flags: %u\n", i, entry.serverFlags[i]));
	ViceLog(0, ("site %d, addr: %s\n", i, afs_inet_ntoa_r(htonl(entry.serverNumber[i]), host)));
	if (entry.serverFlags[i] & VLSF_RWREPLICA) {
	    have_rw++;
	    r_server = malloc(sizeof(struct repl_server));
	    r_server->next = NULL;
	    r_server->addr = entry.serverNumber[i];
	    r_server->state = REPL_SERVER_OK;
	    if (!prev)
		vptr->repl_servers = r_server;
	    else
		prev->next = r_server;
	    prev = r_server;
	}
    }
    vptr->repl_flags &= ~REPL_FLAG_NEEDVLDB;

    if (!have_rw) {
	vptr->repl_status = REPL_NONE;
	ViceLog(0, ("Volume %u has no RW replicas.  Resetting repl_status\n", vptr->header->diskstuff.id));
    } else {
	ViceLog(0, ("Volume %u has %d RW replicas\n", vptr->header->diskstuff.id, have_rw));
	for (r_server = vptr-> repl_servers; r_server; r_server = r_server->next)
	    ViceLog(0, ("Vol %u replica on server %s\n", vptr->header->diskstuff.id, afs_inet_ntoa_r(htonl(r_server->addr), host)));
    }
    return 0;
}


/*
 * Determine if updates need to be stashed for the given volume.
 * - Get VLDB information if we don't already have it
 * - Check that at least one RW replica exists
 * - Check current replication status
 */
int
repl_checkStash(Volume *vptr) {
    /* Get info from VLDB if needed - no info or requested */

    ViceLog(0, ("In check stash, volume %u\n", vptr->header->diskstuff.id));
    ViceLog(0, ("repl_status: %d\n", vptr->repl_status));
    ViceLog(0, ("repl_flags: %d\n", vptr->repl_flags));

    if (vptr->repl_flags & REPL_FLAG_NEEDVLDB) {
	repl_getVldb(vptr);
	ViceLog(0, ("(after VLDB refresh) repl_status: %d\n", vptr->repl_status));
	ViceLog(0, ("(after VLDB refresh) repl_flags: %d\n", vptr->repl_flags));
    }
    if (vptr->repl_status == REPL_STASHING)
	ViceLog(0, ("Stashing, but not forwarding for volume %d\n", vptr->header->diskstuff.id));
    if (vptr->repl_status == REPL_ACTIVE)
	ViceLog(0, ("Stashing and forwarding for volume %d\n", vptr->header->diskstuff.id));
    if (vptr->repl_status == REPL_NONE)
	ViceLog(0, ("Volume %d has no RW replicas\n", vptr->header->diskstuff.id));
    if (vptr->repl_status == REPL_STOPPED || vptr->repl_status == REPL_NONE)
	return 0;
    else
	return 1;
}

void
get_item(struct updateItem *item)
{
    pthread_mutex_lock(&item->item_lock);
    item->ref_count++;
    pthread_mutex_unlock(&item->item_lock);
}

/* Make Connection to make fileserver->fileserver RPC call*/
struct rx_connection *
repl_MakeConnection(afs_int32 serverIp)
{
    struct rx_securityClass *sc;
    struct afsconf_dir *conf_dir;
    afs_uint32 ip = htonl(serverIp);
    afs_int32 scIndex;
    afs_int32 code;
    struct rx_connection *conn;

    conf_dir = afsconf_Open(AFSDIR_SERVER_ETC_DIRPATH);

    code = afsconf_ClientAuth(conf_dir, &sc, &scIndex);
    if (code)
	return NULL;
    conn = rx_NewConnection(ip, htons(7000), REPL_SERVICE_ID, sc, scIndex);

    afsconf_Close(conf_dir);
    rxs_Release(sc);
    return conn;
}

void
repl_PostProc(afs_int32 code)
{
    struct rx_connection *rcon;
    struct updateItem *item;
    struct updateItem *it, *prev;
    afs_int32 ret;
    Volume *vptr;
    struct repl_server *rserver;
    Error ec, client_ec;
    char host[16];

    item = pthread_getspecific(fs_update);
    ViceLog(0, ("In POstProc\n"));
    if (item) {
    ViceLog(0, ("In POstProc, have item\n"));
	/* Need to wait for any earlier related update */
restart:
	UPDATE_LIST_LOCK;
	for (it = update_list_head; it != NULL && it != item; it = it->next) {
	    if (repl_depends_on(item, it)) {
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
	if (!item->fid1.Volume) {
	    item->fid1.Volume = item->volid;
	    item->fid1.Vnode = ROOTVNODE;
	    item->fid1.Unique = 1;
	}
	vptr = VGetVolume(&ec, &client_ec, item->volid);
	ViceLog(0, ("In PostProc, got vptr %p, ec: %d, c_ec: %d\n", vptr, ec, client_ec));
	if (!vptr) return;
	for (rserver = vptr->repl_servers; rserver; rserver = rserver->next) {
	    ViceLog(0, ("In PostProc, replication server: %s\n", afs_inet_ntoa_r(htonl(rserver->addr), host)));
	    /* Only forward if replication is currently active */
	    if (vptr->repl_status == REPL_ACTIVE) {
		/* make connections for each Slave */
		rcon = repl_MakeConnection(rserver->addr);
		ViceLog(0, ("In PostProc, got rcon %p\n", rcon));
		switch(item->rpcId) {
		    case RPC_CreateFile:
			ret = REPL_CreateFile(rcon, &item->fid1, item->name1, &item->status,
				&item->fid2, item->clientViceId);
			break;
		    case RPC_RemoveFile:
			ret = REPL_RemoveFile(rcon, &item->fid1, item->name1, item->clientViceId);
			break;
		    case RPC_Rename:
			ret = REPL_Rename(rcon, &item->fid1, item->name1, &item->fid2, item->name2,
				item->clientViceId);
			break;
		    case RPC_StoreData64:
/*
			ret = rw_StoreData64(rcon, &item->fid1, &item->status, item->pos,
				item->length, item->fileLength, item->clientViceId,
				item->storeBuf);
*/
			ret = 0;
			break;
		    case RPC_RemoveDir:
			ret = REPL_RemoveDir(rcon, &item->fid1, item->name1, item->clientViceId);
			break;
		    case RPC_MakeDir:
			ret = REPL_MakeDir(rcon, &item->fid1, item->name1, &item->status,
				&item->fid2, item->clientViceId);
			break;
		    case RPC_StoreACL:
			ret = REPL_StoreACL(rcon, &item->fid1, &item->acl);
			break;
		    case RPC_StoreStatus:
			ret = REPL_StoreStatus(rcon, &item->fid1, &item->status, item->clientViceId);
			break;
		    case RPC_SetVolumeStatus:
			ret = REPL_SetVolumeStatus(rcon, item->volid, &item->volStatus,
				item->name1, item->name2, item->clientViceId);
			break;
		    case RPC_Symlink:
			ret = REPL_Symlink(rcon, &item->fid1, item->name1, item->name2,
				&item->status, &item->fid2, item->clientViceId);
			break;
		    case RPC_Link:
			ret = REPL_Link(rcon, &item->fid1, item->name1,
				&item->fid2, item->clientViceId);
			break;
		    default:
			ViceLog(0, ("Warning: unhandled stashed RPC, op: %d\n", item->rpcId));
			ret = -1;
		}
		if (ret != 0) {
		    ViceLog(0, ("Error return from remote RPC: %d\n", ret));
		    ViceLog(0, ("Disabling remote updates\n"));
		    /* TODO: remove specific server from list.  should mark replica as bad, break all callbacks */
		}
	    }
	}
	VPutVolume(vptr);
	if (item->rpcId == RPC_StoreData64 && item->storeBuf)
	    free(item->storeBuf);
	/* Remove item from list */
	UPDATE_LIST_LOCK;
	if (item == update_list_head) {
	    update_list_head = item->next;
	    if (update_list_head == NULL)
		update_list_tail = NULL;
	} else {
	    prev = update_list_head;
	    for (it = update_list_head; it != NULL && it != item; it = it->next)
		prev = it;
		ViceLog(0, ("Removing %p from list, prev: %p\n", item, prev));
	    prev->next = item->next;
	    if (item == update_list_tail)
		update_list_tail = prev;
	}
	pthread_mutex_lock(&item->item_lock);
	item->ref_count--;
	item->deleted = 1;
	pthread_cond_broadcast(&item->item_cv);
	if (item->ref_count == 0)
	    free(item);
	else
	    pthread_mutex_unlock(&item->item_lock);
	UPDATE_LIST_UNLOCK;
    } else {
    }
    pthread_setspecific(fs_update, NULL);
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
