#include <turbodb/sqlite_revision.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
#define CALL(x,expected) do { turbodb_status_t rc_=(x); if (rc_!=(expected)) { \
  fprintf(stderr,"FAIL %d rc=%d expected=%d detail=%s\n",__LINE__,(int)rc_,(int)(expected),err.message); return 1; } } while(0)
static turbodb_blob_t blob(const char *s) {
  turbodb_blob_t out={s,strlen(s)};
  return out;
}
static turbodb_string_view_t text(const char *s) {
  return turbodb_view(s);
}
static int query(turbodb_revision_db_t *db, uint64_t at,
                 const char *ns, const char *term, int match) {
  turbodb_error_t err; turbodb_error_init(&err);
  turbodb_revision_scan_config_t cfg; turbodb_revision_scan_config_init(&cfg);
  turbodb_revision_read_t *r=NULL;
  turbodb_revision_scan_t *scan=NULL;
  turbodb_revision_value_t *val=NULL;
  turbodb_blob_t key={NULL,0};
  CALL(turbodb_revision_begin_read(db,at,&r,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_search_text(r,text(ns),
       blob("file-",5),text(term),&cfg,&scan,&err),
       TURBODB_STATUS_OK);
  if (match) {
    CALL(turbodb_revision_scan_next(scan,&key,&val,&err),TURBODB_STATUS_OK);
    CHECK(key.size==6 && memcmp(key.data,"file-A",6)==0);
    turbodb_revision_value_destroy(val);val=NULL;
  }
  CALL(turbodb_revision_scan_next(scan,&key,&val,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);
  turbodb_revision_read_close(r);
  return 0;
}
static int run(const char *path) {
  turbodb_revision_open_config_t cfg;
  turbodb_error_t err;
  turbodb_revision_db_t *db=NULL;
  turbodb_revision_write_t *w=NULL;
  turbodb_revision_read_t *r=NULL;
  turbodb_revision_scan_t *scan=NULL;
  turbodb_revision_scan_config_t sc;
  turbodb_revision_value_t *val=NULL;
  turbodb_blob_t key={NULL,0};
  sqlite3 *raw=NULL; sqlite3_stmt *stmt=NULL;
  uint64_t seq=0;
  turbodb_error_init(&err);
  turbodb_revision_open_config_init(&cfg);
  cfg.path=text(path);cfg.flags=TURBODB_REVISION_OPEN_CREATE;
  (void)remove(path);
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_begin_write(db,blob("req1"),&w,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("source"),blob("file-A"),blob("raw-v1"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
  CHECK(seq==1);
  CALL(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;

  /* v1->v2 is explicit; never silently install an FTS5 schema. */
  cfg.flags=TURBODB_REVISION_OPEN_FTS5;
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_UNSUPPORTED);
  CHECK(db==NULL);
  cfg.flags=TURBODB_REVISION_OPEN_CREATE|TURBODB_REVISION_OPEN_FTS5;
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
  CHECK(sqlite3_open(path,&raw)==SQLITE_OK);
  CHECK(sqlite3_prepare_v2(raw,"PRAGMA user_version",-1,&stmt,NULL)==SQLITE_OK);
  CHECK(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==2);
  CHECK(sqlite3_finalize(stmt)==SQLITE_OK);stmt=NULL;
  CHECK(sqlite3_close(raw)==SQLITE_OK);raw=NULL;
  CALL(turbodb_revision_inspect_request(db,blob("req1"),&seq,&err),TURBODB_STATUS_OK);
  CHECK(seq==1);

  /* Revision + text projection + both graph directions + generation
   * head are published by one SQLite COMMIT/request identity. */
  CALL(turbodb_revision_begin_write(db,blob("req2"),&w,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("source"),blob("file-A"),blob("raw-v2"),1,&err),
       TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_project_text(w,text("source"),blob("file-A"),
       text("fresh lake description"),&err),TURBODB_STATUS_OK);
  /* Another matching document in the SAME namespace must not leak outside
   * the authorized binary key prefix selected by the Host. */
  CALL(turbodb_revision_write_put(w,text("source"),blob("other-Z"),blob("raw-other"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_project_text(w,text("source"),blob("other-Z"),
       text("fresh lake description"),&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("graph"),blob("out/A:B"),blob("evidence-2"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("graph"),blob("in/B:A"),blob("evidence-2"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("generation"),blob("head"),blob("gen2"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
  CHECK(seq==2);
  CHECK(!query(db,2,"source","lake",1));
  /* Scope/key prefix is a hard SQL filter, not a result post-filter. */
  turbodb_revision_scan_config_init(&sc);
  CALL(turbodb_revision_begin_read(db,2,&r,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_search_text(r,text("source"),
       blob("other-"),text("lake"),&sc,&scan,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_scan_next(scan,&key,&val,&err),TURBODB_STATUS_OK);
  CHECK(key.size==7 && memcmp(key.data,"other-Z",7)==0);
  turbodb_revision_value_destroy(val);val=NULL;
  CALL(turbodb_revision_scan_next(scan,&key,&val,&err),TURBODB_STATUS_NOT_FOUND);
  turbodb_revision_scan_close(scan);scan=NULL;
  CALL(turbodb_revision_read_search_text(r,text("source"),
       (turbodb_blob_t){NULL,0},text("lake"),&sc,&scan,&err),
       TURBODB_STATUS_INVALID_ARGUMENT);
  CHECK(scan==NULL);
  turbodb_revision_read_close(r);r=NULL;
  CHECK(!query(db,1,"source","lake",0));
  CHECK(!query(db,2,"graph","lake",0));
  turbodb_revision_scan_config_init(&sc);
  CALL(turbodb_revision_begin_read(db,2,&r,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_read_scan_prefix(r,text("graph"),blob("out/"),&sc,&scan,&err),
       TURBODB_STATUS_OK);
  CALL(turbodb_revision_scan_next(scan,&key,&val,&err),TURBODB_STATUS_OK);
  CHECK(key.size==7 && memcmp(key.data,"out/A:B",7)==0);
  turbodb_revision_value_destroy(val);val=NULL;
  turbodb_revision_scan_close(scan);scan=NULL;
  turbodb_revision_read_close(r);r=NULL;

  CALL(turbodb_revision_begin_write(db,blob("req3"),&w,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("source"),blob("file-A"),blob("raw-v3"),2,&err),
       TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_project_text(w,text("source"),blob("file-A"),
       text("mountain fog"),&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_OK);w=NULL;
  CHECK(seq==3);
  CHECK(!query(db,2,"source","lake",1));
  CHECK(!query(db,3,"source","lake",0));
  CHECK(!query(db,3,"source","mountain",1));

  /* Any failed text projection poisons the whole batch, preserving history. */
  CALL(turbodb_revision_begin_write(db,blob("bad"),&w,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_put(w,text("source"),blob("file-B"),blob("raw"),
       TURBODB_REVISION_EXPECT_ABSENT,&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_project_text(w,text("source"),blob("file-B"),
       text("coast"),&err),TURBODB_STATUS_OK);
  CALL(turbodb_revision_write_project_text(w,text("source"),blob("file-B"),
       text("coast"),&err),TURBODB_STATUS_CONSTRAINT);
  CALL(turbodb_revision_write_publish(w,&seq,&err),TURBODB_STATUS_INVALID_STATE);
  turbodb_revision_write_abort(w);w=NULL;
  CALL(turbodb_revision_inspect_request(db,blob("bad"),&seq,&err),TURBODB_STATUS_NOT_FOUND);
  CHECK(!query(db,3,"source","coast",0));

  CALL(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
  cfg.flags=TURBODB_REVISION_OPEN_READ_ONLY|TURBODB_REVISION_OPEN_FTS5;
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_OK);
  CHECK(!query(db,2,"source","lake",1));
  CALL(turbodb_revision_db_close(db,&err),TURBODB_STATUS_OK);db=NULL;
  /* Reject untrusted extra schema objects in v2 including FTS shadow changes. */
  CHECK(sqlite3_open(path,&raw)==SQLITE_OK);
  CHECK(sqlite3_exec(raw,"CREATE VIEW injected AS SELECT 1;",NULL,NULL,NULL)==SQLITE_OK);
  CHECK(sqlite3_close(raw)==SQLITE_OK);raw=NULL;
  CALL(turbodb_revision_db_open(&cfg,&db,&err),TURBODB_STATUS_UNSUPPORTED);
  CHECK(db==NULL);
  (void)remove(path);
  puts("Revisioned SQLite Graph/FTS typed projection PASS");
  return 0;
}
int main(int argc,char **argv) {
  return run(argc>1 ? argv[1] : "revision_projection_test.db");
}
