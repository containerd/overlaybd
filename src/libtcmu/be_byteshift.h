/*
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#pragma once

#include <endian.h>
#include <stdint.h>
#include <string.h>

static inline void __put_unaligned_be32(uint32_t val, uint8_t *p)
{
	*p++ = val >> 24;
	*p++ = val >> 16;
	*p++ = val >> 8;
	*p++ = val;
}

static inline void put_unaligned_be32(uint32_t val, void *p)
{
	__put_unaligned_be32(val, p);
}

static inline void __put_unaligned_be16(uint16_t val, uint8_t *p)
{
	*p++ = val >> 8;
	*p++ = val;
}

static inline void put_unaligned_be16(uint16_t val, void *p)
{
	__put_unaligned_be16(val, (uint8_t*)p);
}

static inline uint16_t __get_unaligned_be16(const uint8_t *p)
{
	return p[0] << 8 | p[1];
}

static inline uint16_t get_unaligned_be16(const void *p)
{
	return __get_unaligned_be16((uint8_t*)p);
}

static inline uint32_t __get_unaligned_be32(const uint8_t *p)
{
	return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static inline uint32_t get_unaligned_be32(const void *p)
{
	return __get_unaligned_be32(p);
}

static inline uint64_t get_unaligned_be64(const void *p)
{
	uint64_t val;
	memcpy(&val, p, sizeof(val));
	return be64toh(val);
}

static inline void put_unaligned_be64(uint64_t val, void *p)
{
	val = htobe64(val);
	memcpy(p, &val, sizeof(val));
}
