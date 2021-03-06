/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_MP_FIFO_H
#define MPLAYER_MP_FIFO_H

#include "core/bstr.h"

struct mp_fifo;
void mplayer_put_key(struct mp_fifo *fifo, int code);
void mplayer_put_key_utf8(struct mp_fifo *fifo, int mods, struct bstr code);
// Can be freed with talloc_free()
struct input_ctx;
struct MPOpts;
struct mp_fifo *mp_fifo_create(struct input_ctx *input, struct MPOpts *opts);

#endif /* MPLAYER_MP_FIFO_H */
