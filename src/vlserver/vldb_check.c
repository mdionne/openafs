/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/* Read a VLDB file and verify it for correctness */

#define VL  0x001		/* good volume entry */
#define FR  0x002		/* free volume entry */
#define MH  0x004		/* multi-homed entry */

#define RWH 0x010		/* on rw hash chain */
#define ROH 0x020		/* on ro hash chain */
#define BKH 0x040		/* on bk hash chain */
#define NH  0x080		/* on name hash chain */

#define MHC 0x100		/* on multihomed chain */
#define FRC 0x200		/* on free chain */

#define REFRW 0x1000            /* linked from something (RW) */
#define REFRO 0x2000            /* linked from something (RO) */
#define REFBK 0x4000            /* linked from something (BK) */
#define REFN  0x8000            /* linked from something (name) */

#define MULTRW 0x10000         /* multiply-chained (RW) */
#define MULTRO 0x20000         /* multiply-chained (RO) */
#define MULTBK 0x40000         /* multiply-chained (BK) */
#define MULTN  0x80000         /* multiply-chained (name) */

#define MISRWH 0x100000          /* mischained (RW) */
#define MISROH 0x200000          /* mischained (RO) */
#define MISBKH 0x400000          /* mischained (BK) */
#define MISNH  0x800000          /* mischained (name) */

#define VLDB_CHECK_NO_VLDB_CHECK_ERROR 0
#define VLDB_CHECK_WARNING  1
#define VLDB_CHECK_ERROR    2
#define VLDB_CHECK_FATAL    4
#define vldbread(x,y,z) vldbio(x,y,z,0)
#define vldbwrite(x,y,z) vldbio(x,y,z,1)

#include <afsconfig.h>
#include <afs/param.h>


#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#ifdef AFS_NT40_ENV
#include <winsock2.h>
#include <WINNT/afsevent.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

#include "vlserver.h"
#include "vldbint.h"
#include <ubik.h>
#include <afs/afsutil.h>
#include <afs/cmd.h>

#define ADDR(x) (x/sizeof(struct nvlentry))

int fd;
int listentries, listservers, listheader, listuheader, verbose, quiet;

int fix = 0;
int passes = 0;
/* if quiet, don't send anything to stdout */
int quiet = 0;
/*  error level. 0 = no error, 1 = warning, 2 = error, 4 = fatal */
int error_level  = 0;

struct er {
    long addr;
    int type;
} *record;
afs_int32 maxentries;
int serveraddrs[MAXSERVERID + 2];

/*  Used to control what goes to stdout based on quiet flag */
void
quiet_println(const char *fmt,...) {
    va_list args;
    if (!quiet) {
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
    }
}

/*  Used to set the error level and ship messages to stderr */
void
log_error(int eval, const char *fmt, ...)
{
    va_list args;
    if (error_level < eval) error_level  = eval ;  /*  bump up the severity */
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (error_level  == VLDB_CHECK_FATAL) exit(VLDB_CHECK_FATAL);
}


#if 0
int
writeUbikHeader()
{
    /* Bump the version number?? We could cheat and push a new db... */
}
#endif

#define HDRSIZE 64
int
readUbikHeader(void)
{
    int offset, r;
    struct ubik_hdr uheader;

    offset = lseek(fd, 0, 0);
    if (offset != 0) {
	log_error(VLDB_CHECK_FATAL,"error: lseek to 0 failed: %d %d\n", offset, errno);
	return (VLDB_CHECK_FATAL);
    }

    /* now read the info */
    r = read(fd, &uheader, sizeof(uheader));
    if (r != sizeof(uheader)) {
	log_error(VLDB_CHECK_FATAL,"error: read of %lu bytes failed: %d %d\n", sizeof(uheader), r,
	       errno);
	return (VLDB_CHECK_FATAL);
    }

    uheader.magic = ntohl(uheader.magic);
    uheader.size = ntohs(uheader.size);
    uheader.version.epoch = ntohl(uheader.version.epoch);
    uheader.version.counter = ntohl(uheader.version.counter);

    if (listuheader) {
	quiet_println("Ubik Header\n");
	quiet_println("   Magic           = 0x%x\n", uheader.magic);
	quiet_println("   Size            = %u\n", uheader.size);
	quiet_println("   Version.epoch   = %u\n", uheader.version.epoch);
	quiet_println("   Version.counter = %u\n", uheader.version.counter);
    }

    if (uheader.size != HDRSIZE)
	log_error(VLDB_CHECK_WARNING,"VLDB_CHECK_WARNING: Ubik header size is %u (should be %u)\n", uheader.size,
	       HDRSIZE);
    if (uheader.magic != UBIK_MAGIC)
	log_error(VLDB_CHECK_ERROR,"Ubik header magic is 0x%x (should be 0x%x)\n", uheader.magic,
	       UBIK_MAGIC);

    return (0);
}

int
vldbio(int position, void *buffer, int size, int rdwr)
{
    int offset, r, p;

    /* seek to the correct spot. skip ubik stuff */
    p = position + HDRSIZE;
    offset = lseek(fd, p, 0);
    if (offset != p) {
	log_error(VLDB_CHECK_FATAL,"error: lseek to %d failed: %d %d\n", p, offset, errno);
	return (-1);
    }

    if (rdwr == 1)
	r = write(fd, buffer, size);
    else
	r = read(fd, buffer, size);

    if (r != size) {
	log_error(VLDB_CHECK_FATAL,"error: %s of %d bytes failed: %d %d\n", rdwr==1?"write":"read",
	       size, r, errno);
	return (-1);
    }
    return (0);
}

char *
vtype(int type)
{
    static char Type[3];

    if (type == 0)
	strcpy(Type, "rw");
    else if (type == 1)
	strcpy(Type, "ro");
    else if (type == 2)
	strcpy(Type, "bk");
    else
	strcpy(Type, "??");
    return (Type);
}

afs_int32
NameHash(char *volname)
{
    unsigned int hash;
    char *vchar;

    hash = 0;
    for (vchar = volname + strlen(volname) - 1; vchar >= volname; vchar--)
	hash = (hash * 63) + (*((unsigned char *)vchar) - 63);
    return (hash % HASHSIZE);
}

afs_int32
IdHash(afs_uint32 volid)
{
    return ((abs(volid)) % HASHSIZE);
}

#define LEGALCHARS ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
int
InvalidVolname(char *volname)
{
    char *map;
    size_t slen;

    map = LEGALCHARS;
    slen = strlen(volname);
    if (slen >= VL_MAXNAMELEN)
	return 1;
    return (slen != strspn(volname, map));
}

int
validVolumeAddr(afs_uint32 fileOffset)
{
    if (ADDR(fileOffset) >= maxentries) {
	/* Are we in range */
	return 0;
    }
    /*
     * We cannot test whether the offset is aligned
     * since the vl entries are not in a regular array
     */
    return 1;
}

