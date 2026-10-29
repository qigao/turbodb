#include <turbodb/sqlite_revision.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do {if (!(c)) {fprintf(stderr,"CHECK FAIL %s:%d %s\n", __FILE__, __LINE__,#c);return 1;}}while(0)
#define CALL(expr, expected) do {turbodb_status_t code=(expr);if(code!=(expected)){fprintf(stderr,"CALL FAIL %s:%d %s got %d wanted %d (%s)\n",__FILE__,__LINE__, #expr,code,expected,err.message);return 1;}}while(0)
static turbodb_blob_t blob(const void *data,size_t size) { turbodb_blob_t b={data,size};return b; }
static int put(turbodb_revision_write_t *w,const char *key,const char *body,uint64_t expect,turbodb_error_t *err) {
  turbodb_status_t code=turbodb_revision_write_put(w,turbodb_view("graph"),blob(key,strlen(key)),blob(body,strlen(body)),expect,err);
  if(code != TURBODB_STATUS_OK) {fprintf(stderr,"put failed %s: %s\n",key,err->message);return 1;}
  return 0;
}
static int check_next(turbodb_revision_scan_t *scan,const char *key,const char *body, uint64_t seq,int deleted,turbodb_error_t *err){
  turbodb_blob_t k={0};turbodb_revision_value_t *v=NULL;
  turbodb_status_t code=turbodb_revision_scan_next(scan,&k,&v,err);
  if(code != TURBODB_STATUS_OK){fprintf(stderr,"next failed wanted %s got %d %s\n",key,code,err->message);return 1;}
  if(k.size!=strlen(key)||memcmp(k.data,key,k.size)!=0||turbodb_revision_value_sequence(v)!=seq||turbodb_revision_value_is_deleted(v)!=deleted){
    fprintf(stderr,"row mismatch wanted %s seq %llu deleted %d\n",key,(unsigned long long)seq,deleted);turbodb_revision_value_destroy(v);return 1;}
  const turbodb_blob_t val=turbodb_revision_value_bytes(v);
  if(!deleted&&(val.size!=strlen(body)||memcmp(val.data,body,val.size)!=0)){fprintf(stderr,"body mismatch key %s\n",key);turbodb_revision_value_destroy(v);return 1;}
  turbodb_revision_value_destroy(v);
  return 0;
}
static int run(const char *path) {
  turbodb_revision_open_config_t cfg;turbodb_revision_db_t *db=NULL;turbodb_revision_write_t *w=NULL;
  turbodb_revision_scan_config_t opts;turbodb_revision_read_t *read=NULL;turbodb_revision_scan_t *scan=NULL;
  turbodb_revision_value_t *value=NULL; turbodb_blob_t key={0};turbodb_error_t err;uint64_t seq=0;
  turbodb_error_init(&err);turbodb_revision_open_config_init(&cfg);cfg.path=turbodb_view(path);cfg.flags=TURBODB_REVISION_OPEN_CREATE|TURBODB_REVISION_OPEN_WAL;
  (void)remove(path);
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db,blob("req-one",7),&w,&err),TURBODB_STATUS_OK);
  CHECK(!put(w,"out/A:0001","old-1",0,&err));CHECK(!put(w,"out/A:0002","old-2",0,&err));CHECK(!put(w,"in/B:0001","in-first",0,&err));
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;CHECK(seq==1);
  /* This old snapshot is retained even across writer commit in WAL mode. */
  CALL(turbodb_revision_begin_read(db,0,&read,&err),TURBODB_STATUS_OK);CHECK(turbodb_revision_read_sequence(read)==1);
  turbodb_revision_scan_config_init(&opts);CHECK(opts.abi_version==TURBODB_REVISION_SCAN_ABI_VERSION);
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  turbodb_revision_scan_t *second = NULL;
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),
       blob("in/B:",5),&opts,&second,&err),TURBODB_STATUS_BUSY);
  CHECK(second == NULL);
  CALL(turbodb_revision_read_get(read,turbodb_view("graph"),blob("out/A:0001",10),&value,&err),TURBODB_STATUS_BUSY);
  CHECK(value==NULL);
  CHECK(!check_next(scan,"out/A:0001","old-1",1,0,&err));
  CHECK(!check_next(scan,"out/A:0002","old-2",1,0,&err));
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  CHECK(key.data==NULL&&value==NULL);
  turbodb_revision_scan_close(scan);scan=NULL;
  CALL(turbodb_revision_begin_write(db,blob("req-two",7),&w,&err),TURBODB_STATUS_OK);
  CHECK(!put(w,"out/A:0001","new-1",1,&err));
  CALL(turbodb_revision_write_delete(w,turbodb_view("graph"),blob("out/A:0002",10),1,&err),TURBODB_STATUS_OK);
  CHECK(!put(w,"out/A:0003","new-3",0,&err));
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;CHECK(seq==2);
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  CHECK(!check_next(scan,"out/A:0001","old-1",1,0,&err));
  CHECK(!check_next(scan,"out/A:0002","old-2",1,0,&err));
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  /* Reverse adjacency is independently indexed under the same revision. */
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),
       blob("in/B:",5),&opts,&scan,&err),TURBODB_STATUS_OK);
  CHECK(!check_next(scan,"in/B:0001","in-first",1,0,&err));
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  /* read_close closes an active scan, preventing leaked SQLite statements. */
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),
       blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  turbodb_revision_read_close(read);read=NULL;scan=NULL;
  CALL(turbodb_revision_begin_read(db,2,&read,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  CHECK(!check_next(scan,"out/A:0001","new-1",2,0,&err));
  CHECK(!check_next(scan,"out/A:0002","",2,1,&err));
  CHECK(!check_next(scan,"out/A:0003","new-3",2,0,&err));
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  /* Deliberately disclose truncation when max_items is reached. */
  opts.max_items=1;
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  CHECK(!check_next(scan,"out/A:0001","new-1",2,0,&err));
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_LIMIT_EXCEEDED);
  CHECK(value==NULL&&key.data==NULL);
  turbodb_revision_scan_close(scan);scan=NULL;
  opts.max_items=128;opts.max_result_bytes=6;
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_LIMIT_EXCEEDED);
  turbodb_revision_scan_close(scan);scan=NULL;
  opts.max_result_bytes=1024*1024;
  /* Native key prefix may contain 0x00 and 0xff. */
  turbodb_revision_read_close(read);read=NULL;
  CALL(turbodb_revision_begin_write(db,blob("req-bin",7),&w,&err),TURBODB_STATUS_OK);
  const unsigned char kb1[]={0x00,0xff,0x01},kb2[]={0xff,0xff,0xff};
  CALL(turbodb_revision_write_put(w,turbodb_view("bytes"),blob(kb1,sizeof(kb1)),blob("a",1),0,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,turbodb_view("bytes"),blob(kb2,sizeof(kb2)),blob("b",1),0,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
  CALL(turbodb_revision_begin_write(db,blob("req-bulk",8),&w,&err),TURBODB_STATUS_OK);
  for (unsigned index=0; index<256; ++index) {
    char item[32];(void)snprintf(item,sizeof(item),"bulk/%03u",index);
    CHECK(!put(w,item,"x",0,&err));
  }
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
  CALL(turbodb_revision_begin_read(db,0,&read,&err),TURBODB_STATUS_OK);
  opts.max_vm_steps=100;
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("bulk/",5),&opts,&scan,&err),TURBODB_STATUS_OK);
  { int hit_limit=0;
    for (int i=0;i<300;i++) {
      turbodb_status_t status=turbodb_revision_scan_next(scan,&key,&value,&err);
      if (status==TURBODB_STATUS_LIMIT_EXCEEDED) {hit_limit=1;break;}
      if (status==TURBODB_STATUS_NOT_FOUND) break;
      CHECK(status==TURBODB_STATUS_OK);turbodb_revision_value_destroy(value);value=NULL;
    }
    CHECK(hit_limit);
  }
  turbodb_revision_scan_close(scan);scan=NULL;
  opts.max_vm_steps=1000000;

  const unsigned char prefix1[]={0x00,0xff};
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("bytes"),blob(prefix1,sizeof(prefix1)),&opts,&scan,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_OK);
  CHECK(key.size==sizeof(kb1) && memcmp(key.data,kb1,key.size)==0);
  turbodb_revision_value_destroy(value);value=NULL;
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  const unsigned char prefixFF[]={0xff,0xff};
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("bytes"),blob(prefixFF,sizeof(prefixFF)),&opts,&scan,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_OK);
  CHECK(key.size==sizeof(kb2) && memcmp(key.data,kb2,key.size)==0);
  turbodb_revision_value_destroy(value);value=NULL;
  CALL(turbodb_revision_scan_next(scan,&key,&value,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  turbodb_revision_read_close(read);read=NULL;
  CALL(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
  cfg.flags=TURBODB_REVISION_OPEN_READ_ONLY;
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_read(db,1,&read,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_scan_prefix(read,turbodb_view("graph"),blob("out/A:",6),&opts,&scan,&err),TURBODB_STATUS_OK);
  CHECK(!check_next(scan,"out/A:0001","old-1",1,0,&err));
  turbodb_revision_scan_close(scan);scan=NULL;turbodb_revision_read_close(read);read=NULL;
  CALL(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);
  (void)remove(path);return 0;
}
int main(int argc,char**argv){const char*path=argc>1?argv[1]:"graph_prefix_test.db";int rc=run(path);if(!rc)puts("Revisioned SQLite graph prefix snapshot PASS");return rc;}
