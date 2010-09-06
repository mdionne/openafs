/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 *
 * Portions Copyright (c) 2005-2008 Sine Nomine Associates
 */

/* 1/1/89: NB:  this stuff is all going to be replaced.  Don't take it too seriously */
/*

	System:		VICE-TWO
	Module:		volume.c
	Institution:	The Information Technology Center, Carnegie-Mellon University

 */

#include <afsconfig.h>
#include <afs/param.h>


#include <rx/xdr.h>
#include <afs/afsint.h>
#include <ctype.h>
#include <signal.h>
#ifndef AFS_NT40_ENV
#include <sys/param.h>
#if !defined(AFS_SGI_ENV)
#ifdef	AFS_OSF_ENV
#include <ufs/fs.h>
#else /* AFS_OSF_ENV */
#ifdef AFS_VFSINCL_ENV
#define VFS
#ifdef	AFS_SUN5_ENV
#include <sys/fs/ufs_fs.h>
#else
#if defined(AFS_DARWIN_ENV) || defined(AFS_XBSD_ENV)
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#else
#include <ufs/fs.h>
#endif
#endif
#else /* AFS_VFSINCL_ENV */
#if !defined(AFS_AIX_ENV) && !defined(AFS_LINUX20_ENV) && !defined(AFS_XBSD_ENV)
#include <sys/fs.h>
#endif
#endif /* AFS_VFSINCL_ENV */
#endif /* AFS_OSF_ENV */
#endif /* AFS_SGI_ENV */
#endif /* AFS_NT40_ENV */
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#ifdef AFS_NT40_ENV
#include <fcntl.h>
#else
#include <sys/file.h>
#endif
#include <dirent.h>
#ifdef	AFS_AIX_ENV
#include <sys/vfs.h>
#include <fcntl.h>
#else
#ifdef	AFS_HPUX_ENV
#include <fcntl.h>
#include <mntent.h>
#else
#if	defined(AFS_SUN_ENV) || defined(AFS_SUN5_ENV)
#ifdef	AFS_SUN5_ENV
#include <sys/mnttab.h>
#include <sys/mntent.h>
#else
#include <mntent.h>
#endif
#else
#ifndef AFS_NT40_ENV
#if defined(AFS_SGI_ENV)
#include <fcntl.h>
#include <mntent.h>

#else
#ifndef AFS_LINUX20_ENV
#include <fstab.h>		/* Need to find in libc 5, present in libc 6 */
#endif
#endif
#endif /* AFS_SGI_ENV */
#endif
#endif /* AFS_HPUX_ENV */
#endif
#ifndef AFS_NT40_ENV
#include <netdb.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <setjmp.h>
#ifndef ITIMER_REAL
#include <sys/time.h>
#endif /* ITIMER_REAL */
#endif /* AFS_NT40_ENV */
#if defined(AFS_SUN5_ENV) || defined(AFS_NT40_ENV) || defined(AFS_LINUX20_ENV)
#include <string.h>
#else
#include <strings.h>
#endif

#include "nfs.h"
#include <afs/errors.h>
#include "lock.h"
#include "lwp.h"
#include <afs/afssyscalls.h>
#include "ihandle.h"
#include <afs/afsutil.h>
#ifdef AFS_NT40_ENV
#include <io.h>
#endif
#include "daemon_com.h"
#include "fssync.h"
#include "salvsync.h"
#include "vnode.h"
#include "volume.h"
#include "partition.h"
#include "volume_inline.h"
#include "common.h"

#ifdef AFS_PTHREAD_ENV
#include <assert.h>
#else /* AFS_PTHREAD_ENV */
#include "afs/assert.h"
#endif /* AFS_PTHREAD_ENV */
#include "vutils.h"
#ifndef AFS_NT40_ENV
#include <afs/dir.h>
#include <unistd.h>
#endif

#if !defined(offsetof)
#include <stddef.h>
#endif

#ifdef O_LARGEFILE
#define afs_stat	stat64
#define afs_fstat	fstat64
#define afs_open	open64
#else /* !O_LARGEFILE */
#define afs_stat	stat
#define afs_fstat	fstat
#define afs_open	open
#endif /* !O_LARGEFILE */

#ifdef AFS_PTHREAD_ENV
pthread_mutex_t vol_glock_mutex;
pthread_mutex_t vol_trans_mutex;
pthread_cond_t vol_put_volume_cond;
pthread_cond_t vol_sleep_cond;
pthread_cond_t vol_init_attach_cond;
int vol_attach_threads = 1;
#endif /* AFS_PTHREAD_ENV */

/* start-time configurable I/O parameters */
ih_init_params vol_io_params;

#ifdef AFS_DEMAND_ATTACH_FS
pthread_mutex_t vol_salvsync_mutex;

/*
 * Set this to 1 to disallow SALVSYNC communication in all threads; used
 * during shutdown, since the salvageserver may have gone away.
 */
static volatile sig_atomic_t vol_disallow_salvsync = 0;
#endif /* AFS_DEMAND_ATTACH_FS */

#ifdef	AFS_OSF_ENV
extern void *calloc(), *realloc();
#endif

/* Forward declarations */
static Volume *attach2(Error * ec, VolId volumeId, char *path,
		       struct DiskPartition64 *partp, Volume * vp,
		       int isbusy, int mode);
static void ReallyFreeVolume(Volume * vp);
#ifdef AFS_DEMAND_ATTACH_FS
static void FreeVolume(Volume * vp);
#else /* !AFS_DEMAND_ATTACH_FS */
#define FreeVolume(vp) ReallyFreeVolume(vp)
static void VScanUpdateList(void);
#endif /* !AFS_DEMAND_ATTACH_FS */
static void VInitVolumeHeaderCache(afs_uint32 howMany);
static int GetVolumeHeader(Volume * vp);
static void ReleaseVolumeHeader(struct volHeader *hd);
static void FreeVolumeHeader(Volume * vp);
static void AddVolumeToHashTable(Volume * vp, int hashid);
static void DeleteVolumeFromHashTable(Volume * vp);
#if 0
static int VHold(Volume * vp);
#endif
static int VHold_r(Volume * vp);
static void VGetBitmap_r(Error * ec, Volume * vp, VnodeClass class);
static void VReleaseVolumeHandles_r(Volume * vp);
static void VCloseVolumeHandles_r(Volume * vp);
static void LoadVolumeHeader(Error * ec, Volume * vp);
static int VCheckOffline(Volume * vp);
static int VCheckDetach(Volume * vp);
static Volume * GetVolume(Error * ec, Error * client_ec, VolId volumeId, Volume * hint, int flags);

int LogLevel;			/* Vice loglevel--not defined as extern so that it will be
				 * defined when not linked with vice, XXXX */
ProgramType programType;	/* The type of program using the package */
static VolumePackageOptions vol_opts;

/* extended volume package statistics */
VolPkgStats VStats;

#ifdef VOL_LOCK_DEBUG
pthread_t vol_glock_holder = 0;
#endif


#define VOLUME_BITMAP_GROWSIZE	16	/* bytes, => 128vnodes */
					/* Must be a multiple of 4 (1 word) !! */

/* this parameter needs to be tunable at runtime.
 * 128 was really inadequate for largish servers -- at 16384 volumes this
 * puts average chain length at 128, thus an average 65 deref's to find a volptr.
 * talk about bad spatial locality...
 *
 * an AVL or splay tree might work a lot better, but we'll just increase
 * the default hash table size for now
 */
#define DEFAULT_VOLUME_HASH_SIZE 256   /* Must be a power of 2!! */
#define DEFAULT_VOLUME_HASH_MASK (DEFAULT_VOLUME_HASH_SIZE-1)
#define VOLUME_HASH(volumeId) (volumeId&(VolumeHashTable.Mask))

/*
 * turn volume hash chains into partially ordered lists.
 * when the threshold is exceeded between two adjacent elements,
 * perform a chain rebalancing operation.
 *
 * keep the threshold high in order to keep cache line invalidates
 * low "enough" on SMPs
 */
#define VOLUME_HASH_REORDER_THRESHOLD 200

/*
 * when possible, don't just reorder single elements, but reorder
 * entire chains of elements at once.  a chain of elements that
 * exceed the element previous to the pivot by at least CHAIN_THRESH
 * accesses are moved in front of the chain whose elements have at
 * least CHAIN_THRESH less accesses than the pivot element
 */
#define VOLUME_HASH_REORDER_CHAIN_THRESH (VOLUME_HASH_REORDER_THRESHOLD / 2)

#include "rx/rx_queue.h"


VolumeHashTable_t VolumeHashTable = {
    DEFAULT_VOLUME_HASH_SIZE,
    DEFAULT_VOLUME_HASH_MASK,
    NULL
};


static void VInitVolumeHash(void);


#ifndef AFS_HAVE_FFS
/* This macro is used where an ffs() call does not exist. Was in util/ffs.c */
ffs(x)
{
    afs_int32 ffs_i;
    afs_int32 ffs_tmp = x;
    if (ffs_tmp == 0)
	return (-1);
    else
	for (ffs_i = 1;; ffs_i++) {
	    if (ffs_tmp & 1)
		return (ffs_i);
	    else
		ffs_tmp >>= 1;
	}
}
#endif /* !AFS_HAVE_FFS */

#ifdef AFS_PTHREAD_ENV
/**
 * disk partition queue element
 */
typedef struct diskpartition_queue_t {
    struct rx_queue queue;             /**< queue header */
    struct DiskPartition64 *diskP;     /**< disk partition table entry */
} diskpartition_queue_t;

#ifndef AFS_DEMAND_ATTACH_FS

typedef struct vinitvolumepackage_thread_t {
    struct rx_queue queue;
    pthread_cond_t thread_done_cv;
    int n_threads_complete;
} vinitvolumepackage_thread_t;
static void * VInitVolumePackageThread(void * args);

#else  /* !AFS_DEMAND_ATTTACH_FS */
#define VINIT_BATCH_MAX_SIZE 512

/**
 * disk partition work queue
 */
struct partition_queue {
    struct rx_queue head;              /**< diskpartition_queue_t queue */
    pthread_mutex_t mutex;
    pthread_cond_t cv;
};

/**
 * volumes parameters for preattach
 */
struct volume_init_batch {
    struct rx_queue queue;               /**< queue header */
    int thread;                          /**< posting worker thread */
    int last;                            /**< indicates thread is done */
    int size;                            /**< number of volume ids in batch */
    Volume *batch[VINIT_BATCH_MAX_SIZE]; /**< volumes ids to preattach */
};

/**
 * volume parameters work queue
 */
struct volume_init_queue {
    struct rx_queue head;                /**< volume_init_batch queue */
    pthread_mutex_t mutex;
    pthread_cond_t cv;
};

/**
 * volume init worker thread parameters
 */
struct vinitvolumepackage_thread_param {
    int nthreads;                        /**< total number of worker threads */
    int thread;                          /**< thread number for this worker thread */
    struct partition_queue *pq;          /**< queue partitions to scan */
    struct volume_init_queue *vq;        /**< queue of volume to preattach */
};

static void *VInitVolumePackageThread(void *args);
static struct DiskPartition64 *VInitNextPartition(struct partition_queue *pq);
static VolId VInitNextVolumeId(DIR *dirp);
static int VInitPreAttachVolumes(int nthreads, struct volume_init_queue *vq);

#endif /* !AFS_DEMAND_ATTACH_FS */
#endif /* AFS_PTHREAD_ENV */

#ifndef AFS_DEMAND_ATTACH_FS
static int VAttachVolumesByPartition(struct DiskPartition64 *diskP,
				     int * nAttached, int * nUnattached);
#endif /* AFS_DEMAND_ATTACH_FS */


#ifdef AFS_DEMAND_ATTACH_FS
/* demand attach fileserver extensions */

/* XXX
 * in the future we will support serialization of VLRU state into the fs_state
 * disk dumps
 *
 * these structures are the beginning of that effort
 */
struct VLRU_DiskHeader {
    struct versionStamp stamp;            /* magic and structure version number */
    afs_uint32 mtime;                     /* time of dump to disk */
    afs_uint32 num_records;               /* number of VLRU_DiskEntry records */
};

struct VLRU_DiskEntry {
    afs_uint32 vid;                       /* volume ID */
    afs_uint32 idx;                       /* generation */
    afs_uint32 last_get;                  /* timestamp of last get */
};

struct VLRU_StartupQueue {
    struct VLRU_DiskEntry * entry;
    int num_entries;
    int next_idx;
};

typedef struct vshutdown_thread_t {
    struct rx_queue q;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    pthread_cond_t master_cv;
    int n_threads;
    int n_threads_complete;
    int vol_remaining;
    int schedule_version;
    int pass;
    byte n_parts;
    byte n_parts_done_pass;
    byte part_thread_target[VOLMAXPARTS+1];
    byte part_done_pass[VOLMAXPARTS+1];
    struct rx_queue * part_pass_head[VOLMAXPARTS+1];
    int stats[4][VOLMAXPARTS+1];
} vshutdown_thread_t;
static void * VShutdownThread(void * args);


static Volume * VAttachVolumeByVp_r(Error * ec, Volume * vp, int mode);
static int VCheckFree(Volume * vp);

/* VByP List */
static void AddVolumeToVByPList_r(Volume * vp);
static void DeleteVolumeFromVByPList_r(Volume * vp);
static void VVByPListBeginExclusive_r(struct DiskPartition64 * dp);
static void VVByPListEndExclusive_r(struct DiskPartition64 * dp);
static void VVByPListWait_r(struct DiskPartition64 * dp);

/* online salvager */
static int VCheckSalvage(Volume * vp);
#if defined(SALVSYNC_BUILD_CLIENT) || defined(FSSYNC_BUILD_CLIENT)
static int VScheduleSalvage_r(Volume * vp);
#endif

/* Volume hash table */
static void VReorderHash_r(VolumeHashChainHead * head, Volume * pp, Volume * vp);
static void VHashBeginExclusive_r(VolumeHashChainHead * head);
static void VHashEndExclusive_r(VolumeHashChainHead * head);
static void VHashWait_r(VolumeHashChainHead * head);

/* shutdown */
static int ShutdownVByPForPass_r(struct DiskPartition64 * dp, int pass);
static int ShutdownVolumeWalk_r(struct DiskPartition64 * dp, int pass,
				struct rx_queue ** idx);
static void ShutdownController(vshutdown_thread_t * params);
static void ShutdownCreateSchedule(vshutdown_thread_t * params);

/* VLRU */
static void VLRU_ComputeConstants(void);
static void VInitVLRU(void);
static void VLRU_Init_Node_r(Volume * vp);
static void VLRU_Add_r(Volume * vp);
static void VLRU_Delete_r(Volume * vp);
static void VLRU_UpdateAccess_r(Volume * vp);
static void * VLRU_ScannerThread(void * args);
static void VLRU_Scan_r(int idx);
static void VLRU_Promote_r(int idx);
static void VLRU_Demote_r(int idx);
static void VLRU_SwitchQueues(Volume * vp, int new_idx, int append);

/* soft detach */
static int VCheckSoftDetach(Volume * vp, afs_uint32 thresh);
static int VCheckSoftDetachCandidate(Volume * vp, afs_uint32 thresh);
static int VSoftDetachVolume_r(Volume * vp, afs_uint32 thresh);


pthread_key_t VThread_key;
VThreadOptions_t VThread_defaults = {
    0                           /**< allow salvsync */
};
#endif /* AFS_DEMAND_ATTACH_FS */


struct Lock vol_listLock;	/* Lock obtained when listing volumes:
				 * prevents a volume from being missed
				 * if the volume is attached during a
				 * list volumes */


/* Common message used when the volume goes off line */
char *VSalvageMessage =
    "Files in this volume are currently unavailable; call operations";

int VInit;			/* 0 - uninitialized,
				 * 1 - initialized but not all volumes have been attached,
				 * 2 - initialized and all volumes have been attached,
				 * 3 - initialized, all volumes have been attached, and
				 * VConnectFS() has completed. */

static int vinit_attach_abort = 0;

bit32 VolumeCacheCheck;		/* Incremented everytime a volume goes on line--
				 * used to stamp volume headers and in-core
				 * vnodes.  When the volume goes on-line the
				 * vnode will be invalidated
				 * access only with VOL_LOCK held */




/***************************************************/
/* Startup routines                                */
/***************************************************/

#if defined(FAST_RESTART) && defined(AFS_DEMAND_ATTACH_FS)
# error FAST_RESTART and DAFS are incompatible. For the DAFS equivalent \
        of FAST_RESTART, use the -unsafe-nosalvage fileserver argument
#endif

/**
 * assign default values to a VolumePackageOptions struct.
 *
 * Always call this on a VolumePackageOptions struct first, then set any
 * specific options you want, then call VInitVolumePackage2.
 *
 * @param[in]  pt   caller's program type
 * @param[out] opts volume package options
 */
void
VOptDefaults(ProgramType pt, VolumePackageOptions *opts)
{
    opts->nLargeVnodes = opts->nSmallVnodes = 5;
    opts->volcache = 0;

    opts->canScheduleSalvage = 0;
    opts->canUseFSSYNC = 0;
    opts->canUseSALVSYNC = 0;

#ifdef FAST_RESTART
    opts->unsafe_attach = 1;
#else /* !FAST_RESTART */
    opts->unsafe_attach = 0;
#endif /* !FAST_RESTART */

    switch (pt) {
    case fileServer:
	opts->canScheduleSalvage = 1;
	opts->canUseSALVSYNC = 1;
	break;

    case salvageServer:
	opts->canUseFSSYNC = 1;
	break;

    case volumeServer:
	opts->nLargeVnodes = 0;
	opts->nSmallVnodes = 0;

	opts->canScheduleSalvage = 1;
	opts->canUseFSSYNC = 1;
	break;

    default:
	/* noop */
	break;
    }
}

int
VInitVolumePackage2(ProgramType pt, VolumePackageOptions * opts)
{
    int errors = 0;		/* Number of errors while finding vice partitions. */

    programType = pt;
    vol_opts = *opts;

    memset(&VStats, 0, sizeof(VStats));
    VStats.hdr_cache_size = 200;

    VInitPartitionPackage();
    VInitVolumeHash();
#ifdef AFS_DEMAND_ATTACH_FS
    if (programType == fileServer) {
	VInitVLRU();
    } else {
	VLRU_SetOptions(VLRU_SET_ENABLED, 0);
    }
    assert(pthread_key_create(&VThread_key, NULL) == 0);
#endif

#ifdef AFS_PTHREAD_ENV
    assert(pthread_mutex_init(&vol_glock_mutex, NULL) == 0);
    assert(pthread_mutex_init(&vol_trans_mutex, NULL) == 0);
    assert(pthread_cond_init(&vol_put_volume_cond, NULL) == 0);
    assert(pthread_cond_init(&vol_sleep_cond, NULL) == 0);
    assert(pthread_cond_init(&vol_init_attach_cond, NULL) == 0);
#else /* AFS_PTHREAD_ENV */
    IOMGR_Initialize();
#endif /* AFS_PTHREAD_ENV */
    Lock_Init(&vol_listLock);

    srandom(time(0));		/* For VGetVolumeInfo */

#ifdef AFS_DEMAND_ATTACH_FS
    assert(pthread_mutex_init(&vol_salvsync_mutex, NULL) == 0);
#endif /* AFS_DEMAND_ATTACH_FS */

    /* Ok, we have done enough initialization that fileserver can
     * start accepting calls, even though the volumes may not be
     * available just yet.
     */
    VInit = 1;

#if defined(AFS_DEMAND_ATTACH_FS) && defined(SALVSYNC_BUILD_SERVER)
    if (programType == salvageServer) {
	SALVSYNC_salvInit();
    }
#endif /* AFS_DEMAND_ATTACH_FS */
#ifdef FSSYNC_BUILD_SERVER
    if (programType == fileServer) {
	FSYNC_fsInit();
    }
#endif
#if defined(AFS_DEMAND_ATTACH_FS) && defined(SALVSYNC_BUILD_CLIENT)
    if (VCanUseSALVSYNC()) {
	/* establish a connection to the salvager at this point */
	assert(VConnectSALV() != 0);
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    if (opts->volcache > VStats.hdr_cache_size)
	VStats.hdr_cache_size = opts->volcache;
    VInitVolumeHeaderCache(VStats.hdr_cache_size);

    VInitVnodes(vLarge, opts->nLargeVnodes);
    VInitVnodes(vSmall, opts->nSmallVnodes);


    errors = VAttachPartitions();
    if (errors)
	return -1;

    if (programType != fileServer) {
        errors = VInitAttachVolumes(programType);
        if (errors) {
            return -1;
        }
    }

#ifdef FSSYNC_BUILD_CLIENT
    if (VCanUseFSSYNC()) {
	if (!VConnectFS()) {
#ifdef AFS_DEMAND_ATTACH_FS
	    if (programType == salvageServer) {
		Log("Unable to connect to file server; aborted\n");
		exit(1);
	    }
#endif /* AFS_DEMAND_ATTACH_FS */
	    Log("Unable to connect to file server; will retry at need\n");
	}
    }
#endif /* FSSYNC_BUILD_CLIENT */
    return 0;
}


#if !defined(AFS_PTHREAD_ENV)
/**
 * Attach volumes in vice partitions
 *
 * @param[in]  pt         calling program type
 *
 * @return 0
 * @note This is the original, non-threaded version of attach parititions.
 *
 * @post VInit state is 2
 */
int
VInitAttachVolumes(ProgramType pt)
{
    assert(VInit==1);
    if (pt == fileServer) {
	struct DiskPartition64 *diskP;
	/* Attach all the volumes in this partition */
	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    int nAttached = 0, nUnattached = 0;
	    assert(VAttachVolumesByPartition(diskP, &nAttached, &nUnattached) == 0);
	}
    }
    VOL_LOCK;
    VInit = 2;			/* Initialized, and all volumes have been attached */
    LWP_NoYieldSignal(VInitAttachVolumes);
    VOL_UNLOCK;
    return 0;
}
#endif /* !AFS_PTHREAD_ENV */

#if defined(AFS_PTHREAD_ENV) && !defined(AFS_DEMAND_ATTACH_FS)
/**
 * Attach volumes in vice partitions
 *
 * @param[in]  pt         calling program type
 *
 * @return 0
 * @note Threaded version of attach parititions.
 *
 * @post VInit state is 2
 */
int
VInitAttachVolumes(ProgramType pt)
{
    assert(VInit==1);
    if (pt == fileServer) {
	struct DiskPartition64 *diskP;
	struct vinitvolumepackage_thread_t params;
	struct diskpartition_queue_t * dpq;
	int i, threads, parts;
	pthread_t tid;
	pthread_attr_t attrs;

	assert(pthread_cond_init(&params.thread_done_cv,NULL) == 0);
	queue_Init(&params);
	params.n_threads_complete = 0;

	/* create partition work queue */
	for (parts=0, diskP = DiskPartitionList; diskP; diskP = diskP->next, parts++) {
	    dpq = (diskpartition_queue_t *) malloc(sizeof(struct diskpartition_queue_t));
	    assert(dpq != NULL);
	    dpq->diskP = diskP;
	    queue_Append(&params,dpq);
	}

	threads = MIN(parts, vol_attach_threads);

	if (threads > 1) {
	    /* spawn off a bunch of initialization threads */
	    assert(pthread_attr_init(&attrs) == 0);
	    assert(pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) == 0);

	    Log("VInitVolumePackage: beginning parallel fileserver startup\n");
	    Log("VInitVolumePackage: using %d threads to attach volumes on %d partitions\n",
		threads, parts);

	    VOL_LOCK;
	    for (i=0; i < threads; i++) {
                AFS_SIGSET_DECL;
                AFS_SIGSET_CLEAR();
		assert(pthread_create
		       (&tid, &attrs, &VInitVolumePackageThread,
			&params) == 0);
                AFS_SIGSET_RESTORE();
	    }

	    while(params.n_threads_complete < threads) {
		VOL_CV_WAIT(&params.thread_done_cv);
	    }
	    VOL_UNLOCK;

	    assert(pthread_attr_destroy(&attrs) == 0);
	} else {
	    /* if we're only going to run one init thread, don't bother creating
	     * another LWP */
	    Log("VInitVolumePackage: beginning single-threaded fileserver startup\n");
	    Log("VInitVolumePackage: using 1 thread to attach volumes on %d partition(s)\n",
		parts);

	    VInitVolumePackageThread(&params);
	}

	assert(pthread_cond_destroy(&params.thread_done_cv) == 0);
    }
    VOL_LOCK;
    VInit = 2;			/* Initialized, and all volumes have been attached */
    assert(pthread_cond_broadcast(&vol_init_attach_cond) == 0);
    VOL_UNLOCK;
    return 0;
}

static void *
VInitVolumePackageThread(void * args) {

    struct DiskPartition64 *diskP;
    struct vinitvolumepackage_thread_t * params;
    struct diskpartition_queue_t * dpq;

    params = (vinitvolumepackage_thread_t *) args;


    VOL_LOCK;
    /* Attach all the volumes in this partition */
    while (queue_IsNotEmpty(params)) {
        int nAttached = 0, nUnattached = 0;

        if (vinit_attach_abort) {
            Log("Aborting initialization\n");
            goto done;
        }

        dpq = queue_First(params,diskpartition_queue_t);
	queue_Remove(dpq);
	VOL_UNLOCK;
	diskP = dpq->diskP;
	free(dpq);

	assert(VAttachVolumesByPartition(diskP, &nAttached, &nUnattached) == 0);

	VOL_LOCK;
    }

done:
    params->n_threads_complete++;
    pthread_cond_signal(&params->thread_done_cv);
    VOL_UNLOCK;
    return NULL;
}
#endif /* AFS_PTHREAD_ENV && !AFS_DEMAND_ATTACH_FS */

#if defined(AFS_DEMAND_ATTACH_FS)
/**
 * Attach volumes in vice partitions
 *
 * @param[in]  pt         calling program type
 *
 * @return 0
 * @note Threaded version of attach partitions.
 *
 * @post VInit state is 2
 */
int
VInitAttachVolumes(ProgramType pt)
{
    assert(VInit==1);
    if (pt == fileServer) {

	struct DiskPartition64 *diskP;
	struct partition_queue pq;
        struct volume_init_queue vq;

	int i, threads, parts;
	pthread_t tid;
	pthread_attr_t attrs;

	/* create partition work queue */
        queue_Init(&pq);
        assert(pthread_cond_init(&(pq.cv), NULL) == 0);
        assert(pthread_mutex_init(&(pq.mutex), NULL) == 0);
	for (parts = 0, diskP = DiskPartitionList; diskP; diskP = diskP->next, parts++) {
	    struct diskpartition_queue_t *dp;
	    dp = (struct diskpartition_queue_t*)malloc(sizeof(struct diskpartition_queue_t));
	    assert(dp != NULL);
	    dp->diskP = diskP;
	    queue_Append(&pq, dp);
	}

        /* number of worker threads; at least one, not to exceed the number of partitions */
	threads = MIN(parts, vol_attach_threads);

        /* create volume work queue */
        queue_Init(&vq);
        assert(pthread_cond_init(&(vq.cv), NULL) == 0);
        assert(pthread_mutex_init(&(vq.mutex), NULL) == 0);

        assert(pthread_attr_init(&attrs) == 0);
        assert(pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) == 0);

        Log("VInitVolumePackage: beginning parallel fileserver startup\n");
        Log("VInitVolumePackage: using %d threads to pre-attach volumes on %d partitions\n",
		threads, parts);

        /* create threads to scan disk partitions. */
	for (i=0; i < threads; i++) {
	    struct vinitvolumepackage_thread_param *params;
            AFS_SIGSET_DECL;

            params = (struct vinitvolumepackage_thread_param *)malloc(sizeof(struct vinitvolumepackage_thread_param));
            assert(params);
            params->pq = &pq;
            params->vq = &vq;
            params->nthreads = threads;
            params->thread = i+1;

            AFS_SIGSET_CLEAR();
	    assert(pthread_create (&tid, &attrs, &VInitVolumePackageThread, (void*)params) == 0);
            AFS_SIGSET_RESTORE();
	}

        VInitPreAttachVolumes(threads, &vq);

        assert(pthread_attr_destroy(&attrs) == 0);
        assert(pthread_cond_destroy(&pq.cv) == 0);
        assert(pthread_mutex_destroy(&pq.mutex) == 0);
        assert(pthread_cond_destroy(&vq.cv) == 0);
        assert(pthread_mutex_destroy(&vq.mutex) == 0);
    }

    VOL_LOCK;
    VInit = 2;			/* Initialized, and all volumes have been attached */
    assert(pthread_cond_broadcast(&vol_init_attach_cond) == 0);
    VOL_UNLOCK;

    return 0;
}

/**
 * Volume package initialization worker thread. Scan partitions for volume
 * header files. Gather batches of volume ids and dispatch them to
 * the main thread to be preattached.  The volume preattachement is done
 * in the main thread to avoid global volume lock contention.
 */
static void *
VInitVolumePackageThread(void *args)
{
    struct vinitvolumepackage_thread_param *params;
    struct DiskPartition64 *partition;
    struct partition_queue *pq;
    struct volume_init_queue *vq;
    struct volume_init_batch *vb;

    assert(args);
    params = (struct vinitvolumepackage_thread_param *)args;
    pq = params->pq;
    vq = params->vq;
    assert(pq);
    assert(vq);

    vb = (struct volume_init_batch*)malloc(sizeof(struct volume_init_batch));
    assert(vb);
    vb->thread = params->thread;
    vb->last = 0;
    vb->size = 0;

    Log("Scanning partitions on thread %d of %d\n", params->thread, params->nthreads);
    while((partition = VInitNextPartition(pq))) {
        DIR *dirp;
        VolId vid;

        Log("Partition %s: pre-attaching volumes\n", partition->name);
        dirp = opendir(VPartitionPath(partition));
        if (!dirp) {
            Log("opendir on Partition %s failed, errno=%d!\n", partition->name, errno);
            continue;
        }
        while ((vid = VInitNextVolumeId(dirp))) {
            Volume *vp = (Volume*)malloc(sizeof(Volume));
            assert(vp);
            memset(vp, 0, sizeof(Volume));
            vp->device = partition->device;
            vp->partition = partition;
            vp->hashid = vid;
            queue_Init(&vp->vnode_list);
            assert(pthread_cond_init(&V_attachCV(vp), NULL) == 0);

            vb->batch[vb->size++] = vp;
            if (vb->size == VINIT_BATCH_MAX_SIZE) {
                assert(pthread_mutex_lock(&vq->mutex) == 0);
                queue_Append(vq, vb);
                assert(pthread_cond_broadcast(&vq->cv) == 0);
                assert(pthread_mutex_unlock(&vq->mutex) == 0);

                vb = (struct volume_init_batch*)malloc(sizeof(struct volume_init_batch));
                assert(vb);
                vb->thread = params->thread;
                vb->size = 0;
                vb->last = 0;
            }
        }
        closedir(dirp);
    }

    vb->last = 1;
    assert(pthread_mutex_lock(&vq->mutex) == 0);
    queue_Append(vq, vb);
    assert(pthread_cond_broadcast(&vq->cv) == 0);
    assert(pthread_mutex_unlock(&vq->mutex) == 0);

    Log("Partition scan thread %d of %d ended\n", params->thread, params->nthreads);
    free(params);
    return NULL;
}

/**
 * Read next element from the pre-populated partition list.
 */
static struct DiskPartition64*
VInitNextPartition(struct partition_queue *pq)
{
    struct DiskPartition64 *partition;
    struct diskpartition_queue_t *dp; /* queue element */

    if (vinit_attach_abort) {
        Log("Aborting volume preattach thread.\n");
        return NULL;
    }

    /* get next partition to scan */
    assert(pthread_mutex_lock(&pq->mutex) == 0);
    if (queue_IsEmpty(pq)) {
        assert(pthread_mutex_unlock(&pq->mutex) == 0);
        return NULL;
    }
    dp = queue_First(pq, diskpartition_queue_t);
    queue_Remove(dp);
    assert(pthread_mutex_unlock(&pq->mutex) == 0);

    assert(dp);
    assert(dp->diskP);

    partition = dp->diskP;
    free(dp);
    return partition;
}

/**
 * Find next volume id on the partition.
 */
static VolId
VInitNextVolumeId(DIR *dirp)
{
    struct dirent *d;
    VolId vid = 0;
    char *ext;

    while((d = readdir(dirp))) {
        if (vinit_attach_abort) {
            Log("Aborting volume preattach thread.\n");
            break;
        }
        ext = strrchr(d->d_name, '.');
        if (d->d_name[0] == 'V' && ext && strcmp(ext, VHDREXT) == 0) {
            vid = VolumeNumber(d->d_name);
            if (vid) {
               break;
            }
            Log("Warning: bogus volume header file: %s\n", d->d_name);
        }
    }
    return vid;
}

/**
 * Preattach volumes in batches to avoid lock contention.
 */
static int
VInitPreAttachVolumes(int nthreads, struct volume_init_queue *vq)
{
    struct volume_init_batch *vb;
    int i;

    while (nthreads) {
        /* dequeue next volume */
        pthread_mutex_lock(&vq->mutex);
        if (queue_IsEmpty(vq)) {
            pthread_cond_wait(&vq->cv, &vq->mutex);
        }
        vb = queue_First(vq, volume_init_batch);
        queue_Remove(vb);
        pthread_mutex_unlock(&vq->mutex);

        if (vb->size) {
            VOL_LOCK;
            for (i = 0; i<vb->size; i++) {
                Volume *vp;
                Volume *dup;
                Error ec = 0;

                vp = vb->batch[i];
	        dup = VLookupVolume_r(&ec, vp->hashid, NULL);
                if (ec) {
                    Log("Error looking up volume, code=%d\n", ec);
                }
                else if (dup) {
                    Log("Warning: Duplicate volume id %d detected.\n", vp->hashid);
                }
                else {
                    /* put pre-attached volume onto the hash table
                     * and bring it up to the pre-attached state */
                    AddVolumeToHashTable(vp, vp->hashid);
                    AddVolumeToVByPList_r(vp);
                    VLRU_Init_Node_r(vp);
                    VChangeState_r(vp, VOL_STATE_PREATTACHED);
                }
            }
            VOL_UNLOCK;
        }

        if (vb->last) {
            nthreads--;
        }
        free(vb);
    }
    return 0;
}
#endif /* AFS_DEMAND_ATTACH_FS */

#if !defined(AFS_DEMAND_ATTACH_FS)
/*
 * attach all volumes on a given disk partition
 */