void
readheader(struct vlheader *headerp)
{
    int i, j;

    vldbread(0, (char *)headerp, sizeof(*headerp));

    headerp->vital_header.vldbversion =
	ntohl(headerp->vital_header.vldbversion);
    headerp->vital_header.headersize =
	ntohl(headerp->vital_header.headersize);
    headerp->vital_header.freePtr = ntohl(headerp->vital_header.freePtr);
    headerp->vital_header.eofPtr = ntohl(headerp->vital_header.eofPtr);
    headerp->vital_header.allocs = ntohl(headerp->vital_header.allocs);
    headerp->vital_header.frees = ntohl(headerp->vital_header.frees);
    headerp->vital_header.MaxVolumeId =
	ntohl(headerp->vital_header.MaxVolumeId);
    headerp->vital_header.totalEntries[0] =
	ntohl(headerp->vital_header.totalEntries[0]);
    for (i = 0; i < MAXTYPES; i++)
	headerp->vital_header.totalEntries[i] =
	    ntohl(headerp->vital_header.totalEntries[1]);

    headerp->SIT = ntohl(headerp->SIT);
    for (i = 0; i < MAXSERVERID; i++)
	headerp->IpMappedAddr[i] = ntohl(headerp->IpMappedAddr[i]);
    for (i = 0; i < HASHSIZE; i++)
	headerp->VolnameHash[i] = ntohl(headerp->VolnameHash[i]);
    for (i = 0; i < MAXTYPES; i++)
	for (j = 0; j < HASHSIZE; j++)
	    headerp->VolidHash[i][j] = ntohl(headerp->VolidHash[i][j]);

    if (listheader) {
	quiet_println("vldb header\n");
	quiet_println("   vldbversion      = %u\n",
	       headerp->vital_header.vldbversion);
	quiet_println("   headersize       = %u [actual=%lu]\n",
	       headerp->vital_header.headersize, sizeof(*headerp));
	quiet_println("   freePtr          = 0x%x\n", headerp->vital_header.freePtr);
	quiet_println("   eofPtr           = %u\n", headerp->vital_header.eofPtr);
	quiet_println("   allocblock calls = %10u\n", headerp->vital_header.allocs);
	quiet_println("   freeblock  calls = %10u\n", headerp->vital_header.frees);
	quiet_println("   MaxVolumeId      = %u\n",
	       headerp->vital_header.MaxVolumeId);
	quiet_println("   rw vol entries   = %u\n",
	       headerp->vital_header.totalEntries[0]);
	quiet_println("   ro vol entries   = %u\n",
	       headerp->vital_header.totalEntries[1]);
	quiet_println("   bk vol entries   = %u\n",
	       headerp->vital_header.totalEntries[2]);
	quiet_println("   multihome info   = 0x%x (%u)\n", headerp->SIT,
	       headerp->SIT);
	quiet_println("   server ip addr   table: size = %d entries\n",
	       MAXSERVERID + 1);
	quiet_println("   volume name hash table: size = %d buckets\n", HASHSIZE);
	quiet_println("   volume id   hash table: %d tables with %d buckets each\n",
	       MAXTYPES, HASHSIZE);
    }

    /* Check the header size */
    if (headerp->vital_header.headersize != sizeof(*headerp))
	log_error(VLDB_CHECK_WARNING,"Header reports its size as %d (should be %lu)\n",
	       headerp->vital_header.headersize, sizeof(*headerp));
    return;
}

void
writeheader(struct vlheader *headerp)
{
    int i, j;

    headerp->vital_header.vldbversion =
	htonl(headerp->vital_header.vldbversion);
    headerp->vital_header.headersize =
	htonl(headerp->vital_header.headersize);
    headerp->vital_header.freePtr = htonl(headerp->vital_header.freePtr);
    headerp->vital_header.eofPtr = htonl(headerp->vital_header.eofPtr);
    headerp->vital_header.allocs = htonl(headerp->vital_header.allocs);
    headerp->vital_header.frees = htonl(headerp->vital_header.frees);
    headerp->vital_header.MaxVolumeId =
	htonl(headerp->vital_header.MaxVolumeId);
    headerp->vital_header.totalEntries[0] =
	htonl(headerp->vital_header.totalEntries[0]);
    for (i = 0; i < MAXTYPES; i++)
	headerp->vital_header.totalEntries[i] =
	    htonl(headerp->vital_header.totalEntries[1]);

    headerp->SIT = htonl(headerp->SIT);
    for (i = 0; i < MAXSERVERID; i++)
	headerp->IpMappedAddr[i] = htonl(headerp->IpMappedAddr[i]);
    for (i = 0; i < HASHSIZE; i++)
	headerp->VolnameHash[i] = htonl(headerp->VolnameHash[i]);
    for (i = 0; i < MAXTYPES; i++)
	for (j = 0; j < HASHSIZE; j++)
	    headerp->VolidHash[i][j] = htonl(headerp->VolidHash[i][j]);

    vldbwrite(0, (char *)headerp, sizeof(*headerp));
}

void
readMH(afs_int32 addr, struct extentaddr *mhblockP)
{
    int i, j;
    struct extentaddr *e;

    vldbread(addr, (char *)mhblockP, VL_ADDREXTBLK_SIZE);

    mhblockP->ex_count = ntohl(mhblockP->ex_count);
    mhblockP->ex_flags = ntohl(mhblockP->ex_flags);
    for (i = 0; i < VL_MAX_ADDREXTBLKS; i++)
	mhblockP->ex_contaddrs[i] = ntohl(mhblockP->ex_contaddrs[i]);

    for (i = 1; i < VL_MHSRV_PERBLK; i++) {
	e = &(mhblockP[i]);

	/* won't convert hostuuid */
	e->ex_uniquifier = ntohl(e->ex_uniquifier);
	for (j = 0; j < VL_MAXIPADDRS_PERMH; j++)
	    e->ex_addrs[j] = ntohl(e->ex_addrs[j]);
    }
    return;
}

