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

// Source https://github.com/spotify/linux/blob/master/include/linux/kfifo.h
// Modified By wujiaxu <void00@foxmail.com>

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef _OVERLAYBD_KFIFO_H
#define _OVERLAYBD_KFIFO_H

#include <pthread.h>

struct kfifo {
    void *buffer;             /* the buffer holding the data */
    unsigned int size;        /* the size of the allocated buffer */
    unsigned int in;          /* data is added at offset (in % size) */
    unsigned int out;         /* data is extracted from off. (out % size) */
    pthread_spinlock_t *lock; /* protects concurrent modifications */
};

extern struct kfifo *kfifo_alloc(unsigned int size);
extern void kfifo_free(struct kfifo *fifo);
extern unsigned int __kfifo_put(struct kfifo *fifo,
                const void *buffer, unsigned int len);
extern unsigned int __kfifo_get(struct kfifo *fifo,
                void *buffer, unsigned int len);

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
    pthread_spin_lock(fifo->lock);

    __kfifo_reset(fifo);

    pthread_spin_unlock(fifo->lock);
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

    pthread_spin_lock(fifo->lock);

    ret = __kfifo_put(fifo, buffer, len);

    pthread_spin_unlock(fifo->lock);

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

    pthread_spin_lock(fifo->lock);

    ret = __kfifo_get(fifo, buffer, len);

    /*
     * optimization: if the FIFO is empty, set the indices to 0
     * so we don't wrap the next time
     */
    if (fifo->in == fifo->out)
        fifo->in = fifo->out = 0;

    pthread_spin_unlock(fifo->lock);

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

    pthread_spin_lock(fifo->lock);

    ret = __kfifo_len(fifo);

    pthread_spin_unlock(fifo->lock);

    return ret;
}

#endif

#if defined (__cplusplus)
}
#endif