static int
VAttachVolumesByPartition(struct DiskPartition64 *diskP, int * nAttached, int * nUnattached)
{
  DIR * dirp;
  struct dirent * dp;
  int ret = 0;

  Log("Partition %s: attaching volumes\n", diskP->name);
  dirp = opendir(VPartitionPath(diskP));
  if (!dirp) {
    Log("opendir on Partition %s failed!\n", diskP->name);
    return 1;
  }

  while ((dp = readdir(dirp))) {
    char *p;
    p = strrchr(dp->d_name, '.');

    if (vinit_attach_abort) {
      Log("Partition %s: abort attach volumes\n", diskP->name);
      goto done;
    }

    if (p != NULL && strcmp(p, VHDREXT) == 0) {
      Error error;
      Volume *vp;
      vp = VAttachVolumeByName(&error, diskP->name, dp->d_name,
			       V_VOLUPD);
      (*(vp ? nAttached : nUnattached))++;
      if (error == VOFFLINE)
	Log("Volume %d stays offline (/vice/offline/%s exists)\n", VolumeNumber(dp->d_name), dp->d_name);
      else if (LogLevel >= 5) {
	Log("Partition %s: attached volume %d (%s)\n",
	    diskP->name, VolumeNumber(dp->d_name),
	    dp->d_name);
      }
      if (vp) {
	VPutVolume(vp);
      }
    }
  }

  Log("Partition %s: attached %d volumes; %d volumes not attached\n", diskP->name, *nAttached, *nUnattached);
done:
  closedir(dirp);
  return ret;
}
#endif /* !AFS_DEMAND_ATTACH_FS */

/***************************************************/
/* Shutdown routines                               */
/***************************************************/

/*
 * demand attach fs
 * highly multithreaded volume package shutdown
 *
 * with the demand attach fileserver extensions,
 * VShutdown has been modified to be multithreaded.
 * In order to achieve optimal use of many threads,
 * the shutdown code involves one control thread and
 * n shutdown worker threads.  The control thread
 * periodically examines the number of volumes available
 * for shutdown on each partition, and produces a worker
 * thread allocation schedule.  The idea is to eliminate
 * redundant scheduling computation on the workers by
 * having a single master scheduler.
 *
 * The scheduler's objectives are:
 * (1) fairness
 *   each partition with volumes remaining gets allocated
 *   at least 1 thread (assuming sufficient threads)
 * (2) performance
 *   threads are allocated proportional to the number of
 *   volumes remaining to be offlined.  This ensures that
 *   the OS I/O scheduler has many requests to elevator
 *   seek on partitions that will (presumably) take the
 *   longest amount of time (from now) to finish shutdown
 * (3) keep threads busy
 *   when there are extra threads, they are assigned to
 *   partitions using a simple round-robin algorithm
 *
 * In the future, we may wish to add the ability to adapt
 * to the relative performance patterns of each disk
 * partition.
 *
 *
 * demand attach fs
 * multi-step shutdown process
 *
 * demand attach shutdown is a four-step process. Each
 * shutdown "pass" shuts down increasingly more difficult
 * volumes.  The main purpose is to achieve better cache
 * utilization during shutdown.
 *
 * pass 0
 *   shutdown volumes in the unattached, pre-attached
 *   and error states
 * pass 1
 *   shutdown attached volumes with cached volume headers
 * pass 2
 *   shutdown all volumes in non-exclusive states
 * pass 3
 *   shutdown all remaining volumes
 */

#ifdef AFS_DEMAND_ATTACH_FS

void
VShutdown_r(void)
{
    int i;
    struct DiskPartition64 * diskP;
    struct diskpartition_queue_t * dpq;
    vshutdown_thread_t params;
    pthread_t tid;
    pthread_attr_t attrs;

    memset(&params, 0, sizeof(vshutdown_thread_t));

    if (VInit < 2) {
        Log("VShutdown:  aborting attach volumes\n");
        vinit_attach_abort = 1;
        VOL_CV_WAIT(&vol_init_attach_cond);
    }

    for (params.n_parts=0, diskP = DiskPartitionList;
	 diskP; diskP = diskP->next, params.n_parts++);

    Log("VShutdown:  shutting down on-line volumes on %d partition%s...\n",
	params.n_parts, params.n_parts > 1 ? "s" : "");

    if (vol_attach_threads > 1) {
	/* prepare for parallel shutdown */
	params.n_threads = vol_attach_threads;
	assert(pthread_mutex_init(&params.lock, NULL) == 0);
	assert(pthread_cond_init(&params.cv, NULL) == 0);
	assert(pthread_cond_init(&params.master_cv, NULL) == 0);
	assert(pthread_attr_init(&attrs) == 0);
	assert(pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) == 0);
	queue_Init(&params);

	/* setup the basic partition information structures for
	 * parallel shutdown */
	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    /* XXX debug */
	    struct rx_queue * qp, * nqp;
	    Volume * vp;
	    int count = 0;

	    VVByPListWait_r(diskP);
	    VVByPListBeginExclusive_r(diskP);

	    /* XXX debug */
	    for (queue_Scan(&diskP->vol_list, qp, nqp, rx_queue)) {
		vp = (Volume *)((char *)qp - offsetof(Volume, vol_list));
		if (vp->header)
		    count++;
	    }
	    Log("VShutdown: partition %s has %d volumes with attached headers\n",
		VPartitionPath(diskP), count);


	    /* build up the pass 0 shutdown work queue */
	    dpq = (struct diskpartition_queue_t *) malloc(sizeof(struct diskpartition_queue_t));
	    assert(dpq != NULL);
	    dpq->diskP = diskP;
	    queue_Prepend(&params, dpq);

	    params.part_pass_head[diskP->index] = queue_First(&diskP->vol_list, rx_queue);
	}

	Log("VShutdown:  beginning parallel fileserver shutdown\n");
	Log("VShutdown:  using %d threads to offline volumes on %d partition%s\n",
	    vol_attach_threads, params.n_parts, params.n_parts > 1 ? "s" : "" );

	/* do pass 0 shutdown */
	assert(pthread_mutex_lock(&params.lock) == 0);
	for (i=0; i < params.n_threads; i++) {
	    assert(pthread_create
		   (&tid, &attrs, &VShutdownThread,
		    &params) == 0);
	}

	/* wait for all the pass 0 shutdowns to complete */
	while (params.n_threads_complete < params.n_threads) {
	    assert(pthread_cond_wait(&params.master_cv, &params.lock) == 0);
	}
	params.n_threads_complete = 0;
	params.pass = 1;
	assert(pthread_cond_broadcast(&params.cv) == 0);
	assert(pthread_mutex_unlock(&params.lock) == 0);

	Log("VShutdown:  pass 0 completed using the 1 thread per partition algorithm\n");
	Log("VShutdown:  starting passes 1 through 3 using finely-granular mp-fast algorithm\n");

	/* run the parallel shutdown scheduler. it will drop the glock internally */
	ShutdownController(&params);

	/* wait for all the workers to finish pass 3 and terminate */
	while (params.pass < 4) {
	    VOL_CV_WAIT(&params.cv);
	}

	assert(pthread_attr_destroy(&attrs) == 0);
	assert(pthread_cond_destroy(&params.cv) == 0);
	assert(pthread_cond_destroy(&params.master_cv) == 0);
	assert(pthread_mutex_destroy(&params.lock) == 0);

	/* drop the VByPList exclusive reservations */
	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    VVByPListEndExclusive_r(diskP);
	    Log("VShutdown:  %s stats : (pass[0]=%d, pass[1]=%d, pass[2]=%d, pass[3]=%d)\n",
		VPartitionPath(diskP),
		params.stats[0][diskP->index],
		params.stats[1][diskP->index],
		params.stats[2][diskP->index],
		params.stats[3][diskP->index]);
	}

	Log("VShutdown:  shutdown finished using %d threads\n", params.n_threads);
    } else {
	/* if we're only going to run one shutdown thread, don't bother creating
	 * another LWP */
	Log("VShutdown:  beginning single-threaded fileserver shutdown\n");

	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    VShutdownByPartition_r(diskP);
	}
    }

    Log("VShutdown:  complete.\n");
}

#else /* AFS_DEMAND_ATTACH_FS */

void
VShutdown_r(void)
{
    int i;
    Volume *vp, *np;
    afs_int32 code;

    if (VInit < 2) {
        Log("VShutdown:  aborting attach volumes\n");
        vinit_attach_abort = 1;
#ifdef AFS_PTHREAD_ENV
        VOL_CV_WAIT(&vol_init_attach_cond);
#else
        LWP_WaitProcess(VInitAttachVolumes);
#endif /* AFS_PTHREAD_ENV */
    }

    Log("VShutdown:  shutting down on-line volumes...\n");
    for (i = 0; i < VolumeHashTable.Size; i++) {
	/* try to hold first volume in the hash table */
	for (queue_Scan(&VolumeHashTable.Table[i],vp,np,Volume)) {
	    code = VHold_r(vp);
	    if (code == 0) {
		if (LogLevel >= 5)
		    Log("VShutdown:  Attempting to take volume %u offline.\n",
			vp->hashid);

		/* next, take the volume offline (drops reference count) */
		VOffline_r(vp, "File server was shut down");
	    }
	}
    }
    Log("VShutdown:  complete.\n");
}
#endif /* AFS_DEMAND_ATTACH_FS */


void
VShutdown(void)
{
    assert(VInit>0);
    VOL_LOCK;
    VShutdown_r();
    VOL_UNLOCK;
}

/**
 * stop new activity (e.g. SALVSYNC) from occurring
 *
 * Use this to make the volume package less busy; for example, during
 * shutdown. This doesn't actually shutdown/detach anything in the
 * volume package, but prevents certain processes from ocurring. For
 * example, preventing new SALVSYNC communication in DAFS. In theory, we
 * could also use this to prevent new volume attachment, or prevent
 * other programs from checking out volumes, etc.
 */
void
VSetTranquil(void)
{
#ifdef AFS_DEMAND_ATTACH_FS
    /* make sure we don't try to contact the salvageserver, since it may
     * not be around anymore */
    vol_disallow_salvsync = 1;
#endif
}

#ifdef AFS_DEMAND_ATTACH_FS
/*
 * demand attach fs
 * shutdown control thread
 */
static void
ShutdownController(vshutdown_thread_t * params)
{
    /* XXX debug */
    struct DiskPartition64 * diskP;
    Device id;
    vshutdown_thread_t shadow;

    ShutdownCreateSchedule(params);

    while ((params->pass < 4) &&
	   (params->n_threads_complete < params->n_threads)) {
	/* recompute schedule once per second */

	memcpy(&shadow, params, sizeof(vshutdown_thread_t));

	VOL_UNLOCK;
	/* XXX debug */
	Log("ShutdownController:  schedule version=%d, vol_remaining=%d, pass=%d\n",
	    shadow.schedule_version, shadow.vol_remaining, shadow.pass);
	Log("ShutdownController:  n_threads_complete=%d, n_parts_done_pass=%d\n",
	    shadow.n_threads_complete, shadow.n_parts_done_pass);
	for (diskP = DiskPartitionList; diskP; diskP=diskP->next) {
	    id = diskP->index;
	    Log("ShutdownController:  part[%d] : (len=%d, thread_target=%d, done_pass=%d, pass_head=%p)\n",
		id,
		diskP->vol_list.len,
		shadow.part_thread_target[id],
		shadow.part_done_pass[id],
		shadow.part_pass_head[id]);
	}

	sleep(1);
	VOL_LOCK;

	ShutdownCreateSchedule(params);
    }
}

/* create the shutdown thread work schedule.
 * this scheduler tries to implement fairness
 * by allocating at least 1 thread to each
 * partition with volumes to be shutdown,
 * and then it attempts to allocate remaining
 * threads based upon the amount of work left
 */
static void
ShutdownCreateSchedule(vshutdown_thread_t * params)
{
    struct DiskPartition64 * diskP;
    int sum, thr_workload, thr_left;
    int part_residue[VOLMAXPARTS+1];
    Device id;

    /* compute the total number of outstanding volumes */
    sum = 0;
    for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	sum += diskP->vol_list.len;
    }

    params->schedule_version++;
    params->vol_remaining = sum;

    if (!sum)
	return;

    /* compute average per-thread workload */
    thr_workload = sum / params->n_threads;
    if (sum % params->n_threads)
	thr_workload++;

    thr_left = params->n_threads;
    memset(&part_residue, 0, sizeof(part_residue));

    /* for fairness, give every partition with volumes remaining
     * at least one thread */
    for (diskP = DiskPartitionList; diskP && thr_left; diskP = diskP->next) {
	id = diskP->index;
	if (diskP->vol_list.len) {
	    params->part_thread_target[id] = 1;
	    thr_left--;
	} else {
	    params->part_thread_target[id] = 0;
	}
    }

    if (thr_left && thr_workload) {
	/* compute length-weighted workloads */
	int delta;

	for (diskP = DiskPartitionList; diskP && thr_left; diskP = diskP->next) {
	    id = diskP->index;
	    delta = (diskP->vol_list.len / thr_workload) -
		params->part_thread_target[id];
	    if (delta < 0) {
		continue;
	    }
	    if (delta < thr_left) {
		params->part_thread_target[id] += delta;
		thr_left -= delta;
	    } else {
		params->part_thread_target[id] += thr_left;
		thr_left = 0;
		break;
	    }
	}
    }

    if (thr_left) {
	/* try to assign any leftover threads to partitions that
	 * had volume lengths closer to needing thread_target+1 */
	int max_residue, max_id = 0;

	/* compute the residues */
	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    id = diskP->index;
	    part_residue[id] = diskP->vol_list.len -
		(params->part_thread_target[id] * thr_workload);
	}

	/* now try to allocate remaining threads to partitions with the
	 * highest residues */
	while (thr_left) {
	    max_residue = 0;
	    for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
		id = diskP->index;
		if (part_residue[id] > max_residue) {
		    max_residue = part_residue[id];
		    max_id = id;
		}
	    }

	    if (!max_residue) {
		break;
	    }

	    params->part_thread_target[max_id]++;
	    thr_left--;
	    part_residue[max_id] = 0;
	}
    }

    if (thr_left) {
	/* punt and give any remaining threads equally to each partition */
	int alloc;
	if (thr_left >= params->n_parts) {
	    alloc = thr_left / params->n_parts;
	    for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
		id = diskP->index;
		params->part_thread_target[id] += alloc;
		thr_left -= alloc;
	    }
	}

	/* finish off the last of the threads */
	for (diskP = DiskPartitionList; thr_left && diskP; diskP = diskP->next) {
	    id = diskP->index;
	    params->part_thread_target[id]++;
	    thr_left--;
	}
    }
}

/* worker thread for parallel shutdown */
static void *
VShutdownThread(void * args)
{
    vshutdown_thread_t * params;
    int found, pass, schedule_version_save, count;
    struct DiskPartition64 *diskP;
    struct diskpartition_queue_t * dpq;
    Device id;

    params = (vshutdown_thread_t *) args;

    /* acquire the shutdown pass 0 lock */
    assert(pthread_mutex_lock(&params->lock) == 0);

    /* if there's still pass 0 work to be done,
     * get a work entry, and do a pass 0 shutdown */
    if (queue_IsNotEmpty(params)) {
	dpq = queue_First(params, diskpartition_queue_t);
	queue_Remove(dpq);
	assert(pthread_mutex_unlock(&params->lock) == 0);
	diskP = dpq->diskP;
	free(dpq);
	id = diskP->index;

	count = 0;
	while (ShutdownVolumeWalk_r(diskP, 0, &params->part_pass_head[id]))
	    count++;
	params->stats[0][diskP->index] = count;
	assert(pthread_mutex_lock(&params->lock) == 0);
    }

    params->n_threads_complete++;
    if (params->n_threads_complete == params->n_threads) {
      /* notify control thread that all workers have completed pass 0 */
      assert(pthread_cond_signal(&params->master_cv) == 0);
    }
    while (params->pass == 0) {
      assert(pthread_cond_wait(&params->cv, &params->lock) == 0);
    }

    /* switch locks */
    assert(pthread_mutex_unlock(&params->lock) == 0);
    VOL_LOCK;

    pass = params->pass;
    assert(pass > 0);

    /* now escalate through the more complicated shutdowns */
    while (pass <= 3) {
	schedule_version_save = params->schedule_version;
	found = 0;
	/* find a disk partition to work on */
	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    id = diskP->index;
	    if (params->part_thread_target[id] && !params->part_done_pass[id]) {
		params->part_thread_target[id]--;
		found = 1;
		break;
	    }
	}

	if (!found) {
	    /* hmm. for some reason the controller thread couldn't find anything for
	     * us to do. let's see if there's anything we can do */
	    for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
		id = diskP->index;
		if (diskP->vol_list.len && !params->part_done_pass[id]) {
		    found = 1;
		    break;
		} else if (!params->part_done_pass[id]) {
		    params->part_done_pass[id] = 1;
		    params->n_parts_done_pass++;
		    if (pass == 3) {
			Log("VShutdown:  done shutting down volumes on partition %s.\n",
			    VPartitionPath(diskP));
		    }
		}
	    }
	}

	/* do work on this partition until either the controller
	 * creates a new schedule, or we run out of things to do
	 * on this partition */
	if (found) {
	    count = 0;
	    while (!params->part_done_pass[id] &&
		   (schedule_version_save == params->schedule_version)) {
		/* ShutdownVolumeWalk_r will drop the glock internally */
		if (!ShutdownVolumeWalk_r(diskP, pass, &params->part_pass_head[id])) {
		    if (!params->part_done_pass[id]) {
			params->part_done_pass[id] = 1;
			params->n_parts_done_pass++;
			if (pass == 3) {
			    Log("VShutdown:  done shutting down volumes on partition %s.\n",
				VPartitionPath(diskP));
			}
		    }
		    break;
		}
		count++;
	    }

	    params->stats[pass][id] += count;
	} else {
	    /* ok, everyone is done this pass, proceed */

	    /* barrier lock */
	    params->n_threads_complete++;
	    while (params->pass == pass) {
		if (params->n_threads_complete == params->n_threads) {
		    /* we are the last thread to complete, so we will
		     * reinitialize worker pool state for the next pass */
		    params->n_threads_complete = 0;
		    params->n_parts_done_pass = 0;
		    params->pass++;
		    for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
			id = diskP->index;
			params->part_done_pass[id] = 0;
			params->part_pass_head[id] = queue_First(&diskP->vol_list, rx_queue);
		    }

		    /* compute a new thread schedule before releasing all the workers */
		    ShutdownCreateSchedule(params);

		    /* wake up all the workers */
		    assert(pthread_cond_broadcast(&params->cv) == 0);

		    VOL_UNLOCK;
		    Log("VShutdown:  pass %d completed using %d threads on %d partitions\n",
			pass, params->n_threads, params->n_parts);
		    VOL_LOCK;
		} else {
		    VOL_CV_WAIT(&params->cv);
		}
	    }
	    pass = params->pass;
	}

	/* for fairness */
	VOL_UNLOCK;
	pthread_yield();
	VOL_LOCK;
    }

    VOL_UNLOCK;

    return NULL;
}

/* shut down all volumes on a given disk partition
 *
 * note that this function will not allow mp-fast
 * shutdown of a partition */
int
VShutdownByPartition_r(struct DiskPartition64 * dp)
{
    int pass;
    int pass_stats[4];
    int total;

    /* wait for other exclusive ops to finish */
    VVByPListWait_r(dp);

    /* begin exclusive access */
    VVByPListBeginExclusive_r(dp);

    /* pick the low-hanging fruit first,
     * then do the complicated ones last
     * (has the advantage of keeping
     *  in-use volumes up until the bitter end) */
    for (pass = 0, total=0; pass < 4; pass++) {
	pass_stats[pass] = ShutdownVByPForPass_r(dp, pass);
	total += pass_stats[pass];
    }

    /* end exclusive access */
    VVByPListEndExclusive_r(dp);

    Log("VShutdownByPartition:  shut down %d volumes on %s (pass[0]=%d, pass[1]=%d, pass[2]=%d, pass[3]=%d)\n",
	total, VPartitionPath(dp), pass_stats[0], pass_stats[1], pass_stats[2], pass_stats[3]);

    return 0;
}

/* internal shutdown functionality
 *
 * for multi-pass shutdown:
 * 0 to only "shutdown" {pre,un}attached and error state volumes
 * 1 to also shutdown attached volumes w/ volume header loaded
 * 2 to also shutdown attached volumes w/o volume header loaded
 * 3 to also shutdown exclusive state volumes
 *
 * caller MUST hold exclusive access on the hash chain
 * because we drop vol_glock_mutex internally
 *
 * this function is reentrant for passes 1--3
 * (e.g. multiple threads can cooperate to
 *  shutdown a partition mp-fast)
 *
 * pass 0 is not scaleable because the volume state data is
 * synchronized by vol_glock mutex, and the locking overhead
 * is too high to drop the lock long enough to do linked list
 * traversal
 */
static int
ShutdownVByPForPass_r(struct DiskPartition64 * dp, int pass)
{
    struct rx_queue * q = queue_First(&dp->vol_list, rx_queue);
    int i = 0;

    while (ShutdownVolumeWalk_r(dp, pass, &q))
	i++;

    return i;
}

/* conditionally shutdown one volume on partition dp
 * returns 1 if a volume was shutdown in this pass,
 * 0 otherwise */
static int
ShutdownVolumeWalk_r(struct DiskPartition64 * dp, int pass,
		     struct rx_queue ** idx)
{
    struct rx_queue *qp, *nqp;
    Volume * vp;

    qp = *idx;

    for (queue_ScanFrom(&dp->vol_list, qp, qp, nqp, rx_queue)) {
	vp = (Volume *) (((char *)qp) - offsetof(Volume, vol_list));

	switch (pass) {
	case 0:
	    if ((V_attachState(vp) != VOL_STATE_UNATTACHED) &&
		(V_attachState(vp) != VOL_STATE_ERROR) &&
		(V_attachState(vp) != VOL_STATE_DELETED) &&
		(V_attachState(vp) != VOL_STATE_PREATTACHED)) {
		break;
	    }
	case 1:
	    if ((V_attachState(vp) == VOL_STATE_ATTACHED) &&
		(vp->header == NULL)) {
		break;
	    }
	case 2:
	    if (VIsExclusiveState(V_attachState(vp))) {
		break;
	    }
	case 3:
	    *idx = nqp;
	    DeleteVolumeFromVByPList_r(vp);
	    VShutdownVolume_r(vp);
	    vp = NULL;
	    return 1;
	}
    }

    return 0;
}

/*
 * shutdown a specific volume
 */
/* caller MUST NOT hold a heavyweight ref on vp */
int
VShutdownVolume_r(Volume * vp)
{
    int code;

    VCreateReservation_r(vp);

    if (LogLevel >= 5) {
	Log("VShutdownVolume_r:  vid=%u, device=%d, state=%hu\n",
	    vp->hashid, vp->partition->device, V_attachState(vp));
    }

    /* wait for other blocking ops to finish */
    VWaitExclusiveState_r(vp);

    assert(VIsValidState(V_attachState(vp)));

    switch(V_attachState(vp)) {
    case VOL_STATE_SALVAGING:
	/* Leave salvaging volumes alone. Any in-progress salvages will
	 * continue working after viced shuts down. This is intentional.
	 */

    case VOL_STATE_PREATTACHED:
    case VOL_STATE_ERROR:
	VChangeState_r(vp, VOL_STATE_UNATTACHED);
    case VOL_STATE_UNATTACHED:
    case VOL_STATE_DELETED:
	break;
    case VOL_STATE_GOING_OFFLINE:
    case VOL_STATE_SHUTTING_DOWN:
    case VOL_STATE_ATTACHED:
	code = VHold_r(vp);
	if (!code) {
	    if (LogLevel >= 5)
		Log("VShutdown:  Attempting to take volume %u offline.\n",
		    vp->hashid);

	    /* take the volume offline (drops reference count) */
	    VOffline_r(vp, "File server was shut down");
	}
	break;
    default:
	break;
    }

    VCancelReservation_r(vp);
    vp = NULL;
    return 0;
}
#endif /* AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* Header I/O routines                             */
/***************************************************/

/* open a descriptor for the inode (h),
 * read in an on-disk structure into buffer (to) of size (size),
 * verify versionstamp in structure has magic (magic) and
 * optionally verify version (version) if (version) is nonzero
 */
static void
ReadHeader(Error * ec, IHandle_t * h, char *to, int size, bit32 magic,
	   bit32 version)
{
    struct versionStamp *vsn;
    FdHandle_t *fdP;

    *ec = 0;
    if (h == NULL) {
	*ec = VSALVAGE;
	return;
    }

    fdP = IH_OPEN(h);
    if (fdP == NULL) {
	*ec = VSALVAGE;
	return;
    }

    vsn = (struct versionStamp *)to;
    if (FDH_PREAD(fdP, to, size, 0) != size || vsn->magic != magic) {
	*ec = VSALVAGE;
	FDH_REALLYCLOSE(fdP);
	return;
    }
    FDH_CLOSE(fdP);

    /* Check is conditional, in case caller wants to inspect version himself */
    if (version && vsn->version != version) {
	*ec = VSALVAGE;
    }
}

void
WriteVolumeHeader_r(Error * ec, Volume * vp)
{
    IHandle_t *h = V_diskDataHandle(vp);
    FdHandle_t *fdP;

    *ec = 0;

    fdP = IH_OPEN(h);
    if (fdP == NULL) {
	*ec = VSALVAGE;
	return;
    }
    if (FDH_PWRITE(fdP, (char *)&V_disk(vp), sizeof(V_disk(vp)), 0)
	!= sizeof(V_disk(vp))) {
	*ec = VSALVAGE;
	FDH_REALLYCLOSE(fdP);
	return;
    }
    FDH_CLOSE(fdP);
}

/* VolumeHeaderToDisk
 * Allows for storing 64 bit inode numbers in on-disk volume header
 * file.
 */
/* convert in-memory representation of a volume header to the
 * on-disk representation of a volume header */
void
VolumeHeaderToDisk(VolumeDiskHeader_t * dh, VolumeHeader_t * h)
{

    memset(dh, 0, sizeof(VolumeDiskHeader_t));
    dh->stamp = h->stamp;
    dh->id = h->id;
    dh->parent = h->parent;

#ifdef AFS_64BIT_IOPS_ENV
    dh->volumeInfo_lo = (afs_int32) h->volumeInfo & 0xffffffff;
    dh->volumeInfo_hi = (afs_int32) (h->volumeInfo >> 32) & 0xffffffff;
    dh->smallVnodeIndex_lo = (afs_int32) h->smallVnodeIndex & 0xffffffff;
    dh->smallVnodeIndex_hi =
	(afs_int32) (h->smallVnodeIndex >> 32) & 0xffffffff;
    dh->largeVnodeIndex_lo = (afs_int32) h->largeVnodeIndex & 0xffffffff;
    dh->largeVnodeIndex_hi =
	(afs_int32) (h->largeVnodeIndex >> 32) & 0xffffffff;
    dh->linkTable_lo = (afs_int32) h->linkTable & 0xffffffff;
    dh->linkTable_hi = (afs_int32) (h->linkTable >> 32) & 0xffffffff;
    dh->fileACL_lo = (afs_int32) h->fileACL & 0xffffffff;
    dh->fileACL_hi = (afs_int32) (h->fileACL >> 32) & 0xffffffff;

#else
    dh->volumeInfo_lo = h->volumeInfo;
    dh->smallVnodeIndex_lo = h->smallVnodeIndex;
    dh->largeVnodeIndex_lo = h->largeVnodeIndex;
    dh->linkTable_lo = h->linkTable;
    dh->fileACL_lo = h->fileACL;
#endif
}

/* DiskToVolumeHeader
 * Converts an on-disk representation of a volume header to
 * the in-memory representation of a volume header.
 *
 * Makes the assumption that AFS has *always*
 * zero'd the volume header file so that high parts of inode
 * numbers are 0 in older (SGI EFS) volume header files.
 */
void
DiskToVolumeHeader(VolumeHeader_t * h, VolumeDiskHeader_t * dh)
{
    memset(h, 0, sizeof(VolumeHeader_t));
    h->stamp = dh->stamp;
    h->id = dh->id;
    h->parent = dh->parent;

#ifdef AFS_64BIT_IOPS_ENV
    h->volumeInfo =
	(Inode) dh->volumeInfo_lo | ((Inode) dh->volumeInfo_hi << 32);

    h->smallVnodeIndex =
	(Inode) dh->smallVnodeIndex_lo | ((Inode) dh->
					  smallVnodeIndex_hi << 32);

    h->largeVnodeIndex =
	(Inode) dh->largeVnodeIndex_lo | ((Inode) dh->
					  largeVnodeIndex_hi << 32);
    h->linkTable =
	(Inode) dh->linkTable_lo | ((Inode) dh->linkTable_hi << 32);
    h->fileACL =
	(Inode) dh->fileACL_lo | ((Inode) dh->fileACL_hi << 32);
#else
    h->volumeInfo = dh->volumeInfo_lo;
    h->smallVnodeIndex = dh->smallVnodeIndex_lo;
    h->largeVnodeIndex = dh->largeVnodeIndex_lo;
    h->linkTable = dh->linkTable_lo;
    h->fileAcls = dh->fileAcls_lo;
#endif
}


/***************************************************/
/* Volume Attachment routines                      */
/***************************************************/

#ifdef AFS_DEMAND_ATTACH_FS
/**
 * pre-attach a volume given its path.
 *
 * @param[out] ec         outbound error code
 * @param[in]  partition  partition path string
 * @param[in]  name       volume id string
 *
 * @return volume object pointer
 *
 * @note A pre-attached volume will only have its partition
 *       and hashid fields initialized.  At first call to
 *       VGetVolume, the volume will be fully attached.
 *
 */
Volume *
VPreAttachVolumeByName(Error * ec, char *partition, char *name)
{
    Volume * vp;
    VOL_LOCK;
    vp = VPreAttachVolumeByName_r(ec, partition, name);
    VOL_UNLOCK;
    return vp;
}

/**
 * pre-attach a volume given its path.
 *
 * @param[out] ec         outbound error code
 * @param[in]  partition  path to vice partition
 * @param[in]  name       volume id string
 *
 * @return volume object pointer
 *
 * @pre VOL_LOCK held
 *
 * @internal volume package internal use only.
 */
Volume *
VPreAttachVolumeByName_r(Error * ec, char *partition, char *name)
{
    return VPreAttachVolumeById_r(ec,
				  partition,
				  VolumeNumber(name));
}

/**
 * pre-attach a volume given its path and numeric volume id.
 *
 * @param[out] ec          error code return
 * @param[in]  partition   path to vice partition
 * @param[in]  volumeId    numeric volume id
 *
 * @return volume object pointer
 *
 * @pre VOL_LOCK held
 *
 * @internal volume package internal use only.
 */
Volume *
VPreAttachVolumeById_r(Error * ec,
		       char * partition,
		       VolId volumeId)
{
    Volume *vp;
    struct DiskPartition64 *partp;

    *ec = 0;

    assert(programType == fileServer);

    if (!(partp = VGetPartition_r(partition, 0))) {
	*ec = VNOVOL;
	Log("VPreAttachVolumeById_r:  Error getting partition (%s)\n", partition);
	return NULL;
    }

    vp = VLookupVolume_r(ec, volumeId, NULL);
    if (*ec) {
	return NULL;
    }

    return VPreAttachVolumeByVp_r(ec, partp, vp, volumeId);
}

/**
 * preattach a volume.
 *
 * @param[out] ec     outbound error code
 * @param[in]  partp  pointer to partition object
 * @param[in]  vp     pointer to volume object
 * @param[in]  vid    volume id
 *
 * @return volume object pointer
 *
 * @pre VOL_LOCK is held.
 *
 * @warning Returned volume object pointer does not have to
 *          equal the pointer passed in as argument vp.  There
 *          are potential race conditions which can result in
 *          the pointers having different values.  It is up to
 *          the caller to make sure that references are handled
 *          properly in this case.
 *
 * @note If there is already a volume object registered with
 *       the same volume id, its pointer MUST be passed as
 *       argument vp.  Failure to do so will result in a silent
 *       failure to preattach.
 *
 * @internal volume package internal use only.
 */
Volume *
VPreAttachVolumeByVp_r(Error * ec,
		       struct DiskPartition64 * partp,
		       Volume * vp,
		       VolId vid)
{
    Volume *nvp = NULL;

    *ec = 0;

    /* check to see if pre-attach already happened */
    if (vp &&
	(V_attachState(vp) != VOL_STATE_UNATTACHED) &&
	(V_attachState(vp) != VOL_STATE_DELETED) &&
	(V_attachState(vp) != VOL_STATE_PREATTACHED) &&
	!VIsErrorState(V_attachState(vp))) {
	/*
	 * pre-attach is a no-op in all but the following cases:
	 *
	 *   - volume is unattached
	 *   - volume is in an error state
	 *   - volume is pre-attached
	 */
	Log("VPreattachVolumeByVp_r: volume %u not in quiescent state\n", vid);
	goto done;
    } else if (vp) {
	/* we're re-attaching a volume; clear out some old state */
	memset(&vp->salvage, 0, sizeof(struct VolumeOnlineSalvage));

	if (V_partition(vp) != partp) {
	    /* XXX potential race */
	    DeleteVolumeFromVByPList_r(vp);
	}
    } else {
	/* if we need to allocate a new Volume struct,
	 * go ahead and drop the vol glock, otherwise
	 * do the basic setup synchronised, as it's
	 * probably not worth dropping the lock */
	VOL_UNLOCK;

	/* allocate the volume structure */
	vp = nvp = (Volume *) malloc(sizeof(Volume));
	assert(vp != NULL);
	memset(vp, 0, sizeof(Volume));
	queue_Init(&vp->vnode_list);
	assert(pthread_cond_init(&V_attachCV(vp), NULL) == 0);
    }

    /* link the volume with its associated vice partition */
    vp->device = partp->device;
    vp->partition = partp;

    vp->hashid = vid;
    vp->specialStatus = 0;

    /* if we dropped the lock, reacquire the lock,
     * check for pre-attach races, and then add
     * the volume to the hash table */
    if (nvp) {
	VOL_LOCK;
	nvp = VLookupVolume_r(ec, vid, NULL);
	if (*ec) {
	    free(vp);
	    vp = NULL;
	    goto done;
	} else if (nvp) { /* race detected */
	    free(vp);
	    vp = nvp;
	    goto done;
	} else {
	  /* hack to make up for VChangeState_r() decrementing
	   * the old state counter */
	  VStats.state_levels[0]++;
	}
    }

    /* put pre-attached volume onto the hash table
     * and bring it up to the pre-attached state */
    AddVolumeToHashTable(vp, vp->hashid);
    AddVolumeToVByPList_r(vp);
    VLRU_Init_Node_r(vp);
    VChangeState_r(vp, VOL_STATE_PREATTACHED);

    if (LogLevel >= 5)
	Log("VPreAttachVolumeByVp_r:  volume %u pre-attached\n", vp->hashid);

  done:
    if (*ec)
	return NULL;
    else
	return vp;
}
#endif /* AFS_DEMAND_ATTACH_FS */

