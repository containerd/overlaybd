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

// Source https://github.com/torvalds/linux/blob/v2.6.32/include/linux/kfifo.h
// And    https://github.com/torvalds/linux/blob/v2.6.32/kernel/kfifo.c
// Modified By wujiaxu <void00@foxmail.com>

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef _KFIFO_H_62
#define _KFIFO_H_62

#include <pthread.h>
#include <memory.h>
#include <stdlib.h>

struct kfifo {
    pthread_spinlock_t lock; /* protects concurrent modifications */
    void *buffer;            /* the buffer holding the data */
    unsigned int size;       /* the size of the allocated buffer */
    unsigned int in;         /* data is added at offset (in % size) */
    unsigned int out;        /* data is extracted from off. (out % size) */
};

static struct kfifo *kfifo_alloc(unsigned int size);
static void kfifo_free(struct kfifo *fifo);

// lock-free
static void __kfifo_reset(struct kfifo *fifo);
static unsigned int __kfifo_len(const struct kfifo *fifo);
static unsigned int __kfifo_get(struct kfifo *fifo,
                                void *buffer, unsigned int len);
static unsigned int __kfifo_put(struct kfifo *fifo,
                                const void *buffer, unsigned int len);

// thread-safety by spin-lock
static void kfifo_reset(struct kfifo *fifo);
static unsigned int kfifo_len(struct kfifo *fifo);
static unsigned int kfifo_get(struct kfifo *fifo,
                              void *buffer, unsigned int len);
static unsigned int kfifo_put(struct kfifo *fifo,
                              const void *buffer, unsigned int len);

/**
 * __kfifo_reset - removes the entire FIFO contents, no locking version
 * @fifo: the fifo to be emptied.
 */
static inline void __kfifo_reset(struct kfifo *fifo)
{
    fifo->in = fifo->out = 0;
}

/**
 * kfifo_reset - removes the entire FIFO contents
 * @fifo: the fifo to be emptied.
 */
static inline void kfifo_reset(struct kfifo *fifo)
{
    pthread_spin_lock(&fifo->lock);

    __kfifo_reset(fifo);

    pthread_spin_unlock(&fifo->lock);
}

/**
 * kfifo_put - puts some data into the FIFO
 * @fifo: the fifo to be used.
 * @buffer: the data to be added.
 * @len: the length of the data to be added.
 *
 * This function copies at most @len bytes from the @buffer into
 * the FIFO depending on the free space, and returns the number of
 * bytes copied.
 */
static inline unsigned int kfifo_put(struct kfifo *fifo,
                const void *buffer, unsigned int len)
{
    unsigned int ret;

    pthread_spin_lock(&fifo->lock);

    ret = __kfifo_put(fifo, buffer, len);

    pthread_spin_unlock(&fifo->lock);

    return ret;
}

/**
 * kfifo_get - gets some data from the FIFO
 * @fifo: the fifo to be used.
 * @buffer: where the data must be copied.
 * @len: the size of the destination buffer.
 *
 * This function copies at most @len bytes from the FIFO into the
 * @buffer and returns the number of copied bytes.
 */
static inline unsigned int kfifo_get(struct kfifo *fifo,
                     void *buffer, unsigned int len)
{
    unsigned int ret;

    pthread_spin_lock(&fifo->lock);

    ret = __kfifo_get(fifo, buffer, len);

    /*
     * optimization: if the FIFO is empty, set the indices to 0
     * so we don't wrap the next time
     */
    if (fifo->in == fifo->out)
        fifo->in = fifo->out = 0;

    pthread_spin_unlock(&fifo->lock);

    return ret;
}

/**
 * __kfifo_len - returns the number of bytes available in the FIFO, no locking version
 * @fifo: the fifo to be used.
 */
static inline unsigned int __kfifo_len(const struct kfifo *fifo)
{
    return fifo->in - fifo->out;
}

/**
 * kfifo_len - returns the number of bytes available in the FIFO
 * @fifo: the fifo to be used.
 */
static inline unsigned int kfifo_len(struct kfifo *fifo)
{
    unsigned int ret;

    pthread_spin_lock(&fifo->lock);

    ret = __kfifo_len(fifo);

    pthread_spin_unlock(&fifo->lock);

    return ret;
}


static inline unsigned int _min(unsigned int a, unsigned int b)
{
    return (a < b) ? a : b;
}

/**
 * kfifo_alloc - allocates a new FIFO and its internal buffer
 * @size: the size of the internal buffer to be allocated.
 *
 * size MUST be a power of 2
 */
static inline struct kfifo *kfifo_alloc(unsigned int size)
{
    void *buffer;
    struct kfifo *fifo;

    /* size must be a power of 2 */
    if ((size < 2) || (size & (size - 1)))
        return NULL;

    buffer = malloc(size);
    if (!buffer)
        return NULL;

    fifo = (struct kfifo *)malloc(sizeof(struct kfifo));
    if (!fifo)
        return NULL;

    fifo->buffer = buffer;
    fifo->size = size;
    fifo->in = fifo->out = 0;

    if (pthread_spin_init(&fifo->lock, PTHREAD_PROCESS_PRIVATE) != 0)
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
static inline void kfifo_free(struct kfifo *fifo)
{
    pthread_spin_destroy(&fifo->lock);
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
static inline unsigned int __kfifo_put(struct kfifo *fifo,
                                       const void *buffer,
                                       unsigned int len)
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
    memcpy((char *)fifo->buffer + (fifo->in & (fifo->size - 1)), buffer, l);

    /* then put the rest (if any) at the beginning of the buffer */
    memcpy(fifo->buffer, (char *)buffer + l, len - l);

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
static inline unsigned int __kfifo_get(struct kfifo *fifo,
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
    memcpy(buffer, (char *)fifo->buffer + (fifo->out & (fifo->size - 1)), l);

    /* then get the rest (if any) from the beginning of the buffer */
    memcpy((char *)buffer + l, fifo->buffer, len - l);

    /*
     * Ensure that we remove the bytes from the kfifo -before-
     * we update the fifo->out index.
     */

    asm volatile("mfence" ::: "memory");

    fifo->out += len;

    return len;
}

#endif

#if defined (__cplusplus)
}
#endif