void
readentry(afs_int32 addr, struct nvlentry *vlentryp, afs_int32 *type)
{
    int i;

    vldbread(addr, (char *)vlentryp, sizeof(*vlentryp));

    for (i = 0; i < MAXTYPES; i++)
	vlentryp->volumeId[i] = ntohl(vlentryp->volumeId[i]);
    vlentryp->flags = ntohl(vlentryp->flags);
    vlentryp->LockAfsId = ntohl(vlentryp->LockAfsId);
    vlentryp->LockTimestamp = ntohl(vlentryp->LockTimestamp);
    vlentryp->cloneId = ntohl(vlentryp->cloneId);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->nextIdHash[i] = ntohl(vlentryp->nextIdHash[i]);
    vlentryp->nextNameHash = ntohl(vlentryp->nextNameHash);
    for (i = 0; i < NMAXNSERVERS; i++) {
	/* make sure not to ntohl these, as they're chars, not ints */
	vlentryp->serverNumber[i] = vlentryp->serverNumber[i];
	vlentryp->serverPartition[i] = vlentryp->serverPartition[i];
	vlentryp->serverFlags[i] = vlentryp->serverFlags[i];
    }

    if (vlentryp->flags == VLCONTBLOCK) {
	*type = MH;
    } else if (vlentryp->flags == VLFREE) {
	*type = FR;
    } else {
	*type = VL;
    }

    if (listentries) {
	quiet_println("address %u: ", addr);
	if (vlentryp->flags == VLCONTBLOCK) {
	    quiet_println("mh extension block\n");
	} else if (vlentryp->flags == VLFREE) {
	    quiet_println("free vlentry\n");
	} else {
	    quiet_println("vlentry %s\n", vlentryp->name);
	    quiet_println("   rw id = %u ; ro id = %u ; bk id = %u\n",
		   vlentryp->volumeId[0], vlentryp->volumeId[1],
		   vlentryp->volumeId[2]);
	    quiet_println("   flags         =");
	    if (vlentryp->flags & VLF_RWEXISTS)
		quiet_println(" rw");
	    if (vlentryp->flags & VLF_ROEXISTS)
		quiet_println(" ro");
	    if (vlentryp->flags & VLF_BACKEXISTS)
		quiet_println(" bk");
	    if (vlentryp->flags & VLOP_MOVE)
		quiet_println(" lock_move");
	    if (vlentryp->flags & VLOP_RELEASE)
		quiet_println(" lock_release");
	    if (vlentryp->flags & VLOP_BACKUP)
		quiet_println(" lock_backup");
	    if (vlentryp->flags & VLOP_DELETE)
		quiet_println(" lock_delete");
	    if (vlentryp->flags & VLOP_DUMP)
		quiet_println(" lock_dump");

	    /* all bits not covered by VLF_* and VLOP_* constants */
	    if (vlentryp->flags & 0xffff8e0f)
		quiet_println(" errorflag(0x%x)", vlentryp->flags);
	    quiet_println("\n");
	    quiet_println("   LockAfsId     = %d\n", vlentryp->LockAfsId);
	    quiet_println("   LockTimestamp = %d\n", vlentryp->LockTimestamp);
	    quiet_println("   cloneId       = %u\n", vlentryp->cloneId);
	    quiet_println
		("   next hash for rw = %u ; ro = %u ; bk = %u ; name = %u\n",
		 vlentryp->nextIdHash[0], vlentryp->nextIdHash[1],
		 vlentryp->nextIdHash[2], vlentryp->nextNameHash);
	    for (i = 0; i < NMAXNSERVERS; i++) {
		if (vlentryp->serverNumber[i] != 255) {
		    quiet_println("   server %d ; partition %d ; flags =",
			   vlentryp->serverNumber[i],
			   vlentryp->serverPartition[i]);
		    if (vlentryp->serverFlags[i] & VLSF_RWVOL)
			quiet_println(" rw");
		    if (vlentryp->serverFlags[i] & VLSF_ROVOL)
			quiet_println(" ro");
		    if (vlentryp->serverFlags[i] & VLSF_BACKVOL)
			quiet_println(" bk");
		    if (vlentryp->serverFlags[i] & VLSF_NEWREPSITE)
			quiet_println(" newro");
		    quiet_println("\n");
		}
	    }
	}
    }
    return;
}

void
writeentry(afs_int32 addr, struct nvlentry *vlentryp)
{
    int i;

    if (verbose) quiet_println("Writing back entry at addr %u\n", addr);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->volumeId[i] = htonl(vlentryp->volumeId[i]);
    vlentryp->flags = htonl(vlentryp->flags);
    vlentryp->LockAfsId = htonl(vlentryp->LockAfsId);
    vlentryp->LockTimestamp = htonl(vlentryp->LockTimestamp);
    vlentryp->cloneId = htonl(vlentryp->cloneId);
    for (i = 0; i < MAXTYPES; i++)
	vlentryp->nextIdHash[i] = htonl(vlentryp->nextIdHash[i]);
    vlentryp->nextNameHash = htonl(vlentryp->nextNameHash);
    for (i = 0; i < NMAXNSERVERS; i++) {
	/* make sure not to htonl these, as they're chars, not ints */
	vlentryp->serverNumber[i] =  vlentryp->serverNumber[i] ;
	vlentryp->serverPartition[i] = vlentryp->serverPartition[i] ;
	vlentryp->serverFlags[i] = vlentryp->serverFlags[i] ;
    }
    vldbwrite(addr, (char *)vlentryp, sizeof(*vlentryp));
}

void
readSIT(int base, int addr)
{
    int i, j, a;
    char sitbuf[VL_ADDREXTBLK_SIZE];
    struct extentaddr *extent;

    if (!addr)
	return;
    vldbread(addr, sitbuf, VL_ADDREXTBLK_SIZE);
    extent = (struct extentaddr *)sitbuf;

    quiet_println("multihome info block: base %d\n", base);
    if (base == 0) {
	quiet_println("   count = %u\n", ntohl(extent->ex_count));
	quiet_println("   flags = %u\n", ntohl(extent->ex_flags));
	for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    quiet_println("   contaddrs[%d] = %u\n", i,
		   ntohl(extent->ex_contaddrs[i]));
	}
    }
    for (i = 1; i < VL_MHSRV_PERBLK; i++) {
	/* should we skip this entry */
	for (j = 0; j < VL_MAX_ADDREXTBLKS; j++) {
	    if (extent[i].ex_addrs[j])
		break;
	}
	if (j >= VL_MAX_ADDREXTBLKS)
	    continue;

	quiet_println("   base %d index %d:\n", base, i);

	quiet_println("       afsuuid    = (%x %x %x /%d/%d/ /%x/%x/%x/%x/%x/%x/)\n",
	       ntohl(extent[i].ex_hostuuid.time_low),
	       ntohl(extent[i].ex_hostuuid.time_mid),
	       ntohl(extent[i].ex_hostuuid.time_hi_and_version),
	       ntohl(extent[i].ex_hostuuid.clock_seq_hi_and_reserved),
	       ntohl(extent[i].ex_hostuuid.clock_seq_low),
	       ntohl(extent[i].ex_hostuuid.node[0]),
	       ntohl(extent[i].ex_hostuuid.node[1]),
	       ntohl(extent[i].ex_hostuuid.node[2]),
	       ntohl(extent[i].ex_hostuuid.node[3]),
	       ntohl(extent[i].ex_hostuuid.node[4]),
	       ntohl(extent[i].ex_hostuuid.node[5]));
	quiet_println("       uniquifier = %u\n", ntohl(extent[i].ex_uniquifier));
	for (j = 0; j < VL_MAXIPADDRS_PERMH; j++) {
	    a = ntohl(extent[i].ex_addrs[j]);
	    if (a) {
		quiet_println("       %d.%d.%d.%d\n", (a >> 24) & 0xff,
		       (a >> 16) & 0xff, (a >> 8) & 0xff, (a) & 0xff);
	    }
	}
    }
}

/*
 * Read each entry in the database:
 * Record what type of entry it is and its address in the record array.
 * Remember what the maximum volume id we found is and check against the header.
 */
