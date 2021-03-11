/**
 * Copyright (c) 2018 rxi
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _UUID4_H
#define _UUID4_H

#define UUID4_VERSION "1.0.0"

enum { UUID4_ESUCCESS = 0, UUID4_EFAILURE = -1 };

typedef char uuid4_t[16];
typedef char uuid4_string_t[37];

int uuid4_parse(char *in, uuid4_t uu);
void uuid4_unparse_upper(uuid4_t uu, uuid4_string_t out);
void uuid4_clear(uuid4_t uu);
int uuid4_is_null(uuid4_t uu);
void uuid4_generate(uuid4_t uu);
void uuid4_copy(uuid4_t dst, uuid4_t src);
int uuid4_compare(uuid4_t uu1, uuid4_t uu2);
int invalid_uuid4(char *str_uu);

#endif
