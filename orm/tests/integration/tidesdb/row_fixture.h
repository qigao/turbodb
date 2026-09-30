#ifndef ORM_TIDESDB_TEST_ROW_H
#define ORM_TIDESDB_TEST_ROW_H

#include <orm.h>
#include <cmeta/struct.h>
#include <stddef.h>

#define ORM_TIDES_PUBLIC_DATA_PREFIX_SIZE                                  \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(orm_tides_public_row, (long, id), (long, score));

static const cmeta_type_identity orm_tides_public_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesPublicRow");
static const cmeta_type_traits orm_tides_public_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc orm_tides_public_row_type = {
    "orm_tides_public_row", sizeof(orm_tides_public_row),
    _Alignof(orm_tides_public_row), CMETA_T_OBJECT, NULL,
    &orm_tides_public_row_traits, &orm_tides_public_row_identity};
static const cmeta_data_field_desc orm_tides_public_row_fields[] = {
    {"orm.test.TidesPublicRow.id", "id", offsetof(orm_tides_public_row, id),
     &cmeta_data_long},
    {"orm.test.TidesPublicRow.score", "score",
     offsetof(orm_tides_public_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape orm_tides_public_row_shape = {
    StructMeta(orm_tides_public_row), orm_tides_public_row_fields, 2u};
static const cmeta_data_desc orm_tides_public_row_data = {
    .struct_size = ORM_TIDES_PUBLIC_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.TidesPublicRow.data",
    .display_name = "TidesPublicRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &orm_tides_public_row_type,
    .shape = &orm_tides_public_row_shape};


#endif
