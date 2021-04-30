/*
 * A simple kernel FIFO implementation.
 *
 * Copyright (C) 2004 Stelian Pop <stelian@popies.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

// Source https://github.com/spotify/linux/blob/master/kernel/kfifo.c
// Modified By wujiaxu <void00@foxmail.com>

#include "kfifo.h"
#include <assert.h>
#include <errno.h>
#include <memory.h>
#include <stdlib.h>

static inline unsigned int _min(unsigned int a, unsigned int b)
{
    return (a < b) ? a : b;
}

/**
 * kfifo_alloc - allocates a new FIFO and its internal buffer
 * @size: the size of the internal buffer, this have to be a power of 2.
 * @lock: the lock to be used to protect the fifo buffer
 *
 * size MUST be a power of 2
 */
struct kfifo *kfifo_alloc(unsigned int size)
{
    void *buffer;
    struct kfifo *fifo;

    /* size must be a power of 2 */
    if ((size < 2) || (size & (size - 1)))
        return NULL;

    buffer = malloc(size);
    if (!buffer)
        return NULL;

    fifo = malloc(sizeof(struct kfifo));
    if (!fifo)
        return NULL;

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->in = fifo->out = 0;

    if (pthread_spin_init(fifo->lock, PTHREAD_PROCESS_PRIVATE) != 0)
    {
        free(fifo);
        return NULL;
    }

    return fifo;
}

/**
 * kfifo_free - frees the FIFO
 * @fifo: the fifo to be freed.
 */
void kfifo_free(struct kfifo *fifo)
{
    pthread_spin_destroy(fifo->lock);
    free(fifo->buffer);
    free(fifo);
}

/**
 * __kfifo_put - puts some data into the FIFO, no locking version
 * @fifo: the fifo to be used.
 * @buffer: the data to be added.
 * @len: the length of the data to be added.
 *
 * This function copies at most @len bytes from the @buffer into
 * the FIFO depending on the free space, and returns the number of
 * bytes copied.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
unsigned int __kfifo_put(struct kfifo *fifo,
            const void *buffer, unsigned int len)
{
    unsigned int l;

    len = _min(len, fifo->size - fifo->in + fifo->out);

    /*
     * Ensure that we sample the fifo->out index -before- we
     * start putting bytes into the kfifo.
     */

    asm volatile("mfence" ::: "memory");

    /* first put the data starting from fifo->in to buffer end */
    l = _min(len, fifo->size - (fifo->in & (fifo->size - 1)));
    memcpy(fifo->buffer + (fifo->in & (fifo->size - 1)), buffer, l);

    /* then put the rest (if any) at the beginning of the buffer */
    memcpy(fifo->buffer, buffer + l, len - l);

    /*
     * Ensure that we add the bytes to the kfifo -before-
     * we update the fifo->in index.
     */

    asm volatile("sfence" ::: "memory");

    fifo->in += len;

    return len;
}

/**
 * __kfifo_get - gets some data from the FIFO, no locking version
 * @fifo: the fifo to be used.
 * @buffer: where the data must be copied.
 * @len: the size of the destination buffer.
 *
 * This function copies at most @len bytes from the FIFO into the
 * @buffer and returns the number of copied bytes.
 *
 * Note that with only one concurrent reader and one concurrent
 * writer, you don't need extra locking to use these functions.
 */
unsigned int __kfifo_get(struct kfifo *fifo,
             void *buffer, unsigned int len)
{
    unsigned int l;

    len = _min(len, fifo->in - fifo->out);

    /*
     * Ensure that we sample the fifo->in index -before- we
     * start removing bytes from the kfifo.
     */

    asm volatile("lfence" ::: "memory");

    /* first get the data from fifo->out until the end of the buffer */
    l = _min(len, fifo->size - (fifo->out & (fifo->size - 1)));
    memcpy(buffer, fifo->buffer + (fifo->out & (fifo->size - 1)), l);

    /* then get the rest (if any) from the beginning of the buffer */
    memcpy(buffer + l, fifo->buffer, len - l);

    /*
     * Ensure that we remove the bytes from the kfifo -before-
     * we update the fifo->out index.
     */

    asm volatile("mfence" ::: "memory");

    fifo->out += len;

    return len;
}