/* Attach an existing volume, given its pathname, and return a
   pointer to the volume header information.  The volume also
   normally goes online at this time.  An offline volume
   must be reattached to make it go online */
Volume *
VAttachVolumeByName(Error * ec, char *partition, char *name, int mode)
{
    Volume *retVal;
    VOL_LOCK;
    retVal = VAttachVolumeByName_r(ec, partition, name, mode);
    VOL_UNLOCK;
    return retVal;
}

Volume *
VAttachVolumeByName_r(Error * ec, char *partition, char *name, int mode)
{
    Volume *vp = NULL;
    struct DiskPartition64 *partp;
    char path[64];
    int isbusy = 0;
    VolId volumeId;
#ifdef AFS_DEMAND_ATTACH_FS
    VolumeStats stats_save;
    Volume *svp = NULL;
#endif /* AFS_DEMAND_ATTACH_FS */

    *ec = 0;

    volumeId = VolumeNumber(name);

    if (!(partp = VGetPartition_r(partition, 0))) {
	*ec = VNOVOL;
	Log("VAttachVolume: Error getting partition (%s)\n", partition);
	goto done;
    }

    if (VRequiresPartLock()) {
	assert(VInit == 3);
	VLockPartition_r(partition);
    } else if (programType == fileServer) {
#ifdef AFS_DEMAND_ATTACH_FS
	/* lookup the volume in the hash table */
	vp = VLookupVolume_r(ec, volumeId, NULL);
	if (*ec) {
	    return NULL;
	}

	if (vp) {
	    /* save any counters that are supposed to
	     * be monotonically increasing over the
	     * lifetime of the fileserver */
	    memcpy(&stats_save, &vp->stats, sizeof(VolumeStats));
	} else {
	    memset(&stats_save, 0, sizeof(VolumeStats));
	}

	/* if there's something in the hash table, and it's not
	 * in the pre-attach state, then we may need to detach
	 * it before proceeding */
	if (vp && (V_attachState(vp) != VOL_STATE_PREATTACHED)) {
	    VCreateReservation_r(vp);
	    VWaitExclusiveState_r(vp);

	    /* at this point state must be one of:
	     *   - UNATTACHED
	     *   - ATTACHED
	     *   - SHUTTING_DOWN
	     *   - GOING_OFFLINE
	     *   - SALVAGING
	     *   - ERROR
	     *   - DELETED
	     */

	    if (vp->specialStatus == VBUSY)
		isbusy = 1;

	    /* if it's already attached, see if we can return it */
	    if (V_attachState(vp) == VOL_STATE_ATTACHED) {
		VGetVolumeByVp_r(ec, vp);
		if (V_inUse(vp) == fileServer) {
		    VCancelReservation_r(vp);
		    return vp;
		}

		/* otherwise, we need to detach, and attempt to re-attach */
		VDetachVolume_r(ec, vp);
		if (*ec) {
		    Log("VAttachVolume: Error detaching old volume instance (%s)\n", name);
		}
	    } else {
		/* if it isn't fully attached, delete from the hash tables,
		   and let the refcounter handle the rest */
		DeleteVolumeFromHashTable(vp);
		DeleteVolumeFromVByPList_r(vp);
	    }

	    VCancelReservation_r(vp);
	    vp = NULL;
	}

	/* pre-attach volume if it hasn't been done yet */
	if (!vp ||
	    (V_attachState(vp) == VOL_STATE_UNATTACHED) ||
	    (V_attachState(vp) == VOL_STATE_DELETED) ||
	    (V_attachState(vp) == VOL_STATE_ERROR)) {
	    svp = vp;
	    vp = VPreAttachVolumeByVp_r(ec, partp, vp, volumeId);
	    if (*ec) {
		return NULL;
	    }
	}

	assert(vp != NULL);

	/* handle pre-attach races
	 *
	 * multiple threads can race to pre-attach a volume,
	 * but we can't let them race beyond that
	 *
	 * our solution is to let the first thread to bring
	 * the volume into an exclusive state win; the other
	 * threads just wait until it finishes bringing the
	 * volume online, and then they do a vgetvolumebyvp
	 */
	if (svp && (svp != vp)) {
	    /* wait for other exclusive ops to finish */
	    VCreateReservation_r(vp);
	    VWaitExclusiveState_r(vp);

	    /* get a heavyweight ref, kill the lightweight ref, and return */
	    VGetVolumeByVp_r(ec, vp);
	    VCancelReservation_r(vp);
	    return vp;
	}

	/* at this point, we are chosen as the thread to do
	 * demand attachment for this volume. all other threads
	 * doing a getvolume on vp->hashid will block until we finish */

	/* make sure any old header cache entries are invalidated
	 * before proceeding */
	FreeVolumeHeader(vp);

	VChangeState_r(vp, VOL_STATE_ATTACHING);

	/* restore any saved counters */
	memcpy(&vp->stats, &stats_save, sizeof(VolumeStats));
#else /* AFS_DEMAND_ATTACH_FS */
	vp = VGetVolume_r(ec, volumeId);
	if (vp) {
	    if (V_inUse(vp) == fileServer)
		return vp;
	    if (vp->specialStatus == VBUSY)
		isbusy = 1;
	    VDetachVolume_r(ec, vp);
	    if (*ec) {
		Log("VAttachVolume: Error detaching volume (%s)\n", name);
	    }
	    vp = NULL;
	}
#endif /* AFS_DEMAND_ATTACH_FS */
    }

    *ec = 0;
    strcpy(path, VPartitionPath(partp));

    VOL_UNLOCK;

    strcat(path, "/");
    strcat(path, name);

    if (!vp) {
      vp = (Volume *) calloc(1, sizeof(Volume));
      assert(vp != NULL);
      vp->hashid = volumeId;
      vp->device = partp->device;
      vp->partition = partp;
      queue_Init(&vp->vnode_list);
#ifdef AFS_DEMAND_ATTACH_FS
      assert(pthread_cond_init(&V_attachCV(vp), NULL) == 0);
#endif /* AFS_DEMAND_ATTACH_FS */
    }

    /* attach2 is entered without any locks, and returns
     * with vol_glock_mutex held */
    vp = attach2(ec, volumeId, path, partp, vp, isbusy, mode);

    if (VCanUseFSSYNC() && vp) {
#ifdef AFS_DEMAND_ATTACH_FS
	if ((mode == V_VOLUPD) || (VolumeWriteable(vp) && (mode == V_CLONE))) {
	    /* mark volume header as in use so that volser crashes lead to a
	     * salvage attempt */
	    VUpdateVolume_r(ec, vp, 0);
	}
	/* for dafs, we should tell the fileserver, except for V_PEEK
         * where we know it is not necessary */
	if (mode == V_PEEK) {
	    vp->needsPutBack = 0;
	} else {
	    vp->needsPutBack = 1;
	}
#else /* !AFS_DEMAND_ATTACH_FS */
	/* duplicate computation in fssync.c about whether the server
	 * takes the volume offline or not.  If the volume isn't
	 * offline, we must not return it when we detach the volume,
	 * or the server will abort */
	if (mode == V_READONLY || mode == V_PEEK
	    || (!VolumeWriteable(vp) && (mode == V_CLONE || mode == V_DUMP)))
	    vp->needsPutBack = 0;
	else
	    vp->needsPutBack = 1;
#endif /* !AFS_DEMAND_ATTACH_FS */
    }
    /* OK, there's a problem here, but one that I don't know how to
     * fix right now, and that I don't think should arise often.
     * Basically, we should only put back this volume to the server if
     * it was given to us by the server, but since we don't have a vp,
     * we can't run the VolumeWriteable function to find out as we do
     * above when computing vp->needsPutBack.  So we send it back, but
     * there's a path in VAttachVolume on the server which may abort
     * if this volume doesn't have a header.  Should be pretty rare
     * for all of that to happen, but if it does, probably the right
     * fix is for the server to allow the return of readonly volumes
     * that it doesn't think are really checked out. */
#ifdef FSSYNC_BUILD_CLIENT
    if (VCanUseFSSYNC() && vp == NULL &&
	mode != V_SECRETLY && mode != V_PEEK) {

#ifdef AFS_DEMAND_ATTACH_FS
        /* If we couldn't attach but we scheduled a salvage, we already
         * notified the fileserver; don't online it now */
        if (*ec != VSALVAGING)
#endif /* AFS_DEMAND_ATTACH_FS */
	FSYNC_VolOp(volumeId, partition, FSYNC_VOL_ON, 0, NULL);
    } else
#endif
    if (programType == fileServer && vp) {
#ifdef AFS_DEMAND_ATTACH_FS
	/*
	 * we can get here in cases where we don't "own"
	 * the volume (e.g. volume owned by a utility).
	 * short circuit around potential disk header races.
	 */
	if (V_attachState(vp) != VOL_STATE_ATTACHED) {
	    goto done;
	}
#endif
	VUpdateVolume_r(ec, vp, 0);
	if (*ec) {
	    Log("VAttachVolume: Error updating volume\n");
	    if (vp)
		VPutVolume_r(vp);
	    goto done;
	}
	if (VolumeWriteable(vp) && V_dontSalvage(vp) == 0) {
#ifndef AFS_DEMAND_ATTACH_FS
	    /* This is a hack: by temporarily setting the incore
	     * dontSalvage flag ON, the volume will be put back on the
	     * Update list (with dontSalvage OFF again).  It will then
	     * come back in N minutes with DONT_SALVAGE eventually
	     * set.  This is the way that volumes that have never had
	     * it set get it set; or that volumes that have been
	     * offline without DONT SALVAGE having been set also
	     * eventually get it set */
	    V_dontSalvage(vp) = DONT_SALVAGE;
#endif /* !AFS_DEMAND_ATTACH_FS */
	    VAddToVolumeUpdateList_r(ec, vp);
	    if (*ec) {
		Log("VAttachVolume: Error adding volume to update list\n");
		if (vp)
		    VPutVolume_r(vp);
		goto done;
	    }
	}
	if (LogLevel)
	    Log("VOnline:  volume %u (%s) attached and online\n", V_id(vp),
		V_name(vp));
    }

  done:
    if (VRequiresPartLock()) {
	VUnlockPartition_r(partition);
    }
    if (*ec) {
#ifdef AFS_DEMAND_ATTACH_FS
	/* attach failed; make sure we're in error state */
	if (vp && !VIsErrorState(V_attachState(vp))) {
	    VChangeState_r(vp, VOL_STATE_ERROR);
	}
#endif /* AFS_DEMAND_ATTACH_FS */
	return NULL;
    } else {
	return vp;
    }
}

#ifdef AFS_DEMAND_ATTACH_FS
/* VAttachVolumeByVp_r
 *
 * finish attaching a volume that is
 * in a less than fully attached state
 */
/* caller MUST hold a ref count on vp */
static Volume *
VAttachVolumeByVp_r(Error * ec, Volume * vp, int mode)
{
    char name[VMAXPATHLEN];
    int reserve = 0;
    struct DiskPartition64 *partp;
    char path[64];
    int isbusy = 0;
    VolId volumeId;
    Volume * nvp = NULL;
    VolumeStats stats_save;
    *ec = 0;

    /* volume utility should never call AttachByVp */
    assert(programType == fileServer);

    volumeId = vp->hashid;
    partp = vp->partition;
    VolumeExternalName_r(volumeId, name, sizeof(name));


    /* if another thread is performing a blocking op, wait */
    VWaitExclusiveState_r(vp);

    memcpy(&stats_save, &vp->stats, sizeof(VolumeStats));

    /* if it's already attached, see if we can return it */
    if (V_attachState(vp) == VOL_STATE_ATTACHED) {
	VGetVolumeByVp_r(ec, vp);
	if (V_inUse(vp) == fileServer) {
	    return vp;
	} else {
	    if (vp->specialStatus == VBUSY)
		isbusy = 1;
	    VDetachVolume_r(ec, vp);
	    if (*ec) {
		Log("VAttachVolume: Error detaching volume (%s)\n", name);
	    }
	    vp = NULL;
	}
    }

    /* pre-attach volume if it hasn't been done yet */
    if (!vp ||
	(V_attachState(vp) == VOL_STATE_UNATTACHED) ||
	(V_attachState(vp) == VOL_STATE_DELETED) ||
	(V_attachState(vp) == VOL_STATE_ERROR)) {
	nvp = VPreAttachVolumeByVp_r(ec, partp, vp, volumeId);
	if (*ec) {
	    return NULL;
	}
	if (nvp != vp) {
	    reserve = 1;
	    VCreateReservation_r(nvp);
	    vp = nvp;
	}
    }

    assert(vp != NULL);
    VChangeState_r(vp, VOL_STATE_ATTACHING);

    /* restore monotonically increasing stats */
    memcpy(&vp->stats, &stats_save, sizeof(VolumeStats));

    *ec = 0;

    /* compute path to disk header */
    strcpy(path, VPartitionPath(partp));

    VOL_UNLOCK;

    strcat(path, "/");
    strcat(path, name);

    /* do volume attach
     *
     * NOTE: attach2 is entered without any locks, and returns
     * with vol_glock_mutex held */
    vp = attach2(ec, volumeId, path, partp, vp, isbusy, mode);

    /*
     * the event that an error was encountered, or
     * the volume was not brought to an attached state
     * for any reason, skip to the end.  We cannot
     * safely call VUpdateVolume unless we "own" it.
     */
    if (*ec ||
	(vp == NULL) ||
	(V_attachState(vp) != VOL_STATE_ATTACHED)) {
	goto done;
    }

    VUpdateVolume_r(ec, vp, 0);
    if (*ec) {
	Log("VAttachVolume: Error updating volume %u\n", vp->hashid);
	VPutVolume_r(vp);
	goto done;
    }
    if (VolumeWriteable(vp) && V_dontSalvage(vp) == 0) {
#ifndef AFS_DEMAND_ATTACH_FS
	/* This is a hack: by temporarily setting the incore
	 * dontSalvage flag ON, the volume will be put back on the
	 * Update list (with dontSalvage OFF again).  It will then
	 * come back in N minutes with DONT_SALVAGE eventually
	 * set.  This is the way that volumes that have never had
	 * it set get it set; or that volumes that have been
	 * offline without DONT SALVAGE having been set also
	 * eventually get it set */
	V_dontSalvage(vp) = DONT_SALVAGE;
#endif /* !AFS_DEMAND_ATTACH_FS */
	VAddToVolumeUpdateList_r(ec, vp);
	if (*ec) {
	    Log("VAttachVolume: Error adding volume %u to update list\n", vp->hashid);
	    if (vp)
		VPutVolume_r(vp);
	    goto done;
	}
    }
    if (LogLevel)
	Log("VOnline:  volume %u (%s) attached and online\n", V_id(vp),
	    V_name(vp));
  done:
    if (reserve) {
	VCancelReservation_r(nvp);
	reserve = 0;
    }
    if (*ec && (*ec != VOFFLINE) && (*ec != VSALVAGE)) {
	if (vp && !VIsErrorState(V_attachState(vp))) {
	    VChangeState_r(vp, VOL_STATE_ERROR);
	}
	return NULL;
    } else {
	return vp;
    }
}

/**
 * lock a volume on disk (non-blocking).
 *
 * @param[in] vp  The volume to lock
 * @param[in] locktype READ_LOCK or WRITE_LOCK
 *
 * @return operation status
 *  @retval 0 success, lock was obtained
 *  @retval EBUSY a conflicting lock was held by another process
 *  @retval EIO   error acquiring lock
 *
 * @pre If we're in the fileserver, vp is in an exclusive state
 *
 * @pre vp is not already locked
 */
static int
VLockVolumeNB(Volume *vp, int locktype)
{
    int code;

    assert(programType != fileServer || VIsExclusiveState(V_attachState(vp)));
    assert(!(V_attachFlags(vp) & VOL_LOCKED));

    code = VLockVolumeByIdNB(vp->hashid, vp->partition, locktype);
    if (code == 0) {
	V_attachFlags(vp) |= VOL_LOCKED;
    }

    return code;
}

/**
 * unlock a volume on disk that was locked with VLockVolumeNB.
 *
 * @param[in] vp  volume to unlock
 *
 * @pre If we're in the fileserver, vp is in an exclusive state
 *
 * @pre vp has already been locked
 */
static void
VUnlockVolume(Volume *vp)
{
    assert(programType != fileServer || VIsExclusiveState(V_attachState(vp)));
    assert((V_attachFlags(vp) & VOL_LOCKED));

    VUnlockVolumeById(vp->hashid, vp->partition);

    V_attachFlags(vp) &= ~VOL_LOCKED;
}
#endif /* AFS_DEMAND_ATTACH_FS */

/**
 * read in a vol header, possibly lock the vol header, and possibly check out
 * the vol header from the fileserver, as part of volume attachment.
 *
 * @param[out] ec     error code
 * @param[in] vp      volume pointer object
 * @param[in] partp   disk partition object of the attaching partition
 * @param[in] mode    attachment mode such as V_VOLUPD, V_DUMP, etc (see
 *                    volume.h)
 * @param[in] peek    1 to just try to read in the volume header and make sure
 *                    we don't try to lock the vol, or check it out from
 *                    FSSYNC or anything like that; 0 otherwise, for 'normal'
 *                    operation
 *
 * @note As part of DAFS volume attachment, the volume header may be either
 *       read- or write-locked to ensure mutual exclusion of certain volume
 *       operations. In some cases in order to determine whether we need to
 *       read- or write-lock the header, we need to read in the header to see
 *       if the volume is RW or not. So, if we read in the header under a
 *       read-lock and determine that we actually need a write-lock on the
 *       volume header, this function will drop the read lock, acquire a write
 *       lock, and read the header in again.
 */
