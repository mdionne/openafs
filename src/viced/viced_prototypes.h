/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#ifndef _AFS_VICED_VICED_PROTOTYPES_H
#define _AFS_VICED_VICED_PROTOTYPES_H

extern int sendBufSize;
afs_int32 sys_error_to_et(afs_int32 in);
void init_sys_error_to_et(void);

/* afsfileprocs.c */
extern int LogLevel;
extern afs_int32 BlocksSpare;
extern afs_int32 PctSpare;
extern afs_int32 CheckVnodeWithCall(AFSFid *fid, Volume **volptr,
	struct VCallByVol *cbv, Vnode **vptr, int lock);
#if defined(REPL_PROTOTYPES)
extern afs_int32 SAFSS_StoreACL(struct rx_call * acall, struct AFSFid * Fid,
	struct AFSOpaque * AccessList, struct AFSFetchStatus * OutStatus,
	struct AFSVolSync * Sync, int remote_flag);
extern afs_int32 SAFSS_MakeDir(struct rx_call *acall, struct AFSFid *DirFid,
	char *Name, struct AFSStoreStatus *InStatus, struct AFSFid *OutFid,
	struct AFSFetchStatus *OutFidStatus, struct AFSFetchStatus *OutDirStatus,
	struct AFSCallBack *CallBack, struct AFSVolSync *sync,
	int remote_flag, afs_int32 clientViceId);
extern afs_int32 SAFSS_RemoveDir(struct rx_call *acall, struct AFSFid *DirFid,
	char *Name, struct AFSFetchStatus *OutDirStatus, struct AFSVolSync *Sync,
	int remote_flag, afs_int32 clientViceId);
extern afs_int32 SAFSS_CreateFile(struct rx_call *acall, struct AFSFid *DirFid,
	char *Name, struct AFSStoreStatus *InStatus, struct AFSFid *Fid,
	struct AFSFetchStatus *OutFidStatus, struct AFSFetchStatus *OutDirStatus,
	struct AFSCallBack *CallBack, struct AFSVolSync *Sync,
	int remote_flag, afs_int32 clientViceId);
extern afs_int32 SAFSS_RemoveFile(struct rx_call *acall, struct AFSFid *DirFid,
	char *Name, struct AFSFetchStatus *OutDirStatus, struct AFSVolSync *Sync,
	int remote_flag, afs_int32 clientViceId);
#endif


/* callback.c */
extern int InitCallBack(int);
extern int BreakLaterCallBacks(void);
extern int BreakVolumeCallBacksLater(afs_uint32);

/* rw_replication.c */
extern void FS_PostProc(afs_int32 code);
#if defined(REPL_PROTOTYPES)
extern afs_int32 DelayedPopFromUpdateList(void);
extern afs_int32 PushIntoUpdateList(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2,
	struct AFSStoreStatus *pInStatus,
	struct AFSOpaque *pAccessList, afs_uint64 pPos,
	afs_uint64 pLength, afs_uint64 pFileLength, char *storebuf,
	afs_int32 clientViceId);
extern int GetSlaveServersForVolume(struct AFSFid *Fid, struct vldbentry *entry);
extern struct rx_connection *MakeDummyConnection(afs_int32 serverIp);
extern struct AFSUpdateListItem *StashUpdate(afs_int32 pRPCCall, struct AFSFid *pInFid1,
	struct AFSFid *pInFid2, char *pName1, char *pName2,
	struct AFSStoreStatus *pInStatus, struct AFSOpaque *pAccessList,
	afs_uint64 pPos, afs_uint64 pLength, afs_uint64 pFileLength,
	afs_int32 pClientViceId);
extern afs_int32 GetReplicaVolumePackage(struct AFSFid *Fid, Volume **volptr,
	Vnode **targetptr, int chkforDir, int locktype);
extern void PutReplicaVolumePackage(struct Vnode *targetptr, struct Vnode *parentptr,
	struct Volume *volptr);
#endif

#ifdef AFS_DEMAND_ATTACH_FS
/*
 * demand attach fs
 * fileserver state serialization
 */
extern int fs_stateSave(void);
extern int fs_stateRestore(void);
#endif /* AFS_DEMAND_ATTACH_FS */


#endif /* _AFS_VICED_VICED_PROTOTYPES_H */
