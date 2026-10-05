#include "password.h"
#include <gmssl/pbkdf2.h>
#include <gmssl/rand.h>
#include <gmssl/mem.h>
#include <tinytest.h>
#include <string.h>

/* Fault injection stays in this translation unit, without production hooks. */
static bool fail_random,fail_hash,cleared;
static size_t random_calls,hash_calls,clear_calls;
static int password_test_random(uint8_t *out,size_t size) {
  ++random_calls;
  if(fail_random) { memset(out,0xa5,size); return -1; }
  return rand_bytes(out,size);
}
static int password_test_hash(const DIGEST *digest,const char *pass,size_t pass_size,
    const uint8_t *salt,size_t salt_size,size_t iterations,size_t out_size,uint8_t *out) {
  ++hash_calls;
  if(fail_hash) { memset(out,0xa5,out_size); return -1; }
  return pbkdf2_hmac_genkey(digest,pass,pass_size,salt,salt_size,iterations,out_size,out);
}
static void password_test_clear(void *buffer,size_t size) {
  gmssl_secure_clear(buffer,size); ++clear_calls;
  const uint8_t *bytes=buffer;
  for(size_t i=0;i<size;++i) if(bytes[i]) cleared=false;
}
#define rand_bytes password_test_random
#define pbkdf2_hmac_genkey password_test_hash
#define gmssl_secure_clear password_test_clear
#include "../../server/mysql/password.c"
#undef rand_bytes
#undef pbkdf2_hmac_genkey
#undef gmssl_secure_clear