void
ReadAllEntries(struct vlheader *header)
{
    afs_int32 type, rindex, i, j, e;
    int freecount = 0, mhcount = 0, vlcount = 0;
    int rwcount = 0, rocount = 0, bkcount = 0;
    struct nvlentry vlentry;
    afs_uint32 addr;
    afs_uint32 entrysize = 0;
    afs_uint32 maxvolid = 0;

    if (verbose) quiet_println("Read each entry in the database\n");
    for (addr = header->vital_header.headersize;
	 addr < header->vital_header.eofPtr; addr += entrysize) {

	/* Remember the highest volume id */
	readentry(addr, &vlentry, &type);
	if (type == VL) {
	    if (!(vlentry.flags & VLF_RWEXISTS))
		log_error(VLDB_CHECK_WARNING,"VLDB_CHECK_WARNING: VLDB entry '%s' has no RW volume\n",
		       vlentry.name);

	    for (i = 0; i < MAXTYPES; i++)
		if (maxvolid < vlentry.volumeId[i])
		    maxvolid = vlentry.volumeId[i];

	    e = 1;
	    for (j = 0; j < NMAXNSERVERS; j++) {
		if (vlentry.serverNumber[j] == 255)
		    continue;
		if (vlentry.serverFlags[j] & (VLSF_ROVOL | VLSF_NEWREPSITE)) {
		    rocount++;
		    continue;
		}
		if (vlentry.serverFlags[j] & VLSF_RWVOL) {
		    rwcount++;
		    if (vlentry.flags & VLF_BACKEXISTS)
			bkcount++;
		    continue;
		}
		if (!vlentry.serverFlags[j]) {
		    /*e = 0;*/
		    continue;
 		}
		if (e) {
		   log_error
			(VLDB_CHECK_ERROR,"VLDB entry '%s' contains an unknown RW/RO index serverFlag\n",
			 vlentry.name);
		    e = 0;
		}
		quiet_println
		    ("   index %d : serverNumber %d : serverPartition %d : serverFlag %d\n",
		     j, vlentry.serverNumber[j], vlentry.serverPartition[j],
		     vlentry.serverFlags[j]);
	    }
	}

	rindex = addr / sizeof(vlentry);
	if (record[rindex].type) {
	    log_error(VLDB_CHECK_ERROR,"INTERNAL VLDB_CHECK_ERROR: record holder %d already in use\n",
		   rindex);
	    return;
	}
	record[rindex].addr = addr;
	record[rindex].type = type;

	/* Determine entrysize and keep count */
	if (type == VL) {
	    entrysize = sizeof(vlentry);
	    vlcount++;
	} else if (type == FR) {
	    entrysize = sizeof(vlentry);
	    freecount++;
	} else if (type == MH) {
	    entrysize = VL_ADDREXTBLK_SIZE;
	    mhcount++;
	} else {
	    log_error(VLDB_CHECK_ERROR, "Unknown entry at %u. Aborting\n", addr);
	    break;
	}
    }
    if (verbose) {
	quiet_println("Found %d entries, %d free entries, %d multihomed blocks\n",
	       vlcount, freecount, mhcount);
	quiet_println("Found %d RW volumes, %d BK volumes, %d RO volumes\n", rwcount,
	       bkcount, rocount);
    }

    /* Check the maxmimum volume id in the header */
    if (maxvolid != header->vital_header.MaxVolumeId - 1)
	quiet_println
	    ("Header's maximum volume id is %u and largest id found in VLDB is %u\n",
	     header->vital_header.MaxVolumeId, maxvolid);
}

/*
 * Follow each Name hash bucket marking it as read in the record array.
 * Record we found it in the name hash within the record array.
 * Check that the name is hashed correctly.
 */
void
FollowNameHash(struct vlheader *header)
{
    int count = 0, longest = 0, shortest = -1, chainlength;
    struct nvlentry vlentry;
    afs_uint32 addr;
    afs_int32 i, type, rindex;

    /* Now follow the Name Hash Table */
    if (verbose) quiet_println("Check Volume Name Hash\n");
    for (i = 0; i < HASHSIZE; i++) {
	chainlength = 0;

	if (!validVolumeAddr(header->VolnameHash[i])) {
	    log_error(VLDB_CHECK_ERROR,"Name Hash %d: Bad entry %u is out of range\n",
		      i, header->VolnameHash[i]);
	    continue;
	}

	for (addr = header->VolnameHash[i]; addr; addr = vlentry.nextNameHash) {
	    readentry(addr, &vlentry, &type);
	    if (type != VL) {
		log_error(VLDB_CHECK_ERROR,"Name Hash %d: Bad entry at %u: Not a valid vlentry\n",
		       i, addr);
		continue;
	    }

	    rindex = ADDR(addr);

	    /*
	     * we know that the address is valid because we
	     * checked it either above or below
	     */
	    if (record[rindex].addr != addr && record[rindex].addr) {
	        log_error
		    (VLDB_CHECK_ERROR,"INTERNAL VLDB_CHECK_ERROR: addresses %ld and %u use same record slot %d\n",
		     record[rindex].addr, addr, rindex);
	    }
	    if (record[rindex].type & NH) {
	        log_error
		    (VLDB_CHECK_ERROR,"Name Hash %d: Bad entry '%s': Already in the name hash\n",
		     i, vlentry.name);
		record[rindex].type |= MULTN;
		break;
	    }

	    if (!validVolumeAddr(vlentry.nextNameHash)) {
		log_error(VLDB_CHECK_ERROR,"Name Hash forward link of '%s' is out of range\n",
			  vlentry.name);
		record[rindex].type |= MULTN;
		break;
	    }

	    record[rindex].type |= NH;
	    record[rindex].type |= REFN;

	    chainlength++;
	    count++;

	    /* Hash the name and check if in correct hash table */
	    if (NameHash(vlentry.name) != i) {
	        log_error
		    (VLDB_CHECK_ERROR,"Name Hash %d: Bad entry '%s': Incorrect name hash chain (should be in %d)\n",
		     i, vlentry.name, NameHash(vlentry.name));
		record[rindex].type |= MULTN;
	    }
	}
	if (chainlength > longest)
	    longest = chainlength;
	if ((shortest == -1) || (chainlength < shortest))
	    shortest = chainlength;
    }
    if (verbose) {
	quiet_println
	    ("%d entries in name hash, longest is %d, shortest is %d, average length is %f\n",
	     count, longest, shortest, ((float)count / (float)HASHSIZE));
    }
    return;
}

/*
 * Follow the ID hash chains for the RW, RO, and BK hash tables.
 * Record we found it in the id hash within the record array.
 * Check that the ID is hashed correctly.
 */
void
FollowIdHash(struct vlheader *header)
{
    int count = 0, longest = 0, shortest = -1, chainlength;
    struct nvlentry vlentry;
    afs_uint32 addr;
    afs_int32 i, j, hash, type, rindex, ref, badref, badhash;

    /* Now follow the RW, RO, and BK Hash Tables */
    if (verbose) quiet_println("Check RW, RO, and BK id Hashes\n");
    for (i = 0; i < MAXTYPES; i++) {
	hash = ((i == 0) ? RWH : ((i == 1) ? ROH : BKH));
	ref = ((i == 0) ? REFRW : ((i == 1) ? REFRO : REFBK));
	badref = ((i == 0) ? MULTRW : ((i == 1) ? MULTRO : MULTBK));
	badhash = ((i == 0) ? MULTRW : ((i == 1) ? MULTRO : MULTBK));
	count = longest = 0;
	shortest = -1;

	for (j = 0; j < HASHSIZE; j++) {
	    chainlength = 0;
	    if (!validVolumeAddr(header->VolidHash[i][j])) {
		log_error(VLDB_CHECK_ERROR,"%s Hash %d: Bad entry %u is out of range\n",
			  vtype(i), j, header->VolidHash[i][j]);
		continue;
	    }

	    for (addr = header->VolidHash[i][j]; addr;
		 addr = vlentry.nextIdHash[i]) {
		readentry(addr, &vlentry, &type);
		if (type != VL) {
		    log_error
			(VLDB_CHECK_ERROR,"%s Id Hash %d: Bad entry at %u: Not a valid vlentry\n",
			 vtype(i), j, addr);
		    continue;
		}

		rindex = ADDR(addr);
		if (record[rindex].addr != addr && record[rindex].addr) {
		    log_error
			(VLDB_CHECK_ERROR,"INTERNAL VLDB_CHECK_ERROR: addresses %ld and %u use same record slot %d\n",
			 record[rindex].addr, addr, rindex);
		}
		if (record[rindex].type & hash) {
		    log_error
			(VLDB_CHECK_ERROR,"%s Id Hash %d: Bad entry '%s': Already in the hash table\n",
			 vtype(i), j, vlentry.name);
		    record[rindex].type |= badref;
		    break;
		}

		if (!validVolumeAddr(vlentry.nextIdHash[i])) {
		    log_error(VLDB_CHECK_ERROR,"%s Id Hash forward link of '%s' is out of range\n",
			      vtype(i), vlentry.name);
		    record[rindex].type |= badref;
		    break;
		}

		record[rindex].type |= hash;
		record[rindex].type |= ref;

		chainlength++;
		count++;

		/* Hash the id and check if in correct hash table */
		if (IdHash(vlentry.volumeId[i]) != j) {
		   log_error
			(VLDB_CHECK_ERROR,"%s Id Hash %d: Bad entry '%s': Incorrect Id hash chain (should be in %d)\n",
			 vtype(i), j, vlentry.name,
			 IdHash(vlentry.volumeId[i]));
		    record[rindex].type |= badhash;
		}
	    }

	    if (chainlength > longest)
		longest = chainlength;
	    if ((shortest == -1) || (chainlength < shortest))
		shortest = chainlength;
	}
	if (verbose) {
	    quiet_println
		("%d entries in %s hash, longest is %d, shortest is %d, average length is %f\n",
		 count, vtype(i), longest, shortest,((float)count / (float)HASHSIZE));
	}
    }
    return;
}

