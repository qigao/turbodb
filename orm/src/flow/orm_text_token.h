#ifndef ORM_TEXT_TOKEN_H
#define ORM_TEXT_TOKEN_H

#include <cserde/cserde.h>

#include <stddef.h>

cserde_status orm_text_token_sint(const unsigned char *data, size_t size,
                                  cserde_token *out);
cserde_status orm_text_token_uint(const unsigned char *data, size_t size,
                                  cserde_token *out);
cserde_status orm_text_token_float(const unsigned char *data, size_t size,
                                   int finite_only, cserde_token *out);

#endif
