#include <turbodb/sqlite_revision.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do {if(!(e)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#e);return 1;}}while(0)
#define OP(e,want) do {turbodb_status_t actual=(e);if(actual!=(want)){fprintf(stderr,"FAIL line %d got %d want %d: %s\n",__LINE__,actual,(want),err.message);return 1;}}while(0)

typedef struct {sqlite3_file base; sqlite3_file *under; sqlite3_io_methods ops;} wrapfile;
static sqlite3_vfs *under_vfs;
static sqlite3_vfs wrapped_vfs;
static int fail_next_sync=0, fail_next_write=0, write_count=0, sync_count=0;
#define F(p) (((wrapfile *)(p))->under)
#define M(p) (F(p)->pMethods)
static int ioClose(sqlite3_file*p){return M(p)->xClose(F(p));}
static int ioRead(sqlite3_file*p,void*b,int n,sqlite3_int64 o){return M(p)->xRead(F(p),b,n,o);}
static int ioWrite(sqlite3_file*p,const void*b,int n,sqlite3_int64 o){write_count++;if(fail_next_write){fail_next_write=0;return SQLITE_IOERR_WRITE;}return M(p)->xWrite(F(p),b,n,o);}
static int ioTruncate(sqlite3_file*p,sqlite3_int64 n){return M(p)->xTruncate(F(p),n);}
static int ioSync(sqlite3_file*p,int n){sync_count++;if(fail_next_sync){fail_next_sync=0;return SQLITE_IOERR_FSYNC;}return M(p)->xSync(F(p),n);}
static int ioFileSize(sqlite3_file*p,sqlite3_int64*n){return M(p)->xFileSize(F(p),n);}
static int ioLock(sqlite3_file*p,int n){return M(p)->xLock(F(p),n);}
static int ioUnlock(sqlite3_file*p,int n){return M(p)->xUnlock(F(p),n);}
static int ioCheckReservedLock(sqlite3_file*p,int*n){return M(p)->xCheckReservedLock(F(p),n);}
static int ioFileControl(sqlite3_file*p,int op,void*a){return M(p)->xFileControl(F(p),op,a);}
static int ioSectorSize(sqlite3_file*p){return M(p)->xSectorSize(F(p));}
static int ioDeviceCharacteristics(sqlite3_file*p){return M(p)->xDeviceCharacteristics(F(p));}
static int ioShmMap(sqlite3_file*p,int a,int b,int c,void volatile**d){return M(p)->xShmMap(F(p),a,b,c,d);}
static int ioShmLock(sqlite3_file*p,int a,int b,int c){return M(p)->xShmLock(F(p),a,b,c);}
static void ioShmBarrier(sqlite3_file*p){M(p)->xShmBarrier(F(p));}
static int ioShmUnmap(sqlite3_file*p,int a){return M(p)->xShmUnmap(F(p),a);}
static int ioFetch(sqlite3_file*p,sqlite3_int64 a,int b,void**c){return M(p)->xFetch(F(p),a,b,c);}
static int ioUnfetch(sqlite3_file*p,sqlite3_int64 a,void*b){return M(p)->xUnfetch(F(p),a,b);}
static int vOpen(sqlite3_vfs *v,const char *name,sqlite3_file *file,int flags,int *out){
 (void)v;
 wrapfile *w=(wrapfile*)file;
 unsigned char *p=(unsigned char*)file+sizeof(wrapfile);
 uintptr_t addr=((uintptr_t)p+15u)&~(uintptr_t)15u;
 w->under=(sqlite3_file *)addr;
 memset(w->under,0,(size_t)under_vfs->szOsFile);
 int rc=under_vfs->xOpen(under_vfs,name,w->under,flags,out);
 if(rc!=SQLITE_OK)return rc;
 w->ops=*w->under->pMethods;
 w->ops.xClose=ioClose;w->ops.xRead=ioRead;w->ops.xWrite=ioWrite;w->ops.xTruncate=ioTruncate;
 w->ops.xSync=ioSync;w->ops.xFileSize=ioFileSize;w->ops.xLock=ioLock;w->ops.xUnlock=ioUnlock;
 w->ops.xCheckReservedLock=ioCheckReservedLock;w->ops.xFileControl=ioFileControl;w->ops.xSectorSize=ioSectorSize;
 w->ops.xDeviceCharacteristics=ioDeviceCharacteristics;
 if(w->ops.iVersion>=2){w->ops.xShmMap=ioShmMap;w->ops.xShmLock=ioShmLock;w->ops.xShmBarrier=ioShmBarrier;w->ops.xShmUnmap=ioShmUnmap;}
 if(w->ops.iVersion>=3){w->ops.xFetch=ioFetch;w->ops.xUnfetch=ioUnfetch;}
 w->base.pMethods=&w->ops;
 return SQLITE_OK;
}
static int vDelete(sqlite3_vfs*v,const char*n,int f){(void)v;return under_vfs->xDelete(under_vfs,n,f);}
static int vAccess(sqlite3_vfs*v,const char*n,int f,int*out){(void)v;return under_vfs->xAccess(under_vfs,n,f,out);}
static int vFullPathname(sqlite3_vfs*v,const char*n,int bytes,char*out){(void)v;return under_vfs->xFullPathname(under_vfs,n,bytes,out);}
static void *vDlOpen(sqlite3_vfs*v,const char*n){(void)v;return under_vfs->xDlOpen(under_vfs,n);}
static void vDlError(sqlite3_vfs*v,int bytes,char*out){(void)v;under_vfs->xDlError(under_vfs,bytes,out);}
static void (*vDlSym(sqlite3_vfs*v,void*handle,const char*n))(void){(void)v;return under_vfs->xDlSym(under_vfs,handle,n);}
static void vDlClose(sqlite3_vfs*v,void*handle){(void)v;under_vfs->xDlClose(under_vfs,handle);}
static int vRandomness(sqlite3_vfs*v,int bytes,char*out){(void)v;return under_vfs->xRandomness(under_vfs,bytes,out);}
static int vSleep(sqlite3_vfs*v,int ms){(void)v;return under_vfs->xSleep(under_vfs,ms);}
static int vCurrentTime(sqlite3_vfs*v,double*out){(void)v;return under_vfs->xCurrentTime(under_vfs,out);}
static int vGetLastError(sqlite3_vfs*v,int n,char*out){(void)v;return under_vfs->xGetLastError(under_vfs,n,out);}
static int vCurrentTimeInt64(sqlite3_vfs*v,sqlite3_int64*out){(void)v;return under_vfs->xCurrentTimeInt64(under_vfs,out);}
static int vSetSystemCall(sqlite3_vfs*v,const char*n,sqlite3_syscall_ptr ptr){(void)v;return under_vfs->xSetSystemCall(under_vfs,n,ptr);}
static sqlite3_syscall_ptr vGetSystemCall(sqlite3_vfs*v,const char*n){(void)v;return under_vfs->xGetSystemCall(under_vfs,n);}
static const char *vNextSystemCall(sqlite3_vfs*v,const char*n){(void)v;return under_vfs->xNextSystemCall(under_vfs,n);}
static int install(void){
 under_vfs=sqlite3_vfs_find(NULL);if(!under_vfs)return 1;
 wrapped_vfs=*under_vfs;
 wrapped_vfs.zName="revision-fault-vfs";
 wrapped_vfs.pNext=NULL;
 wrapped_vfs.szOsFile=(int)sizeof(wrapfile)+under_vfs->szOsFile+16;
 wrapped_vfs.xOpen=vOpen;wrapped_vfs.xDelete=vDelete;wrapped_vfs.xAccess=vAccess;
 wrapped_vfs.xFullPathname=vFullPathname;wrapped_vfs.xDlOpen=vDlOpen;wrapped_vfs.xDlError=vDlError;
 wrapped_vfs.xDlSym=vDlSym;wrapped_vfs.xDlClose=vDlClose;wrapped_vfs.xRandomness=vRandomness;
 wrapped_vfs.xSleep=vSleep;wrapped_vfs.xCurrentTime=vCurrentTime;wrapped_vfs.xGetLastError=vGetLastError;
 if(wrapped_vfs.iVersion>=2) wrapped_vfs.xCurrentTimeInt64=vCurrentTimeInt64;
 if(wrapped_vfs.iVersion>=3){wrapped_vfs.xSetSystemCall=vSetSystemCall;wrapped_vfs.xGetSystemCall=vGetSystemCall;wrapped_vfs.xNextSystemCall=vNextSystemCall;}
 return sqlite3_vfs_register(&wrapped_vfs,1)!=SQLITE_OK;
}
static turbodb_blob_t blob(const void *d,size_t n){turbodb_blob_t b={d,n};return b;}
/* FTS5 and Graph/Generation were staged in the SAME revision transaction.
 * On a failed xSync the commit outcome can be unknown, but a clean reopen
 * must see both projections or neither. Do not automatically replay. */