/*
 * Follow the free chain.
 * Record we found it in the free chain within the record array.
 */
void
FollowFreeChain(struct vlheader *header)
{
    afs_int32 count = 0;
    struct nvlentry vlentry;
    afs_uint32 addr;
    afs_int32 type, rindex;

    /* Now follow the Free Chain */
    if (verbose) quiet_println("Check Volume Free Chain\n");
    for (addr = header->vital_header.freePtr; addr;
	 addr = vlentry.nextIdHash[0]) {
	readentry(addr, &vlentry, &type);
	if (type != FR) {
	   log_error
		(VLDB_CHECK_ERROR,"Free Chain %d: Bad entry at %u: Not a valid free vlentry (0x%x)\n",
		 count, addr, type);
	    continue;
	}

	rindex = addr / sizeof(vlentry);
	if (record[rindex].addr != addr && record[rindex].addr) {
	   log_error
		(VLDB_CHECK_ERROR,"INTERNAL VLDB_CHECK_ERROR: addresses %u and %ld use same record slot %d\n",
		 record[rindex].addr, addr, rindex);
	}
	if (record[rindex].type & FRC) {
	    log_error(VLDB_CHECK_ERROR,"Free Chain: Bad entry at %u: Already in the free chain\n",
		   addr);
	    break;
	}
	record[rindex].type |= FRC;

	count++;
    }
    if (verbose)
     quiet_println("%d entries on free chain\n", count);
    return;
}

/*
 * Read each multihomed block and mark it as found in the record.
 * Read each entry in each multihomed block and mark the serveraddrs
 * array with the number of ip addresses found for this entry.
 *
 * Then read the IpMappedAddr array in the header.
 * Verify that multihomed entries base and index are valid and points to
 * a good multhomed entry.
 * Mark the serveraddrs array with 1 ip address for regular entries.
 *
 * By the end, the severaddrs array will have a 0 if the entry has no
 * IP addresses in it or the count of the number of IP addresses.
 *
 * The code does not verify if there are duplicate IP addresses in the
 * list. The vlserver does this when a fileserver registeres itself.
 */