static void
attach_volume_header(Error *ec, Volume *vp, struct DiskPartition64 *partp,
                     int mode, int peek)
{
    struct VolumeDiskHeader diskHeader;
    struct VolumeHeader header;
    int code;
    int first_try = 1;
    int lock_tries = 0, checkout_tries = 0;
    int retry;
    VolumeId volid = vp->hashid;
#ifdef FSSYNC_BUILD_CLIENT
    int checkout, done_checkout = 0;
#endif /* FSSYNC_BUILD_CLIENT */
#ifdef AFS_DEMAND_ATTACH_FS
    int locktype = 0, use_locktype = -1;
#endif /* AFS_DEMAND_ATTACH_FS */

 retry:
    retry = 0;
    *ec = 0;

    if (lock_tries > VOL_MAX_CHECKOUT_RETRIES) {
	Log("VAttachVolume: retried too many times trying to lock header for "
	    "vol %lu part %s; giving up\n", afs_printable_uint32_lu(volid),
	    VPartitionPath(partp));
	*ec = VNOVOL;
	goto done;
    }
    if (checkout_tries > VOL_MAX_CHECKOUT_RETRIES) {
	Log("VAttachVolume: retried too many times trying to checkout "
	    "vol %lu part %s; giving up\n", afs_printable_uint32_lu(volid),
	    VPartitionPath(partp));
	*ec = VNOVOL;
	goto done;
    }

    if (VReadVolumeDiskHeader(volid, partp, NULL)) {
	/* short-circuit the 'volume does not exist' case */
	*ec = VNOVOL;
	goto done;
    }

#ifdef FSSYNC_BUILD_CLIENT
    checkout = !done_checkout;
    done_checkout = 1;
    if (!peek && checkout && VMustCheckoutVolume(mode)) {
        SYNC_response res;
        memset(&res, 0, sizeof(res));

	if (FSYNC_VolOp(volid, VPartitionPath(partp), FSYNC_VOL_NEEDVOLUME, mode, &res)
	    != SYNC_OK) {

            if (res.hdr.reason == FSYNC_SALVAGE) {
                Log("VAttachVolume: file server says volume %lu is salvaging\n",
                     afs_printable_uint32_lu(volid));
                *ec = VSALVAGING;
            } else {
	        Log("VAttachVolume: attach of volume %lu apparently denied by file server\n",
                     afs_printable_uint32_lu(volid));
	        *ec = VNOVOL;	/* XXXX */
            }
	    goto done;
	}
    }
#endif

#ifdef AFS_DEMAND_ATTACH_FS
    if (use_locktype < 0) {
	/* don't know whether vol is RO or RW; assume it's RO and we can retry
	 * if it turns out to be RW */
	locktype = VVolLockType(mode, 0);

    } else {
	/* a previous try says we should use use_locktype to lock the volume,
	 * so use that */
	locktype = use_locktype;
    }

    if (!peek && locktype) {
	code = VLockVolumeNB(vp, locktype);
	if (code) {
	    if (code == EBUSY) {
		Log("VAttachVolume: another program has vol %lu locked\n",
	            afs_printable_uint32_lu(volid));
	    } else {
		Log("VAttachVolume: error %d trying to lock vol %lu\n",
	            code, afs_printable_uint32_lu(volid));
	    }

	    *ec = VNOVOL;
	    goto done;
	}
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    code = VReadVolumeDiskHeader(volid, partp, &diskHeader);
    if (code) {
	if (code == EIO) {
	    *ec = VSALVAGE;
	} else {
	    *ec = VNOVOL;
	}
	goto done;
    }

    DiskToVolumeHeader(&header, &diskHeader);

    IH_INIT(vp->vnodeIndex[vLarge].handle, partp->device, header.parent,
	    header.largeVnodeIndex);
    IH_INIT(vp->vnodeIndex[vSmall].handle, partp->device, header.parent,
	    header.smallVnodeIndex);
    IH_INIT(vp->diskDataHandle, partp->device, header.parent,
	    header.volumeInfo);
    IH_INIT(vp->linkHandle, partp->device, header.parent, header.linkTable);
    if (header.fileACL == 0) {
	vp->fileACLHandle = 0;
    } else {
	IH_INIT(vp->fileACLHandle, partp->device, header.parent, header.fileACL);

    }

    if (first_try) {
	/* only need to do this once */
	VOL_LOCK;
	GetVolumeHeader(vp);
	VOL_UNLOCK;
    }

#if defined(AFS_DEMAND_ATTACH_FS) && defined(FSSYNC_BUILD_CLIENT)
    /* demand attach changes the V_PEEK mechanism
     *
     * we can now suck the current disk data structure over
     * the fssync interface without going to disk
     *
     * (technically, we don't need to restrict this feature
     *  to demand attach fileservers.  However, I'm trying
     *  to limit the number of common code changes)
     */
    if (VCanUseFSSYNC() && (mode == V_PEEK || peek)) {
	SYNC_response res;
	res.payload.len = sizeof(VolumeDiskData);
	res.payload.buf = &vp->header->diskstuff;

	if (FSYNC_VolOp(vp->hashid,
			partp->name,
			FSYNC_VOL_QUERY_HDR,
			FSYNC_WHATEVER,
			&res) == SYNC_OK) {
	    goto disk_header_loaded;
	}
    }
#endif /* AFS_DEMAND_ATTACH_FS && FSSYNC_BUILD_CLIENT */
    (void)ReadHeader(ec, V_diskDataHandle(vp), (char *)&V_disk(vp),
		     sizeof(V_disk(vp)), VOLUMEINFOMAGIC, VOLUMEINFOVERSION);

#ifdef AFS_DEMAND_ATTACH_FS
    /* update stats */
    VOL_LOCK;
    IncUInt64(&VStats.hdr_loads);
    IncUInt64(&vp->stats.hdr_loads);
    VOL_UNLOCK;
#endif /* AFS_DEMAND_ATTACH_FS */

    if (*ec) {
	Log("VAttachVolume: Error reading diskDataHandle header for vol %lu; "
	    "error=%u\n", afs_printable_uint32_lu(volid), *ec);
	goto done;
    }

#ifdef AFS_DEMAND_ATTACH_FS
# ifdef FSSYNC_BUILD_CLIENT
 disk_header_loaded:
# endif /* FSSYNC_BUILD_CLIENT */

    /* if the lock type we actually used to lock the volume is different than
     * the lock type we should have used, retry with the lock type we should
     * use */
    use_locktype = VVolLockType(mode, VolumeWriteable(vp));
    if (locktype != use_locktype) {
	retry = 1;
	lock_tries++;
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    *ec = 0;

 done:
#if defined(AFS_DEMAND_ATTACH_FS) && defined(FSSYNC_BUILD_CLIENT)
    if (!peek && *ec == 0 && retry == 0 && VMustCheckoutVolume(mode)) {

	code = FSYNC_VerifyCheckout(volid, VPartitionPath(partp), FSYNC_VOL_NEEDVOLUME, mode);

	if (code == SYNC_DENIED) {
	    /* must retry checkout; fileserver no longer thinks we have
	     * the volume */
	    retry = 1;
	    checkout_tries++;
	    done_checkout = 0;

	} else if (code != SYNC_OK) {
	    *ec = VNOVOL;
	}
    }
#endif /* AFS_DEMAND_ATTACH_FS && FSSYNC_BUILD_CLIENT */

    if (*ec || retry) {
	/* either we are going to be called again for a second pass, or we
	 * encountered an error; clean up in either case */

#ifdef AFS_DEMAND_ATTACH_FS
	if ((V_attachFlags(vp) & VOL_LOCKED)) {
	    VUnlockVolume(vp);
	}
#endif /* AFS_DEMAND_ATTACH_FS */
	if (vp->linkHandle) {
	    IH_RELEASE(vp->vnodeIndex[vLarge].handle);
	    IH_RELEASE(vp->vnodeIndex[vSmall].handle);
	    IH_RELEASE(vp->diskDataHandle);
	    IH_RELEASE(vp->linkHandle);
	}
    }

    if (*ec) {
	return;
    }
    if (retry) {
	first_try = 0;
	goto retry;
    }
}

#ifdef AFS_DEMAND_ATTACH_FS
static void
attach_check_vop(Error *ec, VolumeId volid, struct DiskPartition64 *partp,
                 Volume *vp)
{
    *ec = 0;

    if (vp->pending_vol_op) {

	VOL_LOCK;

	if (vp->pending_vol_op->vol_op_state == FSSYNC_VolOpRunningUnknown) {
	    int code;
	    code = VVolOpLeaveOnlineNoHeader_r(vp, vp->pending_vol_op);
	    if (code == 1) {
		vp->pending_vol_op->vol_op_state = FSSYNC_VolOpRunningOnline;
	    } else if (code == 0) {
		vp->pending_vol_op->vol_op_state = FSSYNC_VolOpRunningOffline;

	    } else {
		/* we need the vol header to determine if the volume can be
		 * left online for the vop, so... get the header */

		VOL_UNLOCK;

		/* attach header with peek=1 to avoid checking out the volume
		 * or locking it; we just want the header info, we're not
		 * messing with the volume itself at all */
		attach_volume_header(ec, vp, partp, V_PEEK, 1);
		if (*ec) {
		    return;
		}

		VOL_LOCK;

		if (VVolOpLeaveOnline_r(vp, vp->pending_vol_op)) {
		    vp->pending_vol_op->vol_op_state = FSSYNC_VolOpRunningOnline;
		} else {
		    vp->pending_vol_op->vol_op_state = FSSYNC_VolOpRunningOffline;
		}

		/* make sure we grab a new vol header and re-open stuff on
		 * actual attachment; we can't keep the data we grabbed, since
		 * it was not done under a lock and thus not safe */
		FreeVolumeHeader(vp);
		VReleaseVolumeHandles_r(vp);
	    }
	}
	/* see if the pending volume op requires exclusive access */
	switch (vp->pending_vol_op->vol_op_state) {
	case FSSYNC_VolOpPending:
	    /* this should never happen */
	    assert(vp->pending_vol_op->vol_op_state != FSSYNC_VolOpPending);
	    break;

	case FSSYNC_VolOpRunningUnknown:
	    /* this should never happen; we resolved 'unknown' above */
	    assert(vp->pending_vol_op->vol_op_state != FSSYNC_VolOpRunningUnknown);
	    break;

	case FSSYNC_VolOpRunningOffline:
	    /* mark the volume down */
	    *ec = VOFFLINE;
	    VChangeState_r(vp, VOL_STATE_UNATTACHED);

	    /* do not set V_offlineMessage here; we don't have ownership of
	     * the volume (and probably do not have the header loaded), so we
	     * can't alter the disk header */

	    /* check to see if we should set the specialStatus flag */
	    if (VVolOpSetVBusy_r(vp, vp->pending_vol_op)) {
	        vp->specialStatus = VBUSY;
	    }
	    break;

	default:
	    break;
	}

	VOL_UNLOCK;
    }
}
#endif /* AFS_DEMAND_ATTACH_FS */

/**
 * volume attachment helper function.
 *
 * @param[out] ec      error code
 * @param[in] volumeId volume ID of the attaching volume
 * @param[in] path     full path to the volume header .vol file
 * @param[in] partp    disk partition object for the attaching partition
 * @param[in] vp       volume object; vp->hashid, vp->device, vp->partition,
 *                     vp->vnode_list, and V_attachCV (for DAFS) should already
 *                     be initialized
 * @param[in] isbusy   1 if vp->specialStatus should be set to VBUSY; that is,
 *                     if there is a volume operation running for this volume
 *                     that should set the volume to VBUSY during its run. 0
 *                     otherwise. (see VVolOpSetVBusy_r)
 * @param[in] mode     attachment mode such as V_VOLUPD, V_DUMP, etc (see
 *                     volume.h)
 *
 * @return pointer to the semi-attached volume pointer
 *  @retval NULL an error occurred (check value of *ec)
 *  @retval vp volume successfully attaching
 *
 * @pre no locks held
 *
 * @post VOL_LOCK held
 */
static Volume *
attach2(Error * ec, VolId volumeId, char *path, struct DiskPartition64 *partp,
        Volume * vp, int isbusy, int mode)
{
    /* have we read in the header successfully? */
    int read_header = 0;

    /* should we FreeVolume(vp) instead of VCheckFree(vp) in the error
     * cleanup? */
    int forcefree = 0;

#ifdef AFS_DEMAND_ATTACH_FS
    /* in the case of an error, to what state should the volume be
     * transitioned? */
    VolState error_state = VOL_STATE_ERROR;
#endif /* AFS_DEMAND_ATTACH_FS */

    *ec = 0;

    vp->vnodeIndex[vLarge].handle = NULL;
    vp->vnodeIndex[vSmall].handle = NULL;
    vp->diskDataHandle = NULL;
    vp->linkHandle = NULL;

#ifdef AFS_DEMAND_ATTACH_FS
    attach_check_vop(ec, volumeId, partp, vp);
    if (!*ec) {
	attach_volume_header(ec, vp, partp, mode, 0);
    }
#else
    attach_volume_header(ec, vp, partp, mode, 0);
#endif /* !AFS_DEMAND_ATTACH_FS */

    if (*ec == VNOVOL) {
	/* if the volume doesn't exist, skip straight to 'error' so we don't
	 * request a salvage */
	goto error;
    }

    if (!*ec) {
	read_header = 1;

	vp->specialStatus = (byte) (isbusy ? VBUSY : 0);
	vp->shuttingDown = 0;
	vp->goingOffline = 0;
	vp->nUsers = 1;
#ifdef AFS_DEMAND_ATTACH_FS
	vp->stats.last_attach = FT_ApproxTime();
	vp->stats.attaches++;
#endif

	VOL_LOCK;
	IncUInt64(&VStats.attaches);
	vp->cacheCheck = ++VolumeCacheCheck;
	/* just in case this ever rolls over */
	if (!vp->cacheCheck)
	    vp->cacheCheck = ++VolumeCacheCheck;
	VOL_UNLOCK;

#ifdef AFS_DEMAND_ATTACH_FS
	V_attachFlags(vp) |= VOL_HDR_LOADED;
	vp->stats.last_hdr_load = vp->stats.last_attach;
#endif /* AFS_DEMAND_ATTACH_FS */
    }

    if (!*ec) {
	struct IndexFileHeader iHead;

#if OPENAFS_VOL_STATS
	/*
	 * We just read in the diskstuff part of the header.  If the detailed
	 * volume stats area has not yet been initialized, we should bzero the
	 * area and mark it as initialized.
	 */
	if (!(V_stat_initialized(vp))) {
	    memset((V_stat_area(vp)), 0, VOL_STATS_BYTES);
	    V_stat_initialized(vp) = 1;
	}
#endif /* OPENAFS_VOL_STATS */

	(void)ReadHeader(ec, vp->vnodeIndex[vSmall].handle,
			 (char *)&iHead, sizeof(iHead),
			 SMALLINDEXMAGIC, SMALLINDEXVERSION);

	if (*ec) {
	    Log("VAttachVolume: Error reading smallVnode vol header %s; error=%u\n", path, *ec);
	}
    }

    if (!*ec) {
	struct IndexFileHeader iHead;

	(void)ReadHeader(ec, vp->vnodeIndex[vLarge].handle,
			 (char *)&iHead, sizeof(iHead),
			 LARGEINDEXMAGIC, LARGEINDEXVERSION);

	if (*ec) {
	    Log("VAttachVolume: Error reading largeVnode vol header %s; error=%u\n", path, *ec);
	}
    }

#ifdef AFS_NAMEI_ENV
    if (!*ec) {
	struct versionStamp stamp;

	(void)ReadHeader(ec, V_linkHandle(vp), (char *)&stamp,
			 sizeof(stamp), LINKTABLEMAGIC, LINKTABLEVERSION);

	if (*ec) {
	    Log("VAttachVolume: Error reading namei vol header %s; error=%u\n", path, *ec);
	}
    }
#endif /* AFS_NAMEI_ENV */

#if defined(AFS_DEMAND_ATTACH_FS)
    if (*ec && ((*ec != VOFFLINE) || (V_attachState(vp) != VOL_STATE_UNATTACHED))) {
        VOL_LOCK;
	if (!VCanScheduleSalvage()) {
	    Log("VAttachVolume: Error attaching volume %s; volume needs salvage; error=%u\n", path, *ec);
	}
	VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
	vp->nUsers = 0;

	goto error;
    } else if (*ec) {
	/* volume operation in progress */
	VOL_LOCK;
	goto error;
    }
#else /* AFS_DEMAND_ATTACH_FS */
    if (*ec) {
	Log("VAttachVolume: Error attaching volume %s; volume needs salvage; error=%u\n", path, *ec);
        VOL_LOCK;
	goto error;
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    if (V_needsSalvaged(vp)) {
	if (vp->specialStatus)
	    vp->specialStatus = 0;
        VOL_LOCK;
#if defined(AFS_DEMAND_ATTACH_FS)
	if (!VCanScheduleSalvage()) {
	    Log("VAttachVolume: volume salvage flag is ON for %s; volume needs salvage\n", path);
	}
	VRequestSalvage_r(ec, vp, SALVSYNC_NEEDED, VOL_SALVAGE_INVALIDATE_HEADER);
	vp->nUsers = 0;

#else /* AFS_DEMAND_ATTACH_FS */
	*ec = VSALVAGE;
#endif /* AFS_DEMAND_ATTACH_FS */

	goto error;
    }

    VOL_LOCK;
    vp->nextVnodeUnique = V_uniquifier(vp);

    if (VShouldCheckInUse(mode) && V_inUse(vp) && VolumeWriteable(vp)) {
	if (!V_needsSalvaged(vp)) {
	    V_needsSalvaged(vp) = 1;
	    VUpdateVolume_r(ec, vp, 0);
	}
#if defined(AFS_DEMAND_ATTACH_FS)
	if (!VCanScheduleSalvage()) {
	    Log("VAttachVolume: volume %s needs to be salvaged; not attached.\n", path);
	}
	VRequestSalvage_r(ec, vp, SALVSYNC_NEEDED, VOL_SALVAGE_INVALIDATE_HEADER);
	vp->nUsers = 0;

#else /* AFS_DEMAND_ATTACH_FS */
	Log("VAttachVolume: volume %s needs to be salvaged; not attached.\n", path);
	*ec = VSALVAGE;
#endif /* AFS_DEMAND_ATTACH_FS */

	goto error;
    }

    if (programType == fileServer && V_destroyMe(vp) == DESTROY_ME) {
	/* Only check destroyMe if we are the fileserver, since the
	 * volserver et al sometimes need to work with volumes with
	 * destroyMe set. Examples are 'temporary' volumes the
	 * volserver creates, and when we create a volume (destroyMe
	 * is set on creation; sometimes a separate volserver
	 * transaction is created to clear destroyMe).
	 */

#if defined(AFS_DEMAND_ATTACH_FS)
	/* schedule a salvage so the volume goes away on disk */
	VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
	VChangeState_r(vp, VOL_STATE_ERROR);
	vp->nUsers = 0;
#endif /* AFS_DEMAND_ATTACH_FS */
	Log("VAttachVolume: volume %s is junk; it should be destroyed at next salvage\n", path);
	*ec = VNOVOL;
	forcefree = 1;
	goto error;
    }

    vp->vnodeIndex[vSmall].bitmap = vp->vnodeIndex[vLarge].bitmap = NULL;
#ifndef BITMAP_LATER
    if (programType == fileServer && VolumeWriteable(vp)) {
	int i;
	for (i = 0; i < nVNODECLASSES; i++) {
	    VGetBitmap_r(ec, vp, i);
	    if (*ec) {
#ifdef AFS_DEMAND_ATTACH_FS
		VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
		vp->nUsers = 0;
#endif /* AFS_DEMAND_ATTACH_FS */
		Log("VAttachVolume: error getting bitmap for volume (%s)\n",
		    path);
		goto error;
	    }
	}
    }
#endif /* BITMAP_LATER */

    if (VInit >= 2 && V_needsCallback(vp)) {
	if (V_BreakVolumeCallbacks) {
	    Log("VAttachVolume: Volume %lu was changed externally; breaking callbacks\n",
	        afs_printable_uint32_lu(V_id(vp)));
	    V_needsCallback(vp) = 0;
	    VOL_UNLOCK;
	    (*V_BreakVolumeCallbacks) (V_id(vp));
	    VOL_LOCK;

	    VUpdateVolume_r(ec, vp, 0);
	}
#ifdef FSSYNC_BUILD_CLIENT
	else if (VCanUseFSSYNC()) {
	    afs_int32 fsync_code;

	    V_needsCallback(vp) = 0;
	    VOL_UNLOCK;
	    fsync_code = FSYNC_VolOp(V_id(vp), NULL, FSYNC_VOL_BREAKCBKS, FSYNC_WHATEVER, NULL);
	    VOL_LOCK;

	    if (fsync_code) {
	        V_needsCallback(vp) = 1;
		Log("Error trying to tell the fileserver to break callbacks for "
		    "changed volume %lu; error code %ld\n",
		    afs_printable_uint32_lu(V_id(vp)),
		    afs_printable_int32_ld(fsync_code));
	    } else {
		VUpdateVolume_r(ec, vp, 0);
	    }
	}
#endif /* FSSYNC_BUILD_CLIENT */

	if (*ec) {
	    Log("VAttachVolume: error %d clearing needsCallback on volume "
	        "%lu; needs salvage\n", (int)*ec,
	        afs_printable_uint32_lu(V_id(vp)));
#ifdef AFS_DEMAND_ATTACH_FS
	    VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
	    vp->nUsers = 0;
#else /* !AFS_DEMAND_ATTACH_FS */
	    *ec = VSALVAGE;
#endif /* !AFS_DEMAND_ATTACh_FS */
	    goto error;
	}
    }

    if (programType == fileServer) {
	if (vp->specialStatus)
	    vp->specialStatus = 0;
	if (V_blessed(vp) && V_inService(vp) && !V_needsSalvaged(vp)) {
	    V_inUse(vp) = fileServer;
	    V_offlineMessage(vp)[0] = '\0';
	}
	if (!V_inUse(vp)) {
	    *ec = VNOVOL;
#ifdef AFS_DEMAND_ATTACH_FS
	    /* Put the vol into PREATTACHED state, so if someone tries to
	     * access it again, we try to attach, see that we're not blessed,
	     * and give a VNOVOL error again. Putting it into UNATTACHED state
	     * would result in a VOFFLINE error instead. */
	    error_state = VOL_STATE_PREATTACHED;
#endif /* AFS_DEMAND_ATTACH_FS */

	    /* mimic e.g. GetVolume errors */
	    if (!V_blessed(vp)) {
		Log("Volume %lu offline: not blessed\n", afs_printable_uint32_lu(V_id(vp)));
		FreeVolumeHeader(vp);
	    } else if (!V_inService(vp)) {
		Log("Volume %lu offline: not in service\n", afs_printable_uint32_lu(V_id(vp)));
		FreeVolumeHeader(vp);
	    } else {
		Log("Volume %lu offline: needs salvage\n", afs_printable_uint32_lu(V_id(vp)));
		*ec = VSALVAGE;
#ifdef AFS_DEMAND_ATTACH_FS
		error_state = VOL_STATE_ERROR;
		/* see if we can recover */
		VRequestSalvage_r(ec, vp, SALVSYNC_NEEDED, VOL_SALVAGE_INVALIDATE_HEADER);
#endif
	    }
#ifdef AFS_DEMAND_ATTACH_FS
	    vp->nUsers = 0;
#endif
	    goto error;
	}
    } else {
#ifdef AFS_DEMAND_ATTACH_FS
	if ((mode != V_PEEK) && (mode != V_SECRETLY))
	    V_inUse(vp) = programType;
#endif /* AFS_DEMAND_ATTACH_FS */
	V_checkoutMode(vp) = mode;
    }

    AddVolumeToHashTable(vp, V_id(vp));
#ifdef AFS_DEMAND_ATTACH_FS
    if (VCanUnlockAttached() && (V_attachFlags(vp) & VOL_LOCKED)) {
	VUnlockVolume(vp);
    }
    if ((programType != fileServer) ||
	(V_inUse(vp) == fileServer)) {
	AddVolumeToVByPList_r(vp);
	VLRU_Add_r(vp);
	VChangeState_r(vp, VOL_STATE_ATTACHED);
    } else {
	VChangeState_r(vp, VOL_STATE_UNATTACHED);
    }
#endif

    return vp;

 error:
#ifdef AFS_DEMAND_ATTACH_FS
    if (!VIsErrorState(V_attachState(vp))) {
	VChangeState_r(vp, error_state);
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    if (read_header) {
	VReleaseVolumeHandles_r(vp);
    }

#ifdef AFS_DEMAND_ATTACH_FS
    VCheckSalvage(vp);
    if (forcefree) {
	FreeVolume(vp);
    } else {
	VCheckFree(vp);
    }
#else /* !AFS_DEMAND_ATTACH_FS */
    FreeVolume(vp);
#endif /* !AFS_DEMAND_ATTACH_FS */
    return NULL;
}

/* Attach an existing volume.
   The volume also normally goes online at this time.
   An offline volume must be reattached to make it go online.
 */

Volume *
VAttachVolume(Error * ec, VolumeId volumeId, int mode)
{
    Volume *retVal;
    VOL_LOCK;
    retVal = VAttachVolume_r(ec, volumeId, mode);
    VOL_UNLOCK;
    return retVal;
}

Volume *
VAttachVolume_r(Error * ec, VolumeId volumeId, int mode)
{
    char *part, *name;
    VGetVolumePath(ec, volumeId, &part, &name);
    if (*ec) {
	Volume *vp;
	Error error;
	vp = VGetVolume_r(&error, volumeId);
	if (vp) {
	    assert(V_inUse(vp) == 0);
	    VDetachVolume_r(ec, vp);
	}
	return NULL;
    }
    return VAttachVolumeByName_r(ec, part, name, mode);
}

/* Increment a reference count to a volume, sans context swaps.  Requires
 * possibly reading the volume header in from the disk, since there's
 * an invariant in the volume package that nUsers>0 ==> vp->header is valid.
 *
 * N.B. This call can fail if we can't read in the header!!  In this case
 * we still guarantee we won't context swap, but the ref count won't be
 * incremented (otherwise we'd violate the invariant).
 */
/* NOTE: with the demand attach fileserver extensions, the global lock
 * is dropped within VHold */
#ifdef AFS_DEMAND_ATTACH_FS
static int
VHold_r(Volume * vp)
{
    Error error;

    VCreateReservation_r(vp);
    VWaitExclusiveState_r(vp);

    LoadVolumeHeader(&error, vp);
    if (error) {
	VCancelReservation_r(vp);
	return error;
    }
    vp->nUsers++;
    VCancelReservation_r(vp);
    return 0;
}
#else /* AFS_DEMAND_ATTACH_FS */
static int
VHold_r(Volume * vp)
{
    Error error;

    LoadVolumeHeader(&error, vp);
    if (error)
	return error;
    vp->nUsers++;
    return 0;
}
#endif /* AFS_DEMAND_ATTACH_FS */

#if 0
static int
VHold(Volume * vp)
{
    int retVal;
    VOL_LOCK;
    retVal = VHold_r(vp);
    VOL_UNLOCK;
    return retVal;
}
#endif


/***************************************************/
/* get and put volume routines                     */
/***************************************************/

/**
 * put back a heavyweight reference to a volume object.
 *
 * @param[in] vp  volume object pointer
 *
 * @pre VOL_LOCK held
 *
 * @post heavyweight volume reference put back.
 *       depending on state, volume may have been taken offline,
 *       detached, salvaged, freed, etc.
 *
 * @internal volume package internal use only
 */
void
VPutVolume_r(Volume * vp)
{
    assert(--vp->nUsers >= 0);
    if (vp->nUsers == 0) {
	VCheckOffline(vp);
	ReleaseVolumeHeader(vp->header);
#ifdef AFS_DEMAND_ATTACH_FS
	if (!VCheckDetach(vp)) {
	    VCheckSalvage(vp);
	    VCheckFree(vp);
	}
#else /* AFS_DEMAND_ATTACH_FS */
	VCheckDetach(vp);
#endif /* AFS_DEMAND_ATTACH_FS */
    }
}

void
VPutVolume(Volume * vp)
{
    VOL_LOCK;
    VPutVolume_r(vp);
    VOL_UNLOCK;
}


/* Get a pointer to an attached volume.  The pointer is returned regardless
   of whether or not the volume is in service or on/off line.  An error
   code, however, is returned with an indication of the volume's status */
Volume *
VGetVolume(Error * ec, Error * client_ec, VolId volumeId)
{
    Volume *retVal;
    VOL_LOCK;
    retVal = GetVolume(ec, client_ec, volumeId, NULL, 0);
    VOL_UNLOCK;
    return retVal;
}

/* same as VGetVolume, but if a volume is waiting to go offline, we return
 * that it is actually offline, instead of waiting for it to go offline */
Volume *
VGetVolumeNoWait(Error * ec, Error * client_ec, VolId volumeId)
{
    Volume *retVal;
    VOL_LOCK;
    retVal = GetVolume(ec, client_ec, volumeId, NULL, 1);
    VOL_UNLOCK;
    return retVal;
}

Volume *
VGetVolume_r(Error * ec, VolId volumeId)
{
    return GetVolume(ec, NULL, volumeId, NULL, 0);
}

/* try to get a volume we've previously looked up */
/* for demand attach fs, caller MUST NOT hold a ref count on vp */
Volume *
VGetVolumeByVp_r(Error * ec, Volume * vp)
{
    return GetVolume(ec, NULL, vp->hashid, vp, 0);
}

/**
 * private interface for getting a volume handle
 *
 * @param[out] ec         error code (0 if no error)
 * @param[out] client_ec  wire error code to be given to clients
 * @param[in]  volumeId   ID of the volume we want
 * @param[in]  hint       optional hint for hash lookups, or NULL
 * @param[in]  nowait     0 to wait for a 'goingOffline' volume to go offline
 *                        before returning, 1 to return immediately
 *
 * @return a volume handle for the specified volume
 *  @retval NULL an error occurred, or the volume is in such a state that
 *               we cannot load a header or return any volume struct
 *
 * @note for DAFS, caller must NOT hold a ref count on 'hint'
 */
static Volume *
GetVolume(Error * ec, Error * client_ec, VolId volumeId, Volume * hint, int nowait)
{
    Volume *vp = hint;
    /* pull this profiling/debugging code out of regular builds */
#ifdef notdef
#define VGET_CTR_INC(x) x++
    unsigned short V0 = 0, V1 = 0, V2 = 0, V3 = 0, V5 = 0, V6 =
	0, V7 = 0, V8 = 0, V9 = 0;
    unsigned short V10 = 0, V11 = 0, V12 = 0, V13 = 0, V14 = 0, V15 = 0;
#else
#define VGET_CTR_INC(x)
#endif
#ifdef AFS_DEMAND_ATTACH_FS
    Volume *avp, * rvp = hint;
#endif

    /*
     * if VInit is zero, the volume package dynamic
     * data structures have not been initialized yet,
     * and we must immediately return an error
     */
    if (VInit == 0) {
	vp = NULL;
	*ec = VOFFLINE;
	if (client_ec) {
	    *client_ec = VOFFLINE;
	}
	goto not_inited;
    }

#ifdef AFS_DEMAND_ATTACH_FS
    if (rvp) {
	VCreateReservation_r(rvp);
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    for (;;) {
	*ec = 0;
	if (client_ec)
	    *client_ec = 0;
	VGET_CTR_INC(V0);

	vp = VLookupVolume_r(ec, volumeId, vp);
	if (*ec) {
	    vp = NULL;
	    break;
	}

#ifdef AFS_DEMAND_ATTACH_FS
	if (rvp && (rvp != vp)) {
	    /* break reservation on old vp */
	    VCancelReservation_r(rvp);
	    rvp = NULL;
	}
#endif /* AFS_DEMAND_ATTACH_FS */

	if (!vp) {
	    VGET_CTR_INC(V1);
	    if (VInit < 2) {
		VGET_CTR_INC(V2);
		/* Until we have reached an initialization level of 2
		 * we don't know whether this volume exists or not.
		 * We can't sleep and retry later because before a volume
		 * is attached, the caller tries to get it first.  Just
		 * return VOFFLINE and the caller can choose whether to
		 * retry the command or not. */
		*ec = VOFFLINE;
		break;
	    }

	    *ec = VNOVOL;
	    break;
	}

	VGET_CTR_INC(V3);
	IncUInt64(&VStats.hdr_gets);

#ifdef AFS_DEMAND_ATTACH_FS
	/* block if someone else is performing an exclusive op on this volume */
	if (rvp != vp) {
	    rvp = vp;
	    VCreateReservation_r(rvp);
	}
	VWaitExclusiveState_r(vp);

	/* short circuit with VNOVOL in the following circumstances:
	 *
	 *   - VOL_STATE_ERROR
	 *   - VOL_STATE_SHUTTING_DOWN
	 */
	if ((V_attachState(vp) == VOL_STATE_ERROR) ||
	    (V_attachState(vp) == VOL_STATE_SHUTTING_DOWN) ||
	    (V_attachState(vp) == VOL_STATE_GOING_OFFLINE)) {
	    *ec = VNOVOL;
	    vp = NULL;
	    break;
	}

	/*
	 * short circuit with VOFFLINE for VOL_STATE_UNATTACHED and
	 *                    VNOVOL   for VOL_STATE_DELETED
	 */
       if ((V_attachState(vp) == VOL_STATE_UNATTACHED) ||
           (V_attachState(vp) == VOL_STATE_DELETED)) {
	   if (vp->specialStatus) {
	       *ec = vp->specialStatus;
	   } else if (V_attachState(vp) == VOL_STATE_DELETED) {
	       *ec = VNOVOL;
	   } else {
	       *ec = VOFFLINE;
	   }
           vp = NULL;
           break;
       }

	/* allowable states:
	 *   - PREATTACHED
	 *   - ATTACHED
	 *   - SALVAGING
	 */

	if (vp->salvage.requested) {
	    VUpdateSalvagePriority_r(vp);
	}

	if (V_attachState(vp) == VOL_STATE_PREATTACHED) {
	    avp = VAttachVolumeByVp_r(ec, vp, 0);
	    if (avp) {
		if (vp != avp) {
		    /* VAttachVolumeByVp_r can return a pointer
		     * != the vp passed to it under certain
		     * conditions; make sure we don't leak
		     * reservations if that happens */
		    vp = avp;
		    VCancelReservation_r(rvp);
		    rvp = avp;
		    VCreateReservation_r(rvp);
		}
		VPutVolume_r(avp);
	    }
	    if (*ec) {
		int endloop = 0;
		switch (*ec) {
		case VSALVAGING:
		    break;
		case VOFFLINE:
		    if (!vp->pending_vol_op) {
			endloop = 1;
		    }
		    break;
		default:
		    *ec = VNOVOL;
		    endloop = 1;
		}
		if (endloop) {
		    vp = NULL;
		    break;
		}
	    }
	}

	if ((V_attachState(vp) == VOL_STATE_SALVAGING) ||
	    (*ec == VSALVAGING)) {
	    if (client_ec) {
		/* see CheckVnode() in afsfileprocs.c for an explanation
		 * of this error code logic */
		afs_uint32 now = FT_ApproxTime();
		if ((vp->stats.last_salvage + (10 * 60)) >= now) {
		    *client_ec = VBUSY;
		} else {
		    *client_ec = VRESTARTING;
		}
	    }
	    *ec = VSALVAGING;
	    vp = NULL;
	    break;
	}
#endif

#ifdef AFS_DEMAND_ATTACH_FS
	/*
	 * this test MUST happen after VAttachVolymeByVp, so vol_op_state is
	 * not VolOpRunningUnknown (attach2 would have converted it to Online
	 * or Offline)
	 */

         /* only valid before/during demand attachment */
         assert(!vp->pending_vol_op || vp->pending_vol_op->vol_op_state != FSSYNC_VolOpRunningUnknown);

         /* deny getvolume due to running mutually exclusive vol op */
         if (vp->pending_vol_op && vp->pending_vol_op->vol_op_state==FSSYNC_VolOpRunningOffline) {
	   /*
	    * volume cannot remain online during this volume operation.
	    * notify client.
	    */
	   if (vp->specialStatus) {
	       /*
		* special status codes outrank normal VOFFLINE code
		*/
	       *ec = vp->specialStatus;
	       if (client_ec) {
		   *client_ec = vp->specialStatus;
	       }
	   } else {
	       if (client_ec) {
		   /* see CheckVnode() in afsfileprocs.c for an explanation
		    * of this error code logic */
		   afs_uint32 now = FT_ApproxTime();
		   if ((vp->stats.last_vol_op + (10 * 60)) >= now) {
		       *client_ec = VBUSY;
		   } else {
		       *client_ec = VRESTARTING;
		   }
	       }
	       *ec = VOFFLINE;
	   }
	   VChangeState_r(vp, VOL_STATE_UNATTACHED);
	   FreeVolumeHeader(vp);
	   vp = NULL;
	   break;
	}
#endif /* AFS_DEMAND_ATTACH_FS */

	LoadVolumeHeader(ec, vp);
	if (*ec) {
	    VGET_CTR_INC(V6);
	    /* Only log the error if it was a totally unexpected error.  Simply
	     * a missing inode is likely to be caused by the volume being deleted */
	    if (errno != ENXIO || LogLevel)
		Log("Volume %u: couldn't reread volume header\n",
		    vp->hashid);
#ifdef AFS_DEMAND_ATTACH_FS
	    if (VCanScheduleSalvage()) {
		VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
	    } else {
		FreeVolume(vp);
		vp = NULL;
	    }
#else /* AFS_DEMAND_ATTACH_FS */
	    FreeVolume(vp);
	    vp = NULL;
#endif /* AFS_DEMAND_ATTACH_FS */
	    break;
	}

	VGET_CTR_INC(V7);
	if (vp->shuttingDown) {
	    VGET_CTR_INC(V8);
	    *ec = VNOVOL;
	    vp = NULL;
	    break;
	}

	if (programType == fileServer) {
	    VGET_CTR_INC(V9);
	    if (vp->goingOffline && !nowait) {
		VGET_CTR_INC(V10);
#ifdef AFS_DEMAND_ATTACH_FS
		/* wait for the volume to go offline */
		if (V_attachState(vp) == VOL_STATE_GOING_OFFLINE) {
		    VWaitStateChange_r(vp);
		}
#elif defined(AFS_PTHREAD_ENV)
		VOL_CV_WAIT(&vol_put_volume_cond);
#else /* AFS_PTHREAD_ENV */
		LWP_WaitProcess(VPutVolume);
#endif /* AFS_PTHREAD_ENV */
		continue;
	    }
	    if (vp->specialStatus) {
		VGET_CTR_INC(V11);
		*ec = vp->specialStatus;
	    } else if (V_inService(vp) == 0 || V_blessed(vp) == 0) {
		VGET_CTR_INC(V12);
		*ec = VNOVOL;
	    } else if (V_inUse(vp) == 0 || vp->goingOffline) {
		VGET_CTR_INC(V13);
		*ec = VOFFLINE;
	    } else {
		VGET_CTR_INC(V14);
	    }
	}
	break;
    }
    VGET_CTR_INC(V15);

#ifdef AFS_DEMAND_ATTACH_FS
    /* if no error, bump nUsers */
    if (vp) {
	vp->nUsers++;
	VLRU_UpdateAccess_r(vp);
    }
    if (rvp) {
	VCancelReservation_r(rvp);
	rvp = NULL;
    }
    if (client_ec && !*client_ec) {
	*client_ec = *ec;
    }
#else /* AFS_DEMAND_ATTACH_FS */
    /* if no error, bump nUsers */
    if (vp) {
	vp->nUsers++;
    }
    if (client_ec) {
	*client_ec = *ec;
    }
#endif /* AFS_DEMAND_ATTACH_FS */

 not_inited:
    assert(vp || *ec);
    return vp;
}


/***************************************************/
/* Volume offline/detach routines                  */
/***************************************************/

/* caller MUST hold a heavyweight ref on vp */
#ifdef AFS_DEMAND_ATTACH_FS
void
VTakeOffline_r(Volume * vp)
{
    Error error;

    assert(vp->nUsers > 0);
    assert(programType == fileServer);

    VCreateReservation_r(vp);
    VWaitExclusiveState_r(vp);

    vp->goingOffline = 1;
    V_needsSalvaged(vp) = 1;

    VRequestSalvage_r(&error, vp, SALVSYNC_ERROR, 0);
    VCancelReservation_r(vp);
}
#else /* AFS_DEMAND_ATTACH_FS */
void
VTakeOffline_r(Volume * vp)
{
    assert(vp->nUsers > 0);
    assert(programType == fileServer);

    vp->goingOffline = 1;
    V_needsSalvaged(vp) = 1;
}
#endif /* AFS_DEMAND_ATTACH_FS */

void
VTakeOffline(Volume * vp)
{
    VOL_LOCK;
    VTakeOffline_r(vp);
    VOL_UNLOCK;
}

/**
 * force a volume offline.
 *
 * @param[in] vp     volume object pointer
 * @param[in] flags  flags (see note below)
 *
 * @note the flag VOL_FORCEOFF_NOUPDATE is a recursion control flag
 *       used when VUpdateVolume_r needs to call VForceOffline_r
 *       (which in turn would normally call VUpdateVolume_r)
 *
 * @see VUpdateVolume_r
 *
 * @pre VOL_LOCK must be held.
 *      for DAFS, caller must hold ref.
 *
 * @note for DAFS, it _is safe_ to call this function from an
 *       exclusive state
 *
 * @post needsSalvaged flag is set.
 *       for DAFS, salvage is requested.
 *       no further references to the volume through the volume
 *       package will be honored.
 *       all file descriptor and vnode caches are invalidated.
 *
 * @warning this is a heavy-handed interface.  it results in
 *          a volume going offline regardless of the current
 *          reference count state.
 *
 * @internal  volume package internal use only
 */
void
VForceOffline_r(Volume * vp, int flags)
{
    Error error;
    if (!V_inUse(vp)) {
#ifdef AFS_DEMAND_ATTACH_FS
	VChangeState_r(vp, VOL_STATE_ERROR);
#endif
	return;
    }

    strcpy(V_offlineMessage(vp),
	   "Forced offline due to internal error: volume needs to be salvaged");
    Log("Volume %u forced offline:  it needs salvaging!\n", V_id(vp));

    V_inUse(vp) = 0;
    vp->goingOffline = 0;
    V_needsSalvaged(vp) = 1;
    if (!(flags & VOL_FORCEOFF_NOUPDATE)) {
	VUpdateVolume_r(&error, vp, VOL_UPDATE_NOFORCEOFF);
    }

#ifdef AFS_DEMAND_ATTACH_FS
    VRequestSalvage_r(&error, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
#endif /* AFS_DEMAND_ATTACH_FS */

#ifdef AFS_PTHREAD_ENV
    assert(pthread_cond_broadcast(&vol_put_volume_cond) == 0);
#else /* AFS_PTHREAD_ENV */
    LWP_NoYieldSignal(VPutVolume);
#endif /* AFS_PTHREAD_ENV */

    VReleaseVolumeHandles_r(vp);
}

/**
 * force a volume offline.
 *
 * @param[in] vp  volume object pointer
 *
 * @see VForceOffline_r
 */
void
VForceOffline(Volume * vp)
{
    VOL_LOCK;
    VForceOffline_r(vp, 0);
    VOL_UNLOCK;
}

/* The opposite of VAttachVolume.  The volume header is written to disk, with
   the inUse bit turned off.  A copy of the header is maintained in memory,
   however (which is why this is VOffline, not VDetach).
 */
void
VOffline_r(Volume * vp, char *message)
{
#ifndef AFS_DEMAND_ATTACH_FS
    Error error;
    VolumeId vid = V_id(vp);
#endif

    assert(programType != volumeUtility && programType != volumeServer);
    if (!V_inUse(vp)) {
	VPutVolume_r(vp);
	return;
    }
    if (V_offlineMessage(vp)[0] == '\0')
	strncpy(V_offlineMessage(vp), message, sizeof(V_offlineMessage(vp)));
    V_offlineMessage(vp)[sizeof(V_offlineMessage(vp)) - 1] = '\0';

    vp->goingOffline = 1;
#ifdef AFS_DEMAND_ATTACH_FS
    VChangeState_r(vp, VOL_STATE_GOING_OFFLINE);
    VCreateReservation_r(vp);
    VPutVolume_r(vp);

    /* wait for the volume to go offline */
    if (V_attachState(vp) == VOL_STATE_GOING_OFFLINE) {
	VWaitStateChange_r(vp);
    }
    VCancelReservation_r(vp);
#else /* AFS_DEMAND_ATTACH_FS */
    VPutVolume_r(vp);
    vp = VGetVolume_r(&error, vid);	/* Wait for it to go offline */
    if (vp)			/* In case it was reattached... */
	VPutVolume_r(vp);
#endif /* AFS_DEMAND_ATTACH_FS */
}

#ifdef AFS_DEMAND_ATTACH_FS
/**
 * Take a volume offline in order to perform a volume operation.
 *
 * @param[inout] ec       address in which to store error code
 * @param[in]    vp       volume object pointer
 * @param[in]    message  volume offline status message
 *
 * @pre
 *    - VOL_LOCK is held
 *    - caller MUST hold a heavyweight ref on vp
 *
 * @post
 *    - volume is taken offline
 *    - if possible, volume operation is promoted to running state
 *    - on failure, *ec is set to nonzero
 *
 * @note Although this function does not return any value, it may
 *       still fail to promote our pending volume operation to
 *       a running state.  Any caller MUST check the value of *ec,
 *       and MUST NOT blindly assume success.
 *
 * @warning if the caller does not hold a lightweight ref on vp,
 *          then it MUST NOT reference vp after this function
 *          returns to the caller.
 *
 * @internal volume package internal use only
 */
void
VOfflineForVolOp_r(Error *ec, Volume *vp, char *message)
{
    assert(vp->pending_vol_op);
    if (!V_inUse(vp)) {
	VPutVolume_r(vp);
        *ec = 1;
	return;
    }
    if (V_offlineMessage(vp)[0] == '\0')
	strncpy(V_offlineMessage(vp), message, sizeof(V_offlineMessage(vp)));
    V_offlineMessage(vp)[sizeof(V_offlineMessage(vp)) - 1] = '\0';

    vp->goingOffline = 1;
    VChangeState_r(vp, VOL_STATE_GOING_OFFLINE);
    VCreateReservation_r(vp);
    VPutVolume_r(vp);

    /* Wait for the volume to go offline */
    while (!VIsOfflineState(V_attachState(vp))) {
        /* do not give corrupted volumes to the volserver */
        if (vp->salvage.requested && vp->pending_vol_op->com.programType != salvageServer) {
           *ec = 1;
	   goto error;
        }
	VWaitStateChange_r(vp);
    }
    *ec = 0;
 error:
    VCancelReservation_r(vp);
}
#endif /* AFS_DEMAND_ATTACH_FS */

void
VOffline(Volume * vp, char *message)
{
    VOL_LOCK;
    VOffline_r(vp, message);
    VOL_UNLOCK;
}

/* This gets used for the most part by utility routines that don't want
 * to keep all the volume headers around.  Generally, the file server won't
 * call this routine, because then the offline message in the volume header
 * (or other information) won't be available to clients. For NAMEI, also
 * close the file handles.  However, the fileserver does call this during
 * an attach following a volume operation.
 */
void
VDetachVolume_r(Error * ec, Volume * vp)
{
    VolumeId volume;
    struct DiskPartition64 *tpartp;
    int notifyServer = 0;
    int  useDone = FSYNC_VOL_ON;

    *ec = 0;			/* always "succeeds" */
    if (VCanUseFSSYNC()) {
	notifyServer = vp->needsPutBack;
	if (V_destroyMe(vp) == DESTROY_ME)
	    useDone = FSYNC_VOL_DONE;
#ifdef AFS_DEMAND_ATTACH_FS
	else if (!V_blessed(vp) || !V_inService(vp))
	    useDone = FSYNC_VOL_LEAVE_OFF;
#endif
    }
    tpartp = vp->partition;
    volume = V_id(vp);
    DeleteVolumeFromHashTable(vp);
    vp->shuttingDown = 1;
#ifdef AFS_DEMAND_ATTACH_FS
    DeleteVolumeFromVByPList_r(vp);
    VLRU_Delete_r(vp);
    VChangeState_r(vp, VOL_STATE_SHUTTING_DOWN);
#else
    if (programType != fileServer)
	V_inUse(vp) = 0;
#endif /* AFS_DEMAND_ATTACH_FS */
    VPutVolume_r(vp);
    /* Will be detached sometime in the future--this is OK since volume is offline */

    /* XXX the following code should really be moved to VCheckDetach() since the volume
     * is not technically detached until the refcounts reach zero
     */
#ifdef FSSYNC_BUILD_CLIENT
    if (VCanUseFSSYNC() && notifyServer) {
	/*
	 * Note:  The server is not notified in the case of a bogus volume
	 * explicitly to make it possible to create a volume, do a partial
	 * restore, then abort the operation without ever putting the volume
	 * online.  This is essential in the case of a volume move operation
	 * between two partitions on the same server.  In that case, there
	 * would be two instances of the same volume, one of them bogus,
	 * which the file server would attempt to put on line
	 */
	FSYNC_VolOp(volume, tpartp->name, useDone, 0, NULL);
	/* XXX this code path is only hit by volume utilities, thus
	 * V_BreakVolumeCallbacks will always be NULL.  if we really
	 * want to break callbacks in this path we need to use FSYNC_VolOp() */
#ifdef notdef
	/* Dettaching it so break all callbacks on it */
	if (V_BreakVolumeCallbacks) {
	    Log("volume %u detached; breaking all call backs\n", volume);
	    (*V_BreakVolumeCallbacks) (volume);
	}
#endif
    }
#endif /* FSSYNC_BUILD_CLIENT */
}

void
VDetachVolume(Error * ec, Volume * vp)
{
    VOL_LOCK;
    VDetachVolume_r(ec, vp);
    VOL_UNLOCK;
}


/***************************************************/
/* Volume fd/inode handle closing routines         */
/***************************************************/

/* For VDetachVolume, we close all cached file descriptors, but keep
 * the Inode handles in case we need to read from a busy volume.
 */
/* for demand attach, caller MUST hold ref count on vp */
static void
VCloseVolumeHandles_r(Volume * vp)
{
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;

    state_save = VChangeState_r(vp, VOL_STATE_OFFLINING);
#endif

    /* demand attach fs
     *
     * XXX need to investigate whether we can perform
     * DFlushVolume outside of vol_glock_mutex...
     *
     * VCloseVnodeFiles_r drops the glock internally */
    DFlushVolume(vp->hashid);
    VCloseVnodeFiles_r(vp);

#ifdef AFS_DEMAND_ATTACH_FS
    VOL_UNLOCK;
#endif

    /* Too time consuming and unnecessary for the volserver */
    if (programType == fileServer) {
	IH_CONDSYNC(vp->vnodeIndex[vLarge].handle);
	IH_CONDSYNC(vp->vnodeIndex[vSmall].handle);
	IH_CONDSYNC(vp->diskDataHandle);
	IH_CONDSYNC(vp->fileACLHandle);
#ifdef AFS_NT40_ENV
	IH_CONDSYNC(vp->linkHandle);
#endif /* AFS_NT40_ENV */
    }

    IH_REALLYCLOSE(vp->vnodeIndex[vLarge].handle);
    IH_REALLYCLOSE(vp->vnodeIndex[vSmall].handle);
    IH_REALLYCLOSE(vp->diskDataHandle);
    IH_REALLYCLOSE(vp->linkHandle);
    IH_REALLYCLOSE(vp->fileACLHandle);

#ifdef AFS_DEMAND_ATTACH_FS
    if ((V_attachFlags(vp) & VOL_LOCKED)) {
	VUnlockVolume(vp);
    }

    VOL_LOCK;
    VChangeState_r(vp, state_save);
#endif
}

/* For both VForceOffline and VOffline, we close all relevant handles.
 * For VOffline, if we re-attach the volume, the files may possible be
 * different than before.
 */
/* for demand attach, caller MUST hold a ref count on vp */
static void
VReleaseVolumeHandles_r(Volume * vp)
{
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;

    state_save = VChangeState_r(vp, VOL_STATE_DETACHING);
#endif

    /* XXX need to investigate whether we can perform
     * DFlushVolume outside of vol_glock_mutex... */
    DFlushVolume(vp->hashid);

    VReleaseVnodeFiles_r(vp); /* releases the glock internally */

#ifdef AFS_DEMAND_ATTACH_FS
    VOL_UNLOCK;
#endif

    /* Too time consuming and unnecessary for the volserver */
    if (programType == fileServer) {
	IH_CONDSYNC(vp->vnodeIndex[vLarge].handle);
	IH_CONDSYNC(vp->vnodeIndex[vSmall].handle);
	IH_CONDSYNC(vp->diskDataHandle);
	IH_CONDSYNC(vp->fileACLHandle);
#ifdef AFS_NT40_ENV
	IH_CONDSYNC(vp->linkHandle);
#endif /* AFS_NT40_ENV */
    }

    IH_RELEASE(vp->vnodeIndex[vLarge].handle);
    IH_RELEASE(vp->vnodeIndex[vSmall].handle);
    IH_RELEASE(vp->diskDataHandle);
    IH_RELEASE(vp->linkHandle);
    IH_RELEASE(vp->fileACLHandle);

#ifdef AFS_DEMAND_ATTACH_FS
    if ((V_attachFlags(vp) & VOL_LOCKED)) {
	VUnlockVolume(vp);
    }

    VOL_LOCK;
    VChangeState_r(vp, state_save);
#endif
}


/***************************************************/
/* Volume write and fsync routines                 */
/***************************************************/

void
VUpdateVolume_r(Error * ec, Volume * vp, int flags)
{
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;

    if (flags & VOL_UPDATE_WAIT) {
	VCreateReservation_r(vp);
	VWaitExclusiveState_r(vp);
    }
#endif

    *ec = 0;
    if (programType == fileServer)
	V_uniquifier(vp) =
	    (V_inUse(vp) ? V_nextVnodeUnique(vp) +
	     200 : V_nextVnodeUnique(vp));

#ifdef AFS_DEMAND_ATTACH_FS
    state_save = VChangeState_r(vp, VOL_STATE_UPDATING);
    VOL_UNLOCK;
#endif

    WriteVolumeHeader_r(ec, vp);

#ifdef AFS_DEMAND_ATTACH_FS
    VOL_LOCK;
    VChangeState_r(vp, state_save);
    if (flags & VOL_UPDATE_WAIT) {
	VCancelReservation_r(vp);
    }
#endif

    if (*ec) {
	Log("VUpdateVolume: error updating volume header, volume %u (%s)\n",
	    V_id(vp), V_name(vp));
	/* try to update on-disk header,
	 * while preventing infinite recursion */
	if (!(flags & VOL_UPDATE_NOFORCEOFF)) {
	    VForceOffline_r(vp, VOL_FORCEOFF_NOUPDATE);
	}
    }
}

void
VUpdateVolume(Error * ec, Volume * vp)
{
    VOL_LOCK;
    VUpdateVolume_r(ec, vp, VOL_UPDATE_WAIT);
    VOL_UNLOCK;
}

void
VSyncVolume_r(Error * ec, Volume * vp, int flags)
{
    FdHandle_t *fdP;
    int code;
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;
#endif

    if (flags & VOL_SYNC_WAIT) {
	VUpdateVolume_r(ec, vp, VOL_UPDATE_WAIT);
    } else {
	VUpdateVolume_r(ec, vp, 0);
    }
    if (!*ec) {
#ifdef AFS_DEMAND_ATTACH_FS
	state_save = VChangeState_r(vp, VOL_STATE_UPDATING);
	VOL_UNLOCK;
#endif
	fdP = IH_OPEN(V_diskDataHandle(vp));
	assert(fdP != NULL);
	code = FDH_SYNC(fdP);
	assert(code == 0);
	FDH_CLOSE(fdP);
#ifdef AFS_DEMAND_ATTACH_FS
	VOL_LOCK;
	VChangeState_r(vp, state_save);
#endif
    }
}

void
VSyncVolume(Error * ec, Volume * vp)
{
    VOL_LOCK;
    VSyncVolume_r(ec, vp, VOL_SYNC_WAIT);
    VOL_UNLOCK;
}


/***************************************************/
/* Volume dealloaction routines                    */
/***************************************************/

#ifdef AFS_DEMAND_ATTACH_FS
static void
FreeVolume(Volume * vp)
{
    /* free the heap space, iff it's safe.
     * otherwise, pull it out of the hash table, so it
     * will get deallocated when all refs to it go away */
    if (!VCheckFree(vp)) {
	DeleteVolumeFromHashTable(vp);
	DeleteVolumeFromVByPList_r(vp);

	/* make sure we invalidate the header cache entry */
	FreeVolumeHeader(vp);
    }
}
#endif /* AFS_DEMAND_ATTACH_FS */

static void
ReallyFreeVolume(Volume * vp)
{
    int i;
    if (!vp)
	return;
#ifdef AFS_DEMAND_ATTACH_FS
    /* debug */
    VChangeState_r(vp, VOL_STATE_FREED);
    if (vp->pending_vol_op)
	free(vp->pending_vol_op);
#endif /* AFS_DEMAND_ATTACH_FS */
    for (i = 0; i < nVNODECLASSES; i++)
	if (vp->vnodeIndex[i].bitmap)
	    free(vp->vnodeIndex[i].bitmap);
    FreeVolumeHeader(vp);
#ifndef AFS_DEMAND_ATTACH_FS
    DeleteVolumeFromHashTable(vp);
#endif /* AFS_DEMAND_ATTACH_FS */
    free(vp);
}

/* check to see if we should shutdown this volume
 * returns 1 if volume was freed, 0 otherwise */
#ifdef AFS_DEMAND_ATTACH_FS
static int
VCheckDetach(Volume * vp)
{
    int ret = 0;
    Error ec = 0;

    if (vp->nUsers || vp->nWaiters)
	return ret;

    if (vp->shuttingDown) {
	ret = 1;
	if ((programType != fileServer) &&
	    (V_inUse(vp) == programType) &&
	    ((V_checkoutMode(vp) == V_VOLUPD) ||
	     (V_checkoutMode(vp) == V_SECRETLY) ||
	     ((V_checkoutMode(vp) == V_CLONE) &&
	      (VolumeWriteable(vp))))) {
	    V_inUse(vp) = 0;
	    VUpdateVolume_r(&ec, vp, VOL_UPDATE_NOFORCEOFF);
	    if (ec) {
		Log("VCheckDetach: volume header update for volume %u "
		    "failed with errno %d\n", vp->hashid, errno);
	    }
	}
	VReleaseVolumeHandles_r(vp);
	VCheckSalvage(vp);
	ReallyFreeVolume(vp);
	if (programType == fileServer) {
	    assert(pthread_cond_broadcast(&vol_put_volume_cond) == 0);
	}
    }
    return ret;
}
#else /* AFS_DEMAND_ATTACH_FS */
static int
VCheckDetach(Volume * vp)
{
    int ret = 0;
    Error ec = 0;

    if (vp->nUsers)
	return ret;

    if (vp->shuttingDown) {
	ret = 1;
	if ((programType != fileServer) &&
	    (V_inUse(vp) == programType) &&
	    ((V_checkoutMode(vp) == V_VOLUPD) ||
	     (V_checkoutMode(vp) == V_SECRETLY) ||
	     ((V_checkoutMode(vp) == V_CLONE) &&
	      (VolumeWriteable(vp))))) {
	    V_inUse(vp) = 0;
	    VUpdateVolume_r(&ec, vp, VOL_UPDATE_NOFORCEOFF);
	    if (ec) {
		Log("VCheckDetach: volume header update for volume %u failed with errno %d\n",
		    vp->hashid, errno);
	    }
	}
	VReleaseVolumeHandles_r(vp);
	ReallyFreeVolume(vp);
	if (programType == fileServer) {
#if defined(AFS_PTHREAD_ENV)
	    assert(pthread_cond_broadcast(&vol_put_volume_cond) == 0);
#else /* AFS_PTHREAD_ENV */
	    LWP_NoYieldSignal(VPutVolume);
#endif /* AFS_PTHREAD_ENV */
	}
    }
    return ret;
}
#endif /* AFS_DEMAND_ATTACH_FS */

/* check to see if we should offline this volume
 * return 1 if volume went offline, 0 otherwise */
#ifdef AFS_DEMAND_ATTACH_FS
static int
VCheckOffline(Volume * vp)
{
    int ret = 0;

    if (vp->goingOffline && !vp->nUsers) {
	Error error;
	assert(programType == fileServer);
	assert((V_attachState(vp) != VOL_STATE_ATTACHED) &&
	       (V_attachState(vp) != VOL_STATE_FREED) &&
	       (V_attachState(vp) != VOL_STATE_PREATTACHED) &&
	       (V_attachState(vp) != VOL_STATE_UNATTACHED) &&
	       (V_attachState(vp) != VOL_STATE_DELETED));

	/* valid states:
	 *
	 * VOL_STATE_GOING_OFFLINE
	 * VOL_STATE_SHUTTING_DOWN
	 * VIsErrorState(V_attachState(vp))
	 * VIsExclusiveState(V_attachState(vp))
	 */

	VCreateReservation_r(vp);
	VChangeState_r(vp, VOL_STATE_OFFLINING);

	ret = 1;
	/* must clear the goingOffline flag before we drop the glock */
	vp->goingOffline = 0;
	V_inUse(vp) = 0;

	VLRU_Delete_r(vp);

	/* perform async operations */
	VUpdateVolume_r(&error, vp, 0);
	VCloseVolumeHandles_r(vp);

	if (LogLevel) {
	    if (V_offlineMessage(vp)[0]) {
		Log("VOffline: Volume %lu (%s) is now offline (%s)\n",
		    afs_printable_uint32_lu(V_id(vp)), V_name(vp),
		    V_offlineMessage(vp));
	    } else {
		Log("VOffline: Volume %lu (%s) is now offline\n",
		    afs_printable_uint32_lu(V_id(vp)), V_name(vp));
	    }
	}

	/* invalidate the volume header cache entry */
	FreeVolumeHeader(vp);

	/* if nothing changed state to error or salvaging,
	 * drop state to unattached */
	if (!VIsErrorState(V_attachState(vp))) {
	    VChangeState_r(vp, VOL_STATE_UNATTACHED);
	}
	VCancelReservation_r(vp);
	/* no usage of vp is safe beyond this point */
    }
    return ret;
}
#else /* AFS_DEMAND_ATTACH_FS */
static int
VCheckOffline(Volume * vp)
{
    int ret = 0;

    if (vp->goingOffline && !vp->nUsers) {
	Error error;
	assert(programType == fileServer);

	ret = 1;
	vp->goingOffline = 0;
	V_inUse(vp) = 0;
	VUpdateVolume_r(&error, vp, 0);
	VCloseVolumeHandles_r(vp);
	if (LogLevel) {
	    Log("VOffline: Volume %u (%s) is now offline", V_id(vp),
		V_name(vp));
	    if (V_offlineMessage(vp)[0])
		Log(" (%s)", V_offlineMessage(vp));
	    Log("\n");
	}
	FreeVolumeHeader(vp);
#ifdef AFS_PTHREAD_ENV
	assert(pthread_cond_broadcast(&vol_put_volume_cond) == 0);
#else /* AFS_PTHREAD_ENV */
	LWP_NoYieldSignal(VPutVolume);
#endif /* AFS_PTHREAD_ENV */
    }
    return ret;
}
#endif /* AFS_DEMAND_ATTACH_FS */

/***************************************************/
/* demand attach fs ref counting routines          */
/***************************************************/

#ifdef AFS_DEMAND_ATTACH_FS
/* the following two functions handle reference counting for
 * asynchronous operations on volume structs.
 *
 * their purpose is to prevent a VDetachVolume or VShutdown
 * from free()ing the Volume struct during an async i/o op */

/* register with the async volume op ref counter */
/* VCreateReservation_r moved into inline code header because it
 * is now needed in vnode.c -- tkeiser 11/20/2007
 */

/**
 * decrement volume-package internal refcount.
 *
 * @param vp  volume object pointer
 *
 * @internal volume package internal use only
 *
 * @pre
 *    @arg VOL_LOCK is held
 *    @arg lightweight refcount held
 *
 * @post volume waiters refcount is decremented; volume may
 *       have been deallocated/shutdown/offlined/salvaged/
 *       whatever during the process
 *
 * @warning once you have tossed your last reference (you can acquire
 *          lightweight refs recursively) it is NOT SAFE to reference
 *          a volume object pointer ever again
 *
 * @see VCreateReservation_r
 *
 * @note DEMAND_ATTACH_FS only
 */
void
VCancelReservation_r(Volume * vp)
{
    assert(--vp->nWaiters >= 0);
    if (vp->nWaiters == 0) {
	VCheckOffline(vp);
	if (!VCheckDetach(vp)) {
	    VCheckSalvage(vp);
	    VCheckFree(vp);
	}
    }
}

/* check to see if we should free this volume now
 * return 1 if volume was freed, 0 otherwise */
static int
VCheckFree(Volume * vp)
{
    int ret = 0;
    if ((vp->nUsers == 0) &&
	(vp->nWaiters == 0) &&
	!(V_attachFlags(vp) & (VOL_IN_HASH |
			       VOL_ON_VBYP_LIST |
			       VOL_IS_BUSY |
			       VOL_ON_VLRU))) {
	ReallyFreeVolume(vp);
	ret = 1;
    }
    return ret;
}
#endif /* AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* online volume operations routines               */
/***************************************************/

#ifdef AFS_DEMAND_ATTACH_FS
/**
 * register a volume operation on a given volume.
 *
 * @param[in] vp       volume object
 * @param[in] vopinfo  volume operation info object
 *
 * @pre VOL_LOCK is held
 *
 * @post volume operation info object attached to volume object.
 *       volume operation statistics updated.
 *
 * @note by "attached" we mean a copy of the passed in object is made
 *
 * @internal volume package internal use only
 */
int
VRegisterVolOp_r(Volume * vp, FSSYNC_VolOp_info * vopinfo)
{
    FSSYNC_VolOp_info * info;

    /* attach a vol op info node to the volume struct */
    info = (FSSYNC_VolOp_info *) malloc(sizeof(FSSYNC_VolOp_info));
    assert(info != NULL);
    memcpy(info, vopinfo, sizeof(FSSYNC_VolOp_info));
    vp->pending_vol_op = info;

    /* update stats */
    vp->stats.last_vol_op = FT_ApproxTime();
    vp->stats.vol_ops++;
    IncUInt64(&VStats.vol_ops);

    return 0;
}

/**
 * deregister the volume operation attached to this volume.
 *
 * @param[in] vp  volume object pointer
 *
 * @pre VOL_LOCK is held
 *
 * @post the volume operation info object is detached from the volume object
 *
 * @internal volume package internal use only
 */
int
VDeregisterVolOp_r(Volume * vp)
{
    if (vp->pending_vol_op) {
	free(vp->pending_vol_op);
	vp->pending_vol_op = NULL;
    }
    return 0;
}
#endif /* AFS_DEMAND_ATTACH_FS */

/**
 * determine whether it is safe to leave a volume online during
 * the volume operation described by the vopinfo object.
 *
 * @param[in] vp        volume object
 * @param[in] vopinfo   volume operation info object
 *
 * @return whether it is safe to leave volume online
 *    @retval 0  it is NOT SAFE to leave the volume online
 *    @retval 1  it is safe to leave the volume online during the operation
 *
 * @pre
 *    @arg VOL_LOCK is held
 *    @arg disk header attached to vp (heavyweight ref on vp will guarantee
 *         this condition is met)
 *
 * @internal volume package internal use only
 */
int
VVolOpLeaveOnline_r(Volume * vp, FSSYNC_VolOp_info * vopinfo)
{
    return (vopinfo->vol_op_state == FSSYNC_VolOpRunningOnline ||
	    (vopinfo->com.command == FSYNC_VOL_NEEDVOLUME &&
	    (vopinfo->com.reason == V_READONLY ||
	     (!VolumeWriteable(vp) &&
	      (vopinfo->com.reason == V_CLONE ||
	       vopinfo->com.reason == V_DUMP)))));
}

/**
 * same as VVolOpLeaveOnline_r, but does not require a volume with an attached
 * header.
 *
 * @param[in] vp        volume object
 * @param[in] vopinfo   volume operation info object
 *
 * @return whether it is safe to leave volume online
 *    @retval 0  it is NOT SAFE to leave the volume online
 *    @retval 1  it is safe to leave the volume online during the operation
 *    @retval -1 unsure; volume header is required in order to know whether or
 *               not is is safe to leave the volume online
 *
 * @pre VOL_LOCK is held
 *
 * @internal volume package internal use only
 */
int
VVolOpLeaveOnlineNoHeader_r(Volume * vp, FSSYNC_VolOp_info * vopinfo)
{
    /* follow the logic in VVolOpLeaveOnline_r; this is the same, except
     * assume that we don't know VolumeWriteable; return -1 if the answer
     * depends on VolumeWriteable */

    if (vopinfo->vol_op_state == FSSYNC_VolOpRunningOnline) {
	return 1;
    }
    if (vopinfo->com.command == FSYNC_VOL_NEEDVOLUME &&
        vopinfo->com.reason == V_READONLY) {

	return 1;
    }
    if (vopinfo->com.command == FSYNC_VOL_NEEDVOLUME &&
        (vopinfo->com.reason == V_CLONE ||
         vopinfo->com.reason == V_DUMP)) {

	/* must know VolumeWriteable */
	return -1;
    }
    return 0;
}

/**
 * determine whether VBUSY should be set during this volume operation.
 *
 * @param[in] vp        volume object
 * @param[in] vopinfo   volume operation info object
 *
 * @return whether VBUSY should be set
 *   @retval 0  VBUSY does NOT need to be set
 *   @retval 1  VBUSY SHOULD be set
 *
 * @pre VOL_LOCK is held
 *
 * @internal volume package internal use only
 */
int
VVolOpSetVBusy_r(Volume * vp, FSSYNC_VolOp_info * vopinfo)
{
    return ((vopinfo->com.command == FSYNC_VOL_OFF &&
	    vopinfo->com.reason == FSYNC_SALVAGE) ||
	    (vopinfo->com.command == FSYNC_VOL_NEEDVOLUME &&
	    (vopinfo->com.reason == V_CLONE ||
	     vopinfo->com.reason == V_DUMP)));
}


/***************************************************/
/* online salvager routines                        */
/***************************************************/
#if defined(AFS_DEMAND_ATTACH_FS)
/**
 * check whether a salvage needs to be performed on this volume.
 *
 * @param[in] vp   pointer to volume object
 *
 * @return status code
 *    @retval 0 no salvage scheduled
 *    @retval 1 a salvage has been scheduled with the salvageserver
 *
 * @pre VOL_LOCK is held
 *
 * @post if salvage request flag is set and nUsers and nWaiters are zero,
 *       then a salvage will be requested
 *
 * @note this is one of the event handlers called by VCancelReservation_r
 *
 * @see VCancelReservation_r
 *
 * @internal volume package internal use only.
 */
static int
VCheckSalvage(Volume * vp)
{
    int ret = 0;
#if defined(SALVSYNC_BUILD_CLIENT) || defined(FSSYNC_BUILD_CLIENT)
    if (vp->nUsers || vp->nWaiters)
	return ret;
    if (vp->salvage.requested) {
	VScheduleSalvage_r(vp);
	ret = 1;
    }
#endif /* SALVSYNC_BUILD_CLIENT || FSSYNC_BUILD_CLIENT */
    return ret;
}

/**
 * request volume salvage.
 *
 * @param[out] ec      computed client error code
 * @param[in]  vp      volume object pointer
 * @param[in]  reason  reason code (passed to salvageserver via SALVSYNC)
 * @param[in]  flags   see flags note below
 *
 * @note flags:
 *       VOL_SALVAGE_INVALIDATE_HEADER causes volume header cache entry
 *                                     to be invalidated.
 *
 * @pre VOL_LOCK is held.
 *
 * @post volume state is changed.
 *       for fileserver, salvage will be requested once refcount reaches zero.
 *
 * @return operation status code
 *   @retval 0  volume salvage will occur
 *   @retval 1  volume salvage could not be scheduled
 *
 * @note DAFS only
 *
 * @note in the fileserver, this call does not synchronously schedule a volume
 *       salvage. rather, it sets volume state so that when volume refcounts
 *       reach zero, a volume salvage will occur. by "refcounts", we mean both
 *       nUsers and nWaiters must be zero.
 *
 * @internal volume package internal use only.
 */
int
VRequestSalvage_r(Error * ec, Volume * vp, int reason, int flags)
{
    int code = 0;
    /*
     * for DAFS volume utilities that are not supposed to schedule salvages,
     * just transition to error state instead
     */
    if (!VCanScheduleSalvage()) {
	VChangeState_r(vp, VOL_STATE_ERROR);
	*ec = VSALVAGE;
	return 1;
    }

    if (programType != fileServer && !VCanUseFSSYNC()) {
        VChangeState_r(vp, VOL_STATE_ERROR);
        *ec = VSALVAGE;
        return 1;
    }

    if (!vp->salvage.requested) {
	vp->salvage.requested = 1;
	vp->salvage.reason = reason;
	vp->stats.last_salvage = FT_ApproxTime();

	/* Note that it is not possible for us to reach this point if a
	 * salvage is already running on this volume (even if the fileserver
	 * was restarted during the salvage). If a salvage were running, the
	 * salvager would have write-locked the volume header file, so when
	 * we tried to lock the volume header, the lock would have failed,
	 * and we would have failed during attachment prior to calling
	 * VRequestSalvage. So we know that we can schedule salvages without
	 * fear of a salvage already running for this volume. */

	if (vp->stats.salvages < SALVAGE_COUNT_MAX) {
	    VChangeState_r(vp, VOL_STATE_SALVAGING);
	    *ec = VSALVAGING;
	} else {
	    Log("VRequestSalvage: volume %u online salvaged too many times; forced offline.\n", vp->hashid);

	    /* make sure neither VScheduleSalvage_r nor
	     * VUpdateSalvagePriority_r try to schedule another salvage */
	    vp->salvage.requested = vp->salvage.scheduled = 0;

	    VChangeState_r(vp, VOL_STATE_ERROR);
	    *ec = VSALVAGE;
	    code = 1;
	}
	if (flags & VOL_SALVAGE_INVALIDATE_HEADER) {
	    /* Instead of ReleaseVolumeHeader, we do FreeVolumeHeader()
               so that the the next VAttachVolumeByVp_r() invocation
               of attach2() will pull in a cached header
               entry and fail, then load a fresh one from disk and attach
               it to the volume.
	    */
	    FreeVolumeHeader(vp);
	}
    }
    return code;
}

/**
 * update salvageserver scheduling priority for a volume.
 *
 * @param[in] vp  pointer to volume object
 *
 * @return operation status
 *   @retval 0  success
 *   @retval 1  request denied, or SALVSYNC communications failure
 *
 * @pre VOL_LOCK is held.
 *
 * @post in-core salvage priority counter is incremented.  if at least
 *       SALVAGE_PRIO_UPDATE_INTERVAL seconds have elapsed since the
 *       last SALVSYNC_RAISEPRIO request, we contact the salvageserver
 *       to update its priority queue.  if no salvage is scheduled,
 *       this function is a no-op.
 *
 * @note DAFS fileserver only
 *
 * @note this should be called whenever a VGetVolume fails due to a
 *       pending salvage request
 *
 * @todo should set exclusive state and drop glock around salvsync call
 *
 * @internal volume package internal use only.
 */
int
VUpdateSalvagePriority_r(Volume * vp)
{
    int ret=0;

#ifdef SALVSYNC_BUILD_CLIENT
    afs_uint32 now;
    int code;

    vp->salvage.prio++;
    now = FT_ApproxTime();

    /* update the salvageserver priority queue occasionally so that
     * frequently requested volumes get moved to the head of the queue
     */
    if ((vp->salvage.scheduled) &&
	(vp->stats.last_salvage_req < (now-SALVAGE_PRIO_UPDATE_INTERVAL))) {
	code = SALVSYNC_SalvageVolume(vp->hashid,
				      VPartitionPath(vp->partition),
				      SALVSYNC_RAISEPRIO,
				      vp->salvage.reason,
				      vp->salvage.prio,
				      NULL);
	vp->stats.last_salvage_req = now;
	if (code != SYNC_OK) {
	    ret = 1;
	}
    }
#endif /* SALVSYNC_BUILD_CLIENT */
    return ret;
}


#if defined(SALVSYNC_BUILD_CLIENT) || defined(FSSYNC_BUILD_CLIENT)

/* A couple of little helper functions. These return true if we tried to
 * use this mechanism to schedule a salvage, false if we haven't tried.
 * If we did try a salvage then the results are contained in code.
 */

static_inline int
try_SALVSYNC(Volume *vp, char *partName, int *code) {
#ifdef SALVSYNC_BUILD_CLIENT
    if (VCanUseSALVSYNC()) {
	Log("Scheduling salvage for volume %lu on part %s over SALVSYNC\n",
	    afs_printable_uint32_lu(vp->hashid), partName);

	/* can't use V_id() since there's no guarantee
	 * we have the disk data header at this point */
	*code = SALVSYNC_SalvageVolume(vp->hashid,
	                               partName,
	                               SALVSYNC_SALVAGE,
	                               vp->salvage.reason,
	                               vp->salvage.prio,
	                               NULL);
	return 1;
    }
#endif
    return 0;
}

static_inline int
try_FSSYNC(Volume *vp, char *partName, int *code) {
#ifdef FSSYNC_BUILD_CLIENT
    if (VCanUseFSSYNC()) {
	Log("Scheduling salvage for volume %lu on part %s over FSSYNC\n",
	    afs_printable_uint32_lu(vp->hashid), partName);

	/*
	 * If we aren't the fileserver, tell the fileserver the volume
	 * needs to be salvaged. We could directly tell the
	 * salvageserver, but the fileserver keeps track of some stats
	 * related to salvages, and handles some other salvage-related
	 * complications for us.
         */
        *code = FSYNC_VolOp(vp->hashid, partName,
                            FSYNC_VOL_FORCE_ERROR, FSYNC_SALVAGE, NULL);
	return 1;
    }
#endif /* FSSYNC_BUILD_CLIENT */
    return 0;
}

/**
 * schedule a salvage with the salvage server or fileserver.
 *
 * @param[in] vp  pointer to volume object
 *
 * @return operation status
 *    @retval 0 salvage scheduled successfully
 *    @retval 1 salvage not scheduled, or SALVSYNC/FSSYNC com error
 *
 * @pre
 *    @arg VOL_LOCK is held.
 *    @arg nUsers and nWaiters should be zero.
 *
 * @post salvageserver or fileserver is sent a salvage request
 *
 * @note If we are the fileserver, the request will be sent to the salvage
 * server over SALVSYNC. If we are not the fileserver, the request will be
 * sent to the fileserver over FSSYNC (FSYNC_VOL_FORCE_ERROR/FSYNC_SALVAGE).
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static int
VScheduleSalvage_r(Volume * vp)
{
    int ret=0;
    int code;
    VolState state_save;
    VThreadOptions_t * thread_opts;
    char partName[16];

    assert(VCanUseSALVSYNC() || VCanUseFSSYNC());

    if (vp->nWaiters || vp->nUsers) {
	return 1;
    }

    /* prevent endless salvage,attach,salvage,attach,... loops */
    if (vp->stats.salvages >= SALVAGE_COUNT_MAX)
	return 1;

    /*
     * don't perform salvsync ops on certain threads
     */
    thread_opts = pthread_getspecific(VThread_key);
    if (thread_opts == NULL) {
	thread_opts = &VThread_defaults;
    }
    if (thread_opts->disallow_salvsync || vol_disallow_salvsync) {
	return 1;
    }

    /*
     * XXX the scheduling process should really be done asynchronously
     *     to avoid fssync deadlocks
     */
    if (!vp->salvage.scheduled) {
	/* if we haven't previously scheduled a salvage, do so now
	 *
	 * set the volume to an exclusive state and drop the lock
	 * around the SALVSYNC call
	 *
	 * note that we do NOT acquire a reservation here -- doing so
	 * could result in unbounded recursion
	 */
	strlcpy(partName, VPartitionPath(vp->partition), sizeof(partName));
	state_save = VChangeState_r(vp, VOL_STATE_SALVSYNC_REQ);
	VOL_UNLOCK;

	assert(try_SALVSYNC(vp, partName, &code) ||
	       try_FSSYNC(vp, partName, &code));

	VOL_LOCK;
	VChangeState_r(vp, state_save);

	if (code == SYNC_OK) {
	    vp->salvage.scheduled = 1;
	    vp->stats.last_salvage_req = FT_ApproxTime();
	    if (VCanUseSALVSYNC()) {
		/* don't record these stats for non-fileservers; let the
		 * fileserver take care of these */
		vp->stats.salvages++;
		IncUInt64(&VStats.salvages);
	    }
	} else {
	    ret = 1;
	    switch(code) {
	    case SYNC_BAD_COMMAND:
	    case SYNC_COM_ERROR:
		break;
	    case SYNC_DENIED:
		Log("VScheduleSalvage_r: Salvage request for volume %lu "
		    "denied\n", afs_printable_uint32_lu(vp->hashid));
		break;
	    default:
		Log("VScheduleSalvage_r: Salvage request for volume %lu "
		    "received unknown protocol error %d\n",
		    afs_printable_uint32_lu(vp->hashid), code);
		break;
	    }

	    if (VCanUseFSSYNC()) {
		VChangeState_r(vp, VOL_STATE_ERROR);
	    }
	}
    }
    return ret;
}
#endif /* SALVSYNC_BUILD_CLIENT || FSSYNC_BUILD_CLIENT */

#ifdef SALVSYNC_BUILD_CLIENT

/**
 * connect to the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @post connection to salvageserver SYNC service established
 *
 * @see VConnectSALV_r
 * @see VDisconnectSALV
 * @see VReconnectSALV
 */
int
VConnectSALV(void)
{
    int retVal;
    VOL_LOCK;
    retVal = VConnectSALV_r();
    VOL_UNLOCK;
    return retVal;
}

/**
 * connect to the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre VOL_LOCK is held.
 *
 * @post connection to salvageserver SYNC service established
 *
 * @see VConnectSALV
 * @see VDisconnectSALV_r
 * @see VReconnectSALV_r
 * @see SALVSYNC_clientInit
 *
 * @internal volume package internal use only.
 */
int
VConnectSALV_r(void)
{
    return SALVSYNC_clientInit();
}

/**
 * disconnect from the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 success
 *
 * @pre client should have a live connection to the salvageserver
 *
 * @post connection to salvageserver SYNC service destroyed
 *
 * @see VDisconnectSALV_r
 * @see VConnectSALV
 * @see VReconnectSALV
 */
int
VDisconnectSALV(void)
{
    VOL_LOCK;
    VDisconnectSALV_r();
    VOL_UNLOCK;
    return 0;
}

/**
 * disconnect from the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 success
 *
 * @pre
 *    @arg VOL_LOCK is held.
 *    @arg client should have a live connection to the salvageserver.
 *
 * @post connection to salvageserver SYNC service destroyed
 *
 * @see VDisconnectSALV
 * @see VConnectSALV_r
 * @see VReconnectSALV_r
 * @see SALVSYNC_clientFinis
 *
 * @internal volume package internal use only.
 */
int
VDisconnectSALV_r(void)
{
    return SALVSYNC_clientFinis();
}

/**
 * disconnect and then re-connect to the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre client should have a live connection to the salvageserver
 *
 * @post old connection is dropped, and a new one is established
 *
 * @see VConnectSALV
 * @see VDisconnectSALV
 * @see VReconnectSALV_r
 */
int
VReconnectSALV(void)
{
    int retVal;
    VOL_LOCK;
    retVal = VReconnectSALV_r();
    VOL_UNLOCK;
    return retVal;
}

/**
 * disconnect and then re-connect to the salvageserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre
 *    @arg VOL_LOCK is held.
 *    @arg client should have a live connection to the salvageserver.
 *
 * @post old connection is dropped, and a new one is established
 *
 * @see VConnectSALV_r
 * @see VDisconnectSALV
 * @see VReconnectSALV
 * @see SALVSYNC_clientReconnect
 *
 * @internal volume package internal use only.
 */
int
VReconnectSALV_r(void)
{
    return SALVSYNC_clientReconnect();
}
#endif /* SALVSYNC_BUILD_CLIENT */
#endif /* AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* FSSYNC routines                                 */
/***************************************************/

/* This must be called by any volume utility which needs to run while the
   file server is also running.  This is separated from VInitVolumePackage2 so
   that a utility can fork--and each of the children can independently
   initialize communication with the file server */
#ifdef FSSYNC_BUILD_CLIENT
/**
 * connect to the fileserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre
 *    @arg VInit must equal 2.
 *    @arg Program Type must not be fileserver or salvager.
 *
 * @post connection to fileserver SYNC service established
 *
 * @see VConnectFS_r
 * @see VDisconnectFS
 * @see VChildProcReconnectFS
 */
int
VConnectFS(void)
{
    int retVal;
    VOL_LOCK;
    retVal = VConnectFS_r();
    VOL_UNLOCK;
    return retVal;
}

/**
 * connect to the fileserver SYNC service.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre
 *    @arg VInit must equal 2.
 *    @arg Program Type must not be fileserver or salvager.
 *    @arg VOL_LOCK is held.
 *
 * @post connection to fileserver SYNC service established
 *
 * @see VConnectFS
 * @see VDisconnectFS_r
 * @see VChildProcReconnectFS_r
 *
 * @internal volume package internal use only.
 */
int
VConnectFS_r(void)
{
    int rc;
    assert((VInit == 2) &&
	   (programType != fileServer) &&
	   (programType != salvager));
    rc = FSYNC_clientInit();
    if (rc)
	VInit = 3;
    return rc;
}

/**
 * disconnect from the fileserver SYNC service.
 *
 * @pre
 *    @arg client should have a live connection to the fileserver.
 *    @arg VOL_LOCK is held.
 *    @arg Program Type must not be fileserver or salvager.
 *
 * @post connection to fileserver SYNC service destroyed
 *
 * @see VDisconnectFS
 * @see VConnectFS_r
 * @see VChildProcReconnectFS_r
 *
 * @internal volume package internal use only.
 */
void
VDisconnectFS_r(void)
{
    assert((programType != fileServer) &&
	   (programType != salvager));
    FSYNC_clientFinis();
    VInit = 2;
}

/**
 * disconnect from the fileserver SYNC service.
 *
 * @pre
 *    @arg client should have a live connection to the fileserver.
 *    @arg Program Type must not be fileserver or salvager.
 *
 * @post connection to fileserver SYNC service destroyed
 *
 * @see VDisconnectFS_r
 * @see VConnectFS
 * @see VChildProcReconnectFS
 */
void
VDisconnectFS(void)
{
    VOL_LOCK;
    VDisconnectFS_r();
    VOL_UNLOCK;
}

/**
 * connect to the fileserver SYNC service from a child process following a fork.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre
 *    @arg VOL_LOCK is held.
 *    @arg current FSYNC handle is shared with a parent process
 *
 * @post current FSYNC handle is discarded and a new connection to the
 *       fileserver SYNC service is established
 *
 * @see VChildProcReconnectFS
 * @see VConnectFS_r
 * @see VDisconnectFS_r
 *
 * @internal volume package internal use only.
 */
int
VChildProcReconnectFS_r(void)
{
    return FSYNC_clientChildProcReconnect();
}

/**
 * connect to the fileserver SYNC service from a child process following a fork.
 *
 * @return operation status
 *    @retval 0 failure
 *    @retval 1 success
 *
 * @pre current FSYNC handle is shared with a parent process
 *
 * @post current FSYNC handle is discarded and a new connection to the
 *       fileserver SYNC service is established
 *
 * @see VChildProcReconnectFS_r
 * @see VConnectFS
 * @see VDisconnectFS
 */
int
VChildProcReconnectFS(void)
{
    int ret;
    VOL_LOCK;
    ret = VChildProcReconnectFS_r();
    VOL_UNLOCK;
    return ret;
}
#endif /* FSSYNC_BUILD_CLIENT */


/***************************************************/
/* volume bitmap routines                          */
/***************************************************/

/**
 * allocate a vnode bitmap number for the vnode
 *
 * @param[out] ec  error code
 * @param[in] vp   volume object pointer
 * @param[in] index vnode index number for the vnode
 * @param[in] flags flag values described in note
 *
 * @note for DAFS, flags parameter controls locking behavior.
 * If (flags & VOL_ALLOC_BITMAP_WAIT) is set, then this function
 * will create a reservation and block on any other exclusive
 * operations.  Otherwise, this function assumes the caller
 * already has exclusive access to vp, and we just change the
 * volume state.
 *
 * @pre VOL_LOCK held
 *
 * @return bit number allocated
 */
/*

 */
int
VAllocBitmapEntry_r(Error * ec, Volume * vp,
		    struct vnodeIndex *index, int flags)
{
    int ret = 0;
    byte *bp, *ep;
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;
#endif /* AFS_DEMAND_ATTACH_FS */

    *ec = 0;

    /* This test is probably redundant */
    if (!VolumeWriteable(vp)) {
	*ec = (bit32) VREADONLY;
	return ret;
    }

#ifdef AFS_DEMAND_ATTACH_FS
    if (flags & VOL_ALLOC_BITMAP_WAIT) {
	VCreateReservation_r(vp);
	VWaitExclusiveState_r(vp);
    }
    state_save = VChangeState_r(vp, VOL_STATE_GET_BITMAP);
#endif /* AFS_DEMAND_ATTACH_FS */

#ifdef BITMAP_LATER
    if ((programType == fileServer) && !index->bitmap) {
	int i;
#ifndef AFS_DEMAND_ATTACH_FS
	/* demand attach fs uses the volume state to avoid races.
	 * specialStatus field is not used at all */
	int wasVBUSY = 0;
	if (vp->specialStatus == VBUSY) {
	    if (vp->goingOffline) {	/* vos dump waiting for the volume to
					 * go offline. We probably come here
					 * from AddNewReadableResidency */
		wasVBUSY = 1;
	    } else {
		while (vp->specialStatus == VBUSY) {
#ifdef AFS_PTHREAD_ENV
		    VOL_UNLOCK;
		    sleep(2);
		    VOL_LOCK;
#else /* !AFS_PTHREAD_ENV */
		    IOMGR_Sleep(2);
#endif /* !AFS_PTHREAD_ENV */
		}
	    }
	}
#endif /* !AFS_DEMAND_ATTACH_FS */

	if (!index->bitmap) {
#ifndef AFS_DEMAND_ATTACH_FS
	    vp->specialStatus = VBUSY;	/* Stop anyone else from using it. */
#endif /* AFS_DEMAND_ATTACH_FS */
	    for (i = 0; i < nVNODECLASSES; i++) {
		VGetBitmap_r(ec, vp, i);
		if (*ec) {
#ifdef AFS_DEMAND_ATTACH_FS
		    VRequestSalvage_r(ec, vp, SALVSYNC_ERROR, VOL_SALVAGE_INVALIDATE_HEADER);
#else /* AFS_DEMAND_ATTACH_FS */
		    DeleteVolumeFromHashTable(vp);
		    vp->shuttingDown = 1;	/* Let who has it free it. */
		    vp->specialStatus = 0;
#endif /* AFS_DEMAND_ATTACH_FS */
		    goto done;
		}
	    }
#ifndef AFS_DEMAND_ATTACH_FS
	    if (!wasVBUSY)
		vp->specialStatus = 0;	/* Allow others to have access. */
#endif /* AFS_DEMAND_ATTACH_FS */
	}
    }
#endif /* BITMAP_LATER */

#ifdef AFS_DEMAND_ATTACH_FS
    VOL_UNLOCK;
#endif /* AFS_DEMAND_ATTACH_FS */
    bp = index->bitmap + index->bitmapOffset;
    ep = index->bitmap + index->bitmapSize;
    while (bp < ep) {
	if ((*(bit32 *) bp) != (bit32) 0xffffffff) {
	    int o;
	    index->bitmapOffset = (afs_uint32) (bp - index->bitmap);
	    while (*bp == 0xff)
		bp++;
	    o = ffs(~*bp) - 1;	/* ffs is documented in BSTRING(3) */
	    *bp |= (1 << o);
	    ret = ((bp - index->bitmap) * 8 + o);
#ifdef AFS_DEMAND_ATTACH_FS
	    VOL_LOCK;
#endif /* AFS_DEMAND_ATTACH_FS */
	    goto done;
	}
	bp += sizeof(bit32) /* i.e. 4 */ ;
    }
    /* No bit map entry--must grow bitmap */
    bp = (byte *)
	realloc(index->bitmap, index->bitmapSize + VOLUME_BITMAP_GROWSIZE);
    assert(bp != NULL);
    index->bitmap = bp;
    bp += index->bitmapSize;
    memset(bp, 0, VOLUME_BITMAP_GROWSIZE);
    index->bitmapOffset = index->bitmapSize;
    index->bitmapSize += VOLUME_BITMAP_GROWSIZE;
    *bp = 1;
    ret = index->bitmapOffset * 8;
#ifdef AFS_DEMAND_ATTACH_FS
    VOL_LOCK;
#endif /* AFS_DEMAND_ATTACH_FS */

 done:
#ifdef AFS_DEMAND_ATTACH_FS
    VChangeState_r(vp, state_save);
    if (flags & VOL_ALLOC_BITMAP_WAIT) {
	VCancelReservation_r(vp);
    }
#endif /* AFS_DEMAND_ATTACH_FS */
    return ret;
}

int
VAllocBitmapEntry(Error * ec, Volume * vp, struct vnodeIndex * index)
{
    int retVal;
    VOL_LOCK;
    retVal = VAllocBitmapEntry_r(ec, vp, index, VOL_ALLOC_BITMAP_WAIT);
    VOL_UNLOCK;
    return retVal;
}

void
VFreeBitMapEntry_r(Error * ec, struct vnodeIndex *index,
		   unsigned bitNumber)
{
    unsigned int offset;

    *ec = 0;
#ifdef BITMAP_LATER
    if (!index->bitmap)
	return;
#endif /* BITMAP_LATER */
    offset = bitNumber >> 3;
    if (offset >= index->bitmapSize) {
	*ec = VNOVNODE;
	return;
    }
    if (offset < index->bitmapOffset)
	index->bitmapOffset = offset & ~3;	/* Truncate to nearest bit32 */
    *(index->bitmap + offset) &= ~(1 << (bitNumber & 0x7));
}

void
VFreeBitMapEntry(Error * ec, struct vnodeIndex *index,
		 unsigned bitNumber)
{
    VOL_LOCK;
    VFreeBitMapEntry_r(ec, index, bitNumber);
    VOL_UNLOCK;
}

/* this function will drop the glock internally.
 * for old pthread fileservers, this is safe thanks to vbusy.
 *
 * for demand attach fs, caller must have already called
 * VCreateReservation_r and VWaitExclusiveState_r */
static void
VGetBitmap_r(Error * ec, Volume * vp, VnodeClass class)
{
    StreamHandle_t *file;
    afs_sfsize_t nVnodes, size;
    struct VnodeClassInfo *vcp = &VnodeClassInfo[class];
    struct vnodeIndex *vip = &vp->vnodeIndex[class];
    struct VnodeDiskObject *vnode;
    unsigned int unique = 0;
    FdHandle_t *fdP;
#ifdef BITMAP_LATER
    byte *BitMap = 0;
#endif /* BITMAP_LATER */
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;
#endif /* AFS_DEMAND_ATTACH_FS */

    *ec = 0;

#ifdef AFS_DEMAND_ATTACH_FS
    state_save = VChangeState_r(vp, VOL_STATE_GET_BITMAP);
#endif /* AFS_DEMAND_ATTACH_FS */
    VOL_UNLOCK;

    fdP = IH_OPEN(vip->handle);
    assert(fdP != NULL);
    file = FDH_FDOPEN(fdP, "r");
    assert(file != NULL);
    vnode = (VnodeDiskObject *) malloc(vcp->diskSize);
    assert(vnode != NULL);
    size = OS_SIZE(fdP->fd_fd);
    assert(size != -1);
    nVnodes = (size <= vcp->diskSize ? 0 : size - vcp->diskSize)
	>> vcp->logSize;
    vip->bitmapSize = ((nVnodes / 8) + 10) / 4 * 4;	/* The 10 is a little extra so
							 * a few files can be created in this volume,
							 * the whole thing is rounded up to nearest 4
							 * bytes, because the bit map allocator likes
							 * it that way */
#ifdef BITMAP_LATER
    BitMap = (byte *) calloc(1, vip->bitmapSize);
    assert(BitMap != NULL);
#else /* BITMAP_LATER */
    vip->bitmap = (byte *) calloc(1, vip->bitmapSize);
    assert(vip->bitmap != NULL);
    vip->bitmapOffset = 0;
#endif /* BITMAP_LATER */
    if (STREAM_ASEEK(file, vcp->diskSize) != -1) {
	int bitNumber = 0;
	for (bitNumber = 0; bitNumber < nVnodes + 100; bitNumber++) {
	    if (STREAM_READ(vnode, vcp->diskSize, 1, file) != 1)
		break;
	    if (vnode->type != vNull) {
#ifdef BITMAP_LATER
		*(BitMap + (bitNumber >> 3)) |= (1 << (bitNumber & 0x7));
#else /* BITMAP_LATER */
		*(vip->bitmap + (bitNumber >> 3)) |= (1 << (bitNumber & 0x7));
#endif /* BITMAP_LATER */
		if (unique <= vnode->uniquifier)
		    unique = vnode->uniquifier + 1;
	    }
#ifndef AFS_PTHREAD_ENV
	    if ((bitNumber & 0x00ff) == 0x0ff) {	/* every 256 iterations */
		IOMGR_Poll();
	    }
#endif /* !AFS_PTHREAD_ENV */
	}
    }
    if (vp->nextVnodeUnique < unique) {
	Log("GetBitmap: bad volume uniquifier for volume %s; volume needs salvage\n", V_name(vp));
	*ec = VSALVAGE;
    }
    /* Paranoia, partly justified--I think fclose after fdopen
     * doesn't seem to close fd.  In any event, the documentation
     * doesn't specify, so it's safer to close it twice.
     */
    STREAM_CLOSE(file);
    FDH_CLOSE(fdP);
    free(vnode);

    VOL_LOCK;
#ifdef BITMAP_LATER
    /* There may have been a racing condition with some other thread, both
     * creating the bitmaps for this volume. If the other thread was faster
     * the pointer to bitmap should already be filled and we can free ours.
     */
    if (vip->bitmap == NULL) {
	vip->bitmap = BitMap;
	vip->bitmapOffset = 0;
    } else
	free((byte *) BitMap);
#endif /* BITMAP_LATER */
#ifdef AFS_DEMAND_ATTACH_FS
    VChangeState_r(vp, state_save);
#endif /* AFS_DEMAND_ATTACH_FS */
}


/***************************************************/
/* Volume Path and Volume Number utility routines  */
/***************************************************/

/**
 * find the first occurrence of a volume header file and return the path.
 *
 * @param[out] ec          outbound error code
 * @param[in]  volumeId    volume id to find
 * @param[out] partitionp  pointer to disk partition path string
 * @param[out] namep       pointer to volume header file name string
 *
 * @post path to first occurrence of volume header is returned in partitionp
 *       and namep, or ec is set accordingly.
 *
 * @warning this function is NOT re-entrant -- partitionp and namep point to
 *          static data segments
 *
 * @note if a volume utility inadvertently leaves behind a stale volume header
 *       on a vice partition, it is possible for callers to get the wrong one,
 *       depending on the order of the disk partition linked list.
 *
 */
void
VGetVolumePath(Error * ec, VolId volumeId, char **partitionp, char **namep)
{
    static char partition[VMAXPATHLEN], name[VMAXPATHLEN];
    char path[VMAXPATHLEN];
    int found = 0;
    struct DiskPartition64 *dp;

    *ec = 0;
    name[0] = '/';
    (void)afs_snprintf(&name[1], (sizeof name) - 1, VFORMAT, afs_printable_uint32_lu(volumeId));
    for (dp = DiskPartitionList; dp; dp = dp->next) {
	struct afs_stat status;
	strcpy(path, VPartitionPath(dp));
	strcat(path, name);
	if (afs_stat(path, &status) == 0) {
	    strcpy(partition, dp->name);
	    found = 1;
	    break;
	}
    }
    if (!found) {
	*ec = VNOVOL;
	*partitionp = *namep = NULL;
    } else {
	*partitionp = partition;
	*namep = name;
    }
}

/**
 * extract a volume number from a volume header filename string.
 *
 * @param[in] name  volume header filename string
 *
 * @return volume number
 *
 * @note the string must be of the form VFORMAT.  the only permissible
 *       deviation is a leading '/' character.
 *
 * @see VFORMAT
 */
int
VolumeNumber(char *name)
{
    if (*name == '/')
	name++;
    return atoi(name + 1);
}

/**
 * compute the volume header filename.
 *
 * @param[in] volumeId
 *
 * @return volume header filename
 *
 * @post volume header filename string is constructed
 *
 * @warning this function is NOT re-entrant -- the returned string is
 *          stored in a static char array.  see VolumeExternalName_r
 *          for a re-entrant equivalent.
 *
 * @see VolumeExternalName_r
 *
 * @deprecated due to the above re-entrancy warning, this interface should
 *             be considered deprecated.  Please use VolumeExternalName_r
 *             in its stead.
 */
char *
VolumeExternalName(VolumeId volumeId)
{
    static char name[VMAXPATHLEN];
    (void)afs_snprintf(name, sizeof name, VFORMAT, afs_printable_uint32_lu(volumeId));
    return name;
}

/**
 * compute the volume header filename.
 *
 * @param[in]     volumeId
 * @param[inout]  name       array in which to store filename
 * @param[in]     len        length of name array
 *
 * @return result code from afs_snprintf
 *
 * @see VolumeExternalName
 * @see afs_snprintf
 *
 * @note re-entrant equivalent of VolumeExternalName
 */
int
VolumeExternalName_r(VolumeId volumeId, char * name, size_t len)
{
    return afs_snprintf(name, len, VFORMAT, afs_printable_uint32_lu(volumeId));
}


/***************************************************/
/* Volume Usage Statistics routines                */
/***************************************************/

#if OPENAFS_VOL_STATS
#define OneDay	(86400)		/* 24 hours' worth of seconds */
#else
#define OneDay	(24*60*60)	/* 24 hours */
#endif /* OPENAFS_VOL_STATS */

static time_t
Midnight(time_t t) {
    struct tm local, *l;
    time_t midnight;

#if defined(AFS_PTHREAD_ENV) && !defined(AFS_NT40_ENV)
    l = localtime_r(&t, &local);
#else
    l = localtime(&t);
#endif

    if (l != NULL) {
	/* the following is strictly speaking problematic on the
	   switching day to daylight saving time, after the switch,
	   as tm_isdst does not match.  Similarly, on the looong day when
	   switching back the OneDay check will not do what naively expected!
	   The effects are minor, though, and more a matter of interpreting
	   the numbers. */
#ifndef AFS_PTHREAD_ENV
	local = *l;
#endif
	local.tm_hour = local.tm_min=local.tm_sec = 0;
	midnight = mktime(&local);
	if (midnight != (time_t) -1) return(midnight);
    }
    return( (t/OneDay)*OneDay );

}

/*------------------------------------------------------------------------
 * [export] VAdjustVolumeStatistics
 *
 * Description:
 *	If we've passed midnight, we need to update all the day use
 *	statistics as well as zeroing the detailed volume statistics
 *	(if we are implementing them).
 *
 * Arguments:
 *	vp : Pointer to the volume structure describing the lucky
 *		volume being considered for update.
 *
 * Returns:
 *	0 (always!)
 *
 * Environment:
 *	Nothing interesting.
 *
 * Side Effects:
 *	As described.
 *------------------------------------------------------------------------*/

int
VAdjustVolumeStatistics_r(Volume * vp)
{
    unsigned int now = FT_ApproxTime();

    if (now - V_dayUseDate(vp) > OneDay) {
	int ndays, i;

	ndays = (now - V_dayUseDate(vp)) / OneDay;
	for (i = 6; i > ndays - 1; i--)
	    V_weekUse(vp)[i] = V_weekUse(vp)[i - ndays];
	for (i = 0; i < ndays - 1 && i < 7; i++)
	    V_weekUse(vp)[i] = 0;
	if (ndays <= 7)
	    V_weekUse(vp)[ndays - 1] = V_dayUse(vp);
	V_dayUse(vp) = 0;
	V_dayUseDate(vp) = Midnight(now);

#if OPENAFS_VOL_STATS
	/*
	 * All we need to do is bzero the entire VOL_STATS_BYTES of
	 * the detailed volume statistics area.
	 */
	memset((V_stat_area(vp)), 0, VOL_STATS_BYTES);
#endif /* OPENAFS_VOL_STATS */
    }

    /*It's been more than a day of collection */
    /*
     * Always return happily.
     */
    return (0);
}				/*VAdjustVolumeStatistics */

int
VAdjustVolumeStatistics(Volume * vp)
{
    int retVal;
    VOL_LOCK;
    retVal = VAdjustVolumeStatistics_r(vp);
    VOL_UNLOCK;
    return retVal;
}

void
VBumpVolumeUsage_r(Volume * vp)
{
    unsigned int now = FT_ApproxTime();
    V_accessDate(vp) = now;
    if (now - V_dayUseDate(vp) > OneDay)
	VAdjustVolumeStatistics_r(vp);
    /*
     * Save the volume header image to disk after every 128 bumps to dayUse.
     */
    if ((V_dayUse(vp)++ & 127) == 0) {
	Error error;
	VUpdateVolume_r(&error, vp, VOL_UPDATE_WAIT);
    }
}

void
VBumpVolumeUsage(Volume * vp)
{
    VOL_LOCK;
    VBumpVolumeUsage_r(vp);
    VOL_UNLOCK;
}

void
VSetDiskUsage_r(void)
{
#ifndef AFS_DEMAND_ATTACH_FS
    static int FifteenMinuteCounter = 0;
#endif

    while (VInit < 2) {
	/* NOTE: Don't attempt to access the partitions list until the
	 * initialization level indicates that all volumes are attached,
	 * which implies that all partitions are initialized. */
#ifdef AFS_PTHREAD_ENV
	sleep(10);
#else /* AFS_PTHREAD_ENV */
	IOMGR_Sleep(10);
#endif /* AFS_PTHREAD_ENV */
    }

    VResetDiskUsage_r();

#ifndef AFS_DEMAND_ATTACH_FS
    if (++FifteenMinuteCounter == 3) {
	FifteenMinuteCounter = 0;
	VScanUpdateList();
    }
#endif /* !AFS_DEMAND_ATTACH_FS */
}

void
VSetDiskUsage(void)
{
    VOL_LOCK;
    VSetDiskUsage_r();
    VOL_UNLOCK;
}


/***************************************************/
/* Volume Update List routines                     */
/***************************************************/

/* The number of minutes that a volume hasn't been updated before the
 * "Dont salvage" flag in the volume header will be turned on */
#define SALVAGE_INTERVAL	(10*60)

/*
 * demand attach fs
 *
 * volume update list functionality has been moved into the VLRU
 * the DONT_SALVAGE flag is now set during VLRU demotion
 */

#ifndef AFS_DEMAND_ATTACH_FS
static VolumeId *UpdateList = NULL;	/* Pointer to array of Volume ID's */
static int nUpdatedVolumes = 0;	        /* Updated with entry in UpdateList, salvage after crash flag on */
static int updateSize = 0;		/* number of entries possible */
#define UPDATE_LIST_SIZE 128	        /* initial size increment (must be a power of 2!) */
#endif /* !AFS_DEMAND_ATTACH_FS */

void
VAddToVolumeUpdateList_r(Error * ec, Volume * vp)
{
    *ec = 0;
    vp->updateTime = FT_ApproxTime();
    if (V_dontSalvage(vp) == 0)
	return;
    V_dontSalvage(vp) = 0;
    VSyncVolume_r(ec, vp, 0);
#ifdef AFS_DEMAND_ATTACH_FS
    V_attachFlags(vp) &= ~(VOL_HDR_DONTSALV);
#else /* !AFS_DEMAND_ATTACH_FS */
    if (*ec)
	return;
    if (UpdateList == NULL) {
	updateSize = UPDATE_LIST_SIZE;
	UpdateList = (VolumeId *) malloc(sizeof(VolumeId) * updateSize);
    } else {
	if (nUpdatedVolumes == updateSize) {
	    updateSize <<= 1;
	    if (updateSize > 524288) {
		Log("warning: there is likely a bug in the volume update scanner\n");
		return;
	    }
	    UpdateList =
		(VolumeId *) realloc(UpdateList,
				     sizeof(VolumeId) * updateSize);
	}
    }
    assert(UpdateList != NULL);
    UpdateList[nUpdatedVolumes++] = V_id(vp);
#endif /* !AFS_DEMAND_ATTACH_FS */
}

#ifndef AFS_DEMAND_ATTACH_FS
static void
VScanUpdateList(void)
{
    int i, gap;
    Volume *vp;
    Error error;
    afs_uint32 now = FT_ApproxTime();
    /* Be careful with this code, since it works with interleaved calls to AddToVolumeUpdateList */
    for (i = gap = 0; i < nUpdatedVolumes; i++) {
	if (gap)
	    UpdateList[i - gap] = UpdateList[i];

	/* XXX this routine needlessly messes up the Volume LRU by
	 * breaking the LRU temporal-locality assumptions.....
	 * we should use a special volume header allocator here */
	vp = VGetVolume_r(&error, UpdateList[i - gap] = UpdateList[i]);
	if (error) {
	    gap++;
	} else if (vp->nUsers == 1 && now - vp->updateTime > SALVAGE_INTERVAL) {
	    V_dontSalvage(vp) = DONT_SALVAGE;
	    VUpdateVolume_r(&error, vp, 0);	/* No need to fsync--not critical */
	    gap++;
	}

	if (vp) {
	    VPutVolume_r(vp);
	}

#ifndef AFS_PTHREAD_ENV
	IOMGR_Poll();
#endif /* !AFS_PTHREAD_ENV */
    }
    nUpdatedVolumes -= gap;
}
#endif /* !AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* Volume LRU routines                             */
/***************************************************/

/* demand attach fs
 * volume LRU
 *
 * with demand attach fs, we attempt to soft detach(1)
 * volumes which have not been accessed in a long time
 * in order to speed up fileserver shutdown
 *
 * (1) by soft detach we mean a process very similar
 *     to VOffline, except the final state of the
 *     Volume will be VOL_STATE_PREATTACHED, instead
 *     of the usual VOL_STATE_UNATTACHED
 */
#ifdef AFS_DEMAND_ATTACH_FS

/* implementation is reminiscent of a generational GC
 *
 * queue 0 is newly attached volumes. this queue is
 * sorted by attach timestamp
 *
 * queue 1 is volumes that have been around a bit
 * longer than queue 0. this queue is sorted by
 * attach timestamp
 *
 * queue 2 is volumes tha have been around the longest.
 * this queue is unsorted
 *
 * queue 3 is volumes that have been marked as
 * candidates for soft detachment. this queue is
 * unsorted
 */
#define VLRU_GENERATIONS  3   /**< number of generations in VLRU */
#define VLRU_QUEUES       5   /**< total number of VLRU queues */

/**
 * definition of a VLRU queue.
 */
struct VLRU_q {
    volatile struct rx_queue q;
    volatile int len;
    volatile int busy;
    pthread_cond_t cv;
};

/**
 * main VLRU data structure.
 */
struct VLRU {
    struct VLRU_q q[VLRU_QUEUES];   /**< VLRU queues */

    /* VLRU config */
    /** time interval (in seconds) between promotion passes for
     *  each young generation queue. */
    afs_uint32 promotion_interval[VLRU_GENERATIONS-1];

    /** time interval (in seconds) between soft detach candidate
     *  scans for each generation queue.
     *
     *  scan_interval[VLRU_QUEUE_CANDIDATE] defines how frequently
     *  we perform a soft detach pass. */
    afs_uint32 scan_interval[VLRU_GENERATIONS+1];

    /* scheduler state */
    int next_idx;                                       /**< next queue to receive attention */
    afs_uint32 last_promotion[VLRU_GENERATIONS-1];      /**< timestamp of last promotion scan */
    afs_uint32 last_scan[VLRU_GENERATIONS+1];           /**< timestamp of last detach scan */

    int scanner_state;                                  /**< state of scanner thread */
    pthread_cond_t cv;                                  /**< state transition CV */
};

/** global VLRU state */
static struct VLRU volume_LRU;

/**
 * defined states for VLRU scanner thread.
 */
typedef enum {
    VLRU_SCANNER_STATE_OFFLINE        = 0,    /**< vlru scanner thread is offline */
    VLRU_SCANNER_STATE_ONLINE         = 1,    /**< vlru scanner thread is online */
    VLRU_SCANNER_STATE_SHUTTING_DOWN  = 2,    /**< vlru scanner thread is shutting down */
    VLRU_SCANNER_STATE_PAUSING        = 3,    /**< vlru scanner thread is getting ready to pause */
    VLRU_SCANNER_STATE_PAUSED         = 4     /**< vlru scanner thread is paused */
} vlru_thread_state_t;

/* vlru disk data header stuff */
#define VLRU_DISK_MAGIC      0x7a8b9cad        /**< vlru disk entry magic number */
#define VLRU_DISK_VERSION    1                 /**< vlru disk entry version number */

/** vlru default expiration time (for eventual fs state serialization of vlru data) */
#define VLRU_DUMP_EXPIRATION_TIME   (60*60*24*7)  /* expire vlru data after 1 week */


/** minimum volume inactivity (in seconds) before a volume becomes eligible for
 *  soft detachment. */
static afs_uint32 VLRU_offline_thresh = VLRU_DEFAULT_OFFLINE_THRESH;

/** time interval (in seconds) between VLRU scanner thread soft detach passes. */
static afs_uint32 VLRU_offline_interval = VLRU_DEFAULT_OFFLINE_INTERVAL;

/** maximum number of volumes to soft detach in a VLRU soft detach pass. */
static afs_uint32 VLRU_offline_max = VLRU_DEFAULT_OFFLINE_MAX;

/** VLRU control flag.  non-zero value implies VLRU subsystem is activated. */
static afs_uint32 VLRU_enabled = 1;

/* queue synchronization routines */
static void VLRU_BeginExclusive_r(struct VLRU_q * q);
static void VLRU_EndExclusive_r(struct VLRU_q * q);
static void VLRU_Wait_r(struct VLRU_q * q);

/**
 * set VLRU subsystem tunable parameters.
 *
 * @param[in] option  tunable option to modify
 * @param[in] val     new value for tunable parameter
 *
 * @pre @c VInitVolumePackage2 has not yet been called.
 *
 * @post tunable parameter is modified
 *
 * @note DAFS only
 *
 * @note valid option parameters are:
 *    @arg @c VLRU_SET_THRESH
 *         set the period of inactivity after which
 *         volumes are eligible for soft detachment
 *    @arg @c VLRU_SET_INTERVAL
 *         set the time interval between calls
 *         to the volume LRU "garbage collector"
 *    @arg @c VLRU_SET_MAX
 *         set the max number of volumes to deallocate
 *         in one GC pass
 */
void
VLRU_SetOptions(int option, afs_uint32 val)
{
    if (option == VLRU_SET_THRESH) {
	VLRU_offline_thresh = val;
    } else if (option == VLRU_SET_INTERVAL) {
	VLRU_offline_interval = val;
    } else if (option == VLRU_SET_MAX) {
	VLRU_offline_max = val;
    } else if (option == VLRU_SET_ENABLED) {
	VLRU_enabled = val;
    }
    VLRU_ComputeConstants();
}

/**
 * compute VLRU internal timing parameters.
 *
 * @post VLRU scanner thread internal timing parameters are computed
 *
 * @note computes internal timing parameters based upon user-modifiable
 *       tunable parameters.
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VLRU_ComputeConstants(void)
{
    afs_uint32 factor = VLRU_offline_thresh / VLRU_offline_interval;

    /* compute the candidate scan interval */
    volume_LRU.scan_interval[VLRU_QUEUE_CANDIDATE] = VLRU_offline_interval;

    /* compute the promotion intervals */
    volume_LRU.promotion_interval[VLRU_QUEUE_NEW] = VLRU_offline_thresh * 2;
    volume_LRU.promotion_interval[VLRU_QUEUE_MID] = VLRU_offline_thresh * 4;

    if (factor > 16) {
	/* compute the gen 0 scan interval */
	volume_LRU.scan_interval[VLRU_QUEUE_NEW] = VLRU_offline_thresh / 8;
    } else {
	/* compute the gen 0 scan interval */
	volume_LRU.scan_interval[VLRU_QUEUE_NEW] = VLRU_offline_interval * 2;
    }
}

/**
 * initialize VLRU subsystem.
 *
 * @pre this function has not yet been called
 *
 * @post VLRU subsystem is initialized and VLRU scanner thread is starting
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VInitVLRU(void)
{
    pthread_t tid;
    pthread_attr_t attrs;
    int i;

    if (!VLRU_enabled) {
	Log("VLRU: disabled\n");
	return;
    }

    /* initialize each of the VLRU queues */
    for (i = 0; i < VLRU_QUEUES; i++) {
	queue_Init(&volume_LRU.q[i]);
	volume_LRU.q[i].len = 0;
	volume_LRU.q[i].busy = 0;
	assert(pthread_cond_init(&volume_LRU.q[i].cv, NULL) == 0);
    }

    /* setup the timing constants */
    VLRU_ComputeConstants();

    /* XXX put inside LogLevel check? */
    Log("VLRU: starting scanner with the following configuration parameters:\n");
    Log("VLRU:  offlining volumes after minimum of %d seconds of inactivity\n", VLRU_offline_thresh);
    Log("VLRU:  running VLRU soft detach pass every %d seconds\n", VLRU_offline_interval);
    Log("VLRU:  taking up to %d volumes offline per pass\n", VLRU_offline_max);
    Log("VLRU:  scanning generation 0 for inactive volumes every %d seconds\n", volume_LRU.scan_interval[0]);
    Log("VLRU:  scanning for promotion/demotion between generations 0 and 1 every %d seconds\n", volume_LRU.promotion_interval[0]);
    Log("VLRU:  scanning for promotion/demotion between generations 1 and 2 every %d seconds\n", volume_LRU.promotion_interval[1]);

    /* start up the VLRU scanner */
    volume_LRU.scanner_state = VLRU_SCANNER_STATE_OFFLINE;
    if (programType == fileServer) {
	assert(pthread_cond_init(&volume_LRU.cv, NULL) == 0);
	assert(pthread_attr_init(&attrs) == 0);
	assert(pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) == 0);
	assert(pthread_create(&tid, &attrs, &VLRU_ScannerThread, NULL) == 0);
    }
}

/**
 * initialize the VLRU-related fields of a newly allocated volume object.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre
 *    @arg @c VOL_LOCK is held.
 *    @arg volume object is not on a VLRU queue.
 *
 * @post VLRU fields are initialized to indicate that volume object is not
 *       currently registered with the VLRU subsystem
 *
 * @note DAFS only
 *
 * @internal volume package interal use only.
 */
static void
VLRU_Init_Node_r(Volume * vp)
{
    if (!VLRU_enabled)
	return;

    assert(queue_IsNotOnQueue(&vp->vlru));
    vp->vlru.idx = VLRU_QUEUE_INVALID;
}

/**
 * add a volume object to a VLRU queue.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre
 *    @arg @c VOL_LOCK is held.
 *    @arg caller MUST hold a lightweight ref on @p vp.
 *    @arg caller MUST NOT hold exclusive ownership of the VLRU queue.
 *
 * @post the volume object is added to the appropriate VLRU queue
 *
 * @note if @c vp->vlru.idx contains the index of a valid VLRU queue,
 *       then the volume is added to that queue.  Otherwise, the value
 *       @c VLRU_QUEUE_NEW is stored into @c vp->vlru.idx and the
 *       volume is added to the NEW generation queue.
 *
 * @note @c VOL_LOCK may be dropped internally
 *
 * @note Volume state is temporarily set to @c VOL_STATE_VLRU_ADD
 *       during the add operation, and is restored to the previous
 *       state prior to return.
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VLRU_Add_r(Volume * vp)
{
    int idx;
    VolState state_save;

    if (!VLRU_enabled)
	return;

    if (queue_IsOnQueue(&vp->vlru))
	return;

    state_save = VChangeState_r(vp, VOL_STATE_VLRU_ADD);

    idx = vp->vlru.idx;
    if ((idx < 0) || (idx >= VLRU_QUEUE_INVALID)) {
	idx = VLRU_QUEUE_NEW;
    }

    VLRU_Wait_r(&volume_LRU.q[idx]);

    /* repeat check since VLRU_Wait_r may have dropped
     * the glock */
    if (queue_IsNotOnQueue(&vp->vlru)) {
	vp->vlru.idx = idx;
	queue_Prepend(&volume_LRU.q[idx], &vp->vlru);
	volume_LRU.q[idx].len++;
	V_attachFlags(vp) |= VOL_ON_VLRU;
	vp->stats.last_promote = FT_ApproxTime();
    }

    VChangeState_r(vp, state_save);
}

/**
 * delete a volume object from a VLRU queue.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre
 *    @arg @c VOL_LOCK is held.
 *    @arg caller MUST hold a lightweight ref on @p vp.
 *    @arg caller MUST NOT hold exclusive ownership of the VLRU queue.
 *
 * @post volume object is removed from the VLRU queue
 *
 * @note @c VOL_LOCK may be dropped internally
 *
 * @note DAFS only
 *
 * @todo We should probably set volume state to something exlcusive
 *       (as @c VLRU_Add_r does) prior to dropping @c VOL_LOCK.
 *
 * @internal volume package internal use only.
 */
static void
VLRU_Delete_r(Volume * vp)
{
    int idx;

    if (!VLRU_enabled)
	return;

    if (queue_IsNotOnQueue(&vp->vlru))
	return;

    /* handle races */
    do {
      idx = vp->vlru.idx;
      if (idx == VLRU_QUEUE_INVALID)
	  return;
      VLRU_Wait_r(&volume_LRU.q[idx]);
    } while (idx != vp->vlru.idx);

    /* now remove from the VLRU and update
     * the appropriate counter */
    queue_Remove(&vp->vlru);
    volume_LRU.q[idx].len--;
    vp->vlru.idx = VLRU_QUEUE_INVALID;
    V_attachFlags(vp) &= ~(VOL_ON_VLRU);
}

/**
 * tell the VLRU subsystem that a volume was just accessed.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre
 *    @arg @c VOL_LOCK is held
 *    @arg caller MUST hold a lightweight ref on @p vp
 *    @arg caller MUST NOT hold exclusive ownership of any VLRU queue
 *
 * @post volume VLRU access statistics are updated.  If the volume was on
 *       the VLRU soft detach candidate queue, it is moved to the NEW
 *       generation queue.
 *
 * @note @c VOL_LOCK may be dropped internally
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VLRU_UpdateAccess_r(Volume * vp)
{
    Volume * rvp = NULL;

    if (!VLRU_enabled)
	return;

    if (queue_IsNotOnQueue(&vp->vlru))
	return;

    assert(V_attachFlags(vp) & VOL_ON_VLRU);

    /* update the access timestamp */
    vp->stats.last_get = FT_ApproxTime();

    /*
     * if the volume is on the soft detach candidate
     * list, we need to safely move it back to a
     * regular generation.  this has to be done
     * carefully so we don't race against the scanner
     * thread.
     */

    /* if this volume is on the soft detach candidate queue,
     * then grab exclusive access to the necessary queues */
    if (vp->vlru.idx == VLRU_QUEUE_CANDIDATE) {
	rvp = vp;
	VCreateReservation_r(rvp);

	VLRU_Wait_r(&volume_LRU.q[VLRU_QUEUE_NEW]);
	VLRU_BeginExclusive_r(&volume_LRU.q[VLRU_QUEUE_NEW]);
	VLRU_Wait_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
	VLRU_BeginExclusive_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
    }

    /* make sure multiple threads don't race to update */
    if (vp->vlru.idx == VLRU_QUEUE_CANDIDATE) {
	VLRU_SwitchQueues(vp, VLRU_QUEUE_NEW, 1);
    }

    if (rvp) {
      VLRU_EndExclusive_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
      VLRU_EndExclusive_r(&volume_LRU.q[VLRU_QUEUE_NEW]);
      VCancelReservation_r(rvp);
    }
}

