/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This is the implementation of the page cache subsystem or "pager".
** 
** The pager is used to access a database disk file.  It implements
** atomic commit and rollback through the use of a journal file that
** is separate from the database file.  The pager also implements file
** locking to prevent two processes from writing the same database
** file simultaneously, or one process from reading the database while
** another is writing.
**
** @(#) $Id: pager.c,v 1.580 2009/04/11 16:27:50 drh Exp $
*/
#ifndef SQLITE_OMIT_DISKIO
#include "sqliteInt.h"

/*
** Macros for troubleshooting.  Normally turned off
*/
#if 0
int sqlite3PagerTrace=1;  /* True to enable tracing */
#define sqlite3DebugPrintf printf
#define PAGERTRACE(X)     if( sqlite3PagerTrace ){ sqlite3DebugPrintf X; }
#else
#define PAGERTRACE(X)
#endif

/*
** The following two macros are used within the PAGERTRACE() macros above
** to print out file-descriptors. 
**
** PAGERID() takes a pointer to a Pager struct as its argument. The
** associated file-descriptor is returned. FILEHANDLEID() takes an sqlite3_file
** struct as its argument.
*/
#define PAGERID(p) ((int)(p->fd))
#define FILEHANDLEID(fd) ((int)fd)

/*
** The page cache as a whole is always in one of the following
** states:
**
**   PAGER_UNLOCK        The page cache is not currently reading or 
**                       writing the database file.  There is no
**                       data held in memory.  This is the initial
**                       state.
**
**   PAGER_SHARED        The page cache is reading the database.
**                       Writing is not permitted.  There can be
**                       multiple readers accessing the same database
**                       file at the same time.
**
**   PAGER_RESERVED      This process has reserved the database for writing
**                       but has not yet made any changes.  Only one process
**                       at a time can reserve the database.  The original
**                       database file has not been modified so other
**                       processes may still be reading the on-disk
**                       database file.
**
**   PAGER_EXCLUSIVE     The page cache is writing the database.
**                       Access is exclusive.  No other processes or
**                       threads can be reading or writing while one
**                       process is writing.
**
**   PAGER_SYNCED        The pager moves to this state from PAGER_EXCLUSIVE
**                       after all dirty pages have been written to the
**                       database file and the file has been synced to
**                       disk. All that remains to do is to remove or
**                       truncate the journal file and the transaction 
**                       will be committed.
**
** The page cache comes up in PAGER_UNLOCK.  The first time a
** sqlite3PagerGet() occurs, the state transitions to PAGER_SHARED.
** After all pages have been released using sqlite_page_unref(),
** the state transitions back to PAGER_UNLOCK.  The first time
** that sqlite3PagerWrite() is called, the state transitions to
** PAGER_RESERVED.  (Note that sqlite3PagerWrite() can only be
** called on an outstanding page which means that the pager must
** be in PAGER_SHARED before it transitions to PAGER_RESERVED.)
** PAGER_RESERVED means that there is an open rollback journal.
** The transition to PAGER_EXCLUSIVE occurs before any changes
** are made to the database file, though writes to the rollback
** journal occurs with just PAGER_RESERVED.  After an sqlite3PagerRollback()
** or sqlite3PagerCommitPhaseTwo(), the state can go back to PAGER_SHARED,
** or it can stay at PAGER_EXCLUSIVE if we are in exclusive access mode.
*/
#define PAGER_UNLOCK      0
#define PAGER_SHARED      1   /* same as SHARED_LOCK */
#define PAGER_RESERVED    2   /* same as RESERVED_LOCK */
#define PAGER_EXCLUSIVE   4   /* same as EXCLUSIVE_LOCK */
#define PAGER_SYNCED      5

/*
** A macro used for invoking the codec if there is one
*/
#ifdef SQLITE_HAS_CODEC
# define CODEC1(P,D,N,X) if( P->xCodec!=0 ){ P->xCodec(P->pCodecArg,D,N,X); }
# define CODEC2(P,D,N,X) ((char*)(P->xCodec!=0?P->xCodec(P->pCodecArg,D,N,X):D))
#else
# define CODEC1(P,D,N,X) /* NO-OP */
# define CODEC2(P,D,N,X) ((char*)D)
#endif

/*
** The maximum allowed sector size. 16MB. If the xSectorsize() method 
** returns a value larger than this, then MAX_SECTOR_SIZE is used instead.
** This could conceivably cause corruption following a power failure on
** such a system. This is currently an undocumented limit.
*/
#define MAX_SECTOR_SIZE 0x0100000

/*
** An instance of the following structure is allocated for each active
** savepoint and statement transaction in the system. All such structures
** are stored in the Pager.aSavepoint[] array, which is allocated and
** resized using sqlite3Realloc().
**
** When a savepoint is created, the PagerSavepoint.iHdrOffset field is
** set to 0. If a journal-header is written into the main journal while
** the savepoint is active, then iHdrOffset is set to the byte offset 
** immediately following the last journal record written into the main
** journal before the journal-header. This is required during savepoint
** rollback (see pagerPlaybackSavepoint()).
*/
typedef struct PagerSavepoint PagerSavepoint;
struct PagerSavepoint {
  i64 iOffset;                 /* Starting offset in main journal */
  i64 iHdrOffset;              /* See above */
  Bitvec *pInSavepoint;        /* Set of pages in this savepoint */
  Pgno nOrig;                  /* Original number of pages in file */
  Pgno iSubRec;                /* Index of first record in sub-journal */
};

/*
** A open page cache is an instance of the following structure.
**
** errCode
**
**   Pager.errCode may be set to SQLITE_IOERR, SQLITE_CORRUPT, or
**   or SQLITE_FULL. Once one of the first three errors occurs, it persists
**   and is returned as the result of every major pager API call.  The
**   SQLITE_FULL return code is slightly different. It persists only until the
**   next successful rollback is performed on the pager cache. Also,
**   SQLITE_FULL does not affect the sqlite3PagerGet() and sqlite3PagerLookup()
**   APIs, they may still be used successfully.
**
** dbSizeValid, dbSize, dbOrigSize, dbFileSize
**
**   Managing the size of the database file in pages is a little complicated.
**   The variable Pager.dbSize contains the number of pages that the database
**   image currently contains. As the database image grows or shrinks this
**   variable is updated. The variable Pager.dbFileSize contains the number
**   of pages in the database file. This may be different from Pager.dbSize
**   if some pages have been appended to the database image but not yet written
**   out from the cache to the actual file on disk. Or if the image has been
**   truncated by an incremental-vacuum operation. The Pager.dbOrigSize variable
**   contains the number of pages in the database image when the current
**   transaction was opened. The contents of all three of these variables is
**   only guaranteed to be correct if the boolean Pager.dbSizeValid is true.
**
**   TODO: Under what conditions is dbSizeValid set? Cleared?
**
** changeCountDone
**
**   This boolean variable is used to make sure that the change-counter 
**   (the 4-byte header field at byte offset 24 of the database file) is 
**   not updated more often than necessary. 
**
**   It is set to true when the change-counter field is updated, which 
**   can only happen if an exclusive lock is held on the database file.
**   It is cleared (set to false) whenever an exclusive lock is 
**   relinquished on the database file. Each time a transaction is committed,
**   The changeCountDone flag is inspected. If it is true, the work of
**   updating the change-counter is omitted for the current transaction.
**
**   This mechanism means that when running in exclusive mode, a connection 
**   need only update the change-counter once, for the first transaction
**   committed.
**
** dbModified
**
**   The dbModified flag is set whenever a database page is dirtied.
**   It is cleared at the end of each transaction.
**
**   It is used when committing or otherwise ending a transaction. If
**   the dbModified flag is clear then less work has to be done.
**
** journalStarted
**
**   This flag is set whenever the the main journal is synced. 
**
**   The point of this flag is that it must be set after the 
**   first journal header in a journal file has been synced to disk.
**   After this has happened, new pages appended to the database 
**   do not need the PGHDR_NEED_SYNC flag set, as they do not need
**   to wait for a journal sync before they can be written out to
**   the database file (see function pager_write()).
**   
** setMaster
**
**   This variable is used to ensure that the master journal file name
**   (if any) is only written into the journal file once.
**
**   When committing a transaction, the master journal file name (if any)
**   may be written into the journal file while the pager is still in
**   PAGER_RESERVED state (see CommitPhaseOne() for the action). It
**   then attempts to upgrade to an exclusive lock. If this attempt
**   fails, then SQLITE_BUSY may be returned to the user and the user
**   may attempt to commit the transaction again later (calling
**   CommitPhaseOne() again). This flag is used to ensure that the 
**   master journal name is only written to the journal file the first
**   time CommitPhaseOne() is called.
**
** doNotSync
**
**   This variable is set and cleared by sqlite3PagerWrite().
**
** needSync
**
**   TODO: It might be easier to set this variable in writeJournalHdr()
**   and writeMasterJournal() only. Change its meaning to "unsynced data
**   has been written to the journal".
*/
struct Pager {
  sqlite3_vfs *pVfs;          /* OS functions to use for IO */
  u8 exclusiveMode;           /* Boolean. True if locking_mode==EXCLUSIVE */
  u8 journalMode;             /* On of the PAGER_JOURNALMODE_* values */
  u8 useJournal;              /* Use a rollback journal on this file */
  u8 noReadlock;              /* Do not bother to obtain readlocks */
  u8 noSync;                  /* Do not sync the journal if true */
  u8 fullSync;                /* Do extra syncs of the journal for robustness */
  u8 sync_flags;              /* One of SYNC_NORMAL or SYNC_FULL */
  u8 tempFile;                /* zFilename is a temporary file */
  u8 readOnly;                /* True for a read-only database */
  u8 memDb;                   /* True to inhibit all file I/O */

  /* The following block contains those class members that are dynamically
  ** modified during normal operations. The other variables in this structure
  ** are either constant throughout the lifetime of the pager, or else
  ** used to store configuration parameters that affect the way the pager 
  ** operates.
  **
  ** The 'state' variable is described in more detail along with the
  ** descriptions of the values it may take - PAGER_UNLOCK etc. Many of the
  ** other variables in this block are described in the comment directly 
  ** above this class definition.
  */
  u8 state;                   /* PAGER_UNLOCK, _SHARED, _RESERVED, etc. */
  u8 dbModified;              /* True if there are any changes to the Db */
  u8 needSync;                /* True if an fsync() is needed on the journal */
  u8 journalStarted;          /* True if header of journal is synced */
  u8 changeCountDone;         /* Set after incrementing the change-counter */
  u8 setMaster;               /* True if a m-j name has been written to jrnl */
  u8 doNotSync;               /* Boolean. While true, do not spill the cache */
  u8 dbSizeValid;             /* Set when dbSize is correct */
  Pgno dbSize;                /* Number of pages in the database */
  Pgno dbOrigSize;            /* dbSize before the current transaction */
  Pgno dbFileSize;            /* Number of pages in the database file */
  int errCode;                /* One of several kinds of errors */
  int nRec;                   /* Pages journalled since last j-header written */
  u32 cksumInit;              /* Quasi-random value added to every checksum */
  u32 nSubRec;                /* Number of records written to sub-journal */
  Bitvec *pInJournal;         /* One bit for each page in the database file */
  sqlite3_file *fd;           /* File descriptor for database */
  sqlite3_file *jfd;          /* File descriptor for main journal */
  sqlite3_file *sjfd;         /* File descriptor for sub-journal */
  i64 journalOff;             /* Current write offset in the journal file */
  i64 journalHdr;             /* Byte offset to previous journal header */
  PagerSavepoint *aSavepoint; /* Array of active savepoints */
  int nSavepoint;             /* Number of elements in aSavepoint[] */
  char dbFileVers[16];        /* Changes whenever database file changes */
  u32 sectorSize;             /* Assumed sector size during rollback */

  int nExtra;                 /* Add this many bytes to each in-memory page */
  u32 vfsFlags;               /* Flags for sqlite3_vfs.xOpen() */
  int pageSize;               /* Number of bytes in a page */
  Pgno mxPgno;                /* Maximum allowed size of the database */
  char *zFilename;            /* Name of the database file */
  char *zJournal;             /* Name of the journal file */
  int (*xBusyHandler)(void*); /* Function to call when busy */
  void *pBusyHandlerArg;      /* Context argument for xBusyHandler */
#ifdef SQLITE_TEST
  int nHit, nMiss;            /* Cache hits and missing */
  int nRead, nWrite;          /* Database pages read/written */
#endif
  void (*xReiniter)(DbPage*); /* Call this routine when reloading pages */
#ifdef SQLITE_HAS_CODEC
  void *(*xCodec)(void*,void*,Pgno,int); /* Routine for en/decoding data */
  void *pCodecArg;            /* First argument to xCodec() */
#endif
  char *pTmpSpace;            /* Pager.pageSize bytes of space for tmp use */
  i64 journalSizeLimit;       /* Size limit for persistent journal files */
  PCache *pPCache;            /* Pointer to page cache object */
  sqlite3_backup *pBackup;    /* Pointer to list of ongoing backup processes */
};

/*
** The following global variables hold counters used for
** testing purposes only.  These variables do not exist in
** a non-testing build.  These variables are not thread-safe.
*/
#ifdef SQLITE_TEST
int sqlite3_pager_readdb_count = 0;    /* Number of full pages read from DB */
int sqlite3_pager_writedb_count = 0;   /* Number of full pages written to DB */
int sqlite3_pager_writej_count = 0;    /* Number of pages written to journal */
# define PAGER_INCR(v)  v++
#else
# define PAGER_INCR(v)
#endif



/*
** Journal files begin with the following magic string.  The data
** was obtained from /dev/random.  It is used only as a sanity check.
**
** Since version 2.8.0, the journal format contains additional sanity
** checking information.  If the power fails while the journal is being
** written, semi-random garbage data might appear in the journal
** file after power is restored.  If an attempt is then made
** to roll the journal back, the database could be corrupted.  The additional
** sanity checking data is an attempt to discover the garbage in the
** journal and ignore it.
**
** The sanity checking information for the new journal format consists
** of a 32-bit checksum on each page of data.  The checksum covers both
** the page number and the pPager->pageSize bytes of data for the page.
** This cksum is initialized to a 32-bit random value that appears in the
** journal file right after the header.  The random initializer is important,
** because garbage data that appears at the end of a journal is likely
** data that was once in other files that have now been deleted.  If the
** garbage data came from an obsolete journal file, the checksums might
** be correct.  But by initializing the checksum to random value which
** is different for every journal, we minimize that risk.
*/
static const unsigned char aJournalMagic[] = {
  0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7,
};

/*
** The size of the of each page record in the journal is given by
** the following macro.
*/
#define JOURNAL_PG_SZ(pPager)  ((pPager->pageSize) + 8)

/*
** The journal header size for this pager. This is usually the same 
** size as a single disk sector. See also setSectorSize().
*/
#define JOURNAL_HDR_SZ(pPager) (pPager->sectorSize)

/*
** The macro MEMDB is true if we are dealing with an in-memory database.
** We do this as a macro so that if the SQLITE_OMIT_MEMORYDB macro is set,
** the value of MEMDB will be a constant and the compiler will optimize
** out code that would never execute.
*/
#ifdef SQLITE_OMIT_MEMORYDB
# define MEMDB 0
#else
# define MEMDB pPager->memDb
#endif

/*
** The maximum legal page number is (2^31 - 1).
*/
#define PAGER_MAX_PGNO 2147483647

#ifndef NDEBUG 
/*
** Usage:
**
**   assert( assert_pager_state(pPager) );
*/
static int assert_pager_state(Pager *pPager){

  /* A temp-file is always in PAGER_EXCLUSIVE or PAGER_SYNCED state. */
  assert( pPager->tempFile==0 || pPager->state>=PAGER_EXCLUSIVE );

  /* The changeCountDone flag is always set for temp-files */
  assert( pPager->tempFile==0 || pPager->changeCountDone );

  return 1;
}
#endif

/*
** Return true if it is necessary to write page *pPg into the sub-journal.
** A page needs to be written into the sub-journal if there exists one
** or more open savepoints for which:
**
**   * The page-number is less than or equal to PagerSavepoint.nOrig, and
**   * The bit corresponding to the page-number is not set in
**     PagerSavepoint.pInSavepoint.
*/
static int subjRequiresPage(PgHdr *pPg){
  Pgno pgno = pPg->pgno;
  Pager *pPager = pPg->pPager;
  int i;
  for(i=0; i<pPager->nSavepoint; i++){
    PagerSavepoint *p = &pPager->aSavepoint[i];
    if( p->nOrig>=pgno && 0==sqlite3BitvecTest(p->pInSavepoint, pgno) ){
      return 1;
    }
  }
  return 0;
}

/*
** Return true if the page is already in the journal file.
*/
static int pageInJournal(PgHdr *pPg){
  return sqlite3BitvecTest(pPg->pPager->pInJournal, pPg->pgno);
}

/*
** Read a 32-bit integer from the given file descriptor.  Store the integer
** that is read in *pRes.  Return SQLITE_OK if everything worked, or an
** error code is something goes wrong.
**
** All values are stored on disk as big-endian.
*/
static int read32bits(sqlite3_file *fd, i64 offset, u32 *pRes){
  unsigned char ac[4];
  int rc = sqlite3OsRead(fd, ac, sizeof(ac), offset);
  if( rc==SQLITE_OK ){
    *pRes = sqlite3Get4byte(ac);
  }
  return rc;
}

/*
** Write a 32-bit integer into a string buffer in big-endian byte order.
*/
#define put32bits(A,B)  sqlite3Put4byte((u8*)A,B)

/*
** Write a 32-bit integer into the given file descriptor.  Return SQLITE_OK
** on success or an error code is something goes wrong.
*/
static int write32bits(sqlite3_file *fd, i64 offset, u32 val){
  char ac[4];
  put32bits(ac, val);
  return sqlite3OsWrite(fd, ac, 4, offset);
}

/*
** The argument to this macro is a file descriptor (type sqlite3_file*).
** Return 0 if it is not open, or non-zero (but not 1) if it is.
**
** This is so that expressions can be written as:
**
**   if( isOpen(pPager->jfd) ){ ...
**
** instead of
**
**   if( pPager->jfd->pMethods ){ ...
*/
#define isOpen(pFd) ((pFd)->pMethods)

/*
** If file pFd is open, call sqlite3OsUnlock() on it.
*/
static int osUnlock(sqlite3_file *pFd, int eLock){
  if( !isOpen(pFd) ){
    return SQLITE_OK;
  }
  return sqlite3OsUnlock(pFd, eLock);
}

/*
** This function determines whether or not the atomic-write optimization
** can be used with this pager. The optimization can be used if:
**
**  (a) the value returned by OsDeviceCharacteristics() indicates that
**      a database page may be written atomically, and
**  (b) the value returned by OsSectorSize() is less than or equal
**      to the page size.
**
** The optimization is also always enabled for temporary files. It is
** an error to call this function if pPager is opened on an in-memory
** database.
**
** If the optimization cannot be used, 0 is returned. If it can be used,
** then the value returned is the size of the journal file when it
** contains rollback data for exactly one page.
*/
#ifdef SQLITE_ENABLE_ATOMIC_WRITE
static int jrnlBufferSize(Pager *pPager){
  assert( !MEMDB );
  if( !pPager->tempFile ){
    int dc;                           /* Device characteristics */
    int nSector;                      /* Sector size */
    int szPage;                       /* Page size */

    assert( isOpen(pPager->fd) );
    dc = sqlite3OsDeviceCharacteristics(pPager->fd);
    nSector = pPager->sectorSize;
    szPage = pPager->pageSize;

    assert(SQLITE_IOCAP_ATOMIC512==(512>>8));
    assert(SQLITE_IOCAP_ATOMIC64K==(65536>>8));
    if( 0==(dc&(SQLITE_IOCAP_ATOMIC|(szPage>>8)) || nSector>szPage) ){
      return 0;
    }
  }

  return JOURNAL_HDR_SZ(pPager) + JOURNAL_PG_SZ(pPager);
}
#endif

/*
** If SQLITE_CHECK_PAGES is defined then we do some sanity checking
** on the cache using a hash function.  This is used for testing
** and debugging only.
*/
#ifdef SQLITE_CHECK_PAGES
/*
** Return a 32-bit hash of the page data for pPage.
*/
static u32 pager_datahash(int nByte, unsigned char *pData){
  u32 hash = 0;
  int i;
  for(i=0; i<nByte; i++){
    hash = (hash*1039) + pData[i];
  }
  return hash;
}
static u32 pager_pagehash(PgHdr *pPage){
  return pager_datahash(pPage->pPager->pageSize, (unsigned char *)pPage->pData);
}
static void pager_set_pagehash(PgHdr *pPage){
  pPage->pageHash = pager_pagehash(pPage);
}

/*
** The CHECK_PAGE macro takes a PgHdr* as an argument. If SQLITE_CHECK_PAGES
** is defined, and NDEBUG is not defined, an assert() statement checks
** that the page is either dirty or still matches the calculated page-hash.
*/
#define CHECK_PAGE(x) checkPage(x)
static void checkPage(PgHdr *pPg){
  Pager *pPager = pPg->pPager;
  assert( !pPg->pageHash || pPager->errCode
      || (pPg->flags&PGHDR_DIRTY) || pPg->pageHash==pager_pagehash(pPg) );
}

#else
#define pager_datahash(X,Y)  0
#define pager_pagehash(X)  0
#define CHECK_PAGE(x)
#endif  /* SQLITE_CHECK_PAGES */

/*
** When this is called the journal file for pager pPager must be open.
** This function attempts to read a master journal file name from the 
** end of the file and, if successful, copies it into memory supplied 
** by the caller. See comments above writeMasterJournal() for the format
** used to store a master journal file name at the end of a journal file.
**
** zMaster must point to a buffer of at least nMaster bytes allocated by
** the caller. This should be sqlite3_vfs.mxPathname+1 (to ensure there is
** enough space to write the master journal name). If the master journal
** name in the journal is longer than nMaster bytes (including a
** nul-terminator), then this is handled as if no master journal name
** were present in the journal.
**
** If a master journal file name is present at the end of the journal
** file, then it is copied into the buffer pointed to by zMaster. A
** nul-terminator byte is appended to the buffer following the master
** journal file name.
**
** If it is determined that no master journal file name is present 
** zMaster[0] is set to 0 and SQLITE_OK returned.
**
** If an error occurs while reading from the journal file, an SQLite
** error code is returned.
*/
static int readMasterJournal(sqlite3_file *pJrnl, char *zMaster, u32 nMaster){
  int rc;                    /* Return code */
  u32 len;                   /* Length in bytes of master journal name */
  i64 szJ;                   /* Total size in bytes of journal file pJrnl */
  u32 cksum;                 /* MJ checksum value read from journal */
  u32 u;                     /* Unsigned loop counter */
  unsigned char aMagic[8];   /* A buffer to hold the magic header */
  zMaster[0] = '\0';

  if( SQLITE_OK!=(rc = sqlite3OsFileSize(pJrnl, &szJ))
   || szJ<16
   || SQLITE_OK!=(rc = read32bits(pJrnl, szJ-16, &len))
   || len>=nMaster 
   || SQLITE_OK!=(rc = read32bits(pJrnl, szJ-12, &cksum))
   || SQLITE_OK!=(rc = sqlite3OsRead(pJrnl, aMagic, 8, szJ-8))
   || memcmp(aMagic, aJournalMagic, 8)
   || SQLITE_OK!=(rc = sqlite3OsRead(pJrnl, zMaster, len, szJ-16-len))
  ){
    return rc;
  }

  /* See if the checksum matches the master journal name */
  for(u=0; u<len; u++){
    cksum -= zMaster[u];
  }
  if( cksum ){
    /* If the checksum doesn't add up, then one or more of the disk sectors
    ** containing the master journal filename is corrupted. This means
    ** definitely roll back, so just return SQLITE_OK and report a (nul)
    ** master-journal filename.
    */
    len = 0;
  }
  zMaster[len] = '\0';
   
  return SQLITE_OK;
}

/*
** Return the offset of the sector boundary at or immediately 
** following the value in pPager->journalOff, assuming a sector 
** size of pPager->sectorSize bytes.
**
** i.e for a sector size of 512:
**
**   Pager.journalOff          Return value
**   ---------------------------------------
**   0                         0
**   512                       512
**   100                       512
**   2000                      2048
** 
*/
static i64 journalHdrOffset(Pager *pPager){
  i64 offset = 0;
  i64 c = pPager->journalOff;
  if( c ){
    offset = ((c-1)/JOURNAL_HDR_SZ(pPager) + 1) * JOURNAL_HDR_SZ(pPager);
  }
  assert( offset%JOURNAL_HDR_SZ(pPager)==0 );
  assert( offset>=c );
  assert( (offset-c)<JOURNAL_HDR_SZ(pPager) );
  return offset;
}

/*
** The journal file must be open when this function is called.
**
** This function is a no-op if the journal file has not been written to
** within the current transaction (i.e. if Pager.journalOff==0).
**
** If doTruncate is non-zero or the Pager.journalSizeLimit variable is
** set to 0, then truncate the journal file to zero bytes in size. Otherwise,
** zero the 28-byte header at the start of the journal file. In either case, 
** if the pager is not in no-sync mode, sync the journal file immediately 
** after writing or truncating it.
**
** If Pager.journalSizeLimit is set to a positive, non-zero value, and
** following the truncation or zeroing described above the size of the 
** journal file in bytes is larger than this value, then truncate the
** journal file to Pager.journalSizeLimit bytes. The journal file does
** not need to be synced following this operation.
**
** If an IO error occurs, abandon processing and return the IO error code.
** Otherwise, return SQLITE_OK.
*/
static int zeroJournalHdr(Pager *pPager, int doTruncate){
  int rc = SQLITE_OK;                               /* Return code */
  assert( isOpen(pPager->jfd) );
  if( pPager->journalOff ){
    const i64 iLimit = pPager->journalSizeLimit;    /* Local cache of jsl */

    IOTRACE(("JZEROHDR %p\n", pPager))
    if( doTruncate || iLimit==0 ){
      rc = sqlite3OsTruncate(pPager->jfd, 0);
    }else{
      static const char zeroHdr[28] = {0};
      rc = sqlite3OsWrite(pPager->jfd, zeroHdr, sizeof(zeroHdr), 0);
    }
    if( rc==SQLITE_OK && !pPager->noSync ){
      rc = sqlite3OsSync(pPager->jfd, SQLITE_SYNC_DATAONLY|pPager->sync_flags);
    }

    /* At this point the transaction is committed but the write lock 
    ** is still held on the file. If there is a size limit configured for 
    ** the persistent journal and the journal file currently consumes more
    ** space than that limit allows for, truncate it now. There is no need
    ** to sync the file following this operation.
    */
    if( rc==SQLITE_OK && iLimit>0 ){
      i64 sz;
      rc = sqlite3OsFileSize(pPager->jfd, &sz);
      if( rc==SQLITE_OK && sz>iLimit ){
        rc = sqlite3OsTruncate(pPager->jfd, iLimit);
      }
    }
  }
  return rc;
}

/*
** The journal file must be open when this routine is called. A journal
** header (JOURNAL_HDR_SZ bytes) is written into the journal file at the
** current location.
**
** The format for the journal header is as follows:
** - 8 bytes: Magic identifying journal format.
** - 4 bytes: Number of records in journal, or -1 no-sync mode is on.
** - 4 bytes: Random number used for page hash.
** - 4 bytes: Initial database page count.
** - 4 bytes: Sector size used by the process that wrote this journal.
** - 4 bytes: Database page size.
** 
** Followed by (JOURNAL_HDR_SZ - 28) bytes of unused space.
*/
static int writeJournalHdr(Pager *pPager){
  int rc = SQLITE_OK;                 /* Return code */
  char *zHeader = pPager->pTmpSpace;  /* Temporary space used to build header */
  u32 nHeader = pPager->pageSize;     /* Size of buffer pointed to by zHeader */
  u32 nWrite;                         /* Bytes of header sector written */
  int ii;                             /* Loop counter */

  assert( isOpen(pPager->jfd) );      /* Journal file must be open. */

  if( nHeader>JOURNAL_HDR_SZ(pPager) ){
    nHeader = JOURNAL_HDR_SZ(pPager);
  }

  /* If there are active savepoints and any of them were created 
  ** since the most recent journal header was written, update the 
  ** PagerSavepoint.iHdrOffset fields now.
  */
  for(ii=0; ii<pPager->nSavepoint; ii++){
    if( pPager->aSavepoint[ii].iHdrOffset==0 ){
      pPager->aSavepoint[ii].iHdrOffset = pPager->journalOff;
    }
  }

  pPager->journalHdr = pPager->journalOff = journalHdrOffset(pPager);
  memcpy(zHeader, aJournalMagic, sizeof(aJournalMagic));

  /* 
  ** Write the nRec Field - the number of page records that follow this
  ** journal header. Normally, zero is written to this value at this time.
  ** After the records are added to the journal (and the journal synced, 
  ** if in full-sync mode), the zero is overwritten with the true number
  ** of records (see syncJournal()).
  **
  ** A faster alternative is to write 0xFFFFFFFF to the nRec field. When
  ** reading the journal this value tells SQLite to assume that the
  ** rest of the journal file contains valid page records. This assumption
  ** is dangerous, as if a failure occurred whilst writing to the journal
  ** file it may contain some garbage data. There are two scenarios
  ** where this risk can be ignored:
  **
  **   * When the pager is in no-sync mode. Corruption can follow a
  **     power failure in this case anyway.
  **
  **   * When the SQLITE_IOCAP_SAFE_APPEND flag is set. This guarantees
  **     that garbage data is never appended to the journal file.
  */
  assert( isOpen(pPager->fd) || pPager->noSync );
  if( (pPager->noSync) || (pPager->journalMode==PAGER_JOURNALMODE_MEMORY)
   || (sqlite3OsDeviceCharacteristics(pPager->fd)&SQLITE_IOCAP_SAFE_APPEND) 
  ){
    put32bits(&zHeader[sizeof(aJournalMagic)], 0xffffffff);
  }else{
    put32bits(&zHeader[sizeof(aJournalMagic)], 0);
  }

  /* The random check-hash initialiser */ 
  sqlite3_randomness(sizeof(pPager->cksumInit), &pPager->cksumInit);
  put32bits(&zHeader[sizeof(aJournalMagic)+4], pPager->cksumInit);
  /* The initial database size */
  put32bits(&zHeader[sizeof(aJournalMagic)+8], pPager->dbOrigSize);
  /* The assumed sector size for this process */
  put32bits(&zHeader[sizeof(aJournalMagic)+12], pPager->sectorSize);

  /* The page size */
  put32bits(&zHeader[sizeof(aJournalMagic)+16], pPager->pageSize);

  /* Initializing the tail of the buffer is not necessary.  Everything
  ** works find if the following memset() is omitted.  But initializing
  ** the memory prevents valgrind from complaining, so we are willing to
  ** take the performance hit.
  */
  memset(&zHeader[sizeof(aJournalMagic)+20], 0,
         nHeader-(sizeof(aJournalMagic)+20));

  /* In theory, it is only necessary to write the 28 bytes that the 
  ** journal header consumes to the journal file here. Then increment the 
  ** Pager.journalOff variable by JOURNAL_HDR_SZ so that the next 
  ** record is written to the following sector (leaving a gap in the file
  ** that will be implicitly filled in by the OS).
  **
  ** However it has been discovered that on some systems this pattern can 
  ** be significantly slower than contiguously writing data to the file,
  ** even if that means explicitly writing data to the block of 
  ** (JOURNAL_HDR_SZ - 28) bytes that will not be used. So that is what
  ** is done. 
  **
  ** The loop is required here in case the sector-size is larger than the 
  ** database page size. Since the zHeader buffer is only Pager.pageSize
  ** bytes in size, more than one call to sqlite3OsWrite() may be required
  ** to populate the entire journal header sector.
  */ 
  for(nWrite=0; rc==SQLITE_OK&&nWrite<JOURNAL_HDR_SZ(pPager); nWrite+=nHeader){
    IOTRACE(("JHDR %p %lld %d\n", pPager, pPager->journalHdr, nHeader))
    rc = sqlite3OsWrite(pPager->jfd, zHeader, nHeader, pPager->journalOff);
    pPager->journalOff += nHeader;
  }

  return rc;
}

/*
** The journal file must be open when this is called. A journal header file
** (JOURNAL_HDR_SZ bytes) is read from the current location in the journal
** file. The current location in the journal file is given by
** pPager->journalOff. See comments above function writeJournalHdr() for
** a description of the journal header format.
**
** If the header is read successfully, *pNRec is set to the number of
** page records following this header and *pDbSize is set to the size of the
** database before the transaction began, in pages. Also, pPager->cksumInit
** is set to the value read from the journal header. SQLITE_OK is returned
** in this case.
**
** If the journal header file appears to be corrupted, SQLITE_DONE is
** returned and *pNRec and *PDbSize are undefined.  If JOURNAL_HDR_SZ bytes
** cannot be read from the journal file an error code is returned.
*/
static int readJournalHdr(
  Pager *pPager,               /* Pager object */
  i64 journalSize,             /* Size of the open journal file in bytes */
  u32 *pNRec,                  /* OUT: Value read from the nRec field */
  u32 *pDbSize                 /* OUT: Value of original database size field */
){
  int rc;                      /* Return code */
  unsigned char aMagic[8];     /* A buffer to hold the magic header */
  i64 iHdrOff;                 /* Offset of journal header being read */

  assert( isOpen(pPager->jfd) );      /* Journal file must be open. */

  /* Advance Pager.journalOff to the start of the next sector. If the
  ** journal file is too small for there to be a header stored at this
  ** point, return SQLITE_DONE.
  */
  pPager->journalOff = journalHdrOffset(pPager);
  if( pPager->journalOff+JOURNAL_HDR_SZ(pPager) > journalSize ){
    return SQLITE_DONE;
  }
  iHdrOff = pPager->journalOff;

  /* Read in the first 8 bytes of the journal header. If they do not match
  ** the  magic string found at the start of each journal header, return
  ** SQLITE_DONE. If an IO error occurs, return an error code. Otherwise,
  ** proceed.
  */
  rc = sqlite3OsRead(pPager->jfd, aMagic, sizeof(aMagic), iHdrOff);
  if( rc ){
    return rc;
  }
  if( memcmp(aMagic, aJournalMagic, sizeof(aMagic))!=0 ){
    return SQLITE_DONE;
  }

  /* Read the first three 32-bit fields of the journal header: The nRec
  ** field, the checksum-initializer and the database size at the start
  ** of the transaction. Return an error code if anything goes wrong.
  */
  if( SQLITE_OK!=(rc = read32bits(pPager->jfd, iHdrOff+8, pNRec))
   || SQLITE_OK!=(rc = read32bits(pPager->jfd, iHdrOff+12, &pPager->cksumInit))
   || SQLITE_OK!=(rc = read32bits(pPager->jfd, iHdrOff+16, pDbSize))
  ){
    return rc;
  }

  if( pPager->journalOff==0 ){
    u32 iPageSize;               /* Page-size field of journal header */
    u32 iSectorSize;             /* Sector-size field of journal header */
    u16 iPageSize16;             /* Copy of iPageSize in 16-bit variable */

    /* Read the page-size and sector-size journal header fields. */
    if( SQLITE_OK!=(rc = read32bits(pPager->jfd, iHdrOff+20, &iSectorSize))
     || SQLITE_OK!=(rc = read32bits(pPager->jfd, iHdrOff+24, &iPageSize))
    ){
      return rc;
    }

    /* Check that the values read from the page-size and sector-size fields
    ** are within range. To be 'in range', both values need to be a power
    ** of two greater than or equal to 512, and not greater than their 
    ** respective compile time maximum limits.
    */
    if( iPageSize<512                  || iSectorSize<512
     || iPageSize>SQLITE_MAX_PAGE_SIZE || iSectorSize>MAX_SECTOR_SIZE
     || ((iPageSize-1)&iPageSize)!=0   || ((iSectorSize-1)&iSectorSize)!=0 
    ){
      /* If the either the page-size or sector-size in the journal-header is 
      ** invalid, then the process that wrote the journal-header must have 
      ** crashed before the header was synced. In this case stop reading 
      ** the journal file here.
      */
      return SQLITE_DONE;
    }

    /* Update the page-size to match the value read from the journal. 
    ** Use a testcase() macro to make sure that malloc failure within 
    ** PagerSetPagesize() is tested.
    */
    iPageSize16 = (u16)iPageSize;
    rc = sqlite3PagerSetPagesize(pPager, &iPageSize16);
    testcase( rc!=SQLITE_OK );
    assert( rc!=SQLITE_OK || iPageSize16==(u16)iPageSize );

    /* Update the assumed sector-size to match the value used by 
    ** the process that created this journal. If this journal was
    ** created by a process other than this one, then this routine
    ** is being called from within pager_playback(). The local value
    ** of Pager.sectorSize is restored at the end of that routine.
    */
    pPager->sectorSize = iSectorSize;
  }

  pPager->journalOff += JOURNAL_HDR_SZ(pPager);
  return rc;
}


/*
** Write the supplied master journal name into the journal file for pager
** pPager at the current location. The master journal name must be the last
** thing written to a journal file. If the pager is in full-sync mode, the
** journal file descriptor is advanced to the next sector boundary before
** anything is written. The format is:
**
**   + 4 bytes: PAGER_MJ_PGNO.
**   + N bytes: Master journal filename in utf-8.
**   + 4 bytes: N (length of master journal name in bytes, no nul-terminator).
**   + 4 bytes: Master journal name checksum.
**   + 8 bytes: aJournalMagic[].
**
** The master journal page checksum is the sum of the bytes in the master
** journal name, where each byte is interpreted as a signed 8-bit integer.
**
** If zMaster is a NULL pointer (occurs for a single database transaction), 
** this call is a no-op.
*/
static int writeMasterJournal(Pager *pPager, const char *zMaster){
  int rc;                          /* Return code */
  int nMaster;                     /* Length of string zMaster */
  i64 iHdrOff;                     /* Offset of header in journal file */
  i64 jrnlSize;                    /* Size of journal file on disk */
  u32 cksum = 0;                   /* Checksum of string zMaster */

  if( !zMaster || pPager->setMaster
   || pPager->journalMode==PAGER_JOURNALMODE_MEMORY 
   || pPager->journalMode==PAGER_JOURNALMODE_OFF 
  ){
    return SQLITE_OK;
  }
  pPager->setMaster = 1;
  assert( isOpen(pPager->jfd) );

  /* Calculate the length in bytes and the checksum of zMaster */
  for(nMaster=0; zMaster[nMaster]; nMaster++){
    cksum += zMaster[nMaster];
  }

  /* If in full-sync mode, advance to the next disk sector before writing
  ** the master journal name. This is in case the previous page written to
  ** the journal has already been synced.
  */
  if( pPager->fullSync ){
    pPager->journalOff = journalHdrOffset(pPager);
  }
  iHdrOff = pPager->journalOff;

  /* Write the master journal data to the end of the journal file. If
  ** an error occurs, return the error code to the caller.
  */
  if( (0 != (rc = write32bits(pPager->jfd, iHdrOff, PAGER_MJ_PGNO(pPager))))
   || (0 != (rc = sqlite3OsWrite(pPager->jfd, zMaster, nMaster, iHdrOff+4)))
   || (0 != (rc = write32bits(pPager->jfd, iHdrOff+4+nMaster, nMaster)))
   || (0 != (rc = write32bits(pPager->jfd, iHdrOff+4+nMaster+4, cksum)))
   || (0 != (rc = sqlite3OsWrite(pPager->jfd, aJournalMagic, 8, iHdrOff+4+nMaster+8)))
  ){
    return rc;
  }
  pPager->journalOff += (nMaster+20);
  pPager->needSync = !pPager->noSync;

  /* If the pager is in peristent-journal mode, then the physical 
  ** journal-file may extend past the end of the master-journal name
  ** and 8 bytes of magic data just written to the file. This is 
  ** dangerous because the code to rollback a hot-journal file
  ** will not be able to find the master-journal name to determine 
  ** whether or not the journal is hot. 
  **
  ** Easiest thing to do in this scenario is to truncate the journal 
  ** file to the required size.
  */ 
  if( SQLITE_OK==(rc = sqlite3OsFileSize(pPager->jfd, &jrnlSize))
   && jrnlSize>pPager->journalOff
  ){
    rc = sqlite3OsTruncate(pPager->jfd, pPager->journalOff);
  }
  return rc;
}

/*
** Find a page in the hash table given its page number. Return
** a pointer to the page or NULL if the requested page is not 
** already in memory.
*/
static PgHdr *pager_lookup(Pager *pPager, Pgno pgno){
  PgHdr *p;                         /* Return value */

  /* It is not possible for a call to PcacheFetch() with createFlag==0 to
  ** fail, since no attempt to allocate dynamic memory will be made.
  */
  (void)sqlite3PcacheFetch(pPager->pPCache, pgno, 0, &p);
  return p;
}

/*
** Unless the pager is in error-state, discard all in-memory pages. If
** the pager is in error-state, then this call is a no-op.
**
** TODO: Why can we not reset the pager while in error state?
*/
static void pager_reset(Pager *pPager){
  if( SQLITE_OK==pPager->errCode ){
    sqlite3BackupRestart(pPager->pBackup);
    sqlite3PcacheClear(pPager->pPCache);
  }
}

/*
** Free all structures in the Pager.aSavepoint[] array and set both
** Pager.aSavepoint and Pager.nSavepoint to zero. Close the sub-journal
** if it is open and the pager is not in exclusive mode.
*/
static void releaseAllSavepoints(Pager *pPager){
  int ii;               /* Iterator for looping through Pager.aSavepoint */
  for(ii=0; ii<pPager->nSavepoint; ii++){
    sqlite3BitvecDestroy(pPager->aSavepoint[ii].pInSavepoint);
  }
  if( !pPager->exclusiveMode ){
    sqlite3OsClose(pPager->sjfd);
  }
  sqlite3_free(pPager->aSavepoint);
  pPager->aSavepoint = 0;
  pPager->nSavepoint = 0;
  pPager->nSubRec = 0;
}

/*
** Set the bit number pgno in the PagerSavepoint.pInSavepoint 
** bitvecs of all open savepoints. Return SQLITE_OK if successful
** or SQLITE_NOMEM if a malloc failure occurs.
*/
static int addToSavepointBitvecs(Pager *pPager, Pgno pgno){
  int ii;                   /* Loop counter */
  int rc = SQLITE_OK;       /* Result code */

  for(ii=0; ii<pPager->nSavepoint; ii++){
    PagerSavepoint *p = &pPager->aSavepoint[ii];
    if( pgno<=p->nOrig ){
      rc |= sqlite3BitvecSet(p->pInSavepoint, pgno);
      testcase( rc==SQLITE_NOMEM );
      assert( rc==SQLITE_OK || rc==SQLITE_NOMEM );
    }
  }
  return rc;
}

/*
** Unlock the database file. This function is a no-op if the pager
** is in exclusive mode.
**
** If the pager is currently in error state, discard the contents of 
** the cache and reset the Pager structure internal state. If there is
** an open journal-file, then the next time a shared-lock is obtained
** on the pager file (by this or any other process), it will be
** treated as a hot-journal and rolled back.
*/
static void pager_unlock(Pager *pPager){
  if( !pPager->exclusiveMode ){
    int rc;                      /* Return code */

    /* Always close the journal file when dropping the database lock.
    ** Otherwise, another connection with journal_mode=delete might
    ** delete the file out from under us.
    */
    sqlite3OsClose(pPager->jfd);
    sqlite3BitvecDestroy(pPager->pInJournal);
    pPager->pInJournal = 0;
    releaseAllSavepoints(pPager);

    /* If the file is unlocked, somebody else might change it. The
    ** values stored in Pager.dbSize etc. might become invalid if
    ** this happens. TODO: Really, this doesn't need to be cleared
    ** until the change-counter check fails in pagerSharedLock().
    */
    pPager->dbSizeValid = 0;

    rc = osUnlock(pPager->fd, NO_LOCK);
    if( rc ){
      pPager->errCode = rc;
    }
    IOTRACE(("UNLOCK %p\n", pPager))

    /* If Pager.errCode is set, the contents of the pager cache cannot be
    ** trusted. Now that the pager file is unlocked, the contents of the
    ** cache can be discarded and the error code safely cleared.
    */
    if( pPager->errCode ){
      if( rc==SQLITE_OK ){
        pPager->errCode = SQLITE_OK;
      }
      pager_reset(pPager);
    }

    pPager->changeCountDone = 0;
    pPager->state = PAGER_UNLOCK;
  }
}

/*
** This function should be called when an IOERR, CORRUPT or FULL error
** may have occurred. The first argument is a pointer to the pager 
** structure, the second the error-code about to be returned by a pager 
** API function. The value returned is a copy of the second argument 
** to this function. 
**
** If the second argument is SQLITE_IOERR, SQLITE_CORRUPT, or SQLITE_FULL
** the error becomes persistent. Until the persisten error is cleared,
** subsequent API calls on this Pager will immediately return the same 
** error code.
**
** A persistent error indicates that the contents of the pager-cache 
** cannot be trusted. This state can be cleared by completely discarding 
** the contents of the pager-cache. If a transaction was active when
** the persistent error occurred, then the rollback journal may need
** to be replayed to restore the contents of the database file (as if
** it were a hot-journal).
*/
static int pager_error(Pager *pPager, int rc){
  int rc2 = rc & 0xff;
  assert(
       pPager->errCode==SQLITE_FULL ||
       pPager->errCode==SQLITE_OK ||
       (pPager->errCode & 0xff)==SQLITE_IOERR
  );
  if(
    rc2==SQLITE_FULL ||
    rc2==SQLITE_IOERR ||
    rc2==SQLITE_CORRUPT
  ){
    pPager->errCode = rc;
    if( pPager->state==PAGER_UNLOCK 
     && sqlite3PcacheRefCount(pPager->pPCache)==0 
    ){
      /* If the pager is already unlocked, call pager_unlock() now to
      ** clear the error state and ensure that the pager-cache is 
      ** completely empty.
      */
      pager_unlock(pPager);
    }
  }
  return rc;
}

/*
** Execute a rollback if a transaction is active and unlock the 
** database file. 
**
** If the pager has already entered the error state, do not attempt 
** the rollback at this time. Instead, pager_unlock() is called. The
** call to pager_unlock() will discard all in-memory pages, unlock
** the database file and clear the error state. If this means that
** there is a hot-journal left in the file-system, the next connection
** to obtain a shared lock on the pager (which may be this one) will
** roll it back.
**
** If the pager has not already entered the error state, but an IO or
** malloc error occurs during a rollback, then this will itself cause 
** the pager to enter the error state. Which will be cleared by the
** call to pager_unlock(), as described above.
*/
static void pagerUnlockAndRollback(Pager *pPager){
  if( pPager->errCode==SQLITE_OK && pPager->state>=PAGER_RESERVED ){
    sqlite3BeginBenignMalloc();
    sqlite3PagerRollback(pPager);
    sqlite3EndBenignMalloc();
  }
  pager_unlock(pPager);
}

/*
** This routine ends a transaction. A transaction is usually ended by 
** either a COMMIT or a ROLLBACK operation. This routine may be called 
** after rollback of a hot-journal, or if an error occurs while opening
** the journal file or writing the very first journal-header of a
** database transaction.
** 
** If the pager is in PAGER_SHARED or PAGER_UNLOCK state when this
** routine is called, it is a no-op (returns SQLITE_OK).
**
** Otherwise, any active savepoints are released.
**
** If the journal file is open, then it is "finalized". Once a journal 
** file has been finalized it is not possible to use it to roll back a 
** transaction. Nor will it be considered to be a hot-journal by this
** or any other database connection. Exactly how a journal is finalized
** depends on whether or not the pager is running in exclusive mode and
** the current journal-mode (Pager.journalMode value), as follows:
**
**   journalMode==MEMORY
**     Journal file descriptor is simply closed. This destroys an 
**     in-memory journal.
**
**   journalMode==TRUNCATE
**     Journal file is truncated to zero bytes in size.
**
**   journalMode==PERSIST
**     The first 28 bytes of the journal file are zeroed. This invalidates
**     the first journal header in the file, and hence the entire journal
**     file. An invalid journal file cannot be rolled back.
**
**   journalMode==DELETE
**     The journal file is closed and deleted using sqlite3OsDelete().
**
**     If the pager is running in exclusive mode, this method of finalizing
**     the journal file is never used. Instead, if the journalMode is
**     DELETE and the pager is in exclusive mode, the method described under
**     journalMode==PERSIST is used instead.
**
** After the journal is finalized, if running in non-exclusive mode, the
** pager moves to PAGER_SHARED state (and downgrades the lock on the
** database file accordingly).
**
** If the pager is running in exclusive mode and is in PAGER_SYNCED state,
** it moves to PAGER_EXCLUSIVE. No locks are downgraded when running in
** exclusive mode.
**
** SQLITE_OK is returned if no error occurs. If an error occurs during
** any of the IO operations to finalize the journal file or unlock the
** database then the IO error code is returned to the user. If the 
** operation to finalize the journal file fails, then the code still
** tries to unlock the database file if not in exclusive mode. If the
** unlock operation fails as well, then the first error code related
** to the first error encountered (the journal finalization one) is
** returned.
*/
static int pager_end_transaction(Pager *pPager, int hasMaster){
  int rc = SQLITE_OK;      /* Error code from journal finalization operation */
  int rc2 = SQLITE_OK;     /* Error code from db file unlock operation */

  if( pPager->state<PAGER_RESERVED ){
    return SQLITE_OK;
  }
  releaseAllSavepoints(pPager);

  assert( isOpen(pPager->jfd) || pPager->pInJournal==0 );
  if( isOpen(pPager->jfd) ){

    /* TODO: There's a problem here if a journal-file was opened in MEMORY
    ** mode and then the journal-mode is changed to TRUNCATE or PERSIST
    ** during the transaction. This code should be changed to assume
    ** that the journal mode has not changed since the transaction was
    ** started. And the sqlite3PagerJournalMode() function should be
    ** changed to make sure that this is the case too.
    */

    /* Finalize the journal file. */
    if( pPager->journalMode==PAGER_JOURNALMODE_MEMORY ){
      int isMemoryJournal = sqlite3IsMemJournal(pPager->jfd);
      sqlite3OsClose(pPager->jfd);
      if( !isMemoryJournal ){
        rc = sqlite3OsDelete(pPager->pVfs, pPager->zJournal, 0);
      }
    }else if( pPager->journalMode==PAGER_JOURNALMODE_TRUNCATE ){
      rc = sqlite3OsTruncate(pPager->jfd, 0);
      pPager->journalOff = 0;
      pPager->journalStarted = 0;
    }else if( pPager->exclusiveMode 
     || pPager->journalMode==PAGER_JOURNALMODE_PERSIST
    ){
      rc = zeroJournalHdr(pPager, hasMaster);
      pager_error(pPager, rc);
      pPager->journalOff = 0;
      pPager->journalStarted = 0;
    }else{
      assert( pPager->journalMode==PAGER_JOURNALMODE_DELETE || rc );
      sqlite3OsClose(pPager->jfd);
      if( rc==SQLITE_OK && !pPager->tempFile ){
        rc = sqlite3OsDelete(pPager->pVfs, pPager->zJournal, 0);
      }
    }

#ifdef SQLITE_CHECK_PAGES
    sqlite3PcacheIterateDirty(pPager->pPCache, pager_set_pagehash);
#endif

    sqlite3PcacheCleanAll(pPager->pPCache);
    sqlite3BitvecDestroy(pPager->pInJournal);
    pPager->pInJournal = 0;
    pPager->nRec = 0;
  }

  if( !pPager->exclusiveMode ){
    rc2 = osUnlock(pPager->fd, SHARED_LOCK);
    pPager->state = PAGER_SHARED;
    pPager->changeCountDone = 0;
  }else if( pPager->state==PAGER_SYNCED ){
    pPager->state = PAGER_EXCLUSIVE;
  }
  pPager->setMaster = 0;
  pPager->needSync = 0;
  pPager->dbModified = 0;

  /* TODO: Is this optimal? Why is the db size invalidated here 
  ** when the database file is not unlocked? */
  pPager->dbOrigSize = 0;
  sqlite3PcacheTruncate(pPager->pPCache, pPager->dbSize);
  if( !MEMDB ){
    pPager->dbSizeValid = 0;
  }

  return (rc==SQLITE_OK?rc2:rc);
}

/*
** Parameter aData must point to a buffer of pPager->pageSize bytes
** of data. Compute and return a checksum based ont the contents of the 
** page of data and the current value of pPager->cksumInit.
**
** This is not a real checksum. It is really just the sum of the 
** random initial value (pPager->cksumInit) and every 200th byte
** of the page data, starting with byte offset (pPager->pageSize%200).
** Each byte is interpreted as an 8-bit unsigned integer.
**
** Changing the formula used to compute this checksum results in an
** incompatible journal file format.
**
** If journal corruption occurs due to a power failure, the most likely 
** scenario is that one end or the other of the record will be changed. 
** It is much less likely that the two ends of the journal record will be
** correct and the middle be corrupt.  Thus, this "checksum" scheme,
** though fast and simple, catches the mostly likely kind of corruption.
*/
static u32 pager_cksum(Pager *pPager, const u8 *aData){
  u32 cksum = pPager->cksumInit;         /* Checksum value to return */
  int i = pPager->pageSize-200;          /* Loop counter */
  while( i>0 ){
    cksum += aData[i];
    i -= 200;
  }
  return cksum;
}

/*
** Read a single page from either the journal file (if isMainJrnl==1) or
** from the sub-journal (if isMainJrnl==0) and playback that page.
** The page begins at offset *pOffset into the file. The *pOffset
** value is increased to the start of the next page in the journal.
**
** The isMainJrnl flag is true if this is the main rollback journal and
** false for the statement journal.  The main rollback journal uses
** checksums - the statement journal does not.
**
** If the page number of the page record read from the (sub-)journal file
** is greater than the current value of Pager.dbSize, then playback is
** skipped and SQLITE_OK is returned.
**
** If pDone is not NULL, then it is a record of pages that have already
** been played back.  If the page at *pOffset has already been played back
** (if the corresponding pDone bit is set) then skip the playback.
** Make sure the pDone bit corresponding to the *pOffset page is set
** prior to returning.
**
** If the page record is successfully read from the (sub-)journal file
** and played back, then SQLITE_OK is returned. If an IO error occurs
** while reading the record from the (sub-)journal file or while writing
** to the database file, then the IO error code is returned. If data
** is successfully read from the (sub-)journal file but appears to be
** corrupted, SQLITE_DONE is returned. Data is considered corrupted in
** two circumstances:
** 
**   * If the record page-number is illegal (0 or PAGER_MJ_PGNO), or
**   * If the record is being rolled back from the main journal file
**     and the checksum field does not match the record content.
**
** Neither of these two scenarios are possible during a savepoint rollback.
**
** If this is a savepoint rollback, then memory may have to be dynamically
** allocated by this function. If this is the case and an allocation fails,
** SQLITE_NOMEM is returned.
*/
static int pager_playback_one_page(
  Pager *pPager,                /* The pager being played back */
  int isMainJrnl,               /* 1 -> main journal. 0 -> sub-journal. */
  int isUnsync,                 /* True if reading from unsynced main journal */
  i64 *pOffset,                 /* Offset of record to playback */
  int isSavepnt,                /* True for a savepoint rollback */
  Bitvec *pDone                 /* Bitvec of pages already played back */
){
  int rc;
  PgHdr *pPg;                   /* An existing page in the cache */
  Pgno pgno;                    /* The page number of a page in journal */
  u32 cksum;                    /* Checksum used for sanity checking */
  u8 *aData;                    /* Temporary storage for the page */
  sqlite3_file *jfd;            /* The file descriptor for the journal file */

  assert( (isMainJrnl&~1)==0 );      /* isMainJrnl is 0 or 1 */
  assert( (isSavepnt&~1)==0 );       /* isSavepnt is 0 or 1 */
  assert( isMainJrnl || pDone );     /* pDone always used on sub-journals */
  assert( isSavepnt || pDone==0 );   /* pDone never used on non-savepoint */

  aData = (u8*)pPager->pTmpSpace;
  assert( aData );         /* Temp storage must have already been allocated */

  /* Read the page number and page data from the journal or sub-journal
  ** file. Return an error code to the caller if an IO error occurs.
  */
  jfd = isMainJrnl ? pPager->jfd : pPager->sjfd;
  rc = read32bits(jfd, *pOffset, &pgno);
  if( rc!=SQLITE_OK ) return rc;
  rc = sqlite3OsRead(jfd, aData, pPager->pageSize, (*pOffset)+4);
  if( rc!=SQLITE_OK ) return rc;
  *pOffset += pPager->pageSize + 4 + isMainJrnl*4;

  /* Sanity checking on the page.  This is more important that I originally
  ** thought.  If a power failure occurs while the journal is being written,
  ** it could cause invalid data to be written into the journal.  We need to
  ** detect this invalid data (with high probability) and ignore it.
  */
  if( pgno==0 || pgno==PAGER_MJ_PGNO(pPager) ){
    assert( !isSavepnt );
    return SQLITE_DONE;
  }
  if( pgno>(Pgno)pPager->dbSize || sqlite3BitvecTest(pDone, pgno) ){
    return SQLITE_OK;
  }
  if( isMainJrnl ){
    rc = read32bits(jfd, (*pOffset)-4, &cksum);
    if( rc ) return rc;
    if( !isSavepnt && pager_cksum(pPager, aData)!=cksum ){
      return SQLITE_DONE;
    }
  }

  if( pDone && (rc = sqlite3BitvecSet(pDone, pgno))!=SQLITE_OK ){
    return rc;
  }

  assert( pPager->state==PAGER_RESERVED || pPager->state>=PAGER_EXCLUSIVE );

  /* If the pager is in RESERVED state, then there must be a copy of this
  ** page in the pager cache. In this case just update the pager cache,
  ** not the database file. The page is left marked dirty in this case.
  **
  ** An exception to the above rule: If the database is in no-sync mode
  ** and a page is moved during an incremental vacuum then the page may
  ** not be in the pager cache. Later: if a malloc() or IO error occurs
  ** during a Movepage() call, then the page may not be in the cache
  ** either. So the condition described in the above paragraph is not
  ** assert()able.
  **
  ** If in EXCLUSIVE state, then we update the pager cache if it exists
  ** and the main file. The page is then marked not dirty.
  **
  ** Ticket #1171:  The statement journal might contain page content that is
  ** different from the page content at the start of the transaction.
  ** This occurs when a page is changed prior to the start of a statement
  ** then changed again within the statement.  When rolling back such a
  ** statement we must not write to the original database unless we know
  ** for certain that original page contents are synced into the main rollback
  ** journal.  Otherwise, a power loss might leave modified data in the
  ** database file without an entry in the rollback journal that can
  ** restore the database to its original form.  Two conditions must be
  ** met before writing to the database files. (1) the database must be
  ** locked.  (2) we know that the original page content is fully synced
  ** in the main journal either because the page is not in cache or else
  ** the page is marked as needSync==0.
  **
  ** 2008-04-14:  When attempting to vacuum a corrupt database file, it
  ** is possible to fail a statement on a database that does not yet exist.
  ** Do not attempt to write if database file has never been opened.
  */
  pPg = pager_lookup(pPager, pgno);
  assert( pPg || !MEMDB );
  PAGERTRACE(("PLAYBACK %d page %d hash(%08x) %s\n",
               PAGERID(pPager), pgno, pager_datahash(pPager->pageSize, aData),
               (isMainJrnl?"main-journal":"sub-journal")
  ));
  if( (pPager->state>=PAGER_EXCLUSIVE)
   && (pPg==0 || 0==(pPg->flags&PGHDR_NEED_SYNC))
   && isOpen(pPager->fd)
   && !isUnsync
  ){
    i64 ofst = (pgno-1)*(i64)pPager->pageSize;
    rc = sqlite3OsWrite(pPager->fd, aData, pPager->pageSize, ofst);
    if( pgno>pPager->dbFileSize ){
      pPager->dbFileSize = pgno;
    }
    sqlite3BackupUpdate(pPager->pBackup, pgno, aData);
  }else if( !isMainJrnl && pPg==0 ){
    /* If this is a rollback of a savepoint and data was not written to
    ** the database and the page is not in-memory, there is a potential
    ** problem. When the page is next fetched by the b-tree layer, it 
    ** will be read from the database file, which may or may not be 
    ** current. 
    **
    ** There are a couple of different ways this can happen. All are quite
    ** obscure. When running in synchronous mode, this can only happen 
    ** if the page is on the free-list at the start of the transaction, then
    ** populated, then moved using sqlite3PagerMovepage().
    **
    ** The solution is to add an in-memory page to the cache containing
    ** the data just read from the sub-journal. Mark the page as dirty 
    ** and if the pager requires a journal-sync, then mark the page as 
    ** requiring a journal-sync before it is written.
    */
    assert( isSavepnt );
    if( (rc = sqlite3PagerAcquire(pPager, pgno, &pPg, 1))!=SQLITE_OK ){
      return rc;
    }
    pPg->flags &= ~PGHDR_NEED_READ;
    sqlite3PcacheMakeDirty(pPg);
  }
  if( pPg ){
    /* No page should ever be explicitly rolled back that is in use, except
    ** for page 1 which is held in use in order to keep the lock on the
    ** database active. However such a page may be rolled back as a result
    ** of an internal error resulting in an automatic call to
    ** sqlite3PagerRollback().
    */
    void *pData;
    pData = pPg->pData;
    memcpy(pData, aData, pPager->pageSize);
    if( pPager->xReiniter ){
      pPager->xReiniter(pPg);
    }
    if( isMainJrnl && (!isSavepnt || *pOffset<=pPager->journalHdr) ){
      /* If the contents of this page were just restored from the main 
      ** journal file, then its content must be as they were when the 
      ** transaction was first opened. In this case we can mark the page
      ** as clean, since there will be no need to write it out to the.
      **
      ** There is one exception to this rule. If the page is being rolled
      ** back as part of a savepoint (or statement) rollback from an 
      ** unsynced portion of the main journal file, then it is not safe
      ** to mark the page as clean. This is because marking the page as
      ** clean will clear the PGHDR_NEED_SYNC flag. Since the page is
      ** already in the journal file (recorded in Pager.pInJournal) and
      ** the PGHDR_NEED_SYNC flag is cleared, if the page is written to
      ** again within this transaction, it will be marked as dirty but
      ** the PGHDR_NEED_SYNC flag will not be set. It could then potentially
      ** be written out into the database file before its journal file
      ** segment is synced. If a crash occurs during or following this,
      ** database corruption may ensue.
      */
      sqlite3PcacheMakeClean(pPg);
    }
#ifdef SQLITE_CHECK_PAGES
    pPg->pageHash = pager_pagehash(pPg);
#endif
    /* If this was page 1, then restore the value of Pager.dbFileVers.
    ** Do this before any decoding. */
    if( pgno==1 ){
      memcpy(&pPager->dbFileVers, &((u8*)pData)[24],sizeof(pPager->dbFileVers));
    }

    /* Decode the page just read from disk */
    CODEC1(pPager, pData, pPg->pgno, 3);
    sqlite3PcacheRelease(pPg);
  }
  return rc;
}

#if !defined(NDEBUG) || defined(SQLITE_COVERAGE_TEST)
/*
** This routine looks ahead into the main journal file and determines
** whether or not the next record (the record that begins at file
** offset pPager->journalOff) is a well-formed page record consisting
** of a valid page number, pPage->pageSize bytes of content, followed
** by a valid checksum.
**
** The pager never needs to know this in order to do its job.   This
** routine is only used from with assert() and testcase() macros.
*/
static int pagerNextJournalPageIsValid(Pager *pPager){
  Pgno pgno;           /* The page number of the page */
  u32 cksum;           /* The page checksum */
  int rc;              /* Return code from read operations */
  sqlite3_file *fd;    /* The file descriptor from which we are reading */
  u8 *aData;           /* Content of the page */

  /* Read the page number header */
  fd = pPager->jfd;
  rc = read32bits(fd, pPager->journalOff, &pgno);
  if( rc!=SQLITE_OK ){ return 0; }                                  /*NO_TEST*/
  if( pgno==0 || pgno==PAGER_MJ_PGNO(pPager) ){ return 0; }         /*NO_TEST*/
  if( pgno>(Pgno)pPager->dbSize ){ return 0; }                      /*NO_TEST*/

  /* Read the checksum */
  rc = read32bits(fd, pPager->journalOff+pPager->pageSize+4, &cksum);
  if( rc!=SQLITE_OK ){ return 0; }                                  /*NO_TEST*/

  /* Read the data and verify the checksum */
  aData = (u8*)pPager->pTmpSpace;
  rc = sqlite3OsRead(fd, aData, pPager->pageSize, pPager->journalOff+4);
  if( rc!=SQLITE_OK ){ return 0; }                                  /*NO_TEST*/
  if( pager_cksum(pPager, aData)!=cksum ){ return 0; }              /*NO_TEST*/

  /* Reach this point only if the page is valid */
  return 1;
}
#endif /* !defined(NDEBUG) || defined(SQLITE_COVERAGE_TEST) */

/*
** Parameter zMaster is the name of a master journal file. A single journal
** file that referred to the master journal file has just been rolled back.
** This routine checks if it is possible to delete the master journal file,
** and does so if it is.
**
** Argument zMaster may point to Pager.pTmpSpace. So that buffer is not 
** available for use within this function.
**
** When a master journal file is created, it is populated with the names 
** of all of its child journals, one after another, formatted as utf-8 
** encoded text. The end of each child journal file is marked with a 
** nul-terminator byte (0x00). i.e. the entire contents of a master journal
** file for a transaction involving two databases might be:
**
**   "/home/bill/a.db-journal\x00/home/bill/b.db-journal\x00"
**
** A master journal file may only be deleted once all of its child 
** journals have been rolled back.
**
** This function reads the contents of the master-journal file into 
** memory and loops through each of the child journal names. For
** each child journal, it checks if:
**
**   * if the child journal exists, and if so
**   * if the child journal contains a reference to master journal 
**     file zMaster
**
** If a child journal can be found that matches both of the criteria
** above, this function returns without doing anything. Otherwise, if
** no such child journal can be found, file zMaster is deleted from
** the file-system using sqlite3OsDelete().
**
** If an IO error within this function, an error code is returned. This
** function allocates memory by calling sqlite3Malloc(). If an allocation
** fails, SQLITE_NOMEM is returned. Otherwise, if no IO or malloc errors 
** occur, SQLITE_OK is returned.
**
** TODO: This function allocates a single block of memory to load
** the entire contents of the master journal file. This could be
** a couple of kilobytes or so - potentially larger than the page 
** size.
*/
static int pager_delmaster(Pager *pPager, const char *zMaster){
  sqlite3_vfs *pVfs = pPager->pVfs;
  int rc;                   /* Return code */
  sqlite3_file *pMaster;    /* Malloc'd master-journal file descriptor */
  sqlite3_file *pJournal;   /* Malloc'd child-journal file descriptor */
  char *zMasterJournal = 0; /* Contents of master journal file */
  i64 nMasterJournal;       /* Size of master journal file */

  /* Allocate space for both the pJournal and pMaster file descriptors.
  ** If successful, open the master journal file for reading.
  */
  pMaster = (sqlite3_file *)sqlite3MallocZero(pVfs->szOsFile * 2);
  pJournal = (sqlite3_file *)(((u8 *)pMaster) + pVfs->szOsFile);
  if( !pMaster ){
    rc = SQLITE_NOMEM;
  }else{
    const int flags = (SQLITE_OPEN_READONLY|SQLITE_OPEN_MASTER_JOURNAL);
    rc = sqlite3OsOpen(pVfs, zMaster, pMaster, flags, 0);
  }
  if( rc!=SQLITE_OK ) goto delmaster_out;

  rc = sqlite3OsFileSize(pMaster, &nMasterJournal);
  if( rc!=SQLITE_OK ) goto delmaster_out;

  if( nMasterJournal>0 ){
    char *zJournal;
    char *zMasterPtr = 0;
    int nMasterPtr = pVfs->mxPathname+1;

    /* Load the entire master journal file into space obtained from
    ** sqlite3_malloc() and pointed to by zMasterJournal. 
    */
    zMasterJournal = (char *)sqlite3Malloc((int)nMasterJournal + nMasterPtr);
    if( !zMasterJournal ){
      rc = SQLITE_NOMEM;
      goto delmaster_out;
    }
    zMasterPtr = &zMasterJournal[nMasterJournal];
    rc = sqlite3OsRead(pMaster, zMasterJournal, (int)nMasterJournal, 0);
    if( rc!=SQLITE_OK ) goto delmaster_out;

    zJournal = zMasterJournal;
    while( (zJournal-zMasterJournal)<nMasterJournal ){
      int exists;
      rc = sqlite3OsAccess(pVfs, zJournal, SQLITE_ACCESS_EXISTS, &exists);
      if( rc!=SQLITE_OK ){
        goto delmaster_out;
      }
      if( exists ){
        /* One of the journals pointed to by the master journal exists.
        ** Open it and check if it points at the master journal. If
        ** so, return without deleting the master journal file.
        */
        int c;
        int flags = (SQLITE_OPEN_READONLY|SQLITE_OPEN_MAIN_JOURNAL);
        rc = sqlite3OsOpen(pVfs, zJournal, pJournal, flags, 0);
        if( rc!=SQLITE_OK ){
          goto delmaster_out;
        }

        rc = readMasterJournal(pJournal, zMasterPtr, nMasterPtr);
        sqlite3OsClose(pJournal);
        if( rc!=SQLITE_OK ){
          goto delmaster_out;
        }

        c = zMasterPtr[0]!=0 && strcmp(zMasterPtr, zMaster)==0;
        if( c ){
          /* We have a match. Do not delete the master journal file. */
          goto delmaster_out;
        }
      }
      zJournal += (sqlite3Strlen30(zJournal)+1);
    }
  }
  
  rc = sqlite3OsDelete(pVfs, zMaster, 0);

delmaster_out:
  if( zMasterJournal ){
    sqlite3_free(zMasterJournal);
  }  
  if( pMaster ){
    sqlite3OsClose(pMaster);
    assert( !isOpen(pJournal) );
  }
  sqlite3_free(pMaster);
  return rc;
}


/*
** This function is used to change the actual size of the database 
** file in the file-system. This only happens when committing a transaction,
** or rolling back a transaction (including rolling back a hot-journal).
**
** If the main database file is not open, or an exclusive lock is not
** held, this function is a no-op. Otherwise, the size of the file is
** changed to nPage pages (nPage*pPager->pageSize bytes). If the file
** on disk is currently larger than nPage pages, then use the VFS
** xTruncate() method to truncate it.
**
** Or, it might might be the case that the file on disk is smaller than 
** nPage pages. Some operating system implementations can get confused if 
** you try to truncate a file to some size that is larger than it 
** currently is, so detect this case and write a single zero byte to 
** the end of the new file instead.
**
** If successful, return SQLITE_OK. If an IO error occurs while modifying
** the database file, return the error code to the caller.
*/
static int pager_truncate(Pager *pPager, Pgno nPage){
  int rc = SQLITE_OK;
  if( pPager->state>=PAGER_EXCLUSIVE && isOpen(pPager->fd) ){
    i64 currentSize, newSize;
    /* TODO: Is it safe to use Pager.dbFileSize here? */
    rc = sqlite3OsFileSize(pPager->fd, &currentSize);
    newSize = pPager->pageSize*(i64)nPage;
    if( rc==SQLITE_OK && currentSize!=newSize ){
      if( currentSize>newSize ){
        rc = sqlite3OsTruncate(pPager->fd, newSize);
      }else{
        rc = sqlite3OsWrite(pPager->fd, "", 1, newSize-1);
      }
      if( rc==SQLITE_OK ){
        pPager->dbFileSize = nPage;
      }
    }
  }
  return rc;
}

/*
** Set the value of the Pager.sectorSize variable for the given
** pager based on the value returned by the xSectorSize method
** of the open database file. The sector size will be used used 
** to determine the size and alignment of journal header and 
** master journal pointers within created journal files.
**
** For temporary files the effective sector size is always 512 bytes.
**
** Otherwise, for non-temporary files, the effective sector size is
** the value returned by the xSectorSize() method rounded up to 512 if
** it is less than 512, or rounded down to MAX_SECTOR_SIZE if it
** is greater than MAX_SECTOR_SIZE.
*/
static void setSectorSize(Pager *pPager){
  assert( isOpen(pPager->fd) || pPager->tempFile );

  if( !pPager->tempFile ){
    /* Sector size doesn't matter for temporary files. Also, the file
    ** may not have been opened yet, in which case the OsSectorSize()
    ** call will segfault.
    */
    pPager->sectorSize = sqlite3OsSectorSize(pPager->fd);
  }
  if( pPager->sectorSize<512 ){
    pPager->sectorSize = 512;
  }
  if( pPager->sectorSize>MAX_SECTOR_SIZE ){
    assert( MAX_SECTOR_SIZE>=512 );
    pPager->sectorSize = MAX_SECTOR_SIZE;
  }
}

/*
** Playback the journal and thus restore the database file to
** the state it was in before we started making changes.  
**
** The journal file format is as follows: 
**
**  (1)  8 byte prefix.  A copy of aJournalMagic[].
**  (2)  4 byte big-endian integer which is the number of valid page records
**       in the journal.  If this value is 0xffffffff, then compute the
**       number of page records from the journal size.
**  (3)  4 byte big-endian integer which is the initial value for the 
**       sanity checksum.
**  (4)  4 byte integer which is the number of pages to truncate the
**       database to during a rollback.
**  (5)  4 byte big-endian integer which is the sector size.  The header
**       is this many bytes in size.
**  (6)  4 byte big-endian integer which is the page case.
**  (7)  4 byte integer which is the number of bytes in the master journal
**       name.  The value may be zero (indicate that there is no master
**       journal.)
**  (8)  N bytes of the master journal name.  The name will be nul-terminated
**       and might be shorter than the value read from (5).  If the first byte
**       of the name is \000 then there is no master journal.  The master
**       journal name is stored in UTF-8.
**  (9)  Zero or more pages instances, each as follows:
**        +  4 byte page number.
**        +  pPager->pageSize bytes of data.
**        +  4 byte checksum
**
** When we speak of the journal header, we mean the first 8 items above.
** Each entry in the journal is an instance of the 9th item.
**
** Call the value from the second bullet "nRec".  nRec is the number of
** valid page entries in the journal.  In most cases, you can compute the
** value of nRec from the size of the journal file.  But if a power
** failure occurred while the journal was being written, it could be the
** case that the size of the journal file had already been increased but
** the extra entries had not yet made it safely to disk.  In such a case,
** the value of nRec computed from the file size would be too large.  For
** that reason, we always use the nRec value in the header.
**
** If the nRec value is 0xffffffff it means that nRec should be computed
** from the file size.  This value is used when the user selects the
** no-sync option for the journal.  A power failure could lead to corruption
** in this case.  But for things like temporary table (which will be
** deleted when the power is restored) we don't care.  
**
** If the file opened as the journal file is not a well-formed
** journal file then all pages up to the first corrupted page are rolled
** back (or no pages if the journal header is corrupted). The journal file
** is then deleted and SQLITE_OK returned, just as if no corruption had
** been encountered.
**
** If an I/O or malloc() error occurs, the journal-file is not deleted
** and an error code is returned.
**
** The isHot parameter indicates that we are trying to rollback a journal
** that might be a hot journal.  Or, it could be that the journal is 
** preserved because of JOURNALMODE_PERSIST or JOURNALMODE_TRUNCATE.
** If the journal really is hot, reset the pager cache prior rolling
** back any content.  If the journal is merely persistent, no reset is
** needed.
*/
static int pager_playback(Pager *pPager, int isHot){
  sqlite3_vfs *pVfs = pPager->pVfs;
  i64 szJ;                 /* Size of the journal file in bytes */
  u32 nRec;                /* Number of Records in the journal */
  u32 u;                   /* Unsigned loop counter */
  Pgno mxPg = 0;           /* Size of the original file in pages */
  int rc;                  /* Result code of a subroutine */
  int res = 1;             /* Value returned by sqlite3OsAccess() */
  char *zMaster = 0;       /* Name of master journal file if any */
  int needPagerReset;      /* True to reset page prior to first page rollback */

  /* Figure out how many records are in the journal.  Abort early if
  ** the journal is empty.
  */
  assert( isOpen(pPager->jfd) );
  rc = sqlite3OsFileSize(pPager->jfd, &szJ);
  if( rc!=SQLITE_OK || szJ==0 ){
    goto end_playback;
  }

  /* Read the master journal name from the journal, if it is present.
  ** If a master journal file name is specified, but the file is not
  ** present on disk, then the journal is not hot and does not need to be
  ** played back.
  **
  ** TODO: Technically the following is an error because it assumes that
  ** buffer Pager.pTmpSpace is (mxPathname+1) bytes or larger. i.e. that
  ** (pPager->pageSize >= pPager->pVfs->mxPathname+1). Using os_unix.c,
  **  mxPathname is 512, which is the same as the minimum allowable value
  ** for pageSize.
  */
  zMaster = pPager->pTmpSpace;
  rc = readMasterJournal(pPager->jfd, zMaster, pPager->pVfs->mxPathname+1);
  if( rc==SQLITE_OK && zMaster[0] ){
    rc = sqlite3OsAccess(pVfs, zMaster, SQLITE_ACCESS_EXISTS, &res);
  }
  zMaster = 0;
  if( rc!=SQLITE_OK || !res ){
    goto end_playback;
  }
  pPager->journalOff = 0;
  needPagerReset = isHot;

  /* This loop terminates either when a readJournalHdr() or 
  ** pager_playback_one_page() call returns SQLITE_DONE or an IO error 
  ** occurs. 
  */
  while( 1 ){
    int isUnsync = 0;

    /* Read the next journal header from the journal file.  If there are
    ** not enough bytes left in the journal file for a complete header, or
    ** it is corrupted, then a process must of failed while writing it.
    ** This indicates nothing more needs to be rolled back.
    */
    rc = readJournalHdr(pPager, szJ, &nRec, &mxPg);
    if( rc!=SQLITE_OK ){ 
      if( rc==SQLITE_DONE ){
        rc = SQLITE_OK;
      }
      goto end_playback;
    }

    /* If nRec is 0xffffffff, then this journal was created by a process
    ** working in no-sync mode. This means that the rest of the journal
    ** file consists of pages, there are no more journal headers. Compute
    ** the value of nRec based on this assumption.
    */
    if( nRec==0xffffffff ){
      assert( pPager->journalOff==JOURNAL_HDR_SZ(pPager) );
      nRec = (int)((szJ - JOURNAL_HDR_SZ(pPager))/JOURNAL_PG_SZ(pPager));
    }

    /* If nRec is 0 and this rollback is of a transaction created by this
    ** process and if this is the final header in the journal, then it means
    ** that this part of the journal was being filled but has not yet been
    ** synced to disk.  Compute the number of pages based on the remaining
    ** size of the file.
    **
    ** The third term of the test was added to fix ticket #2565.
    ** When rolling back a hot journal, nRec==0 always means that the next
    ** chunk of the journal contains zero pages to be rolled back.  But
    ** when doing a ROLLBACK and the nRec==0 chunk is the last chunk in
    ** the journal, it means that the journal might contain additional
    ** pages that need to be rolled back and that the number of pages 
    ** should be computed based on the journal file size.
    */
    testcase( nRec==0 && !isHot
         && pPager->journalHdr+JOURNAL_HDR_SZ(pPager)!=pPager->journalOff
         && ((szJ - pPager->journalOff) / JOURNAL_PG_SZ(pPager))>0
         && pagerNextJournalPageIsValid(pPager)
    );
    if( nRec==0 && !isHot &&
        pPager->journalHdr+JOURNAL_HDR_SZ(pPager)==pPager->journalOff ){
      nRec = (int)((szJ - pPager->journalOff) / JOURNAL_PG_SZ(pPager));
      isUnsync = 1;
    }

    /* If this is the first header read from the journal, truncate the
    ** database file back to its original size.
    */
    if( pPager->journalOff==JOURNAL_HDR_SZ(pPager) ){
      rc = pager_truncate(pPager, mxPg);
      if( rc!=SQLITE_OK ){
        goto end_playback;
      }
      pPager->dbSize = mxPg;
    }

    /* Copy original pages out of the journal and back into the 
    ** database file and/or page cache.
    */
    for(u=0; u<nRec; u++){
      if( needPagerReset ){
        pager_reset(pPager);
        needPagerReset = 0;
      }
      rc = pager_playback_one_page(pPager,1,isUnsync,&pPager->journalOff,0,0);
      if( rc!=SQLITE_OK ){
        if( rc==SQLITE_DONE ){
          rc = SQLITE_OK;
          pPager->journalOff = szJ;
          break;
        }else{
          /* If we are unable to rollback, quit and return the error
          ** code.  This will cause the pager to enter the error state
          ** so that no further harm will be done.  Perhaps the next
          ** process to come along will be able to rollback the database.
          */
          goto end_playback;
        }
      }
    }
  }
  /*NOTREACHED*/
  assert( 0 );

end_playback:
  /* Following a rollback, the database file should be back in its original
  ** state prior to the start of the transaction, so invoke the
  ** SQLITE_FCNTL_DB_UNCHANGED file-control method to disable the
  ** assertion that the transaction counter was modified.
  */
  assert(
    pPager->fd->pMethods==0 ||
    sqlite3OsFileControl(pPager->fd,SQLITE_FCNTL_DB_UNCHANGED,0)>=SQLITE_OK
  );

  /* If this playback is happening automatically as a result of an IO or 
  ** malloc error that occurred after the change-counter was updated but 
  ** before the transaction was committed, then the change-counter 
  ** modification may just have been reverted. If this happens in exclusive 
  ** mode, then subsequent transactions performed by the connection will not
  ** update the change-counter at all. This may lead to cache inconsistency
  ** problems for other processes at some point in the future. So, just
  ** in case this has happened, clear the changeCountDone flag now.
  */
  pPager->changeCountDone = pPager->tempFile;

  if( rc==SQLITE_OK ){
    zMaster = pPager->pTmpSpace;
    rc = readMasterJournal(pPager->jfd, zMaster, pPager->pVfs->mxPathname+1);
    testcase( rc!=SQLITE_OK );
  }
  if( rc==SQLITE_OK ){
    rc = pager_end_transaction(pPager, zMaster[0]!='\0');
    testcase( rc!=SQLITE_OK );
  }
  if( rc==SQLITE_OK && zMaster[0] && res ){
    /* If there was a master journal and this routine will return success,
    ** see if it is possible to delete the master journal.
    */
    rc = pager_delmaster(pPager, zMaster);
    testcase( rc!=SQLITE_OK );
  }

  /* The Pager.sectorSize variable may have been updated while rolling
  ** back a journal created by a process with a different sector size
  ** value. Reset it to the correct value for this process.
  */
  setSectorSize(pPager);
  return rc;
}

/*
** Playback savepoint pSavepoint. Or, if pSavepoint==NULL, then playback
** the entire master journal file. The case pSavepoint==NULL occurs when 
** a ROLLBACK TO command is invoked on a SAVEPOINT that is a transaction 
** savepoint.
**
** When pSavepoint is not NULL (meaning a non-transaction savepoint is 
** being rolled back), then the rollback consists of up to three stages,
** performed in the order specified:
**
**   * Pages are played back from the main journal starting at byte
**     offset PagerSavepoint.iOffset and continuing to 
**     PagerSavepoint.iHdrOffset, or to the end of the main journal
**     file if PagerSavepoint.iHdrOffset is zero.
**
**   * If PagerSavepoint.iHdrOffset is not zero, then pages are played
**     back starting from the journal header immediately following 
**     PagerSavepoint.iHdrOffset to the end of the main journal file.
**
**   * Pages are then played back from the sub-journal file, starting
**     with the PagerSavepoint.iSubRec and continuing to the end of
**     the journal file.
**
** Throughout the rollback process, each time a page is rolled back, the
** corresponding bit is set in a bitvec structure (variable pDone in the
** implementation below). This is used to ensure that a page is only
** rolled back the first time it is encountered in either journal.
**
** If pSavepoint is NULL, then pages are only played back from the main
** journal file. There is no need for a bitvec in this case.
**
** In either case, before playback commences the Pager.dbSize variable
** is reset to the value that it held at the start of the savepoint 
** (or transaction). No page with a page-number greater than this value
** is played back. If one is encountered it is simply skipped.
*/
static int pagerPlaybackSavepoint(Pager *pPager, PagerSavepoint *pSavepoint){
  i64 szJ;                 /* Effective size of the main journal */
  i64 iHdrOff;             /* End of first segment of main-journal records */
  int rc = SQLITE_OK;      /* Return code */
  Bitvec *pDone = 0;       /* Bitvec to ensure pages played back only once */

  assert( pPager->state>=PAGER_SHARED );

  /* Allocate a bitvec to use to store the set of pages rolled back */
  if( pSavepoint ){
    pDone = sqlite3BitvecCreate(pSavepoint->nOrig);
    if( !pDone ){
      return SQLITE_NOMEM;
    }
  }

  /* Set the database size back to the value it was before the savepoint 
  ** being reverted was opened.
  */
  pPager->dbSize = pSavepoint ? pSavepoint->nOrig : pPager->dbOrigSize;

  /* Use pPager->journalOff as the effective size of the main rollback
  ** journal.  The actual file might be larger than this in
  ** PAGER_JOURNALMODE_TRUNCATE or PAGER_JOURNALMODE_PERSIST.  But anything
  ** past pPager->journalOff is off-limits to us.
  */
  szJ = pPager->journalOff;

  /* Begin by rolling back records from the main journal starting at
  ** PagerSavepoint.iOffset and continuing to the next journal header.
  ** There might be records in the main journal that have a page number
  ** greater than the current database size (pPager->dbSize) but those
  ** will be skipped automatically.  Pages are added to pDone as they
  ** are played back.
  */
  if( pSavepoint ){
    iHdrOff = pSavepoint->iHdrOffset ? pSavepoint->iHdrOffset : szJ;
    pPager->journalOff = pSavepoint->iOffset;
    while( rc==SQLITE_OK && pPager->journalOff<iHdrOff ){
      rc = pager_playback_one_page(pPager, 1, 0, &pPager->journalOff, 1, pDone);
    }
    assert( rc!=SQLITE_DONE );
  }else{
    pPager->journalOff = 0;
  }

  /* Continue rolling back records out of the main journal starting at
  ** the first journal header seen and continuing until the effective end
  ** of the main journal file.  Continue to skip out-of-range pages and
  ** continue adding pages rolled back to pDone.
  */
  while( rc==SQLITE_OK && pPager->journalOff<szJ ){
    u32 ii;            /* Loop counter */
    u32 nJRec = 0;     /* Number of Journal Records */
    u32 dummy;
    rc = readJournalHdr(pPager, szJ, &nJRec, &dummy);
    assert( rc!=SQLITE_DONE );

    /*
    ** The "pPager->journalHdr+JOURNAL_HDR_SZ(pPager)==pPager->journalOff"
    ** test is related to ticket #2565.  See the discussion in the
    ** pager_playback() function for additional information.
    */
    assert( !(nJRec==0
         && pPager->journalHdr+JOURNAL_HDR_SZ(pPager)!=pPager->journalOff
         && ((szJ - pPager->journalOff) / JOURNAL_PG_SZ(pPager))>0
         && pagerNextJournalPageIsValid(pPager))
    );
    if( nJRec==0 
     && pPager->journalHdr+JOURNAL_HDR_SZ(pPager)==pPager->journalOff
    ){
      nJRec = (u32)((szJ - pPager->journalOff)/JOURNAL_PG_SZ(pPager));
    }
    for(ii=0; rc==SQLITE_OK && ii<nJRec && pPager->journalOff<szJ; ii++){
      rc = pager_playback_one_page(pPager, 1, 0, &pPager->journalOff, 1, pDone);
    }
    assert( rc!=SQLITE_DONE );
  }
  assert( rc!=SQLITE_OK || pPager->journalOff==szJ );

  /* Finally,  rollback pages from the sub-journal.  Page that were
  ** previously rolled back out of the main journal (and are hence in pDone)
  ** will be skipped.  Out-of-range pages are also skipped.
  */
  if( pSavepoint ){
    u32 ii;            /* Loop counter */
    i64 offset = pSavepoint->iSubRec*(4+pPager->pageSize);
    for(ii=pSavepoint->iSubRec; rc==SQLITE_OK && ii<pPager->nSubRec; ii++){
      assert( offset==ii*(4+pPager->pageSize) );
      rc = pager_playback_one_page(pPager, 0, 0, &offset, 1, pDone);
    }
    assert( rc!=SQLITE_DONE );
  }

  sqlite3BitvecDestroy(pDone);
  if( rc==SQLITE_OK ){
    pPager->journalOff = szJ;
  }
  return rc;
}

/*
** Change the maximum number of in-memory pages that are allowed.
*/
void sqlite3PagerSetCachesize(Pager *pPager, int mxPage){
  sqlite3PcacheSetCachesize(pPager->pPCache, mxPage);
}

/*
** Adjust the robustness of the database to damage due to OS crashes
** or power failures by changing the number of syncs()s when writing
** the rollback journal.  There are three levels:
**
**    OFF       sqlite3OsSync() is never called.  This is the default
**              for temporary and transient files.
**
**    NORMAL    The journal is synced once before writes begin on the
**              database.  This is normally adequate protection, but
**              it is theoretically possible, though very unlikely,
**              that an inopertune power failure could leave the journal
**              in a state which would cause damage to the database
**              when it is rolled back.
**
**    FULL      The journal is synced twice before writes begin on the
**              database (with some additional information - the nRec field
**              of the journal header - being written in between the two
**              syncs).  If we assume that writing a
**              single disk sector is atomic, then this mode provides
**              assurance that the journal will not be corrupted to the
**              point of causing damage to the database during rollback.
**
** Numeric values associated with these states are OFF==1, NORMAL=2,
** and FULL=3.
*/
#ifndef SQLITE_OMIT_PAGER_PRAGMAS
void sqlite3PagerSetSafetyLevel(Pager *pPager, int level, int bFullFsync){
  pPager->noSync =  (level==1 || pPager->tempFile) ?1:0;
  pPager->fullSync = (level==3 && !pPager->tempFile) ?1:0;
  pPager->sync_flags = (bFullFsync?SQLITE_SYNC_FULL:SQLITE_SYNC_NORMAL);
  if( pPager->noSync ) pPager->needSync = 0;
}
#endif

/*
** The following global variable is incremented whenever the library
** attempts to open a temporary file.  This information is used for
** testing and analysis only.  
*/
#ifdef SQLITE_TEST
int sqlite3_opentemp_count = 0;
#endif

/*
** Open a temporary file.
**
** Write the file descriptor into *pFile. Return SQLITE_OK on success 
** or some other error code if we fail. The OS will automatically 
** delete the temporary file when it is closed.
**
** The flags passed to the VFS layer xOpen() call are those specified
** by parameter vfsFlags ORed with the following:
**
**     SQLITE_OPEN_READWRITE
**     SQLITE_OPEN_CREATE
**     SQLITE_OPEN_EXCLUSIVE
**     SQLITE_OPEN_DELETEONCLOSE
*/
static int pagerOpentemp(
  Pager *pPager,        /* The pager object */
  sqlite3_file *pFile,  /* Write the file descriptor here */
  int vfsFlags          /* Flags passed through to the VFS */
){
  int rc;               /* Return code */

#ifdef SQLITE_TEST
  sqlite3_opentemp_count++;  /* Used for testing and analysis only */
#endif

  vfsFlags |=  SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
            SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE;
  rc = sqlite3OsOpen(pPager->pVfs, 0, pFile, vfsFlags, 0);
  assert( rc!=SQLITE_OK || isOpen(pFile) );
  return rc;
}

/*
** Set the busy handler function.
**
** The pager invokes the busy-handler if sqlite3OsLock() returns 
** SQLITE_BUSY when trying to upgrade from no-lock to a SHARED lock,
** or when trying to upgrade from a RESERVED lock to an EXCLUSIVE 
** lock. It does *not* invoke the busy handler when upgrading from
** SHARED to RESERVED, or when upgrading from SHARED to EXCLUSIVE
** (which occurs during hot-journal rollback). Summary:
**
**   Transition                        | Invokes xBusyHandler
**   --------------------------------------------------------
**   NO_LOCK       -> SHARED_LOCK      | Yes
**   SHARED_LOCK   -> RESERVED_LOCK    | No
**   SHARED_LOCK   -> EXCLUSIVE_LOCK   | No
**   RESERVED_LOCK -> EXCLUSIVE_LOCK   | Yes
**
** If the busy-handler callback returns non-zero, the lock is 
** retried. If it returns zero, then the SQLITE_BUSY error is
** returned to the caller of the pager API function.
*/
void sqlite3PagerSetBusyhandler(
  Pager *pPager,                       /* Pager object */
  int (*xBusyHandler)(void *),         /* Pointer to busy-handler function */
  void *pBusyHandlerArg                /* Argument to pass to xBusyHandler */
){  
  pPager->xBusyHandler = xBusyHandler;
  pPager->pBusyHandlerArg = pBusyHandlerArg;
}

/*
** Set the reinitializer for this pager. If not NULL, the reinitializer
** is called when the content of a page in cache is modified (restored)
** as part of a transaction or savepoint rollback. The callback gives 
** higher-level code an opportunity to restore the EXTRA section to 
** agree with the restored page data.
*/
void sqlite3PagerSetReiniter(Pager *pPager, void (*xReinit)(DbPage*)){
  pPager->xReiniter = xReinit;
}

/*
** Change the page size used by the Pager object. The new page size 
** is passed in *pPageSize.
**
** If the pager is in the error state when this function is called, it
** is a no-op. The value returned is the error state error code (i.e. 
** one of SQLITE_IOERR, SQLITE_CORRUPT or SQLITE_FULL).
**
** Otherwise, if all of the following are true:
**
**   * the new page size (value of *pPageSize) is valid (a power 
**     of two between 512 and SQLITE_MAX_PAGE_SIZE, inclusive), and
**
**   * there are no outstanding page references, and
**
**   * the database is either not an in-memory database or it is
**     an in-memory database that currently consists of zero pages.
**
** then the pager object page size is set to *pPageSize.
**
** If the page size is changed, then this function uses sqlite3PagerMalloc() 
** to obtain a new Pager.pTmpSpace buffer. If this allocation attempt 
** fails, SQLITE_NOMEM is returned and the page size remains unchanged. 
** In all other cases, SQLITE_OK is returned.
**
** If the page size is not changed, either because one of the enumerated
** conditions above is not true, the pager was in error state when this
** function was called, or because the memory allocation attempt failed, 
** then *pPageSize is set to the old, retained page size before returning.
*/
int sqlite3PagerSetPagesize(Pager *pPager, u16 *pPageSize){
  int rc = pPager->errCode;
  if( rc==SQLITE_OK ){
    u16 pageSize = *pPageSize;
    assert( pageSize==0 || (pageSize>=512 && pageSize<=SQLITE_MAX_PAGE_SIZE) );
    if( pageSize && pageSize!=pPager->pageSize 
     && (pPager->memDb==0 || pPager->dbSize==0)
     && sqlite3PcacheRefCount(pPager->pPCache)==0 
    ){
      char *pNew = (char *)sqlite3PageMalloc(pageSize);
      if( !pNew ){
        rc = SQLITE_NOMEM;
      }else{
        pager_reset(pPager);
        pPager->pageSize = pageSize;
        sqlite3PageFree(pPager->pTmpSpace);
        pPager->pTmpSpace = pNew;
        sqlite3PcacheSetPageSize(pPager->pPCache, pageSize);
      }
    }
    *pPageSize = (u16)pPager->pageSize;
  }
  return rc;
}

/*
** Return a pointer to the "temporary page" buffer held internally
** by the pager.  This is a buffer that is big enough to hold the
** entire content of a database page.  This buffer is used internally
** during rollback and will be overwritten whenever a rollback
** occurs.  But other modules are free to use it too, as long as
** no rollbacks are happening.
*/
void *sqlite3PagerTempSpace(Pager *pPager){
  return pPager->pTmpSpace;
}

/*
** Attempt to set the maximum database page count if mxPage is positive. 
** Make no changes if mxPage is zero or negative.  And never reduce the
** maximum page count below the current size of the database.
**
** Regardless of mxPage, return the current maximum page count.
*/
int sqlite3PagerMaxPageCount(Pager *pPager, int mxPage){
  if( mxPage>0 ){
    pPager->mxPgno = mxPage;
  }
  sqlite3PagerPagecount(pPager, 0);
  return pPager->mxPgno;
}

/*
** The following set of routines are used to disable the simulated
** I/O error mechanism.  These routines are used to avoid simulated
** errors in places where we do not care about errors.
**
** Unless -DSQLITE_TEST=1 is used, these routines are all no-ops
** and generate no code.
*/
#ifdef SQLITE_TEST
extern int sqlite3_io_error_pending;
extern int sqlite3_io_error_hit;
static int saved_cnt;
void disable_simulated_io_errors(void){
  saved_cnt = sqlite3_io_error_pending;
  sqlite3_io_error_pending = -1;
}
void enable_simulated_io_errors(void){
  sqlite3_io_error_pending = saved_cnt;
}
#else
# define disable_simulated_io_errors()
# define enable_simulated_io_errors()
#endif

/*
** Read the first N bytes from the beginning of the file into memory
** that pDest points to. 
**
** If the pager was opened on a transient file (zFilename==""), or
** opened on a file less than N bytes in size, the output buffer is
** zeroed and SQLITE_OK returned. The rationale for this is that this 
** function is used to read database headers, and a new transient or
** zero sized database has a header than consists entirely of zeroes.
**
** If any IO error apart from SQLITE_IOERR_SHORT_READ is encountered,
** the error code is returned to the caller and the contents of the
** output buffer undefined.
*/
int sqlite3PagerReadFileheader(Pager *pPager, int N, unsigned char *pDest){
  int rc = SQLITE_OK;
  memset(pDest, 0, N);
  assert( isOpen(pPager->fd) || pPager->tempFile );
  if( isOpen(pPager->fd) ){
    IOTRACE(("DBHDR %p 0 %d\n", pPager, N))
    rc = sqlite3OsRead(pPager->fd, pDest, N, 0);
    if( rc==SQLITE_IOERR_SHORT_READ ){
      rc = SQLITE_OK;
    }
  }
  return rc;
}

/*
** Return the total number of pages in the database file associated 
** with pPager. Normally, this is calculated as (<db file size>/<page-size>).
** However, if the file is between 1 and <page-size> bytes in size, then 
** this is considered a 1 page file.
**
** If the pager is in error state when this function is called, then the
** error state error code is returned and *pnPage left unchanged. Or,
** if the file system has to be queried for the size of the file and
** the query attempt returns an IO error, the IO error code is returned
** and *pnPage is left unchanged.
**
** Otherwise, if everything is successful, then SQLITE_OK is returned
** and *pnPage is set to the number of pages in the database.
*/
int sqlite3PagerPagecount(Pager *pPager, int *pnPage){
  Pgno nPage;               /* Value to return via *pnPage */

  /* If the pager is already in the error state, return the error code. */
  if( pPager->errCode ){
    return pPager->errCode;
  }

  /* Determine the number of pages in the file. Store this in nPage. */
  if( pPager->dbSizeValid ){
    nPage = pPager->dbSize;
  }else{
    int rc;                 /* Error returned by OsFileSize() */
    i64 n = 0;              /* File size in bytes returned by OsFileSize() */

    assert( isOpen(pPager->fd) || pPager->tempFile );
    if( isOpen(pPager->fd) && (0 != (rc = sqlite3OsFileSize(pPager->fd, &n))) ){
      pager_error(pPager, rc);
      return rc;
    }
    if( n>0 && n<pPager->pageSize ){
      nPage = 1;
    }else{
      nPage = (Pgno)(n / pPager->pageSize);
    }
    if( pPager->state!=PAGER_UNLOCK ){
      pPager->dbSize = nPage;
      pPager->dbFileSize = nPage;
      pPager->dbSizeValid = 1;
    }
  }

  /* If the current number of pages in the file is greater than the 
  ** configured maximum pager number, increase the allowed limit so
  ** that the file can be read.
  */
  if( nPage>pPager->mxPgno ){
    pPager->mxPgno = (Pgno)nPage;
  }

  /* Set the output variable and return SQLITE_OK */
  if( pnPage ){
    *pnPage = nPage;
  }
  return SQLITE_OK;
}


/*
** Try to obtain a lock of type locktype on the database file. If
** a similar or greater lock is already held, this function is a no-op
** (returning SQLITE_OK immediately).
**
** Otherwise, attempt to obtain the lock using sqlite3OsLock(). Invoke 
** the busy callback if the lock is currently not available. Repeat 
** until the busy callback returns false or until the attempt to 
** obtain the lock succeeds.
**
** Return SQLITE_OK on success and an error code if we cannot obtain
** the lock. If the lock is obtained successfully, set the Pager.state 
** variable to locktype before returning.
*/
static int pager_wait_on_lock(Pager *pPager, int locktype){
  int rc;                              /* Return code */

  /* The OS lock values must be the same as the Pager lock values */
  assert( PAGER_SHARED==SHARED_LOCK );
  assert( PAGER_RESERVED==RESERVED_LOCK );
  assert( PAGER_EXCLUSIVE==EXCLUSIVE_LOCK );

  /* If the file is currently unlocked then the size must be unknown */
  assert( pPager->state>=PAGER_SHARED || pPager->dbSizeValid==0 );

  /* Check that this is either a no-op (because the requested lock is 
  ** already held, or one of the transistions that the busy-handler
  ** may be invoked during, according to the comment above
  ** sqlite3PagerSetBusyhandler().
  */
  assert( (pPager->state>=locktype)
       || (pPager->state==PAGER_UNLOCK && locktype==PAGER_SHARED)
       || (pPager->state==PAGER_RESERVED && locktype==PAGER_EXCLUSIVE)
  );

  if( pPager->state>=locktype ){
    rc = SQLITE_OK;
  }else{
    do {
      rc = sqlite3OsLock(pPager->fd, locktype);
    }while( rc==SQLITE_BUSY && pPager->xBusyHandler(pPager->pBusyHandlerArg) );
    if( rc==SQLITE_OK ){
      pPager->state = (u8)locktype;
      IOTRACE(("LOCK %p %d\n", pPager, locktype))
    }
  }
  return rc;
}

/*
** Truncate the in-memory database file image to nPage pages. This 
** function does not actually modify the database file on disk. It 
** just sets the internal state of the pager object so that the 
** truncation will be done when the current transaction is committed.
*/
void sqlite3PagerTruncateImage(Pager *pPager, Pgno nPage){
  assert( pPager->dbSizeValid );
  assert( pPager->dbSize>=nPage );
  assert( pPager->state>=PAGER_RESERVED );
  pPager->dbSize = nPage;
}

/*
** Shutdown the page cache.  Free all memory and close all files.
**
** If a transaction was in progress when this routine is called, that
** transaction is rolled back.  All outstanding pages are invalidated
** and their memory is freed.  Any attempt to use a page associated
** with this page cache after this function returns will likely
** result in a coredump.
**
** This function always succeeds. If a transaction is active an attempt
** is made to roll it back. If an error occurs during the rollback 
** a hot journal may be left in the filesystem but no error is returned
** to the caller.
*/
int sqlite3PagerClose(Pager *pPager){
  disable_simulated_io_errors();
  sqlite3BeginBenignMalloc();
  pPager->errCode = 0;
  pPager->exclusiveMode = 0;
  pager_reset(pPager);
  if( MEMDB ){
    pager_unlock(pPager);
  }else{
    /* Set Pager.journalHdr to -1 for the benefit of the pager_playback() 
    ** call which may be made from within pagerUnlockAndRollback(). If it
    ** is not -1, then the unsynced portion of an open journal file may
    ** be played back into the database. If a power failure occurs while
    ** this is happening, the database may become corrupt.
    */
    pPager->journalHdr = -1;
    pagerUnlockAndRollback(pPager);
  }
  sqlite3EndBenignMalloc();
  enable_simulated_io_errors();
  PAGERTRACE(("CLOSE %d\n", PAGERID(pPager)));
  IOTRACE(("CLOSE %p\n", pPager))
  sqlite3OsClose(pPager->fd);

  /* BEGIN CRYPTO */
#ifdef SQLITE_HAS_CODEC
  extern int sqlite3FreeCodecArg(void *);
  if(pPager->pCodecArg) sqlite3FreeCodecArg(pPager->pCodecArg);
#endif
  /* END CRYPTO */

  sqlite3PageFree(pPager->pTmpSpace);
  sqlite3PcacheClose(pPager->pPCache);

  assert( !pPager->aSavepoint && !pPager->pInJournal );
  assert( !isOpen(pPager->jfd) && !isOpen(pPager->sjfd) );

  sqlite3_free(pPager);
  return SQLITE_OK;
}

#if !defined(NDEBUG) || defined(SQLITE_TEST)
/*
** Return the page number for page pPg.
*/
Pgno sqlite3PagerPagenumber(DbPage *pPg){
  return pPg->pgno;
}
#endif

/*
** Increment the reference count for page pPg.
*/
void sqlite3PagerRef(DbPage *pPg){
  sqlite3PcacheRef(pPg);
}

/*
** Sync the journal. In other words, make sure all the pages that have
** been written to the journal have actually reached the surface of the
** disk and can be restored in the event of a hot-journal rollback.
**
** If the Pager.needSync flag is not set, then this function is a
** no-op. Otherwise, the actions required depend on the journal-mode
** and the device characteristics of the the file-system, as follows:
**
**   * If the journal file is an in-memory journal file, no action need
**     be taken.
**
**   * Otherwise, if the device does not support the SAFE_APPEND property,
**     then the nRec field of the most recently written journal header
**     is updated to contain the number of journal records that have
**     been written following it. If the pager is operating in full-sync
**     mode, then the journal file is synced before this field is updated.
**
**   * If the device does not support the SEQUENTIAL property, then 
**     journal file is synced.
**
** Or, in pseudo-code:
**
**   if( NOT <in-memory journal> ){
**     if( NOT SAFE_APPEND ){
**       if( <full-sync mode> ) xSync(<journal file>);
**       <update nRec field>
**     } 
**     if( NOT SEQUENTIAL ) xSync(<journal file>);
**   }
**
** The Pager.needSync flag is never be set for temporary files, or any
** file operating in no-sync mode (Pager.noSync set to non-zero).
**
** If successful, this routine clears the PGHDR_NEED_SYNC flag of every 
** page currently held in memory before returning SQLITE_OK. If an IO
** error is encountered, then the IO error code is returned to the caller.
*/
static int syncJournal(Pager *pPager){
  if( pPager->needSync ){
    assert( !pPager->tempFile );
    if( pPager->journalMode!=PAGER_JOURNALMODE_MEMORY ){
      int rc;                              /* Return code */
      const int iDc = sqlite3OsDeviceCharacteristics(pPager->fd);
      assert( isOpen(pPager->jfd) );

      if( 0==(iDc&SQLITE_IOCAP_SAFE_APPEND) ){
        /* Variable iNRecOffset is set to the offset in the journal file
        ** of the nRec field of the most recently written journal header.
        ** This field will be updated following the xSync() operation
        ** on the journal file. */
        i64 iNRecOffset = pPager->journalHdr + sizeof(aJournalMagic);

        /* This block deals with an obscure problem. If the last connection
        ** that wrote to this database was operating in persistent-journal
        ** mode, then the journal file may at this point actually be larger
        ** than Pager.journalOff bytes. If the next thing in the journal
        ** file happens to be a journal-header (written as part of the
        ** previous connections transaction), and a crash or power-failure 
        ** occurs after nRec is updated but before this connection writes 
        ** anything else to the journal file (or commits/rolls back its 
        ** transaction), then SQLite may become confused when doing the 
        ** hot-journal rollback following recovery. It may roll back all
        ** of this connections data, then proceed to rolling back the old,
        ** out-of-date data that follows it. Database corruption.
        **
        ** To work around this, if the journal file does appear to contain
        ** a valid header following Pager.journalOff, then write a 0x00
        ** byte to the start of it to prevent it from being recognized.
        **
        ** Variable iNextHdrOffset is set to the offset at which this
        ** problematic header will occur, if it exists. aMagic is used 
        ** as a temporary buffer to inspect the first couple of bytes of
        ** the potential journal header.
        */
        i64 iNextHdrOffset = journalHdrOffset(pPager);
        u8 aMagic[8];
        rc = sqlite3OsRead(pPager->jfd, aMagic, 8, iNextHdrOffset);
        if( rc==SQLITE_OK && 0==memcmp(aMagic, aJournalMagic, 8) ){
          static const u8 zerobyte = 0;
          rc = sqlite3OsWrite(pPager->jfd, &zerobyte, 1, iNextHdrOffset);
        }
        if( rc!=SQLITE_OK && rc!=SQLITE_IOERR_SHORT_READ ){
          return rc;
        }

        /* Write the nRec value into the journal file header. If in
        ** full-synchronous mode, sync the journal first. This ensures that
        ** all data has really hit the disk before nRec is updated to mark
        ** it as a candidate for rollback.
        **
        ** This is not required if the persistent media supports the
        ** SAFE_APPEND property. Because in this case it is not possible 
        ** for garbage data to be appended to the file, the nRec field
        ** is populated with 0xFFFFFFFF when the journal header is written
        ** and never needs to be updated.
        */
        if( pPager->fullSync && 0==(iDc&SQLITE_IOCAP_SEQUENTIAL) ){
          PAGERTRACE(("SYNC journal of %d\n", PAGERID(pPager)));
          IOTRACE(("JSYNC %p\n", pPager))
          rc = sqlite3OsSync(pPager->jfd, pPager->sync_flags);
          if( rc!=SQLITE_OK ) return rc;
        }
        IOTRACE(("JHDR %p %lld %d\n", pPager, iNRecOffset, 4));
        rc = write32bits(pPager->jfd, iNRecOffset, pPager->nRec);
        if( rc!=SQLITE_OK ) return rc;
      }
      if( 0==(iDc&SQLITE_IOCAP_SEQUENTIAL) ){
        PAGERTRACE(("SYNC journal of %d\n", PAGERID(pPager)));
        IOTRACE(("JSYNC %p\n", pPager))
        rc = sqlite3OsSync(pPager->jfd, pPager->sync_flags| 
          (pPager->sync_flags==SQLITE_SYNC_FULL?SQLITE_SYNC_DATAONLY:0)
        );
        if( rc!=SQLITE_OK ) return rc;
      }
    }

    /* The journal file was just successfully synced. Set Pager.needSync 
    ** to zero and clear the PGHDR_NEED_SYNC flag on all pagess.
    */
    pPager->needSync = 0;
    pPager->journalStarted = 1;
    sqlite3PcacheClearSyncFlags(pPager->pPCache);
  }

  return SQLITE_OK;
}

/*
** The argument is the first in a linked list of dirty pages connected
** by the PgHdr.pDirty pointer. This function writes each one of the
** in-memory pages in the list to the database file. The argument may
** be NULL, representing an empty list. In this case this function is
** a no-op.
**
** The pager must hold at least a RESERVED lock when this function
** is called. Before writing anything to the database file, this lock
** is upgraded to an EXCLUSIVE lock. If the lock cannot be obtained,
** SQLITE_BUSY is returned and no data is written to the database file.
** 
** If the pager is a temp-file pager and the actual file-system file
** is not yet open, it is created and opened before any data is 
** written out.
**
** Once the lock has been upgraded and, if necessary, the file opened,
** the pages are written out to the database file in list order. Writing
** a page is skipped if it meets either of the following criteria:
**
**   * The page number is greater than Pager.dbSize, or
**   * The PGHDR_DONT_WRITE flag is set on the page.
**
** If writing out a page causes the database file to grow, Pager.dbFileSize
** is updated accordingly. If page 1 is written out, then the value cached
** in Pager.dbFileVers[] is updated to match the new value stored in
** the database file.
**
** If everything is successful, SQLITE_OK is returned. If an IO error 
** occurs, an IO error code is returned. Or, if the EXCLUSIVE lock cannot
** be obtained, SQLITE_BUSY is returned.
*/
static int pager_write_pagelist(PgHdr *pList){
  Pager *pPager;                       /* Pager object */
  int rc;                              /* Return code */

  if( pList==0 ) return SQLITE_OK;
  pPager = pList->pPager;

  /* At this point there may be either a RESERVED or EXCLUSIVE lock on the
  ** database file. If there is already an EXCLUSIVE lock, the following
  ** call is a no-op.
  **
  ** Moving the lock from RESERVED to EXCLUSIVE actually involves going
  ** through an intermediate state PENDING.   A PENDING lock prevents new
  ** readers from attaching to the database but is unsufficient for us to
  ** write.  The idea of a PENDING lock is to prevent new readers from
  ** coming in while we wait for existing readers to clear.
  **
  ** While the pager is in the RESERVED state, the original database file
  ** is unchanged and we can rollback without having to playback the
  ** journal into the original database file.  Once we transition to
  ** EXCLUSIVE, it means the database file has been changed and any rollback
  ** will require a journal playback.
  */
  assert( pPager->state>=PAGER_RESERVED );
  rc = pager_wait_on_lock(pPager, EXCLUSIVE_LOCK);

  /* If the file is a temp-file has not yet been opened, open it now. It
  ** is not possible for rc to be other than SQLITE_OK if this branch
  ** is taken, as pager_wait_on_lock() is a no-op for temp-files.
  */
  if( !isOpen(pPager->fd) ){
    assert( pPager->tempFile && rc==SQLITE_OK );
    rc = pagerOpentemp(pPager, pPager->fd, pPager->vfsFlags);
  }

  while( rc==SQLITE_OK && pList ){
    Pgno pgno = pList->pgno;

    /* If there are dirty pages in the page cache with page numbers greater
    ** than Pager.dbSize, this means sqlite3PagerTruncateImage() was called to
    ** make the file smaller (presumably by auto-vacuum code). Do not write
    ** any such pages to the file.
    **
    ** Also, do not write out any page that has the PGHDR_DONT_WRITE flag
    ** set (set by sqlite3PagerDontWrite()).
    */
    if( pgno<=pPager->dbSize && 0==(pList->flags&PGHDR_DONT_WRITE) ){
      i64 offset = (pgno-1)*(i64)pPager->pageSize;         /* Offset to write */
      char *pData = CODEC2(pPager, pList->pData, pgno, 6); /* Data to write */

      /* Write out the page data. */
      rc = sqlite3OsWrite(pPager->fd, pData, pPager->pageSize, offset);

      /* If page 1 was just written, update Pager.dbFileVers to match
      ** the value now stored in the database file. If writing this 
      ** page caused the database file to grow, update dbFileSize. 
      */
      if( pgno==1 ){
        memcpy(&pPager->dbFileVers, &pData[24], sizeof(pPager->dbFileVers));
      }
      if( pgno>pPager->dbFileSize ){
        pPager->dbFileSize = pgno;
      }

      /* Update any backup objects copying the contents of this pager. */
      sqlite3BackupUpdate(pPager->pBackup, pgno, (u8 *)pData);

      PAGERTRACE(("STORE %d page %d hash(%08x)\n",
                   PAGERID(pPager), pgno, pager_pagehash(pList)));
      IOTRACE(("PGOUT %p %d\n", pPager, pgno));
      PAGER_INCR(sqlite3_pager_writedb_count);
      PAGER_INCR(pPager->nWrite);
    }else{
      PAGERTRACE(("NOSTORE %d page %d\n", PAGERID(pPager), pgno));
    }
#ifdef SQLITE_CHECK_PAGES
    pList->pageHash = pager_pagehash(pList);
#endif
    pList = pList->pDirty;
  }

  return rc;
}

/*
** Append a record of the current state of page pPg to the sub-journal. 
** It is the callers responsibility to use subjRequiresPage() to check 
** that it is really required before calling this function.
**
** If successful, set the bit corresponding to pPg->pgno in the bitvecs
** for all open savepoints before returning.
**
** This function returns SQLITE_OK if everything is successful, an IO
** error code if the attempt to write to the sub-journal fails, or 
** SQLITE_NOMEM if a malloc fails while setting a bit in a savepoint
** bitvec.
*/
static int subjournalPage(PgHdr *pPg){
  int rc = SQLITE_OK;
  Pager *pPager = pPg->pPager;
  if( isOpen(pPager->sjfd) ){
    void *pData = pPg->pData;
    i64 offset = pPager->nSubRec*(4+pPager->pageSize);
    char *pData2 = CODEC2(pPager, pData, pPg->pgno, 7);
  
    PAGERTRACE(("STMT-JOURNAL %d page %d\n", PAGERID(pPager), pPg->pgno));
  
    assert( pageInJournal(pPg) || pPg->pgno>pPager->dbOrigSize );
    rc = write32bits(pPager->sjfd, offset, pPg->pgno);
    if( rc==SQLITE_OK ){
      rc = sqlite3OsWrite(pPager->sjfd, pData2, pPager->pageSize, offset+4);
    }
  }
  if( rc==SQLITE_OK ){
    pPager->nSubRec++;
    assert( pPager->nSavepoint>0 );
    rc = addToSavepointBitvecs(pPager, pPg->pgno);
    testcase( rc!=SQLITE_OK );
  }
  return rc;
}


/*
** This function is called by the pcache layer when it has reached some
** soft memory limit. The first argument is a pointer to a Pager object
** (cast as a void*). The pager is always 'purgeable' (not an in-memory
** database). The second argument is a reference to a page that is 
** currently dirty but has no outstanding references. The page
** is always associated with the Pager object passed as the first 
** argument.
**
** The job of this function is to make pPg clean by writing its contents
** out to the database file, if possible. This may involve syncing the
** journal file. 
**
** If successful, sqlite3PcacheMakeClean() is called on the page and
** SQLITE_OK returned. If an IO error occurs while trying to make the
** page clean, the IO error code is returned. If the page cannot be
** made clean for some other reason, but no error occurs, then SQLITE_OK
** is returned by sqlite3PcacheMakeClean() is not called.
*/
static int pagerStress(void *p, PgHdr *pPg){
  Pager *pPager = (Pager *)p;
  int rc = SQLITE_OK;

  assert( pPg->pPager==pPager );
  assert( pPg->flags&PGHDR_DIRTY );

  /* The doNotSync flag is set by the sqlite3PagerWrite() function while it
  ** is journalling a set of two or more database pages that are stored
  ** on the same disk sector. Syncing the journal is not allowed while
  ** this is happening as it is important that all members of such a
  ** set of pages are synced to disk together. So, if the page this function
  ** is trying to make clean will require a journal sync and the doNotSync
  ** flag is set, return without doing anything. The pcache layer will
  ** just have to go ahead and allocate a new page buffer instead of
  ** reusing pPg.
  **
  ** Similarly, if the pager has already entered the error state, do not
  ** try to write the contents of pPg to disk.
  */
  if( pPager->errCode || (pPager->doNotSync && pPg->flags&PGHDR_NEED_SYNC) ){
    return SQLITE_OK;
  }

  /* Sync the journal file if required. */
  if( pPg->flags&PGHDR_NEED_SYNC ){
    rc = syncJournal(pPager);
    if( rc==SQLITE_OK && pPager->fullSync && 
      !(pPager->journalMode==PAGER_JOURNALMODE_MEMORY) &&
      !(sqlite3OsDeviceCharacteristics(pPager->fd)&SQLITE_IOCAP_SAFE_APPEND)
    ){
      pPager->nRec = 0;
      rc = writeJournalHdr(pPager);
    }
  }

  /* If the page number of this page is larger than the current size of
  ** the database image, it may need to be written to the sub-journal.
  ** This is because the call to pager_write_pagelist() below will not
  ** actually write data to the file in this case.
  **
  ** Consider the following sequence of events:
  **
  **   BEGIN;
  **     <journal page X>
  **     <modify page X>
  **     SAVEPOINT sp;
  **       <shrink database file to Y pages>
  **       pagerStress(page X)
  **     ROLLBACK TO sp;
  **
  ** If (X>Y), then when pagerStress is called page X will not be written
  ** out to the database file, but will be dropped from the cache. Then,
  ** following the "ROLLBACK TO sp" statement, reading page X will read
  ** data from the database file. This will be the copy of page X as it
  ** was when the transaction started, not as it was when "SAVEPOINT sp"
  ** was executed.
  **
  ** The solution is to write the current data for page X into the 
  ** sub-journal file now (if it is not already there), so that it will
  ** be restored to its current value when the "ROLLBACK TO sp" is 
  ** executed.
  */
  if( rc==SQLITE_OK && pPg->pgno>pPager->dbSize && subjRequiresPage(pPg) ){
    rc = subjournalPage(pPg);
  }

  /* Write the contents of the page out to the database file. */
  if( rc==SQLITE_OK ){
    pPg->pDirty = 0;
    rc = pager_write_pagelist(pPg);
  }

  /* Mark the page as clean. */
  if( rc==SQLITE_OK ){
    PAGERTRACE(("STRESS %d page %d\n", PAGERID(pPager), pPg->pgno));
    sqlite3PcacheMakeClean(pPg);
  }

  return pager_error(pPager, rc);
}


/*
** Allocate and initialize a new Pager object and put a pointer to it
** in *ppPager. The pager should eventually be freed by passing it
** to sqlite3PagerClose().
**
** The zFilename argument is the path to the database file to open.
** If zFilename is NULL then a randomly-named temporary file is created
** and used as the file to be cached. Temporary files are be deleted
** automatically when they are closed. If zFilename is ":memory:" then 
** all information is held in cache. It is never written to disk. 
** This can be used to implement an in-memory database.
**
** The nExtra parameter specifies the number of bytes of space allocated
** along with each page reference. This space is available to the user
** via the sqlite3PagerGetExtra() API.
**
** The flags argument is used to specify properties that affect the
** operation of the pager. It should be passed some bitwise combination
** of the PAGER_OMIT_JOURNAL and PAGER_NO_READLOCK flags.
**
** The vfsFlags parameter is a bitmask to pass to the flags parameter
** of the xOpen() method of the supplied VFS when opening files. 
**
** If the pager object is allocated and the specified file opened 
** successfully, SQLITE_OK is returned and *ppPager set to point to
** the new pager object. If an error occurs, *ppPager is set to NULL
** and error code returned. This function may return SQLITE_NOMEM
** (sqlite3Malloc() is used to allocate memory), SQLITE_CANTOPEN or 
** various SQLITE_IO_XXX errors.
*/
int sqlite3PagerOpen(
  sqlite3_vfs *pVfs,       /* The virtual file system to use */
  Pager **ppPager,         /* OUT: Return the Pager structure here */
  const char *zFilename,   /* Name of the database file to open */
  int nExtra,              /* Extra bytes append to each in-memory page */
  int flags,               /* flags controlling this file */
  int vfsFlags             /* flags passed through to sqlite3_vfs.xOpen() */
){
  u8 *pPtr;
  Pager *pPager = 0;       /* Pager object to allocate and return */
  int rc = SQLITE_OK;      /* Return code */
  int tempFile = 0;        /* True for temp files (incl. in-memory files) */
  int memDb = 0;           /* True if this is an in-memory file */
  int readOnly = 0;        /* True if this is a read-only file */
  int journalFileSize;     /* Bytes to allocate for each journal fd */
  char *zPathname = 0;     /* Full path to database file */
  int nPathname = 0;       /* Number of bytes in zPathname */
  int useJournal = (flags & PAGER_OMIT_JOURNAL)==0; /* False to omit journal */
  int noReadlock = (flags & PAGER_NO_READLOCK)!=0;  /* True to omit read-lock */
  int pcacheSize = sqlite3PcacheSize();       /* Bytes to allocate for PCache */
  u16 szPageDflt = SQLITE_DEFAULT_PAGE_SIZE;  /* Default page size */

  /* Figure out how much space is required for each journal file-handle
  ** (there are two of them, the main journal and the sub-journal). This
  ** is the maximum space required for an in-memory journal file handle 
  ** and a regular journal file-handle. Note that a "regular journal-handle"
  ** may be a wrapper capable of caching the first portion of the journal
  ** file in memory to implement the atomic-write optimization (see 
  ** source file journal.c).
  */
  if( sqlite3JournalSize(pVfs)>sqlite3MemJournalSize() ){
    journalFileSize = ROUND8(sqlite3JournalSize(pVfs));
  }else{
    journalFileSize = ROUND8(sqlite3MemJournalSize());
  }

  /* Set the output variable to NULL in case an error occurs. */
  *ppPager = 0;

  /* Compute and store the full pathname in an allocated buffer pointed
  ** to by zPathname, length nPathname. Or, if this is a temporary file,
  ** leave both nPathname and zPathname set to 0.
  */
  if( zFilename && zFilename[0] ){
    nPathname = pVfs->mxPathname+1;
    zPathname = sqlite3Malloc(nPathname*2);
    if( zPathname==0 ){
      return SQLITE_NOMEM;
    }
#ifndef SQLITE_OMIT_MEMORYDB
    if( strcmp(zFilename,":memory:")==0 ){
      memDb = 1;
      zPathname[0] = 0;
    }else
#endif
    {
      zPathname[0] = 0; /* Make sure initialized even if FullPathname() fails */
      rc = sqlite3OsFullPathname(pVfs, zFilename, nPathname, zPathname);
    }

    nPathname = sqlite3Strlen30(zPathname);
    if( rc==SQLITE_OK && nPathname+8>pVfs->mxPathname ){
      /* This branch is taken when the journal path required by
      ** the database being opened will be more than pVfs->mxPathname
      ** bytes in length. This means the database cannot be opened,
      ** as it will not be possible to open the journal file or even
      ** check for a hot-journal before reading.
      */
      rc = SQLITE_CANTOPEN;
    }
    if( rc!=SQLITE_OK ){
      sqlite3_free(zPathname);
      return rc;
    }
  }

  /* Allocate memory for the Pager structure, PCache object, the
  ** three file descriptors, the database file name and the journal 
  ** file name. The layout in memory is as follows:
  **
  **     Pager object                    (sizeof(Pager) bytes)
  **     PCache object                   (sqlite3PcacheSize() bytes)
  **     Database file handle            (pVfs->szOsFile bytes)
  **     Sub-journal file handle         (journalFileSize bytes)
  **     Main journal file handle        (journalFileSize bytes)
  **     Database file name              (nPathname+1 bytes)
  **     Journal file name               (nPathname+8+1 bytes)
  */
  pPtr = (u8 *)sqlite3MallocZero(
    ROUND8(sizeof(*pPager)) +      /* Pager structure */
    ROUND8(pcacheSize) +           /* PCache object */
    ROUND8(pVfs->szOsFile) +       /* The main db file */
    journalFileSize * 2 +          /* The two journal files */ 
    nPathname + 1 +                /* zFilename */
    nPathname + 8 + 1              /* zJournal */
  );
  assert( EIGHT_BYTE_ALIGNMENT(journalFileSize) );
  if( !pPtr ){
    sqlite3_free(zPathname);
    return SQLITE_NOMEM;
  }
  pPager =              (Pager*)(pPtr);
  pPager->pPCache =    (PCache*)(pPtr += ROUND8(sizeof(*pPager)));
  pPager->fd =   (sqlite3_file*)(pPtr += ROUND8(pcacheSize));
  pPager->sjfd = (sqlite3_file*)(pPtr += ROUND8(pVfs->szOsFile));
  pPager->jfd =  (sqlite3_file*)(pPtr += journalFileSize);
  pPager->zFilename =    (char*)(pPtr += journalFileSize);
  assert( EIGHT_BYTE_ALIGNMENT(pPager->jfd) );

  /* Fill in the Pager.zFilename and Pager.zJournal buffers, if required. */
  if( zPathname ){
    pPager->zJournal =   (char*)(pPtr += nPathname + 1);
    memcpy(pPager->zFilename, zPathname, nPathname);
    memcpy(pPager->zJournal, zPathname, nPathname);
    memcpy(&pPager->zJournal[nPathname], "-journal", 8);
    sqlite3_free(zPathname);
  }
  pPager->pVfs = pVfs;
  pPager->vfsFlags = vfsFlags;

  /* Open the pager file.
  */
  if( zFilename && zFilename[0] && !memDb ){
    int fout = 0;                    /* VFS flags returned by xOpen() */
    rc = sqlite3OsOpen(pVfs, pPager->zFilename, pPager->fd, vfsFlags, &fout);
    readOnly = (fout&SQLITE_OPEN_READONLY);

    /* If the file was successfully opened for read/write access,
    ** choose a default page size in case we have to create the
    ** database file. The default page size is the maximum of:
    **
    **    + SQLITE_DEFAULT_PAGE_SIZE,
    **    + The value returned by sqlite3OsSectorSize()
    **    + The largest page size that can be written atomically.
    */
    if( rc==SQLITE_OK && !readOnly ){
      setSectorSize(pPager);
      assert(SQLITE_DEFAULT_PAGE_SIZE<=SQLITE_MAX_DEFAULT_PAGE_SIZE);
      if( szPageDflt<pPager->sectorSize ){
        if( pPager->sectorSize>SQLITE_MAX_DEFAULT_PAGE_SIZE ){
          szPageDflt = SQLITE_MAX_DEFAULT_PAGE_SIZE;
        }else{
          szPageDflt = (u16)pPager->sectorSize;
        }
      }
#ifdef SQLITE_ENABLE_ATOMIC_WRITE
      {
        int iDc = sqlite3OsDeviceCharacteristics(pPager->fd);
        int ii;
        assert(SQLITE_IOCAP_ATOMIC512==(512>>8));
        assert(SQLITE_IOCAP_ATOMIC64K==(65536>>8));
        assert(SQLITE_MAX_DEFAULT_PAGE_SIZE<=65536);
        for(ii=szPageDflt; ii<=SQLITE_MAX_DEFAULT_PAGE_SIZE; ii=ii*2){
          if( iDc&(SQLITE_IOCAP_ATOMIC|(ii>>8)) ){
            szPageDflt = ii;
          }
        }
      }
#endif
    }
  }else{
    /* If a temporary file is requested, it is not opened immediately.
    ** In this case we accept the default page size and delay actually
    ** opening the file until the first call to OsWrite().
    **
    ** This branch is also run for an in-memory database. An in-memory
    ** database is the same as a temp-file that is never written out to
    ** disk and uses an in-memory rollback journal.
    */ 
    tempFile = 1;
    pPager->state = PAGER_EXCLUSIVE;
  }

  /* The following call to PagerSetPagesize() serves to set the value of 
  ** Pager.pageSize and to allocate the Pager.pTmpSpace buffer.
  */
  if( rc==SQLITE_OK ){
    assert( pPager->memDb==0 );
    rc = sqlite3PagerSetPagesize(pPager, &szPageDflt);
    testcase( rc!=SQLITE_OK );
  }

  /* If an error occurred in either of the blocks above, free the 
  ** Pager structure and close the file.
  */
  if( rc!=SQLITE_OK ){
    assert( !pPager->pTmpSpace );
    sqlite3OsClose(pPager->fd);
    sqlite3_free(pPager);
    return rc;
  }

  /* Initialize the PCache object. */
  nExtra = ROUND8(nExtra);
  sqlite3PcacheOpen(szPageDflt, nExtra, !memDb,
                    !memDb?pagerStress:0, (void *)pPager, pPager->pPCache);

  PAGERTRACE(("OPEN %d %s\n", FILEHANDLEID(pPager->fd), pPager->zFilename));
  IOTRACE(("OPEN %p %s\n", pPager, pPager->zFilename))

  pPager->useJournal = (u8)useJournal;
  pPager->noReadlock = (noReadlock && readOnly) ?1:0;
  /* pPager->stmtOpen = 0; */
  /* pPager->stmtInUse = 0; */
  /* pPager->nRef = 0; */
  pPager->dbSizeValid = (u8)memDb;
  /* pPager->stmtSize = 0; */
  /* pPager->stmtJSize = 0; */
  /* pPager->nPage = 0; */
  pPager->mxPgno = SQLITE_MAX_PAGE_COUNT;
  /* pPager->state = PAGER_UNLOCK; */
  assert( pPager->state == (tempFile ? PAGER_EXCLUSIVE : PAGER_UNLOCK) );
  /* pPager->errMask = 0; */
  pPager->tempFile = (u8)tempFile;
  assert( tempFile==PAGER_LOCKINGMODE_NORMAL 
          || tempFile==PAGER_LOCKINGMODE_EXCLUSIVE );
  assert( PAGER_LOCKINGMODE_EXCLUSIVE==1 );
  pPager->exclusiveMode = (u8)tempFile; 
  pPager->changeCountDone = pPager->tempFile;
  pPager->memDb = (u8)memDb;
  pPager->readOnly = (u8)readOnly;
  /* pPager->needSync = 0; */
  pPager->noSync = (pPager->tempFile || !useJournal) ?1:0;
  pPager->fullSync = pPager->noSync ?0:1;
  pPager->sync_flags = SQLITE_SYNC_NORMAL;
  /* pPager->pFirst = 0; */
  /* pPager->pFirstSynced = 0; */
  /* pPager->pLast = 0; */
  pPager->nExtra = nExtra;
  pPager->journalSizeLimit = SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT;
  assert( isOpen(pPager->fd) || tempFile );
  setSectorSize(pPager);
  if( memDb ){
    pPager->journalMode = PAGER_JOURNALMODE_MEMORY;
  }
  /* pPager->xBusyHandler = 0; */
  /* pPager->pBusyHandlerArg = 0; */
  /* memset(pPager->aHash, 0, sizeof(pPager->aHash)); */
  *ppPager = pPager;
  return SQLITE_OK;
}



/*
** This function is called after transitioning from PAGER_UNLOCK to
** PAGER_SHARED state. It tests if there is a hot journal present in
** the file-system for the given pager. A hot journal is one that 
** needs to be played back. According to this function, a hot-journal
** file exists if the following criteria are met:
**
**   * The journal file exists in the file system, and
**   * No process holds a RESERVED or greater lock on the database file, and
**   * The database file itself is greater than 0 bytes in size, and
**   * The first byte of the journal file exists and is not 0x00.
**
** If the current size of the database file is 0 but a journal file
** exists, that is probably an old journal left over from a prior
** database with the same name. In this case the journal file is
** just deleted using OsDelete, *pExists is set to 0 and SQLITE_OK
** is returned.
**
** This routine does not check if there is a master journal filename
** at the end of the file. If there is, and that master journal file
** does not exist, then the journal file is not really hot. In this
** case this routine will return a false-positive. The pager_playback()
** routine will discover that the journal file is not really hot and 
** will not roll it back. 
**
** If a hot-journal file is found to exist, *pExists is set to 1 and 
** SQLITE_OK returned. If no hot-journal file is present, *pExists is
** set to 0 and SQLITE_OK returned. If an IO error occurs while trying
** to determine whether or not a hot-journal file exists, the IO error
** code is returned and the value of *pExists is undefined.
*/
static int hasHotJournal(Pager *pPager, int *pExists){
  sqlite3_vfs * const pVfs = pPager->pVfs;
  int rc;                       /* Return code */
  int exists;                   /* True if a journal file is present */

  assert( pPager!=0 );
  assert( pPager->useJournal );
  assert( isOpen(pPager->fd) );
  assert( !isOpen(pPager->jfd) );

  *pExists = 0;
  rc = sqlite3OsAccess(pVfs, pPager->zJournal, SQLITE_ACCESS_EXISTS, &exists);
  if( rc==SQLITE_OK && exists ){
    int locked;                 /* True if some process holds a RESERVED lock */
    rc = sqlite3OsCheckReservedLock(pPager->fd, &locked);
    if( rc==SQLITE_OK && !locked ){
      int nPage;

      /* Check the size of the database file. If it consists of 0 pages,
      ** then delete the journal file. See the header comment above for 
      ** the reasoning here.
      */
      rc = sqlite3PagerPagecount(pPager, &nPage);
      if( rc==SQLITE_OK ){
        if( nPage==0 ){
          rc = sqlite3OsDelete(pVfs, pPager->zJournal, 0);
        }else{
          /* The journal file exists and no other connection has a reserved
          ** or greater lock on the database file. Now check that there is
          ** at least one non-zero bytes at the start of the journal file.
          ** If there is, then we consider this journal to be hot. If not, 
          ** it can be ignored.
          */
          int f = SQLITE_OPEN_READONLY|SQLITE_OPEN_MAIN_JOURNAL;
          rc = sqlite3OsOpen(pVfs, pPager->zJournal, pPager->jfd, f, &f);
          if( rc==SQLITE_OK ){
            u8 first = 0;
            rc = sqlite3OsRead(pPager->jfd, (void *)&first, 1, 0);
            if( rc==SQLITE_IOERR_SHORT_READ ){
              rc = SQLITE_OK;
            }
            sqlite3OsClose(pPager->jfd);
            *pExists = (first!=0);
          }
        }
      }
    }
  }

  return rc;
}

/*
** Read the content for page pPg out of the database file and into 
** pPg->pData. A shared lock or greater must be held on the database
** file before this function is called.
**
** If page 1 is read, then the value of Pager.dbFileVers[] is set to
** the value read from the database file.
**
** If an IO error occurs, then the IO error is returned to the caller.
** Otherwise, SQLITE_OK is returned.
*/
static int readDbPage(PgHdr *pPg){
  Pager *pPager = pPg->pPager; /* Pager object associated with page pPg */
  Pgno pgno = pPg->pgno;       /* Page number to read */
  int rc;                      /* Return code */
  i64 iOffset;                 /* Byte offset of file to read from */

  assert( pPager->state>=PAGER_SHARED && !MEMDB );

  if( !isOpen(pPager->fd) ){
    assert( pPager->tempFile );
    memset(pPg->pData, 0, pPager->pageSize);
    return SQLITE_OK;
  }
  iOffset = (pgno-1)*(i64)pPager->pageSize;
  rc = sqlite3OsRead(pPager->fd, pPg->pData, pPager->pageSize, iOffset);
  if( rc==SQLITE_IOERR_SHORT_READ ){
    rc = SQLITE_OK;
  }
  if( pgno==1 ){
    u8 *dbFileVers = &((u8*)pPg->pData)[24];
    memcpy(&pPager->dbFileVers, dbFileVers, sizeof(pPager->dbFileVers));
  }
  CODEC1(pPager, pPg->pData, pgno, 3);

  PAGER_INCR(sqlite3_pager_readdb_count);
  PAGER_INCR(pPager->nRead);
  IOTRACE(("PGIN %p %d\n", pPager, pgno));
  PAGERTRACE(("FETCH %d page %d hash(%08x)\n",
               PAGERID(pPager), pgno, pager_pagehash(pPg)));

  return rc;
}

/*
** This function is called whenever the upper layer requests a database
** page is requested, before the cache is checked for a suitable page
** or any data is read from the database. It performs the following
** two functions:
**
**   1) If the pager is currently in PAGER_UNLOCK state (no lock held
**      on the database file), then an attempt is made to obtain a
**      SHARED lock on the database file. Immediately after obtaining
**      the SHARED lock, the file-system is checked for a hot-journal,
**      which is played back if present. Following any hot-journal 
**      rollback, the contents of the cache are validated by checking
**      the 'change-counter' field of the database file header and
**      discarded if they are found to be invalid.
**
**   2) If the pager is running in exclusive-mode, and there are currently
**      no outstanding references to any pages, and is in the error state,
**      then an attempt is made to clear the error state by discarding
**      the contents of the page cache and rolling back any open journal
**      file.
**
** If the operation described by (2) above is not attempted, and if the
** pager is in an error state other than SQLITE_FULL when this is called,
** the error state error code is returned. It is permitted to read the
** database when in SQLITE_FULL error state.
**
** Otherwise, if everything is successful, SQLITE_OK is returned. If an
** IO error occurs while locking the database, checking for a hot-journal
** file or rolling back a journal file, the IO error code is returned.
*/
static int pagerSharedLock(Pager *pPager){
  int rc = SQLITE_OK;                /* Return code */
  int isErrorReset = 0;              /* True if recovering from error state */

  /* If this database is opened for exclusive access, has no outstanding 
  ** page references and is in an error-state, this is a chance to clear
  ** the error. Discard the contents of the pager-cache and treat any
  ** open journal file as a hot-journal.
  */
  if( !MEMDB && pPager->exclusiveMode 
   && sqlite3PcacheRefCount(pPager->pPCache)==0 && pPager->errCode 
  ){
    if( isOpen(pPager->jfd) ){
      isErrorReset = 1;
    }
    pPager->errCode = SQLITE_OK;
    pager_reset(pPager);
  }

  /* If the pager is still in an error state, do not proceed. The error 
  ** state will be cleared at some point in the future when all page 
  ** references are dropped and the cache can be discarded.
  */
  if( pPager->errCode && pPager->errCode!=SQLITE_FULL ){
    return pPager->errCode;
  }

  if( pPager->state==PAGER_UNLOCK || isErrorReset ){
    sqlite3_vfs * const pVfs = pPager->pVfs;
    int isHotJournal = 0;
    assert( !MEMDB );
    assert( sqlite3PcacheRefCount(pPager->pPCache)==0 );
    if( !pPager->noReadlock ){
      rc = pager_wait_on_lock(pPager, SHARED_LOCK);
      if( rc!=SQLITE_OK ){
        assert( pPager->state==PAGER_UNLOCK );
        return pager_error(pPager, rc);
      }
    }else if( pPager->state==PAGER_UNLOCK ){
      pPager->state = PAGER_SHARED;
    }
    assert( pPager->state>=SHARED_LOCK );

    /* If a journal file exists, and there is no RESERVED lock on the
    ** database file, then it either needs to be played back or deleted.
    */
    if( !isErrorReset ){
      rc = hasHotJournal(pPager, &isHotJournal);
      if( rc!=SQLITE_OK ){
        goto failed;
      }
    }
    if( isErrorReset || isHotJournal ){
      /* Get an EXCLUSIVE lock on the database file. At this point it is
      ** important that a RESERVED lock is not obtained on the way to the
      ** EXCLUSIVE lock. If it were, another process might open the
      ** database file, detect the RESERVED lock, and conclude that the
      ** database is safe to read while this process is still rolling the 
      ** hot-journal back.
      ** 
      ** Because the intermediate RESERVED lock is not requested, any
      ** other process attempting to access the database file will get to 
      ** this point in the code and fail to obtain its own EXCLUSIVE lock 
      ** on the database file.
      */
      if( pPager->state<EXCLUSIVE_LOCK ){
        rc = sqlite3OsLock(pPager->fd, EXCLUSIVE_LOCK);
        if( rc!=SQLITE_OK ){
          rc = pager_error(pPager, rc);
          goto failed;
        }
        pPager->state = PAGER_EXCLUSIVE;
      }
 
      /* Open the journal for read/write access. This is because in 
      ** exclusive-access mode the file descriptor will be kept open and
      ** possibly used for a transaction later on. On some systems, the
      ** OsTruncate() call used in exclusive-access mode also requires
      ** a read/write file handle.
      */
      if( !isOpen(pPager->jfd) ){
        int res;
        rc = sqlite3OsAccess(pVfs,pPager->zJournal,SQLITE_ACCESS_EXISTS,&res);
        if( rc==SQLITE_OK ){
          if( res ){
            int fout = 0;
            int f = SQLITE_OPEN_READWRITE|SQLITE_OPEN_MAIN_JOURNAL;
            assert( !pPager->tempFile );
            rc = sqlite3OsOpen(pVfs, pPager->zJournal, pPager->jfd, f, &fout);
            assert( rc!=SQLITE_OK || isOpen(pPager->jfd) );
            if( rc==SQLITE_OK && fout&SQLITE_OPEN_READONLY ){
              rc = SQLITE_CANTOPEN;
              sqlite3OsClose(pPager->jfd);
            }
          }else{
            /* If the journal does not exist, that means some other process
            ** has already rolled it back */
            rc = SQLITE_BUSY;
          }
        }
      }
      if( rc!=SQLITE_OK ){
        goto failed;
      }

      /* TODO: Why are these cleared here? Is it necessary? */
      pPager->journalStarted = 0;
      pPager->journalOff = 0;
      pPager->setMaster = 0;
      pPager->journalHdr = 0;
 
      /* Playback and delete the journal.  Drop the database write
      ** lock and reacquire the read lock. Purge the cache before
      ** playing back the hot-journal so that we don't end up with
      ** an inconsistent cache.
      */
      rc = pager_playback(pPager, 1);
      if( rc!=SQLITE_OK ){
        rc = pager_error(pPager, rc);
        goto failed;
      }
      assert( (pPager->state==PAGER_SHARED)
           || (pPager->exclusiveMode && pPager->state>PAGER_SHARED)
      );
    }

    if( sqlite3PcachePagecount(pPager->pPCache)>0 ){
      /* The shared-lock has just been acquired on the database file
      ** and there are already pages in the cache (from a previous
      ** read or write transaction).  Check to see if the database
      ** has been modified.  If the database has changed, flush the
      ** cache.
      **
      ** Database changes is detected by looking at 15 bytes beginning
      ** at offset 24 into the file.  The first 4 of these 16 bytes are
      ** a 32-bit counter that is incremented with each change.  The
      ** other bytes change randomly with each file change when
      ** a codec is in use.
      ** 
      ** There is a vanishingly small chance that a change will not be 
      ** detected.  The chance of an undetected change is so small that
      ** it can be neglected.
      */
      char dbFileVers[sizeof(pPager->dbFileVers)];
      sqlite3PagerPagecount(pPager, 0);

      if( pPager->errCode ){
        rc = pPager->errCode;
        goto failed;
      }

      assert( pPager->dbSizeValid );
      if( pPager->dbSize>0 ){
        IOTRACE(("CKVERS %p %d\n", pPager, sizeof(dbFileVers)));
        rc = sqlite3OsRead(pPager->fd, &dbFileVers, sizeof(dbFileVers), 24);
        if( rc!=SQLITE_OK ){
          goto failed;
        }
      }else{
        memset(dbFileVers, 0, sizeof(dbFileVers));
      }

      if( memcmp(pPager->dbFileVers, dbFileVers, sizeof(dbFileVers))!=0 ){
        pager_reset(pPager);
      }
    }
    assert( pPager->exclusiveMode || pPager->state==PAGER_SHARED );
  }

 failed:
  if( rc!=SQLITE_OK ){
    /* pager_unlock() is a no-op for exclusive mode and in-memory databases. */
    pager_unlock(pPager);
  }
  return rc;
}

/*
** If the reference count has reached zero, rollback any active
** transaction and unlock the pager.
*/ 
static void pagerUnlockIfUnused(Pager *pPager){
  if( sqlite3PcacheRefCount(pPager->pPCache)==0 ){
    pagerUnlockAndRollback(pPager);
  }
}

/*
** Drop a page from the cache using sqlite3PcacheDrop().
**
** If this means there are now no pages with references to them, a rollback
** occurs and the lock on the database is removed.
*/
static void pagerDropPage(DbPage *pPg){
  Pager *pPager = pPg->pPager;
  sqlite3PcacheDrop(pPg);
  pagerUnlockIfUnused(pPager);
}

/*
** Acquire a reference to page number pgno in pager pPager (a page
** reference has type DbPage*). If the requested reference is 
** successfully obtained, it is copied to *ppPage and SQLITE_OK returned.
**
** This function calls pagerSharedLock() to obtain a SHARED lock on
** the database file if such a lock or greater is not already held.
** This may cause hot-journal rollback or a cache purge. See comments
** above function pagerSharedLock() for details.
**
** If the requested page is already in the cache, it is returned. 
** Otherwise, a new page object is allocated and populated with data
** read from the database file. In some cases, the pcache module may
** choose not to allocate a new page object and may reuse an existing
** object with no outstanding references.
**
** The extra data appended to a page is always initialized to zeros the 
** first time a page is loaded into memory. If the page requested is 
** already in the cache when this function is called, then the extra
** data is left as it was when the page object was last used.
**
** If the database image is smaller than the requested page or if a 
** non-zero value is passed as the noContent parameter and the 
** requested page is not already stored in the cache, then no 
** actual disk read occurs. In this case the memory image of the 
** page is initialized to all zeros. 
**
** If noContent is true, it means that we do not care about the contents
** of the page. This occurs in two seperate scenarios:
**
**   a) When reading a free-list leaf page from the database, and
**
**   b) When a savepoint is being rolled back and we need to load
**      a new page into the cache to populate with the data read
**      from the savepoint journal.
**
** If noContent is true, then the data returned is zeroed instead of
** being read from the database. Additionally, the bits corresponding
** to pgno in Pager.pInJournal (bitvec of pages already written to the
** journal file) and the PagerSavepoint.pInSavepoint bitvecs of any open
** savepoints are set. This means if the page is made writable at any
** point in the future, using a call to sqlite3PagerWrite(), its contents
** will not be journaled. This saves IO.
**
** The acquisition might fail for several reasons.  In all cases,
** an appropriate error code is returned and *ppPage is set to NULL.
**
** See also sqlite3PagerLookup().  Both this routine and Lookup() attempt
** to find a page in the in-memory cache first.  If the page is not already
** in memory, this routine goes to disk to read it in whereas Lookup()
** just returns 0.  This routine acquires a read-lock the first time it
** has to go to disk, and could also playback an old journal if necessary.
** Since Lookup() never goes to disk, it never has to deal with locks
** or journal files.
*/
int sqlite3PagerAcquire(
  Pager *pPager,      /* The pager open on the database file */
  Pgno pgno,          /* Page number to fetch */
  DbPage **ppPage,    /* Write a pointer to the page here */
  int noContent       /* Do not bother reading content from disk if true */
){
  PgHdr *pPg = 0;
  int rc;

  assert( assert_pager_state(pPager) );
  assert( pPager->state==PAGER_UNLOCK 
       || sqlite3PcacheRefCount(pPager->pPCache)>0 
       || pgno==1
  );

  /* The maximum page number is 2^31. Return SQLITE_CORRUPT if a page
  ** number greater than this, or zero, is requested.
  */
  if( pgno>PAGER_MAX_PGNO || pgno==0 || pgno==PAGER_MJ_PGNO(pPager) ){
    return SQLITE_CORRUPT_BKPT;
  }

  /* Make sure we have not hit any critical errors.
  */ 
  assert( pPager!=0 );
  *ppPage = 0;

  /* If this is the first page accessed, then get a SHARED lock
  ** on the database file. pagerSharedLock() is a no-op if 
  ** a database lock is already held.
  */
  rc = pagerSharedLock(pPager);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  assert( pPager->state!=PAGER_UNLOCK );

  rc = sqlite3PcacheFetch(pPager->pPCache, pgno, 1, &pPg);
  if( rc!=SQLITE_OK ){
    return rc;
  }
  assert( pPg->pgno==pgno );
  assert( pPg->pPager==pPager || pPg->pPager==0 );
  if( pPg->pPager==0 ){
    /* The pager cache has created a new page. Its content needs to 
    ** be initialized.
    */
    int nMax;
    PAGER_INCR(pPager->nMiss);
    pPg->pPager = pPager;

    rc = sqlite3PagerPagecount(pPager, &nMax);
    if( rc!=SQLITE_OK ){
      sqlite3PagerUnref(pPg);
      return rc;
    }

    if( nMax<(int)pgno || MEMDB || noContent ){
      if( pgno>pPager->mxPgno ){
        sqlite3PagerUnref(pPg);
        return SQLITE_FULL;
      }
      if( noContent ){
        /* Failure to set the bits in the InJournal bit-vectors is benign.
        ** It merely means that we might do some extra work to journal a 
        ** page that does not need to be journaled.  Nevertheless, be sure 
        ** to test the case where a malloc error occurs while trying to set 
        ** a bit in a bit vector.
        */
        sqlite3BeginBenignMalloc();
        if( pgno<=pPager->dbOrigSize ){
          TESTONLY( rc = ) sqlite3BitvecSet(pPager->pInJournal, pgno);
          testcase( rc==SQLITE_NOMEM );
        }
        TESTONLY( rc = ) addToSavepointBitvecs(pPager, pgno);
        testcase( rc==SQLITE_NOMEM );
        sqlite3EndBenignMalloc();
      }else{
        memset(pPg->pData, 0, pPager->pageSize);
      }
      IOTRACE(("ZERO %p %d\n", pPager, pgno));
    }else{
      assert( pPg->pPager==pPager );
      rc = readDbPage(pPg);
      if( rc!=SQLITE_OK ){
        pagerDropPage(pPg);
        return rc;
      }
    }
#ifdef SQLITE_CHECK_PAGES
    pPg->pageHash = pager_pagehash(pPg);
#endif
  }else{
    /* The requested page is in the page cache. */
    PAGER_INCR(pPager->nHit);
  }

  *ppPage = pPg;
  return SQLITE_OK;
}

/*
** Acquire a page if it is already in the in-memory cache.  Do
** not read the page from disk.  Return a pointer to the page,
** or 0 if the page is not in cache. Also, return 0 if the 
** pager is in PAGER_UNLOCK state when this function is called,
** or if the pager is in an error state other than SQLITE_FULL.
**
** See also sqlite3PagerGet().  The difference between this routine
** and sqlite3PagerGet() is that _get() will go to the disk and read
** in the page if the page is not already in cache.  This routine
** returns NULL if the page is not in cache or if a disk I/O error 
** has ever happened.
*/
DbPage *sqlite3PagerLookup(Pager *pPager, Pgno pgno){
  PgHdr *pPg = 0;
  assert( pPager!=0 );
  assert( pgno!=0 );

  if( (pPager->state!=PAGER_UNLOCK)
   && (pPager->errCode==SQLITE_OK || pPager->errCode==SQLITE_FULL)
  ){
    sqlite3PcacheFetch(pPager->pPCache, pgno, 0, &pPg);
  }

  return pPg;
}

/*
** Release a page reference.
**
** If the number of references to the page drop to zero, then the
** page is added to the LRU list.  When all references to all pages
** are released, a rollback occurs and the lock on the database is
** removed.
*/
void sqlite3PagerUnref(DbPage *pPg){
  if( pPg ){
    Pager *pPager = pPg->pPager;
    sqlite3PcacheRelease(pPg);
    pagerUnlockIfUnused(pPager);
  }
}

/*
** If the main journal file has already been opened, ensure that the
** sub-journal file is open too. If the main journal is not open,
** this function is a no-op.
**
** SQLITE_OK is returned if everything goes according to plan. 
** An SQLITE_IOERR_XXX error code is returned if a call to 
** sqlite3OsOpen() fails.
*/
static int openSubJournal(Pager *pPager){
  int rc = SQLITE_OK;
  if( isOpen(pPager->jfd) && !isOpen(pPager->sjfd) ){
    if( pPager->journalMode==PAGER_JOURNALMODE_MEMORY ){
      sqlite3MemJournalOpen(pPager->sjfd);
    }else{
      rc = pagerOpentemp(pPager, pPager->sjfd, SQLITE_OPEN_SUBJOURNAL);
    }
  }
  return rc;
}

/*
** This function is called at the start of every write transaction.
** There must already be a RESERVED or EXCLUSIVE lock on the database 
** file when this routine is called.
**
** Open the journal file for pager pPager and write a journal header
** to the start of it. If there are active savepoints, open the sub-journal
** as well. This function is only used when the journal file is being 
** opened to write a rollback log for a transaction. It is not used 
** when opening a hot journal file to roll it back.
**
** If the journal file is already open (as it may be in exclusive mode),
** then this function just writes a journal header to the start of the
** already open file. 
**
** Whether or not the journal file is opened by this function, the
** Pager.pInJournal bitvec structure is allocated.
**
** Return SQLITE_OK if everything is successful. Otherwise, return 
** SQLITE_NOMEM if the attempt to allocate Pager.pInJournal fails, or 
** an IO error code if opening or writing the journal file fails.
*/
static int pager_open_journal(Pager *pPager){
  int rc = SQLITE_OK;                        /* Return code */
  sqlite3_vfs * const pVfs = pPager->pVfs;   /* Local cache of vfs pointer */

  assert( pPager->state>=PAGER_RESERVED );
  assert( pPager->useJournal );
  assert( pPager->pInJournal==0 );
  
  /* If already in the error state, this function is a no-op. */
  if( pPager->errCode ){
    return pPager->errCode;
  }

  /* TODO: Is it really possible to get here with dbSizeValid==0? If not,
  ** the call to PagerPagecount() can be removed.
  */
  testcase( pPager->dbSizeValid==0 );
  sqlite3PagerPagecount(pPager, 0);

  pPager->pInJournal = sqlite3BitvecCreate(pPager->dbSize);
  if( pPager->pInJournal==0 ){
    return SQLITE_NOMEM;
  }

  /* Open the journal file if it is not already open. */
  if( !isOpen(pPager->jfd) ){
    if( pPager->journalMode==PAGER_JOURNALMODE_MEMORY ){
      sqlite3MemJournalOpen(pPager->jfd);
    }else{
      const int flags =                   /* VFS flags to open journal file */
        SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|
        (pPager->tempFile ? 
          (SQLITE_OPEN_DELETEONCLOSE|SQLITE_OPEN_TEMP_JOURNAL):
          (SQLITE_OPEN_MAIN_JOURNAL)
        );
#ifdef SQLITE_ENABLE_ATOMIC_WRITE
      rc = sqlite3JournalOpen(
          pVfs, pPager->zJournal, pPager->jfd, flags, jrnlBufferSize(pPager)
      );
#else
      rc = sqlite3OsOpen(pVfs, pPager->zJournal, pPager->jfd, flags, 0);
#endif
    }
    assert( rc!=SQLITE_OK || isOpen(pPager->jfd) );
  }


  /* Write the first journal header to the journal file and open 
  ** the sub-journal if necessary.
  */
  if( rc==SQLITE_OK ){
    /* TODO: Check if all of these are really required. */
    pPager->dbOrigSize = pPager->dbSize;
    pPager->journalStarted = 0;
    pPager->needSync = 0;
    pPager->nRec = 0;
    pPager->journalOff = 0;
    pPager->setMaster = 0;
    pPager->journalHdr = 0;
    rc = writeJournalHdr(pPager);
  }
  if( rc==SQLITE_OK && pPager->nSavepoint ){
    rc = openSubJournal(pPager);
  }

  if( rc!=SQLITE_OK ){
    sqlite3BitvecDestroy(pPager->pInJournal);
    pPager->pInJournal = 0;
  }
  return rc;
}

/*
** Begin a write-transaction on the specified pager object. If a 
** write-transaction has already been opened, this function is a no-op.
**
** If the exFlag argument is false, then acquire at least a RESERVED
** lock on the database file. If exFlag is true, then acquire at least
** an EXCLUSIVE lock. If such a lock is already held, no locking 
** functions need be called.
**
** If this is not a temporary or in-memory file and, the journal file is 
** opened if it has not been already. For a temporary file, the opening 
** of the journal file is deferred until there is an actual need to 
** write to the journal. TODO: Why handle temporary files differently?
**
** If the journal file is opened (or if it is already open), then a
** journal-header is written to the start of it.
*/
int sqlite3PagerBegin(Pager *pPager, int exFlag){
  int rc = SQLITE_OK;
  assert( pPager->state!=PAGER_UNLOCK );
  if( pPager->state==PAGER_SHARED ){
    assert( pPager->pInJournal==0 );
    assert( !MEMDB && !pPager->tempFile );

    /* Obtain a RESERVED lock on the database file. If the exFlag parameter
    ** is true, then immediately upgrade this to an EXCLUSIVE lock. The
    ** busy-handler callback can be used when upgrading to the EXCLUSIVE
    ** lock, but not when obtaining the RESERVED lock.
    */
    rc = sqlite3OsLock(pPager->fd, RESERVED_LOCK);
    if( rc==SQLITE_OK ){
      pPager->state = PAGER_RESERVED;
      if( exFlag ){
        rc = pager_wait_on_lock(pPager, EXCLUSIVE_LOCK);
      }
    }

    /* If the required locks were successfully obtained, open the journal
    ** file and write the first journal-header to it.
    */
    if( rc==SQLITE_OK && pPager->useJournal
     && pPager->journalMode!=PAGER_JOURNALMODE_OFF 
    ){
      rc = pager_open_journal(pPager);
    }
  }else if( isOpen(pPager->jfd) && pPager->journalOff==0 ){
    /* This happens when the pager was in exclusive-access mode the last
    ** time a (read or write) transaction was successfully concluded
    ** by this connection. Instead of deleting the journal file it was 
    ** kept open and either was truncated to 0 bytes or its header was
    ** overwritten with zeros.
    */
    assert( pPager->nRec==0 );
    assert( pPager->dbOrigSize==0 );
    assert( pPager->pInJournal==0 );
    rc = pager_open_journal(pPager);
  }

  PAGERTRACE(("TRANSACTION %d\n", PAGERID(pPager)));
  assert( !isOpen(pPager->jfd) || pPager->journalOff>0 || rc!=SQLITE_OK );
  return rc;
}

/*
** Mark a single data page as writeable. The page is written into the 
** main journal or sub-journal as required. If the page is written into
** one of the journals, the corresponding bit is set in the 
** Pager.pInJournal bitvec and the PagerSavepoint.pInSavepoint bitvecs
** of any open savepoints as appropriate.
*/
static int pager_write(PgHdr *pPg){
  void *pData = pPg->pData;
  Pager *pPager = pPg->pPager;
  int rc = SQLITE_OK;

  /* Check for errors
  */
  if( pPager->errCode ){ 
    return pPager->errCode;
  }
  if( pPager->readOnly ){
    return SQLITE_PERM;
  }

  assert( !pPager->setMaster );

  CHECK_PAGE(pPg);

  /* Mark the page as dirty.  If the page has already been written
  ** to the journal then we can return right away.
  */
  sqlite3PcacheMakeDirty(pPg);
  if( pageInJournal(pPg) && !subjRequiresPage(pPg) ){
    pPager->dbModified = 1;
  }else{

    /* If we get this far, it means that the page needs to be
    ** written to the transaction journal or the ckeckpoint journal
    ** or both.
    **
    ** First check to see that the transaction journal exists and
    ** create it if it does not.
    */
    assert( pPager->state!=PAGER_UNLOCK );
    rc = sqlite3PagerBegin(pPager, 0);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    assert( pPager->state>=PAGER_RESERVED );
    if( !isOpen(pPager->jfd) && pPager->useJournal
          && pPager->journalMode!=PAGER_JOURNALMODE_OFF ){
      rc = pager_open_journal(pPager);
      if( rc!=SQLITE_OK ) return rc;
    }
    pPager->dbModified = 1;
  
    /* The transaction journal now exists and we have a RESERVED or an
    ** EXCLUSIVE lock on the main database file.  Write the current page to
    ** the transaction journal if it is not there already.
    */
    if( !pageInJournal(pPg) && isOpen(pPager->jfd) ){
      if( pPg->pgno<=pPager->dbOrigSize ){
        u32 cksum;
        char *pData2;

        /* We should never write to the journal file the page that
        ** contains the database locks.  The following assert verifies
        ** that we do not. */
        assert( pPg->pgno!=PAGER_MJ_PGNO(pPager) );
        pData2 = CODEC2(pPager, pData, pPg->pgno, 7);
        cksum = pager_cksum(pPager, (u8*)pData2);
        rc = write32bits(pPager->jfd, pPager->journalOff, pPg->pgno);
        if( rc==SQLITE_OK ){
          rc = sqlite3OsWrite(pPager->jfd, pData2, pPager->pageSize,
                              pPager->journalOff + 4);
          pPager->journalOff += pPager->pageSize+4;
        }
        if( rc==SQLITE_OK ){
          rc = write32bits(pPager->jfd, pPager->journalOff, cksum);
          pPager->journalOff += 4;
        }
        IOTRACE(("JOUT %p %d %lld %d\n", pPager, pPg->pgno, 
                 pPager->journalOff, pPager->pageSize));
        PAGER_INCR(sqlite3_pager_writej_count);
        PAGERTRACE(("JOURNAL %d page %d needSync=%d hash(%08x)\n",
             PAGERID(pPager), pPg->pgno, 
             ((pPg->flags&PGHDR_NEED_SYNC)?1:0), pager_pagehash(pPg)));

        /* Even if an IO or diskfull error occurred while journalling the
        ** page in the block above, set the need-sync flag for the page.
        ** Otherwise, when the transaction is rolled back, the logic in
        ** playback_one_page() will think that the page needs to be restored
        ** in the database file. And if an IO error occurs while doing so,
        ** then corruption may follow.
        */
        if( !pPager->noSync ){
          pPg->flags |= PGHDR_NEED_SYNC;
          pPager->needSync = 1;
        }

        /* An error has occurred writing to the journal file. The 
        ** transaction will be rolled back by the layer above.
        */
        if( rc!=SQLITE_OK ){
          return rc;
        }

        pPager->nRec++;
        assert( pPager->pInJournal!=0 );
        rc = sqlite3BitvecSet(pPager->pInJournal, pPg->pgno);
        testcase( rc==SQLITE_NOMEM );
        assert( rc==SQLITE_OK || rc==SQLITE_NOMEM );
        rc |= addToSavepointBitvecs(pPager, pPg->pgno);
        if( rc!=SQLITE_OK ){
          assert( rc==SQLITE_NOMEM );
          return rc;
        }
      }else{
        if( !pPager->journalStarted && !pPager->noSync ){
          pPg->flags |= PGHDR_NEED_SYNC;
          pPager->needSync = 1;
        }
        PAGERTRACE(("APPEND %d page %d needSync=%d\n",
                PAGERID(pPager), pPg->pgno,
               ((pPg->flags&PGHDR_NEED_SYNC)?1:0)));
      }
    }
  
    /* If the statement journal is open and the page is not in it,
    ** then write the current page to the statement journal.  Note that
    ** the statement journal format differs from the standard journal format
    ** in that it omits the checksums and the header.
    */
    if( subjRequiresPage(pPg) ){
      rc = subjournalPage(pPg);
    }
  }

  /* Update the database size and return.
  */
  assert( pPager->state>=PAGER_SHARED );
  if( pPager->dbSize<pPg->pgno ){
    pPager->dbSize = pPg->pgno;
  }
  return rc;
}

/*
** Mark a data page as writeable. This routine must be called before 
** making changes to a page. The caller must check the return value 
** of this function and be careful not to change any page data unless 
** this routine returns SQLITE_OK.
**
** The difference between this function and pager_write() is that this
** function also deals with the special case where 2 or more pages
** fit on a single disk sector. In this case all co-resident pages
** must have been written to the journal file before returning.
**
** If an error occurs, SQLITE_NOMEM or an IO error code is returned
** as appropriate. Otherwise, SQLITE_OK.
*/
int sqlite3PagerWrite(DbPage *pDbPage){
  int rc = SQLITE_OK;

  PgHdr *pPg = pDbPage;
  Pager *pPager = pPg->pPager;
  Pgno nPagePerSector = (pPager->sectorSize/pPager->pageSize);

  if( nPagePerSector>1 ){
    Pgno nPageCount;          /* Total number of pages in database file */
    Pgno pg1;                 /* First page of the sector pPg is located on. */
    int nPage;                /* Number of pages starting at pg1 to journal */
    int ii;                   /* Loop counter */
    int needSync = 0;         /* True if any page has PGHDR_NEED_SYNC */

    /* Set the doNotSync flag to 1. This is because we cannot allow a journal
    ** header to be written between the pages journaled by this function.
    */
    assert( !MEMDB );
    assert( pPager->doNotSync==0 );
    pPager->doNotSync = 1;

    /* This trick assumes that both the page-size and sector-size are
    ** an integer power of 2. It sets variable pg1 to the identifier
    ** of the first page of the sector pPg is located on.
    */
    pg1 = ((pPg->pgno-1) & ~(nPagePerSector-1)) + 1;

    sqlite3PagerPagecount(pPager, (int *)&nPageCount);
    if( pPg->pgno>nPageCount ){
      nPage = (pPg->pgno - pg1)+1;
    }else if( (pg1+nPagePerSector-1)>nPageCount ){
      nPage = nPageCount+1-pg1;
    }else{
      nPage = nPagePerSector;
    }
    assert(nPage>0);
    assert(pg1<=pPg->pgno);
    assert((pg1+nPage)>pPg->pgno);

    for(ii=0; ii<nPage && rc==SQLITE_OK; ii++){
      Pgno pg = pg1+ii;
      PgHdr *pPage;
      if( pg==pPg->pgno || !sqlite3BitvecTest(pPager->pInJournal, pg) ){
        if( pg!=PAGER_MJ_PGNO(pPager) ){
          rc = sqlite3PagerGet(pPager, pg, &pPage);
          if( rc==SQLITE_OK ){
            rc = pager_write(pPage);
            if( pPage->flags&PGHDR_NEED_SYNC ){
              needSync = 1;
              assert(pPager->needSync);
            }
            sqlite3PagerUnref(pPage);
          }
        }
      }else if( (pPage = pager_lookup(pPager, pg))!=0 ){
        if( pPage->flags&PGHDR_NEED_SYNC ){
          needSync = 1;
        }
        sqlite3PagerUnref(pPage);
      }
    }

    /* If the PGHDR_NEED_SYNC flag is set for any of the nPage pages 
    ** starting at pg1, then it needs to be set for all of them. Because
    ** writing to any of these nPage pages may damage the others, the
    ** journal file must contain sync()ed copies of all of them
    ** before any of them can be written out to the database file.
    */
    if( needSync ){
      assert( !MEMDB && pPager->noSync==0 );
      for(ii=0; ii<nPage && needSync; ii++){
        PgHdr *pPage = pager_lookup(pPager, pg1+ii);
        if( pPage ){
          pPage->flags |= PGHDR_NEED_SYNC;
          sqlite3PagerUnref(pPage);
        }
      }
      assert(pPager->needSync);
    }

    assert( pPager->doNotSync==1 );
    pPager->doNotSync = 0;
  }else{
    rc = pager_write(pDbPage);
  }
  return rc;
}

/*
** Return TRUE if the page given in the argument was previously passed
** to sqlite3PagerWrite().  In other words, return TRUE if it is ok
** to change the content of the page.
*/
#ifndef NDEBUG
int sqlite3PagerIswriteable(DbPage *pPg){
  return pPg->flags&PGHDR_DIRTY;
}
#endif

/*
** A call to this routine tells the pager that it is not necessary to
** write the information on page pPg back to the disk, even though
** that page might be marked as dirty.  This happens, for example, when
** the page has been added as a leaf of the freelist and so its
** content no longer matters.
**
** The overlying software layer calls this routine when all of the data
** on the given page is unused. The pager marks the page as clean so
** that it does not get written to disk.
**
** Tests show that this optimization can quadruple the speed of large 
** DELETE operations.
*/
void sqlite3PagerDontWrite(PgHdr *pPg){
  Pager *pPager = pPg->pPager;
  if( (pPg->flags&PGHDR_DIRTY) && pPager->nSavepoint==0 ){
    PAGERTRACE(("DONT_WRITE page %d of %d\n", pPg->pgno, PAGERID(pPager)));
    IOTRACE(("CLEAN %p %d\n", pPager, pPg->pgno))
    pPg->flags |= PGHDR_DONT_WRITE;
#ifdef SQLITE_CHECK_PAGES
    pPg->pageHash = pager_pagehash(pPg);
#endif
  }
}

/*
** This routine is called to increment the value of the database file 
** change-counter, stored as a 4-byte big-endian integer starting at 
** byte offset 24 of the pager file.
**
** If the isDirect flag is zero, then this is done by calling 
** sqlite3PagerWrite() on page 1, then modifying the contents of the
** page data. In this case the file will be updated when the current
** transaction is committed.
**
** The isDirect flag may only be non-zero if the library was compiled
** with the SQLITE_ENABLE_ATOMIC_WRITE macro defined. In this case,
** if isDirect is non-zero, then the database file is updated directly
** by writing an updated version of page 1 using a call to the 
** sqlite3OsWrite() function.
*/
static int pager_incr_changecounter(Pager *pPager, int isDirectMode){
  int rc = SQLITE_OK;

  /* Declare and initialize constant integer 'isDirect'. If the
  ** atomic-write optimization is enabled in this build, then isDirect
  ** is initialized to the value passed as the isDirectMode parameter
  ** to this function. Otherwise, it is always set to zero.
  **
  ** The idea is that if the atomic-write optimization is not
  ** enabled at compile time, the compiler can omit the tests of
  ** 'isDirect' below, as well as the block enclosed in the
  ** "if( isDirect )" condition.
  */
#ifndef SQLITE_ENABLE_ATOMIC_WRITE
  const int isDirect = 0;
  assert( isDirectMode==0 );
  UNUSED_PARAMETER(isDirectMode);
#else
  const int isDirect = isDirectMode;
#endif

  assert( pPager->state>=PAGER_RESERVED );
  if( !pPager->changeCountDone && pPager->dbSize>0 ){
    PgHdr *pPgHdr;                /* Reference to page 1 */
    u32 change_counter;           /* Initial value of change-counter field */

    assert( !pPager->tempFile && isOpen(pPager->fd) );

    /* Open page 1 of the file for writing. */
    rc = sqlite3PagerGet(pPager, 1, &pPgHdr);
    assert( pPgHdr==0 || rc==SQLITE_OK );

    /* If page one was fetched successfully, and this function is not
    ** operating in direct-mode, make page 1 writable.
    */
    if( rc==SQLITE_OK && !isDirect ){
      rc = sqlite3PagerWrite(pPgHdr);
    }

    if( rc==SQLITE_OK ){
      /* Increment the value just read and write it back to byte 24. */
      change_counter = sqlite3Get4byte((u8*)pPager->dbFileVers);
      change_counter++;
      put32bits(((char*)pPgHdr->pData)+24, change_counter);

      /* If running in direct mode, write the contents of page 1 to the file. */
      if( isDirect ){
        const void *zBuf = pPgHdr->pData;
        assert( pPager->dbFileSize>0 );
        rc = sqlite3OsWrite(pPager->fd, zBuf, pPager->pageSize, 0);
      }

      /* If everything worked, set the changeCountDone flag. */
      if( rc==SQLITE_OK ){
        pPager->changeCountDone = 1;
      }
    }

    /* Release the page reference. */
    sqlite3PagerUnref(pPgHdr);
  }
  return rc;
}

/*
** Sync the pager file to disk. This is a no-op for in-memory files
** or pages with the Pager.noSync flag set.
**
** If successful, or called on a pager for which it is a no-op, this
** function returns SQLITE_OK. Otherwise, an IO error code is returned.
*/
int sqlite3PagerSync(Pager *pPager){
  int rc;                              /* Return code */
  if( MEMDB || pPager->noSync ){
    rc = SQLITE_OK;
  }else{
    rc = sqlite3OsSync(pPager->fd, pPager->sync_flags);
  }
  return rc;
}

/*
** Sync the database file for the pager pPager. zMaster points to the name
** of a master journal file that should be written into the individual
** journal file. zMaster may be NULL, which is interpreted as no master
** journal (a single database transaction).
**
** This routine ensures that:
**
**   * The database file change-counter is updated,
**   * the journal is synced (unless the atomic-write optimization is used),
**   * all dirty pages are written to the database file, 
**   * the database file is truncated (if required), and
**   * the database file synced. 
**
** The only thing that remains to commit the transaction is to finalize 
** (delete, truncate or zero the first part of) the journal file (or 
** delete the master journal file if specified).
**
** Note that if zMaster==NULL, this does not overwrite a previous value
** passed to an sqlite3PagerCommitPhaseOne() call.
**
** If the final parameter - noSync - is true, then the database file itself
** is not synced. The caller must call sqlite3PagerSync() directly to
** sync the database file before calling CommitPhaseTwo() to delete the
** journal file in this case.
*/
int sqlite3PagerCommitPhaseOne(
  Pager *pPager,                  /* Pager object */
  const char *zMaster,            /* If not NULL, the master journal name */
  int noSync                      /* True to omit the xSync on the db file */
){
  int rc = SQLITE_OK;             /* Return code */

  if( pPager->errCode ){
    return pPager->errCode;
  }

  PAGERTRACE(("DATABASE SYNC: File=%s zMaster=%s nSize=%d\n", 
      pPager->zFilename, zMaster, pPager->dbSize));

  /* If this is an in-memory db, or no pages have been written to, or this
  ** function has already been called, it is a no-op.
  */
  if( MEMDB && pPager->dbModified ){
    sqlite3BackupRestart(pPager->pBackup);
  }else if( pPager->state!=PAGER_SYNCED && pPager->dbModified ){

    /* The following block updates the change-counter. Exactly how it
    ** does this depends on whether or not the atomic-update optimization
    ** was enabled at compile time, and if this transaction meets the 
    ** runtime criteria to use the operation: 
    **
    **    * The file-system supports the atomic-write property for
    **      blocks of size page-size, and 
    **    * This commit is not part of a multi-file transaction, and
    **    * Exactly one page has been modified and store in the journal file.
    **
    ** If the optimization was not enabled at compile time, then the
    ** pager_incr_changecounter() function is called to update the change
    ** counter in 'indirect-mode'. If the optimization is compiled in but
    ** is not applicable to this transaction, call sqlite3JournalCreate()
    ** to make sure the journal file has actually been created, then call
    ** pager_incr_changecounter() to update the change-counter in indirect
    ** mode. 
    **
    ** Otherwise, if the optimization is both enabled and applicable,
    ** then call pager_incr_changecounter() to update the change-counter
    ** in 'direct' mode. In this case the journal file will never be
    ** created for this transaction.
    */
#ifdef SQLITE_ENABLE_ATOMIC_WRITE
    PgHdr *pPg;
    assert( isOpen(pPager->jfd) || pPager->journalMode==PAGER_JOURNALMODE_OFF );
    if( !zMaster && isOpen(pPager->jfd) 
     && pPager->journalOff==jrnlBufferSize(pPager) 
     && pPager->dbSize>=pPager->dbFileSize
     && (0==(pPg = sqlite3PcacheDirtyList(pPager->pPCache)) || 0==pPg->pDirty)
    ){
      /* Update the db file change counter via the direct-write method. The 
      ** following call will modify the in-memory representation of page 1 
      ** to include the updated change counter and then write page 1 
      ** directly to the database file. Because of the atomic-write 
      ** property of the host file-system, this is safe.
      */
      rc = pager_incr_changecounter(pPager, 1);
    }else{
      rc = sqlite3JournalCreate(pPager->jfd);
      if( rc==SQLITE_OK ){
        rc = pager_incr_changecounter(pPager, 0);
      }
    }
#else
    rc = pager_incr_changecounter(pPager, 0);
#endif
    if( rc!=SQLITE_OK ) goto commit_phase_one_exit;

    /* If this transaction has made the database smaller, then all pages
    ** being discarded by the truncation must be written to the journal
    ** file. This can only happen in auto-vacuum mode.
    **
    ** Before reading the pages with page numbers larger than the 
    ** current value of Pager.dbSize, set dbSize back to the value
    ** that it took at the start of the transaction. Otherwise, the
    ** calls to sqlite3PagerGet() return zeroed pages instead of 
    ** reading data from the database file.
    */
#ifndef SQLITE_OMIT_AUTOVACUUM
    if( pPager->dbSize<pPager->dbOrigSize
     && pPager->journalMode!=PAGER_JOURNALMODE_OFF 
    ){
      Pgno i;                                   /* Iterator variable */
      const Pgno iSkip = PAGER_MJ_PGNO(pPager); /* Pending lock page */
      const Pgno dbSize = pPager->dbSize;       /* Database image size */ 
      pPager->dbSize = pPager->dbOrigSize;
      for( i=dbSize+1; i<=pPager->dbOrigSize; i++ ){
        if( !sqlite3BitvecTest(pPager->pInJournal, i) && i!=iSkip ){
          PgHdr *pPage;             /* Page to journal */
          rc = sqlite3PagerGet(pPager, i, &pPage);
          if( rc!=SQLITE_OK ) goto commit_phase_one_exit;
          rc = sqlite3PagerWrite(pPage);
          sqlite3PagerUnref(pPage);
          if( rc!=SQLITE_OK ) goto commit_phase_one_exit;
        }
      } 
      pPager->dbSize = dbSize;
    }
#endif

    /* Write the master journal name into the journal file. If a master 
    ** journal file name has already been written to the journal file, 
    ** or if zMaster is NULL (no master journal), then this call is a no-op.
    */
    rc = writeMasterJournal(pPager, zMaster);
    if( rc!=SQLITE_OK ) goto commit_phase_one_exit;

    /* Sync the journal file. If the atomic-update optimization is being
    ** used, this call will not create the journal file or perform any
    ** real IO.
    */
    rc = syncJournal(pPager);
    if( rc!=SQLITE_OK ) goto commit_phase_one_exit;

    /* Write all dirty pages to the database file. */
    rc = pager_write_pagelist(sqlite3PcacheDirtyList(pPager->pPCache));
    if( rc!=SQLITE_OK ){
      assert( rc!=SQLITE_IOERR_BLOCKED );
      goto commit_phase_one_exit;
    }
    sqlite3PcacheCleanAll(pPager->pPCache);

    /* If the file on disk is not the same size as the database image,
    ** then use pager_truncate to grow or shrink the file here.
    */
    if( pPager->dbSize!=pPager->dbFileSize ){
      Pgno nNew = pPager->dbSize - (pPager->dbSize==PAGER_MJ_PGNO(pPager));
      assert( pPager->state>=PAGER_EXCLUSIVE );
      rc = pager_truncate(pPager, nNew);
      if( rc!=SQLITE_OK ) goto commit_phase_one_exit;
    }

    /* Finally, sync the database file. */
    if( !pPager->noSync && !noSync ){
      rc = sqlite3OsSync(pPager->fd, pPager->sync_flags);
    }
    IOTRACE(("DBSYNC %p\n", pPager))

    pPager->state = PAGER_SYNCED;
  }

commit_phase_one_exit:
  if( rc==SQLITE_IOERR_BLOCKED ){
    /* pager_incr_changecounter() may attempt to obtain an exclusive
    ** lock to spill the cache and return IOERR_BLOCKED. But since 
    ** there is no chance the cache is inconsistent, it is
    ** better to return SQLITE_BUSY.
    **/
    rc = SQLITE_BUSY;
  }
  return rc;
}


/*
** When this function is called, the database file has been completely
** updated to reflect the changes made by the current transaction and
** synced to disk. The journal file still exists in the file-system 
** though, and if a failure occurs at this point it will eventually
** be used as a hot-journal and the current transaction rolled back.
**
** This function finalizes the journal file, either by deleting, 
** truncating or partially zeroing it, so that it cannot be used 
** for hot-journal rollback. Once this is done the transaction is
** irrevocably committed.
**
** If an error occurs, an IO error code is returned and the pager
** moves into the error state. Otherwise, SQLITE_OK is returned.
*/
int sqlite3PagerCommitPhaseTwo(Pager *pPager){
  int rc = SQLITE_OK;                  /* Return code */

  /* Do not proceed if the pager is already in the error state. */
  if( pPager->errCode ){
    return pPager->errCode;
  }

  /* This function should not be called if the pager is not in at least
  ** PAGER_RESERVED state. And indeed SQLite never does this. But it is
  ** nice to have this defensive block here anyway.
  */
  if( NEVER(pPager->state<PAGER_RESERVED) ){
    return SQLITE_ERROR;
  }

  /* An optimization. If the database was not actually modified during
  ** this transaction, the pager is running in exclusive-mode and is
  ** using persistent journals, then this function is a no-op.
  **
  ** The start of the journal file currently contains a single journal 
  ** header with the nRec field set to 0. If such a journal is used as
  ** a hot-journal during hot-journal rollback, 0 changes will be made
  ** to the database file. So there is no need to zero the journal 
  ** header. Since the pager is in exclusive mode, there is no need
  ** to drop any locks either.
  */
  if( pPager->dbModified==0 && pPager->exclusiveMode 
   && pPager->journalMode==PAGER_JOURNALMODE_PERSIST
  ){
    assert( pPager->journalOff==JOURNAL_HDR_SZ(pPager) );
    return SQLITE_OK;
  }

  PAGERTRACE(("COMMIT %d\n", PAGERID(pPager)));
  assert( pPager->state==PAGER_SYNCED || MEMDB || !pPager->dbModified );
  rc = pager_end_transaction(pPager, pPager->setMaster);
  return pager_error(pPager, rc);
}

/*
** Rollback all changes. The database falls back to PAGER_SHARED mode.
**
** This function performs two tasks:
**
**   1) It rolls back the journal file, restoring all database file and 
**      in-memory cache pages to the state they were in when the transaction
**      was opened, and
**   2) It finalizes the journal file, so that it is not used for hot
**      rollback at any point in the future.
**
** subject to the following qualifications:
**
** * If the journal file is not yet open when this function is called,
**   then only (2) is performed. In this case there is no journal file
**   to roll back.
**
** * If in an error state other than SQLITE_FULL, then task (1) is 
**   performed. If successful, task (2). Regardless of the outcome
**   of either, the error state error code is returned to the caller
**   (i.e. either SQLITE_IOERR or SQLITE_CORRUPT).
**
** * If the pager is in PAGER_RESERVED state, then attempt (1). Whether
**   or not (1) is succussful, also attempt (2). If successful, return
**   SQLITE_OK. Otherwise, enter the error state and return the first 
**   error code encountered. 
**
**   In this case there is no chance that the database was written to. 
**   So is safe to finalize the journal file even if the playback 
**   (operation 1) failed. However the pager must enter the error state
**   as the contents of the in-memory cache are now suspect.
**
** * Finally, if in PAGER_EXCLUSIVE state, then attempt (1). Only
**   attempt (2) if (1) is successful. Return SQLITE_OK if successful,
**   otherwise enter the error state and return the error code from the 
**   failing operation.
**
**   In this case the database file may have been written to. So if the
**   playback operation did not succeed it would not be safe to finalize
**   the journal file. It needs to be left in the file-system so that
**   some other process can use it to restore the database state (by
**   hot-journal rollback).
*/
int sqlite3PagerRollback(Pager *pPager){
  int rc = SQLITE_OK;                  /* Return code */
  PAGERTRACE(("ROLLBACK %d\n", PAGERID(pPager)));
  if( !pPager->dbModified || !isOpen(pPager->jfd) ){
    rc = pager_end_transaction(pPager, pPager->setMaster);
  }else if( pPager->errCode && pPager->errCode!=SQLITE_FULL ){
    if( pPager->state>=PAGER_EXCLUSIVE ){
      pager_playback(pPager, 0);
    }
    rc = pPager->errCode;
  }else{
    if( pPager->state==PAGER_RESERVED ){
      int rc2;
      rc = pager_playback(pPager, 0);
      rc2 = pager_end_transaction(pPager, pPager->setMaster);
      if( rc==SQLITE_OK ){
        rc = rc2;
      }
    }else{
      rc = pager_playback(pPager, 0);
    }

    if( !MEMDB ){
      pPager->dbSizeValid = 0;
    }

    /* If an error occurs during a ROLLBACK, we can no longer trust the pager
    ** cache. So call pager_error() on the way out to make any error 
    ** persistent.
    */
    rc = pager_error(pPager, rc);
  }
  return rc;
}

/*
** Return TRUE if the database file is opened read-only.  Return FALSE
** if the database is (in theory) writable.
*/
u8 sqlite3PagerIsreadonly(Pager *pPager){
  return pPager->readOnly;
}

/*
** Return the number of references to the pager.
*/
int sqlite3PagerRefcount(Pager *pPager){
  return sqlite3PcacheRefCount(pPager->pPCache);
}

/*
** Return the number of references to the specified page.
*/
int sqlite3PagerPageRefcount(DbPage *pPage){
  return sqlite3PcachePageRefcount(pPage);
}

#ifdef SQLITE_TEST
/*
** This routine is used for testing and analysis only.
*/
int *sqlite3PagerStats(Pager *pPager){
  static int a[11];
  a[0] = sqlite3PcacheRefCount(pPager->pPCache);
  a[1] = sqlite3PcachePagecount(pPager->pPCache);
  a[2] = sqlite3PcacheGetCachesize(pPager->pPCache);
  a[3] = pPager->dbSizeValid ? (int) pPager->dbSize : -1;
  a[4] = pPager->state;
  a[5] = pPager->errCode;
  a[6] = pPager->nHit;
  a[7] = pPager->nMiss;
  a[8] = 0;  /* Used to be pPager->nOvfl */
  a[9] = pPager->nRead;
  a[10] = pPager->nWrite;
  return a;
}
#endif

/*
** Return true if this is an in-memory pager.
*/
int sqlite3PagerIsMemdb(Pager *pPager){
  return MEMDB;
}

/*
** Check that there are at least nSavepoint savepoints open. If there are
** currently less than nSavepoints open, then open one or more savepoints
** to make up the difference. If the number of savepoints is already
** equal to nSavepoint, then this function is a no-op.
**
** If a memory allocation fails, SQLITE_NOMEM is returned. If an error 
** occurs while opening the sub-journal file, then an IO error code is
** returned. Otherwise, SQLITE_OK.
*/
int sqlite3PagerOpenSavepoint(Pager *pPager, int nSavepoint){
  int rc = SQLITE_OK;                       /* Return code */
  int nCurrent = pPager->nSavepoint;        /* Current number of savepoints */

  if( nSavepoint>nCurrent && pPager->useJournal ){
    int ii;                                 /* Iterator variable */
    PagerSavepoint *aNew;                   /* New Pager.aSavepoint array */

    /* Either there is no active journal or the sub-journal is open or 
    ** the journal is always stored in memory */
    assert( pPager->nSavepoint==0 || isOpen(pPager->sjfd) ||
            pPager->journalMode==PAGER_JOURNALMODE_MEMORY );

    /* Grow the Pager.aSavepoint array using realloc(). Return SQLITE_NOMEM
    ** if the allocation fails. Otherwise, zero the new portion in case a 
    ** malloc failure occurs while populating it in the for(...) loop below.
    */
    aNew = (PagerSavepoint *)sqlite3Realloc(
        pPager->aSavepoint, sizeof(PagerSavepoint)*nSavepoint
    );
    if( !aNew ){
      return SQLITE_NOMEM;
    }
    memset(&aNew[nCurrent], 0, (nSavepoint-nCurrent) * sizeof(PagerSavepoint));
    pPager->aSavepoint = aNew;
    pPager->nSavepoint = nSavepoint;

    /* Populate the PagerSavepoint structures just allocated. */
    for(ii=nCurrent; ii<nSavepoint; ii++){
      assert( pPager->dbSizeValid );
      aNew[ii].nOrig = pPager->dbSize;
      if( isOpen(pPager->jfd) && pPager->journalOff>0 ){
        aNew[ii].iOffset = pPager->journalOff;
      }else{
        aNew[ii].iOffset = JOURNAL_HDR_SZ(pPager);
      }
      aNew[ii].iSubRec = pPager->nSubRec;
      aNew[ii].pInSavepoint = sqlite3BitvecCreate(pPager->dbSize);
      if( !aNew[ii].pInSavepoint ){
        return SQLITE_NOMEM;
      }
    }

    /* Open the sub-journal, if it is not already opened. */
    rc = openSubJournal(pPager);
  }

  return rc;
}

/*
** This function is called to rollback or release (commit) a savepoint.
** The savepoint to release or rollback need not be the most recently 
** created savepoint.
**
** Parameter op is always either SAVEPOINT_ROLLBACK or SAVEPOINT_RELEASE.
** If it is SAVEPOINT_RELEASE, then release and destroy the savepoint with
** index iSavepoint. If it is SAVEPOINT_ROLLBACK, then rollback all changes
** that have occurred since the specified savepoint was created.
**
** The savepoint to rollback or release is identified by parameter 
** iSavepoint. A value of 0 means to operate on the outermost savepoint
** (the first created). A value of (Pager.nSavepoint-1) means operate
** on the most recently created savepoint. If iSavepoint is greater than
** (Pager.nSavepoint-1), then this function is a no-op.
**
** If a negative value is passed to this function, then the current
** transaction is rolled back. This is different to calling 
** sqlite3PagerRollback() because this function does not terminate
** the transaction or unlock the database, it just restores the 
** contents of the database to its original state. 
**
** In any case, all savepoints with an index greater than iSavepoint 
** are destroyed. If this is a release operation (op==SAVEPOINT_RELEASE),
** then savepoint iSavepoint is also destroyed.
**
** This function may return SQLITE_NOMEM if a memory allocation fails,
** or an IO error code if an IO error occurs while rolling back a 
** savepoint. If no errors occur, SQLITE_OK is returned.
*/ 
int sqlite3PagerSavepoint(Pager *pPager, int op, int iSavepoint){
  int rc = SQLITE_OK;

  assert( op==SAVEPOINT_RELEASE || op==SAVEPOINT_ROLLBACK );
  assert( iSavepoint>=0 || op==SAVEPOINT_ROLLBACK );

  if( iSavepoint<pPager->nSavepoint ){
    int ii;            /* Iterator variable */
    int nNew;          /* Number of remaining savepoints after this op. */

    /* Figure out how many savepoints will still be active after this
    ** operation. Store this value in nNew. Then free resources associated 
    ** with any savepoints that are destroyed by this operation.
    */
    nNew = iSavepoint + (op==SAVEPOINT_ROLLBACK);
    for(ii=nNew; ii<pPager->nSavepoint; ii++){
      sqlite3BitvecDestroy(pPager->aSavepoint[ii].pInSavepoint);
    }
    pPager->nSavepoint = nNew;

    /* If this is a rollback operation, playback the specified savepoint.
    ** If this is a temp-file, it is possible that the journal file has
    ** not yet been opened. In this case there have been no changes to
    ** the database file, so the playback operation can be skipped.
    */
    if( op==SAVEPOINT_ROLLBACK && isOpen(pPager->jfd) ){
      PagerSavepoint *pSavepoint = (nNew==0)?0:&pPager->aSavepoint[nNew-1];
      rc = pagerPlaybackSavepoint(pPager, pSavepoint);
      assert(rc!=SQLITE_DONE);
    }
  
    /* If this is a release of the outermost savepoint, truncate 
    ** the sub-journal to zero bytes in size. */
    if( nNew==0 && op==SAVEPOINT_RELEASE && isOpen(pPager->sjfd) ){
      assert( rc==SQLITE_OK );
      rc = sqlite3OsTruncate(pPager->sjfd, 0);
      pPager->nSubRec = 0;
    }
  }
  return rc;
}

/*
** Return the full pathname of the database file.
*/
const char *sqlite3PagerFilename(Pager *pPager){
  return pPager->zFilename;
}

/*
** Return the VFS structure for the pager.
*/
const sqlite3_vfs *sqlite3PagerVfs(Pager *pPager){
  return pPager->pVfs;
}

/*
** Return the file handle for the database file associated
** with the pager.  This might return NULL if the file has
** not yet been opened.
*/
sqlite3_file *sqlite3PagerFile(Pager *pPager){
  return pPager->fd;
}

/*
** Return the full pathname of the journal file.
*/
const char *sqlite3PagerJournalname(Pager *pPager){
  return pPager->zJournal;
}

/*
** Return true if fsync() calls are disabled for this pager.  Return FALSE
** if fsync()s are executed normally.
*/
int sqlite3PagerNosync(Pager *pPager){
  return pPager->noSync;
}

#ifdef SQLITE_HAS_CODEC
/*
** Set the codec for this pager
*/
void sqlite3PagerSetCodec(
  Pager *pPager,
  void *(*xCodec)(void*,void*,Pgno,int),
  void *pCodecArg
){
  pPager->xCodec = xCodec;
  pPager->pCodecArg = pCodecArg;
}
#endif

#ifndef SQLITE_OMIT_AUTOVACUUM
/*
** Move the page pPg to location pgno in the file.
**
** There must be no references to the page previously located at
** pgno (which we call pPgOld) though that page is allowed to be
** in cache.  If the page previously located at pgno is not already
** in the rollback journal, it is not put there by by this routine.
**
** References to the page pPg remain valid. Updating any
** meta-data associated with pPg (i.e. data stored in the nExtra bytes
** allocated along with the page) is the responsibility of the caller.
**
** A transaction must be active when this routine is called. It used to be
** required that a statement transaction was not active, but this restriction
** has been removed (CREATE INDEX needs to move a page when a statement
** transaction is active).
**
** If the fourth argument, isCommit, is non-zero, then this page is being
** moved as part of a database reorganization just before the transaction 
** is being committed. In this case, it is guaranteed that the database page 
** pPg refers to will not be written to again within this transaction.
**
** This function may return SQLITE_NOMEM or an IO error code if an error
** occurs. Otherwise, it returns SQLITE_OK.
*/
int sqlite3PagerMovepage(Pager *pPager, DbPage *pPg, Pgno pgno, int isCommit){
  PgHdr *pPgOld;               /* The page being overwritten. */
  Pgno needSyncPgno = 0;       /* Old value of pPg->pgno, if sync is required */
  int rc;                      /* Return code */
  Pgno origPgno;               /* The original page number */

  assert( pPg->nRef>0 );

  /* If the page being moved is dirty and has not been saved by the latest
  ** savepoint, then save the current contents of the page into the 
  ** sub-journal now. This is required to handle the following scenario:
  **
  **   BEGIN;
  **     <journal page X, then modify it in memory>
  **     SAVEPOINT one;
  **       <Move page X to location Y>
  **     ROLLBACK TO one;
  **
  ** If page X were not written to the sub-journal here, it would not
  ** be possible to restore its contents when the "ROLLBACK TO one"
  ** statement were is processed.
  **
  ** subjournalPage() may need to allocate space to store pPg->pgno into
  ** one or more savepoint bitvecs. This is the reason this function
  ** may return SQLITE_NOMEM.
  */
  if( pPg->flags&PGHDR_DIRTY 
   && subjRequiresPage(pPg)
   && SQLITE_OK!=(rc = subjournalPage(pPg))
  ){
    return rc;
  }

  PAGERTRACE(("MOVE %d page %d (needSync=%d) moves to %d\n", 
      PAGERID(pPager), pPg->pgno, (pPg->flags&PGHDR_NEED_SYNC)?1:0, pgno));
  IOTRACE(("MOVE %p %d %d\n", pPager, pPg->pgno, pgno))

  /* If the journal needs to be sync()ed before page pPg->pgno can
  ** be written to, store pPg->pgno in local variable needSyncPgno.
  **
  ** If the isCommit flag is set, there is no need to remember that
  ** the journal needs to be sync()ed before database page pPg->pgno 
  ** can be written to. The caller has already promised not to write to it.
  */
  if( (pPg->flags&PGHDR_NEED_SYNC) && !isCommit ){
    needSyncPgno = pPg->pgno;
    assert( pageInJournal(pPg) || pPg->pgno>pPager->dbOrigSize );
    assert( pPg->flags&PGHDR_DIRTY );
    assert( pPager->needSync );
  }

  /* If the cache contains a page with page-number pgno, remove it
  ** from its hash chain. Also, if the PgHdr.needSync was set for 
  ** page pgno before the 'move' operation, it needs to be retained 
  ** for the page moved there.
  */
  pPg->flags &= ~PGHDR_NEED_SYNC;
  pPgOld = pager_lookup(pPager, pgno);
  assert( !pPgOld || pPgOld->nRef==1 );
  if( pPgOld ){
    pPg->flags |= (pPgOld->flags&PGHDR_NEED_SYNC);
    sqlite3PcacheDrop(pPgOld);
  }

  origPgno = pPg->pgno;
  sqlite3PcacheMove(pPg, pgno);
  sqlite3PcacheMakeDirty(pPg);
  pPager->dbModified = 1;

  if( needSyncPgno ){
    /* If needSyncPgno is non-zero, then the journal file needs to be 
    ** sync()ed before any data is written to database file page needSyncPgno.
    ** Currently, no such page exists in the page-cache and the 
    ** "is journaled" bitvec flag has been set. This needs to be remedied by
    ** loading the page into the pager-cache and setting the PgHdr.needSync 
    ** flag.
    **
    ** If the attempt to load the page into the page-cache fails, (due
    ** to a malloc() or IO failure), clear the bit in the pInJournal[]
    ** array. Otherwise, if the page is loaded and written again in
    ** this transaction, it may be written to the database file before
    ** it is synced into the journal file. This way, it may end up in
    ** the journal file twice, but that is not a problem.
    **
    ** The sqlite3PagerGet() call may cause the journal to sync. So make
    ** sure the Pager.needSync flag is set too.
    */
    PgHdr *pPgHdr;
    assert( pPager->needSync );
    rc = sqlite3PagerGet(pPager, needSyncPgno, &pPgHdr);
    if( rc!=SQLITE_OK ){
      if( pPager->pInJournal && needSyncPgno<=pPager->dbOrigSize ){
        sqlite3BitvecClear(pPager->pInJournal, needSyncPgno);
      }
      return rc;
    }
    pPager->needSync = 1;
    assert( pPager->noSync==0 && !MEMDB );
    pPgHdr->flags |= PGHDR_NEED_SYNC;
    sqlite3PcacheMakeDirty(pPgHdr);
    sqlite3PagerUnref(pPgHdr);
  }

  /*
  ** For an in-memory database, make sure the original page continues
  ** to exist, in case the transaction needs to roll back.  We allocate
  ** the page now, instead of at rollback, because we can better deal
  ** with an out-of-memory error now.  Ticket #3761.
  */
  if( MEMDB ){
    DbPage *pNew;
    rc = sqlite3PagerAcquire(pPager, origPgno, &pNew, 1);
    if( rc!=SQLITE_OK ) return rc;
    sqlite3PagerUnref(pNew);
  }

  return SQLITE_OK;
}
#endif

/*
** Return a pointer to the data for the specified page.
*/
void *sqlite3PagerGetData(DbPage *pPg){
  assert( pPg->nRef>0 || pPg->pPager->memDb );
  return pPg->pData;
}

/*
** Return a pointer to the Pager.nExtra bytes of "extra" space 
** allocated along with the specified page.
*/
void *sqlite3PagerGetExtra(DbPage *pPg){
  Pager *pPager = pPg->pPager;
  return (pPager?pPg->pExtra:0);
}

/*
** Get/set the locking-mode for this pager. Parameter eMode must be one
** of PAGER_LOCKINGMODE_QUERY, PAGER_LOCKINGMODE_NORMAL or 
** PAGER_LOCKINGMODE_EXCLUSIVE. If the parameter is not _QUERY, then
** the locking-mode is set to the value specified.
**
** The returned value is either PAGER_LOCKINGMODE_NORMAL or
** PAGER_LOCKINGMODE_EXCLUSIVE, indicating the current (possibly updated)
** locking-mode.
*/
int sqlite3PagerLockingMode(Pager *pPager, int eMode){
  assert( eMode==PAGER_LOCKINGMODE_QUERY
            || eMode==PAGER_LOCKINGMODE_NORMAL
            || eMode==PAGER_LOCKINGMODE_EXCLUSIVE );
  assert( PAGER_LOCKINGMODE_QUERY<0 );
  assert( PAGER_LOCKINGMODE_NORMAL>=0 && PAGER_LOCKINGMODE_EXCLUSIVE>=0 );
  if( eMode>=0 && !pPager->tempFile ){
    pPager->exclusiveMode = (u8)eMode;
  }
  return (int)pPager->exclusiveMode;
}

/*
** Get/set the journal-mode for this pager. Parameter eMode must be one of:
**
**    PAGER_JOURNALMODE_QUERY
**    PAGER_JOURNALMODE_DELETE
**    PAGER_JOURNALMODE_TRUNCATE
**    PAGER_JOURNALMODE_PERSIST
**    PAGER_JOURNALMODE_OFF
**    PAGER_JOURNALMODE_MEMORY
**
** If the parameter is not _QUERY, then the journal-mode is set to the
** value specified.  Except, an in-memory database can only have its
** journal mode set to _OFF or _MEMORY.  Attempts to change the journal
** mode of an in-memory database to something other than _OFF or _MEMORY
** are silently ignored.
**
** The returned indicate the current (possibly updated) journal-mode.
*/
int sqlite3PagerJournalMode(Pager *pPager, int eMode){
  assert( eMode==PAGER_JOURNALMODE_QUERY
            || eMode==PAGER_JOURNALMODE_DELETE
            || eMode==PAGER_JOURNALMODE_TRUNCATE
            || eMode==PAGER_JOURNALMODE_PERSIST
            || eMode==PAGER_JOURNALMODE_OFF 
            || eMode==PAGER_JOURNALMODE_MEMORY );
  assert( PAGER_JOURNALMODE_QUERY<0 );
  if( eMode>=0 && (!MEMDB || eMode==PAGER_JOURNALMODE_MEMORY 
                          || eMode==PAGER_JOURNALMODE_OFF) ){
    pPager->journalMode = (u8)eMode;
  }
  return (int)pPager->journalMode;
}

/*
** Get/set the size-limit used for persistent journal files.
**
** Setting the size limit to -1 means no limit is enforced.
** An attempt to set a limit smaller than -1 is a no-op.
*/
i64 sqlite3PagerJournalSizeLimit(Pager *pPager, i64 iLimit){
  if( iLimit>=-1 ){
    pPager->journalSizeLimit = iLimit;
  }
  return pPager->journalSizeLimit;
}

/*
** Return a pointer to the pPager->pBackup variable. The backup module
** in backup.c maintains the content of this variable. This module
** uses it opaquely as an argument to sqlite3BackupRestart() and
** sqlite3BackupUpdate() only.
*/
sqlite3_backup **sqlite3PagerBackupPtr(Pager *pPager){
  return &pPager->pBackup;
}

#endif /* SQLITE_OMIT_DISKIO */

/* BEGIN CRYPTO */
#ifdef SQLITE_HAS_CODEC
void sqlite3pager_get_codec(Pager *pPager, void **ctx) {
  *ctx = pPager->pCodecArg;
}

int sqlite3pager_is_mj_pgno(Pager *pPager, Pgno pgno) {
  return (PAGER_MJ_PGNO(pPager) == pgno) ? 1 : 0;
}

sqlite3_file *sqlite3Pager_get_fd(Pager *pPager) {
  return (isOpen(pPager->fd)) ? pPager->fd : NULL;
}

#endif
/* END CRYPTO */

