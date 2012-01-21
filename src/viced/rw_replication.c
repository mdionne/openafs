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