/**
 * switch a volume between two VLRU queues.
 *
 * @param[in] vp       pointer to volume object
 * @param[in] new_idx  index of VLRU queue onto which the volume will be moved
 * @param[in] append   controls whether the volume will be appended or
 *                     prepended to the queue.  A nonzero value means it will
 *                     be appended; zero means it will be prepended.
 *
 * @pre The new (and old, if applicable) queue(s) must either be owned
 *      exclusively by the calling thread for asynchronous manipulation,
 *      or the queue(s) must be quiescent and VOL_LOCK must be held.
 *      Please see VLRU_BeginExclusive_r, VLRU_EndExclusive_r and VLRU_Wait_r
 *      for further details of the queue asynchronous processing mechanism.
 *
 * @post If the volume object was already on a VLRU queue, it is
 *       removed from the queue.  Depending on the value of the append
 *       parameter, the volume object is either appended or prepended
 *       to the VLRU queue referenced by the new_idx parameter.
 *
 * @note DAFS only
 *
 * @see VLRU_BeginExclusive_r
 * @see VLRU_EndExclusive_r
 * @see VLRU_Wait_r
 *
 * @internal volume package internal use only.
 */
static void
VLRU_SwitchQueues(Volume * vp, int new_idx, int append)
{
    if (queue_IsNotOnQueue(&vp->vlru))
	return;

    queue_Remove(&vp->vlru);
    volume_LRU.q[vp->vlru.idx].len--;

    /* put the volume back on the correct generational queue */
    if (append) {
	queue_Append(&volume_LRU.q[new_idx], &vp->vlru);
    } else {
	queue_Prepend(&volume_LRU.q[new_idx], &vp->vlru);
    }

    volume_LRU.q[new_idx].len++;
    vp->vlru.idx = new_idx;
}

