#ifndef _AFS_VICED_RW_REPLICATION_H
#define _AFS_VICED_RW_REPLICATION_H

/*
 * This structure holds all the details needed to forward an update
 * to remote RW replicas.
 * Some fields are only meaningful for certain types of updates.
 */
struct updateItem {
    afs_int32 rpcId;		/* RPC id for the remote call */
    struct AFSFid fid1;		/* File or Dir FID */
    struct AFSFid fid2;		/* Target FID (create/remove ops)
				   Target Dir (rename) */
    struct AFSFid delFid;	/* FID deleted by a Rename op */
    char *name1;
    char *name2;
    AFSStoreStatus status;
    AFSOpaque acl;		/* For StoreACL */
    afs_uint64 pos;		/* For StoreData */
    afs_uint64 length;		/* For StoreData */
    afs_uint64 fileLength;	/* For StoreData */
    afs_int32 clientViceId;
    struct updateItem *next; 	/* Pointer to next item in the list */
    char *storeBuf;		/* Buffer for StoreData data */
    afs_int32 volid;		/* Volume ID */
    AFSStoreVolumeStatus volStatus;

    /* Fields used for synchronisation */
#if defined(AFS_PTHREAD_ENV)
    pthread_mutex_t item_lock;
    pthread_cond_t item_cv;
    int ref_count;
    int deleted;
#endif
};

extern struct updateItem *update_list_head;
extern struct updateItem *update_list_tail;

#if defined(AFS_PTHREAD_ENV)
/* Thread local pointer to this thread's update */
extern pthread_key_t fs_update;

/* Lock to protect the list of pending updates */
extern pthread_mutex_t update_list_mutex;
#define UPDATE_LIST_LOCK \
        osi_Assert(pthread_mutex_lock(&update_list_mutex) == 0)
#define UPDATE_LIST_UNLOCK \
        osi_Assert(pthread_mutex_unlock(&update_list_mutex) == 0)
#endif

/* Define constants to identify update operations (rpcId field) */
#define RPC_StoreData 133
#define RPC_StoreACL 134
#define RPC_StoreStatus 135
#define RPC_RemoveFile 136
#define RPC_CreateFile 137
#define RPC_Rename 138
#define RPC_Symlink 139
#define RPC_Link 140
#define RPC_MakeDir 141
#define RPC_RemoveDir 142
#define RPC_SetVolumeStatus 150
#define RPC_StoreData64 65538

/*
 * Function prototypes
 *
 * Defined here instead of viced_prototypes.h because they can't be parsed by
 * some of its users.
 */
extern struct updateItem *stashUpdate(afs_int32 rpcId, struct AFSFid *fid1,
	struct AFSFid *fid2, char *name1, char *name2, struct AFSStoreStatus *status,
	struct AFSOpaque *acl, afs_uint64 pos, afs_uint64 length, afs_uint64 fileLen,
        afs_int32 clientViceId, char *buf, afs_int32 volid,
        AFSStoreVolumeStatus *volStatus, struct AFSFid *delFid);
extern afs_int32 repl_init(afs_uint32 bindhost, struct rx_securityClass **securityClasses,
	afs_int32 numClasses);

#endif /* _AFS_VICED_RW_REPLICATION_H */