static int verify_projected_fault(const char *original) {
 char path[1024];
 CHECK(strlen(original) < sizeof(path)-32);
 (void)snprintf(path, sizeof(path), "%s-fts.db", original);
 (void)remove(path);
 turbodb_revision_db_t *db=NULL;
 turbodb_revision_write_t *w=NULL;
 turbodb_revision_read_t *read=NULL;
 turbodb_revision_scan_t *scan=NULL;
 turbodb_revision_value_t *value=NULL;
 turbodb_revision_scan_config_t scan_config;
 turbodb_revision_open_config_t cfg;
 turbodb_error_t err;
 uint64_t seq=0;
 turbodb_status_t status;
 turbodb_blob_t key={NULL,0};
 turbodb_error_init(&err);
 turbodb_revision_open_config_init(&cfg);
 cfg.path=turbodb_view(path);
 cfg.flags=TURBODB_REVISION_OPEN_CREATE|TURBODB_REVISION_OPEN_FTS5;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_begin_write(db,blob("fts-req",7),&w,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_put(w,turbodb_view("source"),blob("fileA",5),
        blob("revision",8),TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_project_text(w,turbodb_view("source"),
        blob("fileA",5),turbodb_view("bright sky"),&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_put(w,turbodb_view("graph"),blob("out/A:B",7),
        blob("rev1",4),TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_put(w,turbodb_view("generation"),blob("head",4),
        blob("gen1",4),TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
 fail_next_sync=1;
 status=turbodb_revision_write_publish(w,&seq,&err);
 CHECK(status==TURBODB_STATUS_COMMIT_UNKNOWN);
 turbodb_revision_write_abort(w);w=NULL;
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 cfg.flags=TURBODB_REVISION_OPEN_READ_ONLY|TURBODB_REVISION_OPEN_FTS5;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 status=turbodb_revision_inspect_request(db,blob("fts-req",7),&seq,&err);
 CHECK(status==TURBODB_STATUS_OK || status==TURBODB_STATUS_NOT_FOUND);
 const int committed=(status==TURBODB_STATUS_OK);
 OP(turbodb_revision_begin_read(db,0,&read,&err),TURBODB_STATUS_OK);
 status=turbodb_revision_read_get(read,turbodb_view("graph"),blob("out/A:B",7),
          &value,&err);
 CHECK(status==(committed?TURBODB_STATUS_OK:TURBODB_STATUS_NOT_FOUND));
 turbodb_revision_value_destroy(value);value=NULL;
 status=turbodb_revision_read_get(read,turbodb_view("generation"),blob("head",4),
          &value,&err);
 CHECK(status==(committed?TURBODB_STATUS_OK:TURBODB_STATUS_NOT_FOUND));
 turbodb_revision_value_destroy(value);value=NULL;
 turbodb_revision_scan_config_init(&scan_config);
 OP(turbodb_revision_read_search_text(read,turbodb_view("source"),
       blob("file",4),turbodb_view("sky"),&scan_config,&scan,&err),TURBODB_STATUS_OK);
 status=turbodb_revision_scan_next(scan,&key,&value,&err);
 CHECK(status==(committed?TURBODB_STATUS_OK:TURBODB_STATUS_NOT_FOUND));
 if(committed) {
   CHECK(key.size==5 && memcmp(key.data,"fileA",5)==0);
   turbodb_revision_value_destroy(value);value=NULL;
   OP(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
 }
 turbodb_revision_scan_close(scan);scan=NULL;
 turbodb_revision_read_close(read);read=NULL;
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 (void)remove(path);
 puts("FTS+Graph uncertain commit inspection PASS");
 return 0;
}

int main(int argc, char **argv){
 const char *path=argc > 1 ? argv[1] : "sqlite_revision_fault_test.db";
 (void)remove(path);
 CHECK(install()==0);
 turbodb_revision_db_t *db=NULL;
 turbodb_revision_write_t *w=NULL;
 turbodb_revision_open_config_t cfg;
 turbodb_error_t err;
 turbodb_error_init(&err);
 turbodb_revision_open_config_init(&cfg);
 cfg.path=turbodb_view(path);cfg.flags=TURBODB_REVISION_OPEN_CREATE;cfg.busy_timeout_ms=100;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_begin_write(db,blob("req1",4),&w,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_put(w,turbodb_view("ns"),blob("key",3),blob("old",3),0,&err),TURBODB_STATUS_OK);
 uint64_t seq=0;OP(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
 OP(turbodb_revision_begin_write(db,blob("req2",4),&w,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_write_put(w,turbodb_view("ns"),blob("key",3),blob("new",3),1,&err),TURBODB_STATUS_OK);
 fail_next_sync=1;
 turbodb_status_t status=turbodb_revision_write_publish(w,&seq,&err);
 fprintf(stderr,"Fault commit=%d err=%s write_count=%d sync_count=%d\n",status,err.message,write_count,sync_count);
 CHECK(status==TURBODB_STATUS_COMMIT_UNKNOWN);
 turbodb_revision_write_abort(w);w=NULL;
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 cfg.flags=TURBODB_REVISION_OPEN_READ_ONLY;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 status=turbodb_revision_inspect_request(db,blob("req2",4),&seq,&err);
 fprintf(stderr,"Reinspect req2=%d seq=%llu\n",status,(unsigned long long)seq);
 CHECK(status==TURBODB_STATUS_NOT_FOUND || (status==TURBODB_STATUS_OK && seq==2));
 {
   const int was_committed = status == TURBODB_STATUS_OK;
   turbodb_revision_read_t *reader = NULL;
   turbodb_revision_value_t *value = NULL;
   OP(turbodb_revision_begin_read(db, 0, &reader, &err), TURBODB_STATUS_OK);
   OP(turbodb_revision_read_get(reader, turbodb_view("ns"), blob("key",3), &value, &err), TURBODB_STATUS_OK);
   const turbodb_blob_t latest = turbodb_revision_value_bytes(value);
   CHECK(latest.size == 3 &&
     memcmp(latest.data, was_committed ? "new" : "old", 3) == 0);
   turbodb_revision_value_destroy(value);
   turbodb_revision_read_close(reader);
 }
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 cfg.flags=0;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_begin_write(db,blob("req3",4),&w,&err),TURBODB_STATUS_OK);
 fail_next_write=1;
 status=turbodb_revision_write_put(w,turbodb_view("ns"),blob("other",5),blob("bad",3),0,&err);
 fprintf(stderr,"faulted xWrite status=%d msg=%s\n",status,err.message);
 CHECK(status==TURBODB_STATUS_DATASTORE_ERROR);
 turbodb_revision_write_abort(w);w=NULL;
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 cfg.flags=TURBODB_REVISION_OPEN_READ_ONLY;
 OP(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
 OP(turbodb_revision_inspect_request(db,blob("req3",4),&seq,&err),TURBODB_STATUS_NOT_FOUND);
 OP(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
 CHECK(verify_projected_fault(path)==0);
 CHECK(sqlite3_vfs_register(under_vfs,1)==SQLITE_OK);
 CHECK(sqlite3_vfs_unregister(&wrapped_vfs)==SQLITE_OK);
 puts("fault vfs C11 PASS");
 return 0;
}