/**
 * VLRU background thread.
 *
 * The VLRU Scanner Thread is responsible for periodically scanning through
 * each VLRU queue looking for volumes which should be moved to another
 * queue, or soft detached.
 *
 * @param[in] args  unused thread arguments parameter
 *
 * @return unused thread return value
 *    @retval NULL always
 *
 * @internal volume package internal use only.
 */
static void *
VLRU_ScannerThread(void * args)
{
    afs_uint32 now, min_delay, delay;
    int i, min_idx, min_op, overdue, state;

    /* set t=0 for promotion cycle to be
     * fileserver startup */
    now = FT_ApproxTime();
    for (i=0; i < VLRU_GENERATIONS-1; i++) {
	volume_LRU.last_promotion[i] = now;
    }

    /* don't start the scanner until VLRU_offline_thresh
     * plus a small delay for VInitVolumePackage2 to finish
     * has gone by */

    sleep(VLRU_offline_thresh + 60);

    /* set t=0 for scan cycle to be now */
    now = FT_ApproxTime();
    for (i=0; i < VLRU_GENERATIONS+1; i++) {
	volume_LRU.last_scan[i] = now;
    }

    VOL_LOCK;
    if (volume_LRU.scanner_state == VLRU_SCANNER_STATE_OFFLINE) {
	volume_LRU.scanner_state = VLRU_SCANNER_STATE_ONLINE;
    }

    while ((state = volume_LRU.scanner_state) != VLRU_SCANNER_STATE_SHUTTING_DOWN) {
	/* check to see if we've been asked to pause */
	if (volume_LRU.scanner_state == VLRU_SCANNER_STATE_PAUSING) {
	    volume_LRU.scanner_state = VLRU_SCANNER_STATE_PAUSED;
	    assert(pthread_cond_broadcast(&volume_LRU.cv) == 0);
	    do {
		VOL_CV_WAIT(&volume_LRU.cv);
	    } while (volume_LRU.scanner_state == VLRU_SCANNER_STATE_PAUSED);
	}

	/* scheduling can happen outside the glock */
	VOL_UNLOCK;

	/* figure out what is next on the schedule */

	/* figure out a potential schedule for the new generation first */
	overdue = 0;
	min_delay = volume_LRU.scan_interval[0] + volume_LRU.last_scan[0] - now;
	min_idx = 0;
	min_op = 0;
	if (min_delay > volume_LRU.scan_interval[0]) {
	    /* unsigned overflow -- we're overdue to run this scan */
	    min_delay = 0;
	    overdue = 1;
	}

	/* if we're not overdue for gen 0, figure out schedule for candidate gen */
	if (!overdue) {
	    i = VLRU_QUEUE_CANDIDATE;
	    delay = volume_LRU.scan_interval[i] + volume_LRU.last_scan[i] - now;
	    if (delay < min_delay) {
		min_delay = delay;
		min_idx = i;
	    }
	    if (delay > volume_LRU.scan_interval[i]) {
		/* unsigned overflow -- we're overdue to run this scan */
		min_delay = 0;
		min_idx = i;
		overdue = 1;
	    }
	}

	/* if we're still not overdue for something, figure out schedules for promotions */
	for (i=0; !overdue && i < VLRU_GENERATIONS-1; i++) {
	    delay = volume_LRU.promotion_interval[i] + volume_LRU.last_promotion[i] - now;
	    if (delay < min_delay) {
		min_delay = delay;
		min_idx = i;
		min_op = 1;
	    }
	    if (delay > volume_LRU.promotion_interval[i]) {
		/* unsigned overflow -- we're overdue to run this promotion */
		min_delay = 0;
		min_idx = i;
		min_op = 1;
		overdue = 1;
		break;
	    }
	}

	/* sleep as needed */
	if (min_delay) {
	    sleep(min_delay);
	}

	/* do whatever is next */
	VOL_LOCK;
	if (min_op) {
	    VLRU_Promote_r(min_idx);
	    VLRU_Demote_r(min_idx+1);
	} else {
	    VLRU_Scan_r(min_idx);
	}
	now = FT_ApproxTime();
    }

    Log("VLRU scanner asked to go offline (scanner_state=%d)\n", state);

    /* signal that scanner is down */
    volume_LRU.scanner_state = VLRU_SCANNER_STATE_OFFLINE;
    assert(pthread_cond_broadcast(&volume_LRU.cv) == 0);
    VOL_UNLOCK;
    return NULL;
}

