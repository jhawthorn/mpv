/*
 * This file is part of mpv.
 * Copyright (c) 2012 wm4
 * Copyright (c) 2013 Stefano Pigozzi <stefano.pigozzi@gmail.com>
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MPV_MP_RING_H
#define MPV_MP_RING_H

/**
 * @file
 * A simple non-blocking SPSC (single producer, single consumer) ringbuffer
 * implementation. Thread safety is accomplished through atomic operations.
 */

struct mp_ring;

/**
 * Instantiate a new ringbuffer
 *
 * @param talloc_ctx talloc context of the newly created object
 * @param size total size in bytes
 * @return the newly created ringbuffer
 */
struct mp_ring *mp_ring_new(void *talloc_ctx, int size);

/**
 * Instantiate a new ringbuffer
 *
 * @param talloc_ctx talloc context of the newly created object
 * @param elements number of elements
 * @param element_size size of each element in bytes
 * @return the newly created ringbuffer
 * @note This delegates most of the work to `mp_ring_new`
 */
struct mp_ring *mp_ring_new2(void *talloc_ctx, int elements, int element_size);

/**
 * Instantiate a new ringbuffer
 *
 * @param talloc_ctx talloc context of the newly created object
 * @param bps bytes per second
 * @param element_size size of each element in bytes
 * @return the newly created ringbuffer
 * @note This delegates most of the work to `mp_ring_new2`
 */
struct mp_ring *mp_ring_new3(void *talloc_ctx, int bps, int element_size);

/**
 * Read data from the ringbuffer
 *
 * @param buffer target ringbuffer instance
 * @param data buffer to write the read data to
 * @param len length of `data` buffer in bytes
 * @return amount of bytes read into dest buffer
 */
int mp_ring_read(struct mp_ring *buffer, unsigned char *dest, int len);

/**
 * Read data from the ringbuffer
 *
 * @param buffer target ringbuffer instance
 * @param ctx context for the callback function
 * @param len length of `data` buffer in bytes
 * @param func generic read function to customize reading behaviour
 * @return amount of bytes read
 * @note this function behaves similarly to `av_fifo_generic_read` and was 
 *       actually added for compatibility with code that was written for it.
 */
int mp_ring_read2(struct mp_ring *buffer, void *ctx, int len,
        void (*func)(void*, void*, int));

/**
 * Write data to the ringbuffer
 *
 * @param buffer target ringbuffer instance
 * @param src buffer to read the read data from
 * @param len length of `data` buffer in bytes
 * @return amount of bytes written into the ringbuffer instance
 */
int mp_ring_write(struct mp_ring *buffer, unsigned char *src, int len);

/**
 * Drains data from the ringbuffer
 *
 * @param buffer target ringbuffer instance
 * @param len maximum amount of data to drain in bytes
 * @return amount of bytes drained from the ringbuffer
 */
int mp_ring_drain(struct mp_ring *buffer, int len);

/**
 * Reset the ringbuffer discarding any content
 *
 * @param buffer target ringbuffer instance
 */
void mp_ring_reset(struct mp_ring *buffer);

/**
 * Get the available size for writing
 *
 * @param buffer target ringbuffer instance
 * @return amount of bytes in the ringbuffer that are free to be written
 */
int mp_ring_available(struct mp_ring *buffer);

/**
 * Get the total size
 *
 * @param buffer target ringbuffer instance
 * @return total amount of bytes in the ringbuffer
 */
int mp_ring_size(struct mp_ring *buffer);

/**
 * Get the available size for reading
 *
 * @param buffer target ringbuffer instance
 * @return amount of bytes in the ringbuffer that are ready to be read
 */
int mp_ring_buffered(struct mp_ring *buffer);

/**
 * Get a string representation of the ringbuffer
 *
 * @param buffer target ringbuffer instance
 * @param talloc_ctx talloc context of the newly created string
 * @return string representing the ringbuffer
 */
char *mp_ring_repr(struct mp_ring *buffer, void *talloc_ctx);

#endif