void
CheckIpAddrs(struct vlheader *header)
{
    int mhblocks = 0;
    afs_int32 i, j, m, rindex;
    afs_int32 mhentries, regentries;
    afs_uint32 caddrs[VL_MAX_ADDREXTBLKS];
    char mhblock[VL_ADDREXTBLK_SIZE];
    struct extentaddr *MHblock = (struct extentaddr *)mhblock;
    struct extentaddr *e;
    int ipindex, ipaddrs;
    afsUUID nulluuid;

    memset(&nulluuid, 0, sizeof(nulluuid));

    if (verbose)
	quiet_println("Check Multihomed blocks\n");

    if (header->SIT) {
	/* Read the first MH block and from it, gather the
	 * addresses of all the mh blocks.
	 */
	readMH(header->SIT, MHblock);
	if (MHblock->ex_flags != VLCONTBLOCK) {
	   log_error
		(VLDB_CHECK_ERROR,"Multihomed Block 0: Bad entry at %u: Not a valid multihomed block\n",
		 header->SIT);
	}

	for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    caddrs[i] = MHblock->ex_contaddrs[i];
	}

	if (header->SIT != caddrs[0]) {
	   log_error
		(VLDB_CHECK_ERROR,"MH block does not point to self %u in header, %u in block\n",
		 header->SIT, caddrs[0]);
	}

	/* Now read each MH block and record it in the record array */
	for (i = 0; i < VL_MAX_ADDREXTBLKS; i++) {
	    if (!caddrs[i])
		continue;

	    readMH(caddrs[i], MHblock);
	    if (MHblock->ex_flags != VLCONTBLOCK) {
	        log_error
		    (VLDB_CHECK_ERROR,"Multihomed Block 0: Bad entry at %u: Not a valid multihomed block\n",
		     header->SIT);
	    }

	    rindex = caddrs[i] / sizeof(vlentry);
	    if (record[rindex].addr != caddrs[i] && record[rindex].addr) {
	        log_error
		    (VLDB_CHECK_ERROR,"INTERNAL VLDB_CHECK_ERROR: addresses %u and %u use same record slot %d\n",
		     record[rindex].addr, caddrs[i], rindex);
	    }
	    if (record[rindex].type & FRC) {
	        log_error
		    (VLDB_CHECK_ERROR,"MH Blocks Chain %d: Bad entry at %ld: Already a MH block\n",
		     i, record[rindex].addr);
		break;
	    }
	    record[rindex].type |= MHC;

	    mhblocks++;

	    /* Read each entry in a multihomed block.
	     * Find the pointer to the entry in the IpMappedAddr array and
	     * verify that the entry is good (has IP addresses in it).
	     */
	    mhentries = 0;
	    for (j = 1; j < VL_MHSRV_PERBLK; j++) {
		e = (struct extentaddr *)&(MHblock[j]);

		/* Search the IpMappedAddr array for the reference to this entry */
		for (ipindex = 0; ipindex < MAXSERVERID; ipindex++) {
		    if (((header->IpMappedAddr[ipindex] & 0xff000000) ==
			 0xff000000)
			&&
			(((header->
			   IpMappedAddr[ipindex] & 0x00ff0000) >> 16) == i)
			&& ((header->IpMappedAddr[ipindex] & 0x0000ffff) ==
			    j)) {
			break;
		    }
		}
		if (ipindex >= MAXSERVERID)
		    ipindex = -1;
		else
		    serveraddrs[ipindex] = -1;

		if (memcmp(&e->ex_hostuuid, &nulluuid, sizeof(afsUUID)) == 0) {
		    if (ipindex != -1) {
		        log_error
			    (VLDB_CHECK_ERROR,"Server Addrs index %d references null MH block %d, index %d\n",
			     ipindex, i, j);
			serveraddrs[ipindex] = 0;	/* avoids printing 2nd error below */
		    }
		    continue;
		}

		/* Step through each ip address and count the good addresses */
		ipaddrs = 0;
		for (m = 0; m < VL_MAXIPADDRS_PERMH; m++) {
		    if (e->ex_addrs[m])
			ipaddrs++;
		}

		/* If we found any good ip addresses, mark it in the serveraddrs record */
		if (ipaddrs) {
		    mhentries++;
		    if (ipindex == -1) {
		        log_error
			    (VLDB_CHECK_ERROR,"MH block %d, index %d: Not referenced by server addrs\n",
			     i, j);
		    } else {
			serveraddrs[ipindex] = ipaddrs;	/* It is good */
		    }
		}

		if (listservers && ipaddrs) {
		    quiet_println("MH block %d, index %d:", i, j);
		    for (m = 0; m < VL_MAXIPADDRS_PERMH; m++) {
			if (!e->ex_addrs[m])
			    continue;
			quiet_println(" %d.%d.%d.%d",
			       (e->ex_addrs[m] & 0xff000000) >> 24,
			       (e->ex_addrs[m] & 0x00ff0000) >> 16,
			       (e->ex_addrs[m] & 0x0000ff00) >> 8,
			       (e->ex_addrs[m] & 0x000000ff));
		    }
		    quiet_println("\n");
		}
	    }
/*
 *      if (mhentries != MHblock->ex_count) {
 *	   quiet_println("MH blocks says it has %d entries (found %d)\n",
 *		  MHblock->ex_count, mhentries);
 *	}
 */
	}
    }
    if (verbose)
	quiet_println("%d multihomed blocks\n", mhblocks);

    /* Check the server addresses */
    if (verbose)
	quiet_println("Check server addresses\n");
    mhentries = regentries = 0;
    for (i = 0; i <= MAXSERVERID; i++) {
	if (header->IpMappedAddr[i]) {
	    if ((header->IpMappedAddr[i] & 0xff000000) == 0xff000000) {
		mhentries++;
		if (((header->IpMappedAddr[i] & 0x00ff0000) >> 16) >
		    VL_MAX_ADDREXTBLKS)
		   log_error
			(VLDB_CHECK_ERROR,"IP Addr for entry %d: Multihome block is bad (%d)\n",
			 i, ((header->IpMappedAddr[i] & 0x00ff0000) >> 16));
		if (((header->IpMappedAddr[i] & 0x0000ffff) > VL_MHSRV_PERBLK)
		    || ((header->IpMappedAddr[i] & 0x0000ffff) < 1))
		    log_error
			(VLDB_CHECK_ERROR,"IP Addr for entry %d: Multihome index is bad (%d)\n",
			 i, (header->IpMappedAddr[i] & 0x0000ffff));
		if (serveraddrs[i] == -1) {
		    log_error
			(VLDB_CHECK_WARNING,"warning: IP Addr for entry %d: Multihome entry has no ip addresses\n",
			 i);
		    serveraddrs[i] = 0;
		}
		if (listservers) {
		    quiet_println("   Server ip addr %d = MH block %d, index %d\n",
			   i, (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
			   (header->IpMappedAddr[i] & 0x0000ffff));
		}
	    } else {
		regentries++;
		serveraddrs[i] = 1;	/* It is good */
		if (listservers) {
		    quiet_println("   Server ip addr %d = %d.%d.%d.%d\n", i,
			   (header->IpMappedAddr[i] & 0xff000000) >> 24,
			   (header->IpMappedAddr[i] & 0x00ff0000) >> 16,
			   (header->IpMappedAddr[i] & 0x0000ff00) >> 8,
			   (header->IpMappedAddr[i] & 0x000000ff));
		}
	    }
	}
    }
    if (verbose) {
	quiet_println("%d simple entries, %d multihomed entries, Total = %d\n",
	       regentries, mhentries, mhentries + regentries);
    }
    return;
}

char *
nameForAddr(afs_uint32 addr, int hashtype, afs_uint32 *hash, char *buffer)
{
    /*
     * We need to simplify the reporting, while retaining
     * legible messages.  This is a helper function.  The return address
     * is either a fixed char or the provided buffer - so don't use the
     * name after the valid lifetime of the buffer.
     */
    afs_int32 type;
    struct nvlentry entry;
    if (!addr) {
	/* Distinguished, invalid, hash */
	*hash = 0xFFFFFFFF;
	return "empty";
    } else if (!validVolumeAddr(addr)) {
	/* Different, invalid, hash */
	*hash = 0XFFFFFFFE;
	return "invalid";
    }
    readentry(addr, &entry, &type);
    if (VL != type) {
	*hash = 0XFFFFFFFE;
	return "invalid";
    }
    if (hashtype >= MAXTYPES) {
	*hash = NameHash(entry.name);
    } else {
	*hash = IdHash(entry.volumeId[hashtype]);
    }
    sprintf(buffer, "for '%s'", entry.name);
    return buffer;
}

void
reportHashChanges(struct vlheader *header, afs_uint32 oldnamehash[HASHSIZE], afs_uint32 oldidhash[MAXTYPES][HASHSIZE])
{
    int i, j;
    afs_uint32 oldhash, newhash;
    char oldNameBuffer[10 + VL_MAXNAMELEN];
    char newNameBuffer[10 + VL_MAXNAMELEN];
    char *oldname, *newname;
    /*
     * report hash changes
     */

    for (i = 0; i < HASHSIZE; i++) {
	if (oldnamehash[i] != header->VolnameHash[i]) {

	    oldname = nameForAddr(oldnamehash[i], MAXTYPES, &oldhash, oldNameBuffer);
	    newname = nameForAddr(header->VolnameHash[i], MAXTYPES, &newhash, newNameBuffer);
	    if (verbose || (oldhash != newhash)) {
		quiet_println("FIX: Name hash header at %d was %s, is now %s\n", i, oldname, newname);
	    }
	}
	for (j = 0; j < MAXTYPES; j++) {
	    if (oldidhash[j][i] != header->VolidHash[j][i]) {

		oldname = nameForAddr(oldidhash[j][i], j, &oldhash, oldNameBuffer);
		newname = nameForAddr(header->VolidHash[j][i], j, &newhash, newNameBuffer);
		if (verbose || (oldhash != newhash)) {
		    quiet_println("FIX: %s hash header at %d was %s, is now %s\n", vtype(j), i, oldname, newname);
		}
	    }
	}
    }
}