static tdsql_mysql_password_record golden(void) {
  /* Independently derived PBKDF2-HMAC-SHA256: password=test-password,
   * salt=00..0f, iterations=600000, derived-key bytes=32. */
  tdsql_mysql_password_record record={.iterations=TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS,
    .salt={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    .hash={0xca,0xe9,0xc8,0x01,0x37,0x45,0x96,0xf1,0x7d,0xe4,0x8b,0xa4,0xed,0x70,0x61,0x69,
      0x2b,0x5d,0x0a,0xb4,0x33,0x93,0x2a,0x7d,0x3c,0xdf,0x69,0x8d,0xfd,0x8b,0xb3,0xe8}};
  return record;
}
static tdsql_mysql_password_record record,output,sentinel;
static turbodb_error_t error;
static bool verified;
static const uint8_t password[]="test-password";

spec("MySQL TLS full authentication password verifier") {
  before_each() {
    fail_random=false; fail_hash=false; cleared=true;
    random_calls=0; hash_calls=0; clear_calls=0;
    record=golden(); memset(&sentinel,0xa5,sizeof(sentinel)); output=sentinel;
    turbodb_error_init(&error); verified=true;
  }
  it("matches an independent PBKDF2 vector and distinguishes a wrong password") {
    uint8_t owned[sizeof(password)]; memcpy(owned,password,sizeof(owned));
    check_equal(tdsql_mysql_password_verify(&record,owned,sizeof(owned),true,&verified,&error),TURBODB_STATUS_OK);
    check_true(verified); check_equal(owned,password,sizeof(owned));
    owned[0]='x';
    check_equal(tdsql_mysql_password_verify(&record,owned,sizeof(owned),true,&verified,&error),TURBODB_STATUS_OK);
    check_false(verified); check_equal(hash_calls,2u); check_equal(clear_calls,2u); check_true(cleared);
  }
  it("generates independent random salts and verifies an owned immutable record") {
    vstr text={(const char *)password,sizeof(password)-1};
    check_equal(tdsql_mysql_password_make(text,record.iterations,&output,&error),TURBODB_STATUS_OK);
    tdsql_mysql_password_record first=output;
    check_equal(tdsql_mysql_password_make(text,record.iterations,&output,&error),TURBODB_STATUS_OK);
    check_not_equal(first.salt,output.salt,sizeof(first.salt));
    check_equal(tdsql_mysql_password_verify(&first,password,sizeof(password),true,&verified,&error),TURBODB_STATUS_OK);
    check_true(verified); check_equal(random_calls,2u); check_equal(hash_calls,3u); check_true(cleared);
    gmssl_secure_clear(&first,sizeof(first)); gmssl_secure_clear(&output,sizeof(output));
  }
  it("rejects plaintext before accessing credentials or performing expensive work") {
    check_equal(tdsql_mysql_password_verify(NULL,NULL,0,false,&verified,&error),TURBODB_STATUS_INVALID_STATE);
    check_true(verified); check_equal(hash_calls,0u); check_equal(clear_calls,0u);
  }
  it("rejects missing embedded and excess terminators without deriving a key") {
    const uint8_t malformed[]={'p',0,'x',0},empty[]={0};
    check_equal(tdsql_mysql_password_verify(&record,password,sizeof(password)-1,true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(&record,malformed,sizeof(malformed),true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(&record,empty,sizeof(empty),true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(&record,NULL,1,true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(&record,password,0,true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_true(verified); check_equal(hash_calls,0u);
  }
  it("enforces byte limits and accepts exactly the maximum password length") {
    uint8_t bytes[TDSQL_MYSQL_PASSWORD_BYTES+2]; memset(bytes,'x',sizeof(bytes)); bytes[sizeof(bytes)-1]=0;
    vstr too_large={(const char *)bytes,TDSQL_MYSQL_PASSWORD_BYTES+1};
    check_equal(tdsql_mysql_password_make(too_large,record.iterations,&output,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(tdsql_mysql_password_verify(&record,bytes,sizeof(bytes),true,&verified,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(hash_calls,0u); check_equal(random_calls,0u);
    bytes[TDSQL_MYSQL_PASSWORD_BYTES]=0;
    check_equal(tdsql_mysql_password_verify(&record,bytes,TDSQL_MYSQL_PASSWORD_BYTES+1,true,&verified,&error),TURBODB_STATUS_OK);
    check_false(verified); check_equal(hash_calls,1u); check_true(cleared);
  }
  it("rejects invalid work factors and missing outputs before hashing or randomness") {
    const uint32_t invalid[]={0,TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS-1,TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS+1,UINT32_MAX};
    vstr text={(const char *)password,sizeof(password)-1};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(tdsql_mysql_password_make(text,invalid[i],&output,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      record.iterations=invalid[i];
      check_equal(tdsql_mysql_password_verify(&record,password,sizeof(password),true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    }
    record=golden();
    check_equal(tdsql_mysql_password_make(text,record.iterations,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(NULL,password,sizeof(password),true,&verified,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_mysql_password_verify(&record,password,sizeof(password),true,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(random_calls,0u); check_equal(hash_calls,0u); check_true(verified);
    check_equal(&output,&sentinel,sizeof(output));
  }
  it("clears partially written salt on CSPRNG failure and preserves output") {
    fail_random=true; vstr text={(const char *)password,sizeof(password)-1};
    check_equal(tdsql_mysql_password_make(text,record.iterations,&output,&error),TURBODB_STATUS_INTERNAL_ERROR);
    check_equal(&output,&sentinel,sizeof(output)); check_equal(random_calls,1u); check_equal(hash_calls,0u);
    check_equal(clear_calls,1u); check_true(cleared); check_false(strstr(error.message,(const char *)password)!=NULL);
  }
  it("clears partially derived secrets on failure without changing caller results") {
    fail_hash=true; vstr text={(const char *)password,sizeof(password)-1};
    check_equal(tdsql_mysql_password_make(text,record.iterations,&output,&error),TURBODB_STATUS_INTERNAL_ERROR);
    check_equal(&output,&sentinel,sizeof(output));
    check_equal(tdsql_mysql_password_verify(&record,password,sizeof(password),true,&verified,&error),TURBODB_STATUS_INTERNAL_ERROR);
    check_true(verified); check_equal(clear_calls,2u); check_true(cleared);
    check_false(strstr(error.message,(const char *)password)!=NULL);
  }
  it("accepts the maximum configured work factor without lowering it") {
    fail_hash=true; record.iterations=TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS;
    check_equal(tdsql_mysql_password_verify(&record,password,sizeof(password),true,&verified,&error),TURBODB_STATUS_INTERNAL_ERROR);
    check_equal(hash_calls,1u); check_equal(clear_calls,1u); check_true(cleared);
  }
  it("atomically measures and emits the protocol full-auth request") {
    uint8_t bytes[]={0xa5,0xa5}; const uint8_t original[]={0xa5,0xa5},expected[]={1,4};
    tdsql_mysql_output out={bytes,1,9};
    check_equal(tdsql_mysql_full_auth_encode(&out),TDSQL_MYSQL_LIMIT);
    check_equal(out.size,9u); check_equal(bytes,original,sizeof(bytes));
    out.capacity=sizeof(bytes); out.data=NULL;
    check_equal(tdsql_mysql_full_auth_encode(&out),TDSQL_MYSQL_OK); check_equal(out.size,sizeof(expected));
    out.data=bytes; check_equal(tdsql_mysql_full_auth_encode(&out),TDSQL_MYSQL_OK); check_equal(bytes,expected,sizeof(bytes));
    check_equal(tdsql_mysql_full_auth_encode(NULL),TDSQL_MYSQL_INVALID);
  }
}
