#include <orm.h>
#include <stdint.h>

typedef orm_status_t (ORM_C_CALL *orm_retain_connection_fn)(orm_connection_t *);
typedef orm_status_t (ORM_C_CALL *orm_retain_query_fn)(orm_query_t *);
typedef orm_status_t (ORM_C_CALL *orm_retain_transaction_fn)(orm_transaction_t *);
typedef orm_status_t (ORM_C_CALL *orm_close_connection_fn)(orm_connection_t *, orm_error_t *);

_Static_assert(_Generic(&orm_connection_retain, orm_retain_connection_fn: 1, default: 0),
               "connection retain signature drift");
_Static_assert(_Generic(&orm_query_retain, orm_retain_query_fn: 1, default: 0),
               "query retain signature drift");
_Static_assert(_Generic(&orm_transaction_retain, orm_retain_transaction_fn: 1, default: 0),
               "transaction retain signature drift");
_Static_assert(_Generic(&orm_connection_close, orm_close_connection_fn: 1, default: 0),
               "connection close signature drift");

int main(void) {
  orm_retain_connection_fn volatile retain_connection = &orm_connection_retain;
  orm_retain_query_fn volatile retain_query = &orm_query_retain;
  orm_retain_transaction_fn volatile retain_transaction = &orm_transaction_retain;
  orm_close_connection_fn volatile close_connection = &orm_connection_close;
  if (orm_c_abi_version() != ORM_C_ABI_VERSION) return 1;
  if (ORM_STATUS_COMMIT_UNKNOWN != 16 || ORM_STATUS_CLEANUP_FAILED != 17) return 2;
  if (retain_connection(NULL) != ORM_STATUS_INVALID_ARGUMENT) return 3;
  if (retain_query(NULL) != ORM_STATUS_INVALID_ARGUMENT) return 4;
  if (retain_transaction(NULL) != ORM_STATUS_INVALID_ARGUMENT) return 5;
  if (close_connection(NULL, NULL) != ORM_STATUS_INVALID_ARGUMENT) return 6;
  return 0;
}
