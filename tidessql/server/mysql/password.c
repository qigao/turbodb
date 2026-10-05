#include "password.h"
#include <gmssl/pbkdf2.h>
#include <gmssl/rand.h>
#include <gmssl/mem.h>
#include <stdio.h>
#include <string.h>

static turbodb_status_t password_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  if(error && error->struct_size>=sizeof(*error)) {
    error->status=status; (void)snprintf(error->message,sizeof(error->message),"MySQL password: %s",reason);
  }
  return status;
}
static bool password_iterations(uint32_t count) {
  return count>=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS && count<=TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS;
}
static turbodb_status_t password_bytes(const uint8_t *bytes,size_t size,turbodb_error_t *error) {
  if(!bytes || !size) return password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty credential");
  if(size>TDSQL_MYSQL_PASSWORD_BYTES) return password_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"credential byte limit exceeded");
  return memchr(bytes,0,size)?password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"embedded credential terminator"):TURBODB_STATUS_OK;
}
static turbodb_status_t password_hash(const tdsql_mysql_password_record *record,const uint8_t *bytes,size_t size,
    uint8_t hash[TDSQL_MYSQL_PASSWORD_HASH_BYTES],turbodb_error_t *error) {
  if(pbkdf2_hmac_genkey(DIGEST_sha256(),(const char *)bytes,size,record->salt,sizeof(record->salt),
      record->iterations,TDSQL_MYSQL_PASSWORD_HASH_BYTES,hash)!=1)
    return password_error(error,TURBODB_STATUS_INTERNAL_ERROR,"password derivation failed");
  return TURBODB_STATUS_OK;
}
turbodb_status_t tdsql_mysql_password_make(vstr password,uint32_t iterations,tdsql_mysql_password_record *out,turbodb_error_t *error) {
  if(!out || !password_iterations(iterations)) return password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid verifier output or work factor");
  turbodb_status_t status=password_bytes((const uint8_t *)password.data,password.len,error);
  if(status!=TURBODB_STATUS_OK) return status;
  tdsql_mysql_password_record record={.iterations=iterations};
  if(rand_bytes(record.salt,sizeof(record.salt))!=1) status=password_error(error,TURBODB_STATUS_INTERNAL_ERROR,"credential randomness failed");
  if(status==TURBODB_STATUS_OK) status=password_hash(&record,(const uint8_t *)password.data,password.len,record.hash,error);
  if(status==TURBODB_STATUS_OK) *out=record;
  gmssl_secure_clear(&record,sizeof(record)); return status;
}
turbodb_status_t tdsql_mysql_password_verify(const tdsql_mysql_password_record *record,const uint8_t *payload,size_t size,
    bool tls,bool *verified,turbodb_error_t *error) {
  if(!tls) return password_error(error,TURBODB_STATUS_INVALID_STATE,"TLS establishment required");
  if(!record || !verified || !password_iterations(record->iterations)) return password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid verifier or work factor");
  if(!payload || !size) return password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing full-auth payload");
  if(size>TDSQL_MYSQL_PASSWORD_BYTES+1) return password_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"full-auth byte limit exceeded");
  if(payload[size-1]!=0) return password_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"unterminated full-auth payload");
  turbodb_status_t status=password_bytes(payload,size-1,error);
  if(status!=TURBODB_STATUS_OK) return status;
  uint8_t hash[TDSQL_MYSQL_PASSWORD_HASH_BYTES]={0}; status=password_hash(record,payload,size-1,hash,error);
  if(status==TURBODB_STATUS_OK) *verified=gmssl_secure_memcmp(hash,record->hash,sizeof(hash))==0;
  gmssl_secure_clear(hash,sizeof(hash)); return status;
}
tdsql_mysql_status tdsql_mysql_full_auth_encode(tdsql_mysql_output *out) {
  static const uint8_t request[]={1,4};
  if(!out) return TDSQL_MYSQL_INVALID;
  if(out->capacity<sizeof(request)) return TDSQL_MYSQL_LIMIT;
  if(out->data) memcpy(out->data,request,sizeof(request));
  out->size=sizeof(request); return TDSQL_MYSQL_OK;
}