/**
 * promote volumes from one VLRU generation to the next.
 *
 * This routine scans a VLRU generation looking for volumes which are
 * eligible to be promoted to the next generation.  All volumes which
 * meet the eligibility requirement are promoted.
 *
 * Promotion eligibility is based upon meeting both of the following
 * requirements:
 *
 *    @arg The volume has been accessed since the last promotion:
 *         @c (vp->stats.last_get >= vp->stats.last_promote)
 *    @arg The last promotion occurred at least
 *         @c volume_LRU.promotion_interval[idx] seconds ago
 *
 * As a performance optimization, promotions are "globbed".  In other
 * words, we promote arbitrarily large contiguous sublists of elements
 * as one operation.
 *
 * @param[in] idx  VLRU queue index to scan
 *
 * @note DAFS only
 *
 * @internal VLRU internal use only.
 */
static void
VLRU_Promote_r(int idx)
{
    int len, chaining, promote;
    afs_uint32 now, thresh;
    struct rx_queue *qp, *nqp;
    Volume * vp, *start = NULL, *end = NULL;

    /* get exclusive access to two chains, and drop the glock */
    VLRU_Wait_r(&volume_LRU.q[idx]);
    VLRU_BeginExclusive_r(&volume_LRU.q[idx]);
    VLRU_Wait_r(&volume_LRU.q[idx+1]);
    VLRU_BeginExclusive_r(&volume_LRU.q[idx+1]);
    VOL_UNLOCK;

    thresh = volume_LRU.promotion_interval[idx];
    now = FT_ApproxTime();

    len = chaining = 0;
    for (queue_ScanBackwards(&volume_LRU.q[idx], qp, nqp, rx_queue)) {
	vp = (Volume *)((char *)qp - offsetof(Volume, vlru));
	promote = (((vp->stats.last_promote + thresh) <= now) &&
		   (vp->stats.last_get >= vp->stats.last_promote));

	if (chaining) {
	    if (promote) {
		vp->vlru.idx++;
		len++;
		start = vp;
	    } else {
		/* promote and prepend chain */
		queue_MoveChainAfter(&volume_LRU.q[idx+1], &start->vlru, &end->vlru);
		chaining = 0;
	    }
	} else {
	    if (promote) {
		vp->vlru.idx++;
		len++;
		chaining = 1;
		start = end = vp;
	    }
	}
    }

    if (chaining) {
	/* promote and prepend */
	queue_MoveChainAfter(&volume_LRU.q[idx+1], &start->vlru, &end->vlru);
    }

    if (len) {
	volume_LRU.q[idx].len -= len;
	volume_LRU.q[idx+1].len += len;
    }

    /* release exclusive access to the two chains */
    VOL_LOCK;
    volume_LRU.last_promotion[idx] = now;
    VLRU_EndExclusive_r(&volume_LRU.q[idx+1]);
    VLRU_EndExclusive_r(&volume_LRU.q[idx]);
}

/* run the demotions */
static void
VLRU_Demote_r(int idx)
{
    Error ec;
    int len, chaining, demote;
    afs_uint32 now, thresh;
    struct rx_queue *qp, *nqp;
    Volume * vp, *start = NULL, *end = NULL;
    Volume ** salv_flag_vec = NULL;
    int salv_vec_offset = 0;

    assert(idx == VLRU_QUEUE_MID || idx == VLRU_QUEUE_OLD);

    /* get exclusive access to two chains, and drop the glock */
    VLRU_Wait_r(&volume_LRU.q[idx-1]);
    VLRU_BeginExclusive_r(&volume_LRU.q[idx-1]);
    VLRU_Wait_r(&volume_LRU.q[idx]);
    VLRU_BeginExclusive_r(&volume_LRU.q[idx]);
    VOL_UNLOCK;

    /* no big deal if this allocation fails */
    if (volume_LRU.q[idx].len) {
	salv_flag_vec = (Volume **) malloc(volume_LRU.q[idx].len * sizeof(Volume *));
    }

    now = FT_ApproxTime();
    thresh = volume_LRU.promotion_interval[idx-1];

    len = chaining = 0;
    for (queue_ScanBackwards(&volume_LRU.q[idx], qp, nqp, rx_queue)) {
	vp = (Volume *)((char *)qp - offsetof(Volume, vlru));
	demote = (((vp->stats.last_promote + thresh) <= now) &&
		  (vp->stats.last_get < (now - thresh)));

	/* we now do volume update list DONT_SALVAGE flag setting during
	 * demotion passes */
	if (salv_flag_vec &&
	    !(V_attachFlags(vp) & VOL_HDR_DONTSALV) &&
	    demote &&
	    (vp->updateTime < (now - SALVAGE_INTERVAL)) &&
	    (V_attachState(vp) == VOL_STATE_ATTACHED)) {
	    salv_flag_vec[salv_vec_offset++] = vp;
	    VCreateReservation_r(vp);
	}

	if (chaining) {
	    if (demote) {
		vp->vlru.idx--;
		len++;
		start = vp;
	    } else {
		/* demote and append chain */
		queue_MoveChainBefore(&volume_LRU.q[idx-1], &start->vlru, &end->vlru);
		chaining = 0;
	    }
	} else {
	    if (demote) {
		vp->vlru.idx--;
		len++;
		chaining = 1;
		start = end = vp;
	    }
	}
    }

    if (chaining) {
	queue_MoveChainBefore(&volume_LRU.q[idx-1], &start->vlru, &end->vlru);
    }

    if (len) {
	volume_LRU.q[idx].len -= len;
	volume_LRU.q[idx-1].len += len;
    }

    /* release exclusive access to the two chains */
    VOL_LOCK;
    VLRU_EndExclusive_r(&volume_LRU.q[idx]);
    VLRU_EndExclusive_r(&volume_LRU.q[idx-1]);

    /* now go back and set the DONT_SALVAGE flags as appropriate */
    if (salv_flag_vec) {
	int i;
	for (i = 0; i < salv_vec_offset; i++) {
	    vp = salv_flag_vec[i];
	    if (!(V_attachFlags(vp) & VOL_HDR_DONTSALV) &&
		(vp->updateTime < (now - SALVAGE_INTERVAL)) &&
		(V_attachState(vp) == VOL_STATE_ATTACHED)) {
		ec = VHold_r(vp);
		if (!ec) {
		    V_attachFlags(vp) |= VOL_HDR_DONTSALV;
		    V_dontSalvage(vp) = DONT_SALVAGE;
		    VUpdateVolume_r(&ec, vp, 0);
		    VPutVolume_r(vp);
		}
	    }
	    VCancelReservation_r(vp);
	}
	free(salv_flag_vec);
    }
}

/* run a pass of the VLRU GC scanner */
static void
VLRU_Scan_r(int idx)
{
    afs_uint32 now, thresh;
    struct rx_queue *qp, *nqp;
    Volume * vp;
    int i, locked = 1;

    assert(idx == VLRU_QUEUE_NEW || idx == VLRU_QUEUE_CANDIDATE);

    /* gain exclusive access to the idx VLRU */
    VLRU_Wait_r(&volume_LRU.q[idx]);
    VLRU_BeginExclusive_r(&volume_LRU.q[idx]);

    if (idx != VLRU_QUEUE_CANDIDATE) {
	/* gain exclusive access to the candidate VLRU */
	VLRU_Wait_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
	VLRU_BeginExclusive_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
    }

    now = FT_ApproxTime();
    thresh = now - VLRU_offline_thresh;

    /* perform candidate selection and soft detaching */
    if (idx == VLRU_QUEUE_CANDIDATE) {
	/* soft detach some volumes from the candidate pool */
	VOL_UNLOCK;
	locked = 0;

	for (i=0,queue_ScanBackwards(&volume_LRU.q[idx], qp, nqp, rx_queue)) {
	    vp = (Volume *)((char *)qp - offsetof(Volume, vlru));
	    if (i >= VLRU_offline_max) {
		break;
	    }
	    /* check timestamp to see if it's a candidate for soft detaching */
	    if (vp->stats.last_get <= thresh) {
		VOL_LOCK;
		if (VCheckSoftDetach(vp, thresh))
		    i++;
		VOL_UNLOCK;
	    }
	}
    } else {
	/* scan for volumes to become soft detach candidates */
	for (i=1,queue_ScanBackwards(&volume_LRU.q[idx], qp, nqp, rx_queue),i++) {
	    vp = (Volume *)((char *)qp - offsetof(Volume, vlru));

	    /* check timestamp to see if it's a candidate for soft detaching */
	    if (vp->stats.last_get <= thresh) {
		VCheckSoftDetachCandidate(vp, thresh);
	    }

	    if (!(i&0x7f)) {   /* lock coarsening optimization */
		VOL_UNLOCK;
		pthread_yield();
		VOL_LOCK;
	    }
	}
    }

    /* relinquish exclusive access to the VLRU chains */
    if (!locked) {
	VOL_LOCK;
    }
    volume_LRU.last_scan[idx] = now;
    if (idx != VLRU_QUEUE_CANDIDATE) {
	VLRU_EndExclusive_r(&volume_LRU.q[VLRU_QUEUE_CANDIDATE]);
    }
    VLRU_EndExclusive_r(&volume_LRU.q[idx]);
}

/* check whether volume is safe to soft detach
 * caller MUST NOT hold a ref count on vp */
static int
VCheckSoftDetach(Volume * vp, afs_uint32 thresh)
{
    int ret=0;

    if (vp->nUsers || vp->nWaiters)
	return 0;

    if (vp->stats.last_get <= thresh) {
	ret = VSoftDetachVolume_r(vp, thresh);
    }

    return ret;
}

/* check whether volume should be made a
 * soft detach candidate */
static int
VCheckSoftDetachCandidate(Volume * vp, afs_uint32 thresh)
{
    int idx, ret = 0;
    if (vp->nUsers || vp->nWaiters)
	return 0;

    idx = vp->vlru.idx;

    assert(idx == VLRU_QUEUE_NEW);

    if (vp->stats.last_get <= thresh) {
	/* move to candidate pool */
	queue_Remove(&vp->vlru);
	volume_LRU.q[VLRU_QUEUE_NEW].len--;
	queue_Prepend(&volume_LRU.q[VLRU_QUEUE_CANDIDATE], &vp->vlru);
	vp->vlru.idx = VLRU_QUEUE_CANDIDATE;
	volume_LRU.q[VLRU_QUEUE_CANDIDATE].len++;
	ret = 1;
    }

    return ret;
}


/* begin exclusive access on VLRU */
static void
VLRU_BeginExclusive_r(struct VLRU_q * q)
{
    assert(q->busy == 0);
    q->busy = 1;
}

/* end exclusive access on VLRU */
static void
VLRU_EndExclusive_r(struct VLRU_q * q)
{
    assert(q->busy);
    q->busy = 0;
    assert(pthread_cond_broadcast(&q->cv) == 0);
}

/* wait for another thread to end exclusive access on VLRU */
static void
VLRU_Wait_r(struct VLRU_q * q)
{
    while(q->busy) {
	VOL_CV_WAIT(&q->cv);
    }
}

/* demand attach fs
 * volume soft detach
 *
 * caller MUST NOT hold a ref count on vp */
static int
VSoftDetachVolume_r(Volume * vp, afs_uint32 thresh)
{
    afs_uint32 ts_save;
    int ret = 0;

    assert(vp->vlru.idx == VLRU_QUEUE_CANDIDATE);

    ts_save = vp->stats.last_get;
    if (ts_save > thresh)
	return 0;

    if (vp->nUsers || vp->nWaiters)
	return 0;

    if (VIsExclusiveState(V_attachState(vp))) {
	return 0;
    }

    switch (V_attachState(vp)) {
    case VOL_STATE_UNATTACHED:
    case VOL_STATE_PREATTACHED:
    case VOL_STATE_ERROR:
    case VOL_STATE_GOING_OFFLINE:
    case VOL_STATE_SHUTTING_DOWN:
    case VOL_STATE_SALVAGING:
    case VOL_STATE_DELETED:
	volume_LRU.q[vp->vlru.idx].len--;

	/* create and cancel a reservation to
	 * give the volume an opportunity to
	 * be deallocated */
	VCreateReservation_r(vp);
	queue_Remove(&vp->vlru);
	vp->vlru.idx = VLRU_QUEUE_INVALID;
	V_attachFlags(vp) &= ~(VOL_ON_VLRU);
	VCancelReservation_r(vp);
	return 0;
    default:
	break;
    }

    /* hold the volume and take it offline.
     * no need for reservations, as VHold_r
     * takes care of that internally. */
    if (VHold_r(vp) == 0) {
	/* vhold drops the glock, so now we should
	 * check to make sure we aren't racing against
	 * other threads.  if we are racing, offlining vp
	 * would be wasteful, and block the scanner for a while
	 */
	if (vp->nWaiters ||
	    (vp->nUsers > 1) ||
	    (vp->shuttingDown) ||
	    (vp->goingOffline) ||
	    (vp->stats.last_get != ts_save)) {
	    /* looks like we're racing someone else. bail */
	    VPutVolume_r(vp);
	    vp = NULL;
	} else {
	    /* pull it off the VLRU */
	    assert(vp->vlru.idx == VLRU_QUEUE_CANDIDATE);
	    volume_LRU.q[VLRU_QUEUE_CANDIDATE].len--;
	    queue_Remove(&vp->vlru);
	    vp->vlru.idx = VLRU_QUEUE_INVALID;
	    V_attachFlags(vp) &= ~(VOL_ON_VLRU);

	    /* take if offline */
	    VOffline_r(vp, "volume has been soft detached");

	    /* invalidate the volume header cache */
	    FreeVolumeHeader(vp);

	    /* update stats */
	    IncUInt64(&VStats.soft_detaches);
	    vp->stats.soft_detaches++;

	    /* put in pre-attached state so demand
	     * attacher can work on it */
	    VChangeState_r(vp, VOL_STATE_PREATTACHED);
	    ret = 1;
	}
    }
    return ret;
}
#endif /* AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* Volume Header Cache routines                    */
/***************************************************/

/**
 * volume header cache.
 */
struct volume_hdr_LRU_t volume_hdr_LRU;

/**
 * initialize the volume header cache.
 *
 * @param[in] howMany  number of header cache entries to preallocate
 *
 * @pre VOL_LOCK held.  Function has never been called before.
 *
 * @post howMany cache entries are allocated, initialized, and added
 *       to the LRU list.  Header cache statistics are initialized.
 *
 * @note only applicable to fileServer program type.  Should only be
 *       called once during volume package initialization.
 *
 * @internal volume package internal use only.
 */
static void
VInitVolumeHeaderCache(afs_uint32 howMany)
{
    struct volHeader *hp;
    if (programType != fileServer)
	return;
    queue_Init(&volume_hdr_LRU);
    volume_hdr_LRU.stats.free = 0;
    volume_hdr_LRU.stats.used = howMany;
    volume_hdr_LRU.stats.attached = 0;
    hp = (struct volHeader *)(calloc(howMany, sizeof(struct volHeader)));
    assert(hp != NULL);

    while (howMany--)
	/* We are using ReleaseVolumeHeader to initialize the values on the header list
 	 * to ensure they have the right values
 	 */
	ReleaseVolumeHeader(hp++);
}

/**
 * get a volume header and attach it to the volume object.
 *
 * @param[in] vp  pointer to volume object
 *
 * @return cache entry status
 *    @retval 0  volume header was newly attached; cache data is invalid
 *    @retval 1  volume header was previously attached; cache data is valid
 *
 * @pre VOL_LOCK held.  For DAFS, lightweight ref must be held on volume object.
 *
 * @post volume header attached to volume object.  if necessary, header cache
 *       entry on LRU is synchronized to disk.  Header is removed from LRU list.
 *
 * @note VOL_LOCK may be dropped
 *
 * @warning this interface does not load header data from disk.  it merely
 *          attaches a header object to the volume object, and may sync the old
 *          header cache data out to disk in the process.
 *
 * @internal volume package internal use only.
 */
static int
GetVolumeHeader(Volume * vp)
{
    Error error;
    struct volHeader *hd;
    int old;
    static int everLogged = 0;

#ifdef AFS_DEMAND_ATTACH_FS
    VolState vp_save = 0, back_save = 0;

    /* XXX debug 9/19/05 we've apparently got
     * a ref counting bug somewhere that's
     * breaking the nUsers == 0 => header on LRU
     * assumption */
    if (vp->header && queue_IsNotOnQueue(vp->header)) {
	Log("nUsers == 0, but header not on LRU\n");
	return 1;
    }
#endif

    old = (vp->header != NULL);	/* old == volume already has a header */

    if (programType != fileServer) {
	/* for volume utilities, we allocate volHeaders as needed */
	if (!vp->header) {
	    hd = (struct volHeader *)calloc(1, sizeof(*vp->header));
	    assert(hd != NULL);
	    vp->header = hd;
	    hd->back = vp;
#ifdef AFS_DEMAND_ATTACH_FS
	    V_attachFlags(vp) |= VOL_HDR_ATTACHED;
#endif
	}
    } else {
	/* for the fileserver, we keep a volume header cache */
	if (old) {
	    /* the header we previously dropped in the lru is
	     * still available. pull it off the lru and return */
	    hd = vp->header;
	    queue_Remove(hd);
	    assert(hd->back == vp);
#ifdef AFS_DEMAND_ATTACH_FS
            V_attachFlags(vp) &= ~(VOL_HDR_IN_LRU);
#endif
	} else {
	    /* we need to grab a new element off the LRU */
	    if (queue_IsNotEmpty(&volume_hdr_LRU)) {
		/* grab an element and pull off of LRU */
		hd = queue_First(&volume_hdr_LRU, volHeader);
		queue_Remove(hd);
	    } else {
		/* LRU is empty, so allocate a new volHeader
		 * this is probably indicative of a leak, so let the user know */
		hd = (struct volHeader *)calloc(1, sizeof(struct volHeader));
		assert(hd != NULL);
		if (!everLogged) {
		    Log("****Allocated more volume headers, probably leak****\n");
		    everLogged = 1;
		}
		volume_hdr_LRU.stats.free++;
	    }
	    if (hd->back) {
		/* this header used to belong to someone else.
		 * we'll need to check if the header needs to
		 * be sync'd out to disk */

#ifdef AFS_DEMAND_ATTACH_FS
		/* if hd->back were in an exclusive state, then
		 * its volHeader would not be on the LRU... */
		assert(!VIsExclusiveState(V_attachState(hd->back)));
#endif

		if (hd->diskstuff.inUse) {
		    /* volume was in use, so we'll need to sync
		     * its header to disk */

#ifdef AFS_DEMAND_ATTACH_FS
		    back_save = VChangeState_r(hd->back, VOL_STATE_UPDATING);
		    vp_save = VChangeState_r(vp, VOL_STATE_HDR_ATTACHING);
		    VCreateReservation_r(hd->back);
		    VOL_UNLOCK;
#endif

		    WriteVolumeHeader_r(&error, hd->back);
		    /* Ignore errors; catch them later */

#ifdef AFS_DEMAND_ATTACH_FS
		    VOL_LOCK;
#endif
		}

		hd->back->header = NULL;
#ifdef AFS_DEMAND_ATTACH_FS
		V_attachFlags(hd->back) &= ~(VOL_HDR_ATTACHED | VOL_HDR_LOADED | VOL_HDR_IN_LRU);

		if (hd->diskstuff.inUse) {
		    VChangeState_r(hd->back, back_save);
		    VCancelReservation_r(hd->back);
		    VChangeState_r(vp, vp_save);
		}
#endif
	    } else {
		volume_hdr_LRU.stats.attached++;
	    }
	    hd->back = vp;
	    vp->header = hd;
#ifdef AFS_DEMAND_ATTACH_FS
	    V_attachFlags(vp) |= VOL_HDR_ATTACHED;
#endif
	}
	volume_hdr_LRU.stats.free--;
	volume_hdr_LRU.stats.used++;
    }
    IncUInt64(&VStats.hdr_gets);
#ifdef AFS_DEMAND_ATTACH_FS
    IncUInt64(&vp->stats.hdr_gets);
    vp->stats.last_hdr_get = FT_ApproxTime();
#endif
    return old;
}


/**
 * make sure volume header is attached and contains valid cache data.
 *
 * @param[out] ec  outbound error code
 * @param[in]  vp  pointer to volume object
 *
 * @pre VOL_LOCK held.  For DAFS, lightweight ref held on vp.
 *
 * @post header cache entry attached, and loaded with valid data, or
 *       *ec is nonzero, and the header is released back into the LRU.
 *
 * @internal volume package internal use only.
 */
static void
LoadVolumeHeader(Error * ec, Volume * vp)
{
#ifdef AFS_DEMAND_ATTACH_FS
    VolState state_save;
    afs_uint32 now;
    *ec = 0;

    if (vp->nUsers == 0 && !GetVolumeHeader(vp)) {
	IncUInt64(&VStats.hdr_loads);
	state_save = VChangeState_r(vp, VOL_STATE_HDR_LOADING);
	VOL_UNLOCK;

	ReadHeader(ec, V_diskDataHandle(vp), (char *)&V_disk(vp),
		   sizeof(V_disk(vp)), VOLUMEINFOMAGIC,
		   VOLUMEINFOVERSION);
	IncUInt64(&vp->stats.hdr_loads);
	now = FT_ApproxTime();

	VOL_LOCK;
	if (!*ec) {
	    V_attachFlags(vp) |= VOL_HDR_LOADED;
	    vp->stats.last_hdr_load = now;
	}
	VChangeState_r(vp, state_save);
    }
#else /* AFS_DEMAND_ATTACH_FS */
    *ec = 0;
    if (vp->nUsers == 0 && !GetVolumeHeader(vp)) {
	IncUInt64(&VStats.hdr_loads);

	ReadHeader(ec, V_diskDataHandle(vp), (char *)&V_disk(vp),
		   sizeof(V_disk(vp)), VOLUMEINFOMAGIC,
		   VOLUMEINFOVERSION);
    }
#endif /* AFS_DEMAND_ATTACH_FS */
    if (*ec) {
	/* maintain (nUsers==0) => header in LRU invariant */
	FreeVolumeHeader(vp);
    }
}

/**
 * release a header cache entry back into the LRU list.
 *
 * @param[in] hd  pointer to volume header cache object
 *
 * @pre VOL_LOCK held.
 *
 * @post header cache object appended onto end of LRU list.
 *
 * @note only applicable to fileServer program type.
 *
 * @note used to place a header cache entry back into the
 *       LRU pool without invalidating it as a cache entry.
 *
 * @internal volume package internal use only.
 */
static void
ReleaseVolumeHeader(struct volHeader *hd)
{
    if (programType != fileServer)
	return;
    if (!hd || queue_IsOnQueue(hd))	/* no header, or header already released */
	return;
    queue_Append(&volume_hdr_LRU, hd);
#ifdef AFS_DEMAND_ATTACH_FS
    if (hd->back) {
	V_attachFlags(hd->back) |= VOL_HDR_IN_LRU;
    }
#endif
    volume_hdr_LRU.stats.free++;
    volume_hdr_LRU.stats.used--;
}