int
WorkerBee(struct cmd_syndesc *as, void *arock)
{
    char *dbfile;
    afs_int32 type;
    struct vlheader header;
    struct nvlentry vlentry, vlentry2;
    int i, j;
    afs_uint32 oldnamehash[HASHSIZE];
    afs_uint32 oldidhash[MAXTYPES][HASHSIZE];

    error_level = 0;  /*  start clean with no error status */
    dbfile = as->parms[0].items->data;	/* -database */
    listuheader = (as->parms[1].items ? 1 : 0);	/* -uheader  */
    listheader = (as->parms[2].items ? 1 : 0);	/* -vheader  */
    listservers = (as->parms[3].items ? 1 : 0);	/* -servers  */
    listentries = (as->parms[4].items ? 1 : 0);	/* -entries  */
    verbose = (as->parms[5].items ? 1 : 0);	/* -verbose  */
    quiet = (as->parms[6].items ? 1 : 0);  /* -quiet */
    fix = (as->parms[7].items ? 1 : 0);    /* -fix  */

    /* sanity check */
    if (quiet && (verbose || listuheader || listheader ||listservers \
                || listentries)) {
        log_error(VLDB_CHECK_FATAL," -quiet cannot be used other display flags\n");
        return VLDB_CHECK_FATAL;
    }


    /* open the vldb database file */
    fd = open(dbfile, (fix > 0)?O_RDWR:O_RDONLY, 0);
    if (fd < 0) {
	log_error(VLDB_CHECK_FATAL,"can't open file '%s'. error = %d\n", dbfile, errno);
	return 0;
    }

    /* read the ubik header and the vldb database header */
    readUbikHeader();
    readheader(&header);
    if (header.vital_header.vldbversion < 3) {
	log_error(VLDB_CHECK_FATAL,"does not support vldb with version less than 3\n");
	return VLDB_CHECK_FATAL;
    }

    maxentries = (header.vital_header.eofPtr / sizeof(vlentry)) + 1;
    record = (struct er *)malloc(maxentries * sizeof(struct er));
    memset(record, 0, (maxentries * sizeof(struct er)));
    memset(serveraddrs, 0, sizeof(serveraddrs));

    /* Will fill in the record array of entries it found */
    ReadAllEntries(&header);
    listentries = 0;		/* Listed all the entries */

    /* Check the multihomed blocks for valid entries as well as
     * the IpMappedAddrs array in the header for valid entries.
     */
    CheckIpAddrs(&header);

    /* Follow the hash tables */
    FollowNameHash(&header);
    FollowIdHash(&header);

    /* Follow the chain of free entries */
    FollowFreeChain(&header);

    /* Now check the record we have been keeping for inconsistencies
     * For valid vlentries, also check that the server we point to is
     * valid (the serveraddrs array).
     */
    if (verbose)
	quiet_println("Verify each volume entry\n");
    for (i = 0; i < maxentries; i++) {
	int nextp = 0;
	int reft = 0;
	int hash = 0;
        int nexthash = 0;
	afs_uint32 *nextpp = NULL;
	char *which = NULL;

	if (record[i].type == 0)
	    continue;

	/* If a vlentry, verify that its name is valid, its name and ids are
	 * on the hash chains, and its server numbers are good.
	 */
	if (record[i].type & VL) {
	    int foundbad = 0;
	    int foundbroken = 0;
	    char volidbuf[256];

	    readentry(record[i].addr, &vlentry, &type);

	    if (InvalidVolname(vlentry.name))
		log_error(VLDB_CHECK_ERROR,"Volume '%s' at addr %ld has an invalid name\n",
		       vlentry.name, record[i].addr);

	    if (!(record[i].type & NH)) {
		nextp = ADDR(vlentry.nextNameHash);
		reft = REFN;
		hash = NameHash(vlentry.name);
		nextpp = &vlentry.nextNameHash;
		which = "name";
		volidbuf[0]='\0';
		foundbad = 1;
	    }

	    if (vlentry.volumeId[0] && !(record[i].type & RWH)) {
		nextp = ADDR(vlentry.nextIdHash[0]);
		reft = REFRW;
		hash = IdHash(vlentry.volumeId[0]);
		nextpp = &(vlentry.nextIdHash[0]);
		which = "RW";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[0]);
		foundbad = 1;
	    }

	    if (vlentry.volumeId[1] && !(record[i].type & ROH)) {
		nextp = ADDR(vlentry.nextIdHash[1]);
		reft = REFRO;
		hash = IdHash(vlentry.volumeId[1]);
		nextpp = &(vlentry.nextIdHash[1]);
		which = "RO";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[1]);
		foundbad = 1;
	    }

	    if (vlentry.volumeId[2] && !(record[i].type & BKH)) {
		nextp = ADDR(vlentry.nextIdHash[2]);
		reft = REFBK;
		hash = IdHash(vlentry.volumeId[2]);
		nextpp = &(vlentry.nextIdHash[2]);
		which = "BK";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[2]);
		foundbad = 1;
	    }

	    if (!validVolumeAddr(vlentry.nextNameHash) ||
		record[ADDR(vlentry.nextNameHash)].type & MULTN) {
		nextp = ADDR(vlentry.nextNameHash);
		reft = REFN;
		hash = NameHash(vlentry.name);
		nextpp = &vlentry.nextNameHash;
		which = "name";
		volidbuf[0]='\0';
		if (validVolumeAddr(vlentry.nextNameHash)) {
		    readentry(vlentry.nextNameHash, &vlentry2, &type);
		    nexthash = NameHash(vlentry2.name);
		} else {
		    nexthash = 0xFFFFFFFF;
		}
		if (hash != nexthash)
		    foundbroken = 1;
	    }

	    if (!validVolumeAddr(vlentry.nextIdHash[0]) ||
		record[ADDR(vlentry.nextIdHash[0])].type & MULTRW) {
		nextp = ADDR(vlentry.nextIdHash[0]);
		reft = REFRW;
		hash = IdHash(vlentry.volumeId[0]);
		nextpp = &(vlentry.nextIdHash[0]);
		which = "RW";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[0]);
		if (validVolumeAddr(vlentry.nextIdHash[0])) {
		    readentry(vlentry.nextIdHash[0], &vlentry2, &type);
		    nexthash = IdHash(vlentry2.volumeId[0]);
		} else {
		    nexthash = 0xFFFFFFFF;
		}
		if (hash != nexthash)
		    foundbroken = 1;
	    }

	    if (!validVolumeAddr(vlentry.nextIdHash[1]) ||
		record[ADDR(vlentry.nextIdHash[1])].type & MULTRO) {
		nextp = ADDR(vlentry.nextIdHash[1]);
		reft = REFRO;
		hash = IdHash(vlentry.volumeId[1]);
		nextpp = &(vlentry.nextIdHash[1]);
		which = "RO";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[1]);
		if (validVolumeAddr(vlentry.nextIdHash[1])) {
		    readentry(vlentry.nextIdHash[1], &vlentry2, &type);
		    nexthash = IdHash(vlentry2.volumeId[1]);
		} else {
		    nexthash = 0xFFFFFFFF;
		}
		if (hash != nexthash)
		    foundbroken = 1;
	    }

	    if (!validVolumeAddr(vlentry.nextIdHash[2]) ||
		record[ADDR(vlentry.nextIdHash[2])].type & MULTBK) {
		nextp = ADDR(vlentry.nextIdHash[2]);
		reft = REFBK;
		hash = IdHash(vlentry.volumeId[2]);
		nextpp = &(vlentry.nextIdHash[2]);
		which = "BK";
		sprintf(volidbuf, "id %u ", vlentry.volumeId[2]);
		if (validVolumeAddr(vlentry.nextIdHash[2])) {
		    readentry(vlentry.nextIdHash[2], &vlentry2, &type);
		    nexthash = IdHash(vlentry2.volumeId[2]);
		} else {
		    nexthash = 0xFFFFFFFF;
		}
		if (hash != nexthash)
		    foundbroken = 1;
	    }

	    if (foundbroken) {
		log_error(VLDB_CHECK_ERROR, "%d: Volume '%s' %s forward link in %s hash chain is broken (hash %d != %d)\n", i,
			  vlentry.name, volidbuf, which, hash, nexthash);
	    } else if (foundbad) {
		log_error(VLDB_CHECK_ERROR, "%d: Volume '%s' %snot found in %s hash %d\n", i,
		       vlentry.name, volidbuf, which, hash);
	    }

	    for (j = 0; j < NMAXNSERVERS; j++) {
		if ((vlentry.serverNumber[j] != 255)
		    && (serveraddrs[vlentry.serverNumber[j]] == 0)) {
		   log_error
			(VLDB_CHECK_ERROR,"Volume '%s', index %d points to empty server entry %d\n",
			 vlentry.name, j, vlentry.serverNumber[j]);
		}
	    }

	    if (record[i].type & 0xffff0f00)
	        log_error
		    (VLDB_CHECK_ERROR,"Volume '%s' id %u also found on other chains (0x%x)\n",
		     vlentry.name, vlentry.volumeId[0], record[i].type);

	    /* A free entry */
	} else if (record[i].type & FR) {
	    if (!(record[i].type & FRC))
		log_error(VLDB_CHECK_ERROR,"Free vlentry at %ld not on free chain\n",
		       record[i].addr);

	    if (record[i].type & 0xfffffdf0)
	        log_error
		    (VLDB_CHECK_ERROR,"Free vlentry at %ld also found on other chains (0x%x)\n",
		     record[i].addr, record[i].type);

	    /* A multihomed entry */
	} else if (record[i].type & MH) {
	    if (!(record[i].type & MHC))
		log_error(VLDB_CHECK_ERROR,"Multihomed block at %ld is orphaned\n",
		       record[i].addr);

	    if (record[i].type & 0xfffffef0)
	        log_error
		    (VLDB_CHECK_ERROR,"Multihomed block at %ld also found on other chains (0x%x)\n",
		     record[i].addr, record[i].type);

	} else {
	    log_error(VLDB_CHECK_ERROR,"Unknown entry type at %u (0x%x)\n", record[i].addr,
		   record[i].type);
	}
    }

    if (fix) {
	/*
	 * If we are fixing we will rebuild all the hash lists from the ground up
	 */
	memcpy(oldnamehash, header.VolnameHash, sizeof(oldnamehash));
	memset(header.VolnameHash, 0, sizeof(header.VolnameHash));

	memcpy(oldidhash, header.VolidHash, sizeof(oldidhash));
	memset(header.VolidHash, 0, sizeof(header.VolidHash));
	quiet_println("Rebuilding %u entries\n", maxentries);
    } else {
	quiet_println("Scanning %u entries for possible repairs\n", maxentries);
    }
    for (i = 0; i < maxentries; i++) {
	afs_uint32 hash;
	if (record[i].type & VL) {
	    readentry(record[i].addr, &vlentry, &type);
	    if (!(record[i].type & REFN)) {
		log_error(VLDB_CHECK_ERROR,"%d: Record %ld (type 0x%x) not in a name chain\n", i,
		       record[i].addr, record[i].type);
	    }
	    if (vlentry.volumeId[0] && !(record[i].type & REFRW)) {
		log_error(VLDB_CHECK_ERROR,"%d: Record %ld (type 0x%x) not in a RW chain\n", i,
		       record[i].addr, record[i].type);
	    }
	    if (vlentry.volumeId[1] && !(record[i].type & REFRO)) {
		log_error(VLDB_CHECK_ERROR,"%d: Record %ld (type 0x%x) not in a RO chain\n", i,
		       record[i].addr, record[i].type);
	    }
	    if (vlentry.volumeId[2] && !(record[i].type & REFBK)) {
		log_error(VLDB_CHECK_ERROR,"%d: Record %ld (type 0x%x) not in a BK chain\n", i,
		       record[i].addr, record[i].type);
	    }
	    if (fix) {
		afs_uint32 oldhash, newhash;
		char oldNameBuffer[10 + VL_MAXNAMELEN];
		char newNameBuffer[10 + VL_MAXNAMELEN];
		char *oldname, *newname;

		/*
		 * Put the current hash table contexts into our 'next'
		 * and our address into the hash table.
		 */
		hash = NameHash(vlentry.name);

		if (vlentry.nextNameHash != header.VolnameHash[hash]) {
		    oldname = nameForAddr(vlentry.nextNameHash, MAXTYPES, &oldhash, oldNameBuffer);
		    newname = nameForAddr(header.VolnameHash[hash], MAXTYPES, &newhash, newNameBuffer);
		    if (verbose || ((oldhash != newhash) &&
                                    (0 != vlentry.nextNameHash) &&
                                    (0 != header.VolnameHash[hash]))) {
			/*
			 * That is, only report if we are verbose
			 * or the hash is changing (and one side wasn't NULL
			 */
			quiet_println("FIX: Name hash link for '%s' was %s, is now %s\n",
                              vlentry.name, oldname, newname);
		    }
		}

		vlentry.nextNameHash = header.VolnameHash[hash];
		header.VolnameHash[hash] = record[i].addr;

		for (j = 0; j < MAXTYPES; j++) {

		    if (0 == vlentry.volumeId[j]) {
			/*
			 * No volume of that type.  Continue
			 */
			continue;
		    }
		    hash = IdHash(vlentry.volumeId[j]);

		    if (vlentry.nextIdHash[j] != header.VolidHash[j][hash]) {
			oldname = nameForAddr(vlentry.nextIdHash[j], j, &oldhash, oldNameBuffer);
			newname = nameForAddr(header.VolidHash[j][hash], j, &newhash, newNameBuffer);
			if (verbose || ((oldhash != newhash) &&
					(0 != vlentry.nextIdHash[j]) &&
					(0 != header.VolidHash[j][hash]))) {
			    quiet_println("FIX: %s hash link for '%s' was %s, is now %s\n",
					  vtype(j), vlentry.name, oldname, newname);
			}
		    }

		    vlentry.nextIdHash[j] = header.VolidHash[j][hash];
		    header.VolidHash[j][hash] = record[i].addr;
		}
		writeentry(record[i].addr, &vlentry);
	    }
	}
    }
    if (fix) {
	reportHashChanges(&header, oldnamehash, oldidhash);
	writeheader(&header);
    }

    close(fd);

    return error_level;
}

int
main(int argc, char **argv)
{
    struct cmd_syndesc *ts;

    setlinebuf(stdout);

    ts = cmd_CreateSyntax(NULL, WorkerBee, NULL, "vldb check");
    cmd_AddParm(ts, "-database", CMD_SINGLE, CMD_REQUIRED, "vldb_file");
    cmd_AddParm(ts, "-uheader", CMD_FLAG, CMD_OPTIONAL,
		"Display UBIK header");
    cmd_AddParm(ts, "-vheader", CMD_FLAG, CMD_OPTIONAL,
		"Display VLDB header");
    cmd_AddParm(ts, "-servers", CMD_FLAG, CMD_OPTIONAL,
		"Display server list");
    cmd_AddParm(ts, "-entries", CMD_FLAG, CMD_OPTIONAL, "Display entries");
    cmd_AddParm(ts, "-verbose", CMD_FLAG, CMD_OPTIONAL, "verbose");
    cmd_AddParm(ts, "-quiet", CMD_FLAG, CMD_OPTIONAL, "quiet");
    cmd_AddParm(ts, "-fix", CMD_FLAG, CMD_OPTIONAL, "attempt to patch the database (potentially dangerous)");

    return cmd_Dispatch(argc, argv);
}