/**
 * free/invalidate a volume header cache entry.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre VOL_LOCK is held.
 *
 * @post For fileserver, header cache entry is returned to LRU, and it is
 *       invalidated as a cache entry.  For volume utilities, the header
 *       cache entry is freed.
 *
 * @note For fileserver, this should be utilized instead of ReleaseVolumeHeader
 *       whenever it is necessary to invalidate the header cache entry.
 *
 * @see ReleaseVolumeHeader
 *
 * @internal volume package internal use only.
 */
static void
FreeVolumeHeader(Volume * vp)
{
    struct volHeader *hd = vp->header;
    if (!hd)
	return;
    if (programType == fileServer) {
	ReleaseVolumeHeader(hd);
	hd->back = NULL;
    } else {
	free(hd);
    }
#ifdef AFS_DEMAND_ATTACH_FS
    V_attachFlags(vp) &= ~(VOL_HDR_ATTACHED | VOL_HDR_IN_LRU | VOL_HDR_LOADED);
#endif
    volume_hdr_LRU.stats.attached--;
    vp->header = NULL;
}


/***************************************************/
/* Volume Hash Table routines                      */
/***************************************************/

/**
 * set size of volume object hash table.
 *
 * @param[in] logsize   log(2) of desired hash table size
 *
 * @return operation status
 *    @retval 0 success
 *    @retval -1 failure
 *
 * @pre MUST be called prior to VInitVolumePackage2
 *
 * @post Volume Hash Table will have 2^logsize buckets
 */
int
VSetVolHashSize(int logsize)
{
    /* 64 to 16384 hash buckets seems like a reasonable range */
    if ((logsize < 6 ) || (logsize > 14)) {
        return -1;
    }

    if (!VInit) {
        VolumeHashTable.Size = 1 << logsize;
        VolumeHashTable.Mask = VolumeHashTable.Size - 1;
    } else {
	/* we can't yet support runtime modification of this
	 * parameter. we'll need a configuration rwlock to
	 * make runtime modification feasible.... */
	return -1;
    }
    return 0;
}

/**
 * initialize dynamic data structures for volume hash table.
 *
 * @post hash table is allocated, and fields are initialized.
 *
 * @internal volume package internal use only.
 */
static void
VInitVolumeHash(void)
{
    int i;

    VolumeHashTable.Table = (VolumeHashChainHead *) calloc(VolumeHashTable.Size,
							   sizeof(VolumeHashChainHead));
    assert(VolumeHashTable.Table != NULL);

    for (i=0; i < VolumeHashTable.Size; i++) {
	queue_Init(&VolumeHashTable.Table[i]);
#ifdef AFS_DEMAND_ATTACH_FS
	assert(pthread_cond_init(&VolumeHashTable.Table[i].chain_busy_cv, NULL) == 0);
#endif /* AFS_DEMAND_ATTACH_FS */
    }
}

/**
 * add a volume object to the hash table.
 *
 * @param[in] vp      pointer to volume object
 * @param[in] hashid  hash of volume id
 *
 * @pre VOL_LOCK is held.  For DAFS, caller must hold a lightweight
 *      reference on vp.
 *
 * @post volume is added to hash chain.
 *
 * @internal volume package internal use only.
 *
 * @note For DAFS, VOL_LOCK may be dropped in order to wait for an
 *       asynchronous hash chain reordering to finish.
 */
static void
AddVolumeToHashTable(Volume * vp, int hashid)
{
    VolumeHashChainHead * head;

    if (queue_IsOnQueue(vp))
	return;

    head = &VolumeHashTable.Table[VOLUME_HASH(hashid)];

#ifdef AFS_DEMAND_ATTACH_FS
    /* wait for the hash chain to become available */
    VHashWait_r(head);

    V_attachFlags(vp) |= VOL_IN_HASH;
    vp->chainCacheCheck = ++head->cacheCheck;
#endif /* AFS_DEMAND_ATTACH_FS */

    head->len++;
    vp->hashid = hashid;
    queue_Append(head, vp);
    vp->vnodeHashOffset = VolumeHashOffset_r();
}

/**
 * delete a volume object from the hash table.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre VOL_LOCK is held.  For DAFS, caller must hold a lightweight
 *      reference on vp.
 *
 * @post volume is removed from hash chain.
 *
 * @internal volume package internal use only.
 *
 * @note For DAFS, VOL_LOCK may be dropped in order to wait for an
 *       asynchronous hash chain reordering to finish.
 */
static void
DeleteVolumeFromHashTable(Volume * vp)
{
    VolumeHashChainHead * head;

    if (!queue_IsOnQueue(vp))
	return;

    head = &VolumeHashTable.Table[VOLUME_HASH(vp->hashid)];

#ifdef AFS_DEMAND_ATTACH_FS
    /* wait for the hash chain to become available */
    VHashWait_r(head);

    V_attachFlags(vp) &= ~(VOL_IN_HASH);
    head->cacheCheck++;
#endif /* AFS_DEMAND_ATTACH_FS */

    head->len--;
    queue_Remove(vp);
    /* do NOT reset hashid to zero, as the online
     * salvager package may need to know the volume id
     * after the volume is removed from the hash */
}

/**
 * lookup a volume object in the hash table given a volume id.
 *
 * @param[out] ec        error code return
 * @param[in]  volumeId  volume id
 * @param[in]  hint      volume object which we believe could be the correct
                         mapping
 *
 * @return volume object pointer
 *    @retval NULL  no such volume id is registered with the hash table.
 *
 * @pre VOL_LOCK is held.  For DAFS, caller must hold a lightweight
        ref on hint.
 *
 * @post volume object with the given id is returned.  volume object and
 *       hash chain access statistics are updated.  hash chain may have
 *       been reordered.
 *
 * @note For DAFS, VOL_LOCK may be dropped in order to wait for an
 *       asynchronous hash chain reordering operation to finish, or
 *       in order for us to perform an asynchronous chain reordering.
 *
 * @note Hash chain reorderings occur when the access count for the
 *       volume object being looked up exceeds the sum of the previous
 *       node's (the node ahead of it in the hash chain linked list)
 *       access count plus the constant VOLUME_HASH_REORDER_THRESHOLD.
 *
 * @note For DAFS, the hint parameter allows us to short-circuit if the
 *       cacheCheck fields match between the hash chain head and the
 *       hint volume object.
 */
Volume *
VLookupVolume_r(Error * ec, VolId volumeId, Volume * hint)
{
    int looks = 0;
    Volume * vp, *np;
#ifdef AFS_DEMAND_ATTACH_FS
    Volume *pp;
#endif
    VolumeHashChainHead * head;
    *ec = 0;

    head = &VolumeHashTable.Table[VOLUME_HASH(volumeId)];

#ifdef AFS_DEMAND_ATTACH_FS
    /* wait for the hash chain to become available */
    VHashWait_r(head);

    /* check to see if we can short circuit without walking the hash chain */
    if (hint && (hint->chainCacheCheck == head->cacheCheck)) {
	IncUInt64(&hint->stats.hash_short_circuits);
	return hint;
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    /* someday we need to either do per-chain locks, RWlocks,
     * or both for volhash access.
     * (and move to a data structure with better cache locality) */

    /* search the chain for this volume id */
    for(queue_Scan(head, vp, np, Volume)) {
	looks++;
	if ((vp->hashid == volumeId)) {
	    break;
	}
    }

    if (queue_IsEnd(head, vp)) {
	vp = NULL;
    }

#ifdef AFS_DEMAND_ATTACH_FS
    /* update hash chain statistics */
    {
	afs_uint64 lks;
	FillInt64(lks, 0, looks);
	AddUInt64(head->looks, lks, &head->looks);
	AddUInt64(VStats.hash_looks, lks, &VStats.hash_looks);
	IncUInt64(&head->gets);
    }

    if (vp) {
	afs_uint64 thresh;
	IncUInt64(&vp->stats.hash_lookups);

	/* for demand attach fileserver, we permit occasional hash chain reordering
	 * so that frequently looked up volumes move towards the head of the chain */
	pp = queue_Prev(vp, Volume);
	if (!queue_IsEnd(head, pp)) {
	    FillInt64(thresh, 0, VOLUME_HASH_REORDER_THRESHOLD);
	    AddUInt64(thresh, pp->stats.hash_lookups, &thresh);
	    if (GEInt64(vp->stats.hash_lookups, thresh)) {
		VReorderHash_r(head, pp, vp);
	    }
	}

	/* update the short-circuit cache check */
	vp->chainCacheCheck = head->cacheCheck;
    }
#endif /* AFS_DEMAND_ATTACH_FS */

    return vp;
}

#ifdef AFS_DEMAND_ATTACH_FS
/* perform volume hash chain reordering.
 *
 * advance a subchain beginning at vp ahead of
 * the adjacent subchain ending at pp */
static void
VReorderHash_r(VolumeHashChainHead * head, Volume * pp, Volume * vp)
{
    Volume *tp, *np, *lp;
    afs_uint64 move_thresh;

    /* this should never be called if the chain is already busy, so
     * no need to wait for other exclusive chain ops to finish */

    /* this is a rather heavy set of operations,
     * so let's set the chain busy flag and drop
     * the vol_glock */
    VHashBeginExclusive_r(head);
    VOL_UNLOCK;

    /* scan forward in the chain from vp looking for the last element
     * in the chain we want to advance */
    FillInt64(move_thresh, 0, VOLUME_HASH_REORDER_CHAIN_THRESH);
    AddUInt64(move_thresh, pp->stats.hash_lookups, &move_thresh);
    for(queue_ScanFrom(head, vp, tp, np, Volume)) {
	if (LTInt64(tp->stats.hash_lookups, move_thresh)) {
	    break;
	}
    }
    lp = queue_Prev(tp, Volume);

    /* scan backwards from pp to determine where to splice and
     * insert the subchain we're advancing */
    for(queue_ScanBackwardsFrom(head, pp, tp, np, Volume)) {
	if (GTInt64(tp->stats.hash_lookups, move_thresh)) {
	    break;
	}
    }
    tp = queue_Next(tp, Volume);

    /* rebalance chain(vp,...,lp) ahead of chain(tp,...,pp) */
    queue_MoveChainBefore(tp,vp,lp);

    VOL_LOCK;
    IncUInt64(&VStats.hash_reorders);
    head->cacheCheck++;
    IncUInt64(&head->reorders);

    /* wake up any threads waiting for the hash chain */
    VHashEndExclusive_r(head);
}


/* demand-attach fs volume hash
 * asynchronous exclusive operations */

/**
 * begin an asynchronous exclusive operation on a volume hash chain.
 *
 * @param[in] head   pointer to volume hash chain head object
 *
 * @pre VOL_LOCK held.  hash chain is quiescent.
 *
 * @post hash chain marked busy.
 *
 * @note this interface is used in conjunction with VHashEndExclusive_r and
 *       VHashWait_r to perform asynchronous (wrt VOL_LOCK) operations on a
 *       volume hash chain.  Its main use case is hash chain reordering, which
 *       has the potential to be a highly latent operation.
 *
 * @see VHashEndExclusive_r
 * @see VHashWait_r
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VHashBeginExclusive_r(VolumeHashChainHead * head)
{
    assert(head->busy == 0);
    head->busy = 1;
}

/**
 * relinquish exclusive ownership of a volume hash chain.
 *
 * @param[in] head   pointer to volume hash chain head object
 *
 * @pre VOL_LOCK held.  thread owns the hash chain exclusively.
 *
 * @post hash chain is marked quiescent.  threads awaiting use of
 *       chain are awakened.
 *
 * @see VHashBeginExclusive_r
 * @see VHashWait_r
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VHashEndExclusive_r(VolumeHashChainHead * head)
{
    assert(head->busy);
    head->busy = 0;
    assert(pthread_cond_broadcast(&head->chain_busy_cv) == 0);
}

/**
 * wait for all asynchronous operations on a hash chain to complete.
 *
 * @param[in] head   pointer to volume hash chain head object
 *
 * @pre VOL_LOCK held.
 *
 * @post hash chain object is quiescent.
 *
 * @see VHashBeginExclusive_r
 * @see VHashEndExclusive_r
 *
 * @note DAFS only
 *
 * @note This interface should be called before any attempt to
 *       traverse the hash chain.  It is permissible for a thread
 *       to gain exclusive access to the chain, and then perform
 *       latent operations on the chain asynchronously wrt the
 *       VOL_LOCK.
 *
 * @warning if waiting is necessary, VOL_LOCK is dropped
 *
 * @internal volume package internal use only.
 */
static void
VHashWait_r(VolumeHashChainHead * head)
{
    while (head->busy) {
	VOL_CV_WAIT(&head->chain_busy_cv);
    }
}
#endif /* AFS_DEMAND_ATTACH_FS */


/***************************************************/
/* Volume by Partition List routines               */
/***************************************************/

/*
 * demand attach fileserver adds a
 * linked list of volumes to each
 * partition object, thus allowing
 * for quick enumeration of all
 * volumes on a partition
 */

#ifdef AFS_DEMAND_ATTACH_FS
/**
 * add a volume to its disk partition VByPList.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre either the disk partition VByPList is owned exclusively
 *      by the calling thread, or the list is quiescent and
 *      VOL_LOCK is held.
 *
 * @post volume is added to disk partition VByPList
 *
 * @note DAFS only
 *
 * @warning it is the caller's responsibility to ensure list
 *          quiescence.
 *
 * @see VVByPListWait_r
 * @see VVByPListBeginExclusive_r
 * @see VVByPListEndExclusive_r
 *
 * @internal volume package internal use only.
 */
static void
AddVolumeToVByPList_r(Volume * vp)
{
    if (queue_IsNotOnQueue(&vp->vol_list)) {
	queue_Append(&vp->partition->vol_list, &vp->vol_list);
	V_attachFlags(vp) |= VOL_ON_VBYP_LIST;
	vp->partition->vol_list.len++;
    }
}

/**
 * delete a volume from its disk partition VByPList.
 *
 * @param[in] vp  pointer to volume object
 *
 * @pre either the disk partition VByPList is owned exclusively
 *      by the calling thread, or the list is quiescent and
 *      VOL_LOCK is held.
 *
 * @post volume is removed from the disk partition VByPList
 *
 * @note DAFS only
 *
 * @warning it is the caller's responsibility to ensure list
 *          quiescence.
 *
 * @see VVByPListWait_r
 * @see VVByPListBeginExclusive_r
 * @see VVByPListEndExclusive_r
 *
 * @internal volume package internal use only.
 */
static void
DeleteVolumeFromVByPList_r(Volume * vp)
{
    if (queue_IsOnQueue(&vp->vol_list)) {
	queue_Remove(&vp->vol_list);
	V_attachFlags(vp) &= ~(VOL_ON_VBYP_LIST);
	vp->partition->vol_list.len--;
    }
}

/**
 * begin an asynchronous exclusive operation on a VByPList.
 *
 * @param[in] dp   pointer to disk partition object
 *
 * @pre VOL_LOCK held.  VByPList is quiescent.
 *
 * @post VByPList marked busy.
 *
 * @note this interface is used in conjunction with VVByPListEndExclusive_r and
 *       VVByPListWait_r to perform asynchronous (wrt VOL_LOCK) operations on a
 *       VByPList.
 *
 * @see VVByPListEndExclusive_r
 * @see VVByPListWait_r
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
/* take exclusive control over the list */
static void
VVByPListBeginExclusive_r(struct DiskPartition64 * dp)
{
    assert(dp->vol_list.busy == 0);
    dp->vol_list.busy = 1;
}

/**
 * relinquish exclusive ownership of a VByPList.
 *
 * @param[in] dp   pointer to disk partition object
 *
 * @pre VOL_LOCK held.  thread owns the VByPList exclusively.
 *
 * @post VByPList is marked quiescent.  threads awaiting use of
 *       the list are awakened.
 *
 * @see VVByPListBeginExclusive_r
 * @see VVByPListWait_r
 *
 * @note DAFS only
 *
 * @internal volume package internal use only.
 */
static void
VVByPListEndExclusive_r(struct DiskPartition64 * dp)
{
    assert(dp->vol_list.busy);
    dp->vol_list.busy = 0;
    assert(pthread_cond_broadcast(&dp->vol_list.cv) == 0);
}

/**
 * wait for all asynchronous operations on a VByPList to complete.
 *
 * @param[in] dp  pointer to disk partition object
 *
 * @pre VOL_LOCK is held.
 *
 * @post disk partition's VByP list is quiescent
 *
 * @note DAFS only
 *
 * @note This interface should be called before any attempt to
 *       traverse the VByPList.  It is permissible for a thread
 *       to gain exclusive access to the list, and then perform
 *       latent operations on the list asynchronously wrt the
 *       VOL_LOCK.
 *
 * @warning if waiting is necessary, VOL_LOCK is dropped
 *
 * @see VVByPListEndExclusive_r
 * @see VVByPListBeginExclusive_r
 *
 * @internal volume package internal use only.
 */
static void
VVByPListWait_r(struct DiskPartition64 * dp)
{
    while (dp->vol_list.busy) {
	VOL_CV_WAIT(&dp->vol_list.cv);
    }
}
#endif /* AFS_DEMAND_ATTACH_FS */

/***************************************************/
/* Volume Cache Statistics routines                */
/***************************************************/

void
VPrintCacheStats_r(void)
{
    afs_uint32 get_hi, get_lo, load_hi, load_lo;
    struct VnodeClassInfo *vcp;
    vcp = &VnodeClassInfo[vLarge];
    Log("Large vnode cache, %d entries, %d allocs, %d gets (%d reads), %d writes\n", vcp->cacheSize, vcp->allocs, vcp->gets, vcp->reads, vcp->writes);
    vcp = &VnodeClassInfo[vSmall];
    Log("Small vnode cache,%d entries, %d allocs, %d gets (%d reads), %d writes\n", vcp->cacheSize, vcp->allocs, vcp->gets, vcp->reads, vcp->writes);
    SplitInt64(VStats.hdr_gets, get_hi, get_lo);
    SplitInt64(VStats.hdr_loads, load_hi, load_lo);
    Log("Volume header cache, %d entries, %d gets, %d replacements\n",
	VStats.hdr_cache_size, get_lo, load_lo);
}

void
VPrintCacheStats(void)
{
    VOL_LOCK;
    VPrintCacheStats_r();
    VOL_UNLOCK;
}

#ifdef AFS_DEMAND_ATTACH_FS
static double
UInt64ToDouble(afs_uint64 * x)
{
    static double c32 = 4.0 * 1.073741824 * 1000000000.0;
    afs_uint32 h, l;
    SplitInt64(*x, h, l);
    return (((double)h) * c32) + ((double) l);
}

static char *
DoubleToPrintable(double x, char * buf, int len)
{
    static double billion = 1000000000.0;
    afs_uint32 y[3];

    y[0] = (afs_uint32) (x / (billion * billion));
    y[1] = (afs_uint32) ((x - (((double)y[0]) * billion * billion)) / billion);
    y[2] = (afs_uint32) (x - ((((double)y[0]) * billion * billion) + (((double)y[1]) * billion)));

    if (y[0]) {
	snprintf(buf, len, "%d%09d%09d", y[0], y[1], y[2]);
    } else if (y[1]) {
	snprintf(buf, len, "%d%09d", y[1], y[2]);
    } else {
	snprintf(buf, len, "%d", y[2]);
    }
    buf[len-1] = '\0';
    return buf;
}

struct VLRUExtStatsEntry {
    VolumeId volid;
};

struct VLRUExtStats {
    afs_uint32 len;
    afs_uint32 used;
    struct {
	afs_uint32 start;
	afs_uint32 len;
    } queue_info[VLRU_QUEUE_INVALID];
    struct VLRUExtStatsEntry * vec;
};

/**
 * add a 256-entry fudge factor onto the vector in case state changes
 * out from under us.
 */
#define VLRU_EXT_STATS_VEC_LEN_FUDGE   256

/**
 * collect extended statistics for the VLRU subsystem.
 *
 * @param[out] stats  pointer to stats structure to be populated
 * @param[in] nvols   number of volumes currently known to exist
 *
 * @pre VOL_LOCK held
 *
 * @post stats->vec allocated and populated
 *
 * @return operation status
 *    @retval 0 success
 *    @retval 1 failure
 */
static int
VVLRUExtStats_r(struct VLRUExtStats * stats, afs_uint32 nvols)
{
    afs_uint32 cur, idx, len;
    struct rx_queue * qp, * nqp;
    Volume * vp;
    struct VLRUExtStatsEntry * vec;

    len = nvols + VLRU_EXT_STATS_VEC_LEN_FUDGE;
    vec = stats->vec = calloc(len,
			      sizeof(struct VLRUExtStatsEntry));
    if (vec == NULL) {
	return 1;
    }

    cur = 0;
    for (idx = VLRU_QUEUE_NEW; idx < VLRU_QUEUE_INVALID; idx++) {
	VLRU_Wait_r(&volume_LRU.q[idx]);
	VLRU_BeginExclusive_r(&volume_LRU.q[idx]);
	VOL_UNLOCK;

	stats->queue_info[idx].start = cur;

	for (queue_Scan(&volume_LRU.q[idx], qp, nqp, rx_queue)) {
	    if (cur == len) {
		/* out of space in vec */
		break;
	    }
	    vp = (Volume *)((char *)qp - offsetof(Volume, vlru));
	    vec[cur].volid = vp->hashid;
	    cur++;
	}

	stats->queue_info[idx].len = cur - stats->queue_info[idx].start;

	VOL_LOCK;
	VLRU_EndExclusive_r(&volume_LRU.q[idx]);
    }

    stats->len = len;
    stats->used = cur;
    return 0;
}

#define ENUMTOSTRING(en)  #en
#define ENUMCASE(en) \
    case en: \
        return ENUMTOSTRING(en); \
        break

static char *
vlru_idx_to_string(int idx)
{
    switch (idx) {
	ENUMCASE(VLRU_QUEUE_NEW);
	ENUMCASE(VLRU_QUEUE_MID);
	ENUMCASE(VLRU_QUEUE_OLD);
	ENUMCASE(VLRU_QUEUE_CANDIDATE);
	ENUMCASE(VLRU_QUEUE_HELD);
	ENUMCASE(VLRU_QUEUE_INVALID);
    default:
	return "**UNKNOWN**";
    }
}

void
VPrintExtendedCacheStats_r(int flags)
{
    int i;
    afs_uint32 vol_sum = 0;
    struct stats {
	double min;
	double max;
	double sum;
	double avg;
    };
    struct stats looks, gets, reorders, len;
    struct stats ch_looks, ch_gets, ch_reorders;
    char pr_buf[4][32];
    VolumeHashChainHead *head;
    Volume *vp, *np;
    struct VLRUExtStats vlru_stats;

    /* zero out stats */
    memset(&looks, 0, sizeof(struct stats));
    memset(&gets, 0, sizeof(struct stats));
    memset(&reorders, 0, sizeof(struct stats));
    memset(&len, 0, sizeof(struct stats));
    memset(&ch_looks, 0, sizeof(struct stats));
    memset(&ch_gets, 0, sizeof(struct stats));
    memset(&ch_reorders, 0, sizeof(struct stats));

    for (i = 0; i < VolumeHashTable.Size; i++) {
	head = &VolumeHashTable.Table[i];

	VHashWait_r(head);
	VHashBeginExclusive_r(head);
	VOL_UNLOCK;

	ch_looks.sum    = UInt64ToDouble(&head->looks);
	ch_gets.sum     = UInt64ToDouble(&head->gets);
	ch_reorders.sum = UInt64ToDouble(&head->reorders);

	/* update global statistics */
	{
	    looks.sum    += ch_looks.sum;
	    gets.sum     += ch_gets.sum;
	    reorders.sum += ch_reorders.sum;
	    len.sum      += (double)head->len;
	    vol_sum      += head->len;

	    if (i == 0) {
		len.min      = (double) head->len;
		len.max      = (double) head->len;
		looks.min    = ch_looks.sum;
		looks.max    = ch_looks.sum;
		gets.min     = ch_gets.sum;
		gets.max     = ch_gets.sum;
		reorders.min = ch_reorders.sum;
		reorders.max = ch_reorders.sum;
	    } else {
		if (((double)head->len) < len.min)
		    len.min = (double) head->len;
		if (((double)head->len) > len.max)
		    len.max = (double) head->len;
		if (ch_looks.sum < looks.min)
		    looks.min = ch_looks.sum;
		else if (ch_looks.sum > looks.max)
		    looks.max = ch_looks.sum;
		if (ch_gets.sum < gets.min)
		    gets.min = ch_gets.sum;
		else if (ch_gets.sum > gets.max)
		    gets.max = ch_gets.sum;
		if (ch_reorders.sum < reorders.min)
		    reorders.min = ch_reorders.sum;
		else if (ch_reorders.sum > reorders.max)
		    reorders.max = ch_reorders.sum;
	    }
	}

	if ((flags & VOL_STATS_PER_CHAIN2) && queue_IsNotEmpty(head)) {
	    /* compute detailed per-chain stats */
	    struct stats hdr_loads, hdr_gets;
	    double v_looks, v_loads, v_gets;

	    /* initialize stats with data from first element in chain */
	    vp = queue_First(head, Volume);
	    v_looks = UInt64ToDouble(&vp->stats.hash_lookups);
	    v_loads = UInt64ToDouble(&vp->stats.hdr_loads);
	    v_gets  = UInt64ToDouble(&vp->stats.hdr_gets);
	    ch_gets.min = ch_gets.max = v_looks;
	    hdr_loads.min = hdr_loads.max = v_loads;
	    hdr_gets.min = hdr_gets.max = v_gets;
	    hdr_loads.sum = hdr_gets.sum = 0;

	    vp = queue_Next(vp, Volume);

	    /* pull in stats from remaining elements in chain */
	    for (queue_ScanFrom(head, vp, vp, np, Volume)) {
		v_looks = UInt64ToDouble(&vp->stats.hash_lookups);
		v_loads = UInt64ToDouble(&vp->stats.hdr_loads);
		v_gets  = UInt64ToDouble(&vp->stats.hdr_gets);

		hdr_loads.sum += v_loads;
		hdr_gets.sum += v_gets;

		if (v_looks < ch_gets.min)
		    ch_gets.min = v_looks;
		else if (v_looks > ch_gets.max)
		    ch_gets.max = v_looks;

		if (v_loads < hdr_loads.min)
		    hdr_loads.min = v_loads;
		else if (v_loads > hdr_loads.max)
		    hdr_loads.max = v_loads;

		if (v_gets < hdr_gets.min)
		    hdr_gets.min = v_gets;
		else if (v_gets > hdr_gets.max)
		    hdr_gets.max = v_gets;
	    }

	    /* compute per-chain averages */
	    ch_gets.avg = ch_gets.sum / ((double)head->len);
	    hdr_loads.avg = hdr_loads.sum / ((double)head->len);
	    hdr_gets.avg = hdr_gets.sum / ((double)head->len);

	    /* dump per-chain stats */
	    Log("Volume hash chain %d : len=%d, looks=%s, reorders=%s\n",
		i, head->len,
		DoubleToPrintable(ch_looks.sum, pr_buf[0], sizeof(pr_buf[0])),
		DoubleToPrintable(ch_reorders.sum, pr_buf[1], sizeof(pr_buf[1])));
	    Log("\tVolume gets : min=%s, max=%s, avg=%s, total=%s\n",
		DoubleToPrintable(ch_gets.min, pr_buf[0], sizeof(pr_buf[0])),
		DoubleToPrintable(ch_gets.max, pr_buf[1], sizeof(pr_buf[1])),
		DoubleToPrintable(ch_gets.avg, pr_buf[2], sizeof(pr_buf[2])),
		DoubleToPrintable(ch_gets.sum, pr_buf[3], sizeof(pr_buf[3])));
	    Log("\tHDR gets : min=%s, max=%s, avg=%s, total=%s\n",
		DoubleToPrintable(hdr_gets.min, pr_buf[0], sizeof(pr_buf[0])),
		DoubleToPrintable(hdr_gets.max, pr_buf[1], sizeof(pr_buf[1])),
		DoubleToPrintable(hdr_gets.avg, pr_buf[2], sizeof(pr_buf[2])),
		DoubleToPrintable(hdr_gets.sum, pr_buf[3], sizeof(pr_buf[3])));
	    Log("\tHDR loads : min=%s, max=%s, avg=%s, total=%s\n",
		DoubleToPrintable(hdr_loads.min, pr_buf[0], sizeof(pr_buf[0])),
		DoubleToPrintable(hdr_loads.max, pr_buf[1], sizeof(pr_buf[1])),
		DoubleToPrintable(hdr_loads.avg, pr_buf[2], sizeof(pr_buf[2])),
		DoubleToPrintable(hdr_loads.sum, pr_buf[3], sizeof(pr_buf[3])));
	} else if (flags & VOL_STATS_PER_CHAIN) {
	    /* dump simple per-chain stats */
	    Log("Volume hash chain %d : len=%d, looks=%s, gets=%s, reorders=%s\n",
		i, head->len,
		DoubleToPrintable(ch_looks.sum, pr_buf[0], sizeof(pr_buf[0])),
		DoubleToPrintable(ch_gets.sum, pr_buf[1], sizeof(pr_buf[1])),
		DoubleToPrintable(ch_reorders.sum, pr_buf[2], sizeof(pr_buf[2])));
	}

	VOL_LOCK;
	VHashEndExclusive_r(head);
    }

    VOL_UNLOCK;

    /* compute global averages */
    len.avg      = len.sum      / ((double)VolumeHashTable.Size);
    looks.avg    = looks.sum    / ((double)VolumeHashTable.Size);
    gets.avg     = gets.sum     / ((double)VolumeHashTable.Size);
    reorders.avg = reorders.sum / ((double)VolumeHashTable.Size);

    /* dump global stats */
    Log("Volume hash summary: %d buckets\n", VolumeHashTable.Size);
    Log(" chain length : min=%s, max=%s, avg=%s, total=%s\n",
	DoubleToPrintable(len.min, pr_buf[0], sizeof(pr_buf[0])),
	DoubleToPrintable(len.max, pr_buf[1], sizeof(pr_buf[1])),
	DoubleToPrintable(len.avg, pr_buf[2], sizeof(pr_buf[2])),
	DoubleToPrintable(len.sum, pr_buf[3], sizeof(pr_buf[3])));
    Log(" looks : min=%s, max=%s, avg=%s, total=%s\n",
	DoubleToPrintable(looks.min, pr_buf[0], sizeof(pr_buf[0])),
	DoubleToPrintable(looks.max, pr_buf[1], sizeof(pr_buf[1])),
	DoubleToPrintable(looks.avg, pr_buf[2], sizeof(pr_buf[2])),
	DoubleToPrintable(looks.sum, pr_buf[3], sizeof(pr_buf[3])));
    Log(" gets : min=%s, max=%s, avg=%s, total=%s\n",
	DoubleToPrintable(gets.min, pr_buf[0], sizeof(pr_buf[0])),
	DoubleToPrintable(gets.max, pr_buf[1], sizeof(pr_buf[1])),
	DoubleToPrintable(gets.avg, pr_buf[2], sizeof(pr_buf[2])),
	DoubleToPrintable(gets.sum, pr_buf[3], sizeof(pr_buf[3])));
    Log(" reorders : min=%s, max=%s, avg=%s, total=%s\n",
	DoubleToPrintable(reorders.min, pr_buf[0], sizeof(pr_buf[0])),
	DoubleToPrintable(reorders.max, pr_buf[1], sizeof(pr_buf[1])),
	DoubleToPrintable(reorders.avg, pr_buf[2], sizeof(pr_buf[2])),
	DoubleToPrintable(reorders.sum, pr_buf[3], sizeof(pr_buf[3])));

    /* print extended disk related statistics */
    {
	struct DiskPartition64 * diskP;
	afs_uint32 vol_count[VOLMAXPARTS+1];
	byte part_exists[VOLMAXPARTS+1];
	Device id;
	int i;

	memset(vol_count, 0, sizeof(vol_count));
	memset(part_exists, 0, sizeof(part_exists));

	VOL_LOCK;

	for (diskP = DiskPartitionList; diskP; diskP = diskP->next) {
	    id = diskP->index;
	    vol_count[id] = diskP->vol_list.len;
	    part_exists[id] = 1;
	}

	VOL_UNLOCK;
	for (i = 0; i <= VOLMAXPARTS; i++) {
	    if (part_exists[i]) {
		/* XXX while this is currently safe, it is a violation
		 *     of the VGetPartitionById_r interface contract. */
		diskP = VGetPartitionById_r(i, 0);
		if (diskP) {
		    Log("Partition %s has %d online volumes\n",
			VPartitionPath(diskP), diskP->vol_list.len);
		}
	    }
	}
	VOL_LOCK;
    }

    /* print extended VLRU statistics */
    if (VVLRUExtStats_r(&vlru_stats, vol_sum) == 0) {
	afs_uint32 idx, cur, lpos;
	VolumeId line[5];

        VOL_UNLOCK;

	Log("VLRU State Dump:\n\n");

	for (idx = VLRU_QUEUE_NEW; idx < VLRU_QUEUE_INVALID; idx++) {
	    Log("\t%s:\n", vlru_idx_to_string(idx));

	    lpos = 0;
	    for (cur = vlru_stats.queue_info[idx].start;
		 cur < vlru_stats.queue_info[idx].len;
		 cur++) {
		line[lpos++] = vlru_stats.vec[cur].volid;
		if (lpos==5) {
		    Log("\t\t%u, %u, %u, %u, %u,\n",
			line[0], line[1], line[2], line[3], line[4]);
		    lpos = 0;
		}
	    }

	    if (lpos) {
		while (lpos < 5) {
		    line[lpos++] = 0;
		}
		Log("\t\t%u, %u, %u, %u, %u\n",
		    line[0], line[1], line[2], line[3], line[4]);
	    }
	    Log("\n");
	}

	free(vlru_stats.vec);

	VOL_LOCK;
    }
}

void
VPrintExtendedCacheStats(int flags)
{
    VOL_LOCK;
    VPrintExtendedCacheStats_r(flags);
    VOL_UNLOCK;
}
#endif /* AFS_DEMAND_ATTACH_FS */

afs_int32
VCanScheduleSalvage(void)
{
    return vol_opts.canScheduleSalvage;
}

afs_int32
VCanUseFSSYNC(void)
{
    return vol_opts.canUseFSSYNC;
}

afs_int32
VCanUseSALVSYNC(void)
{
    return vol_opts.canUseSALVSYNC;
}

afs_int32
VCanUnsafeAttach(void)
{
    return vol_opts.unsafe_attach;
}
