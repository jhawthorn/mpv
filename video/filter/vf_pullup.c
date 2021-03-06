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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "core/mp_msg.h"
#include "core/cpudetect.h"

#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf.h"

#include "video/memcpy_pic.h"

#include "pullup.h"

#undef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))

struct vf_priv_s {
	struct pullup_context *ctx;
	int init;
	int fakecount;
	char *qbuf;
	double lastpts;
};

static void init_pullup(struct vf_instance *vf, mp_image_t *mpi)
{
	struct pullup_context *c = vf->priv->ctx;

		c->format = PULLUP_FMT_Y;
		c->nplanes = 4;
		pullup_preinit_context(c);
		c->bpp[0] = c->bpp[1] = c->bpp[2] = 8;
		c->w[0] = mpi->w;
		c->h[0] = mpi->h;
		c->w[1] = c->w[2] = mpi->chroma_width;
		c->h[1] = c->h[2] = mpi->chroma_height;
		c->w[3] = ((mpi->w+15)/16) * ((mpi->h+15)/16);
		c->h[3] = 2;
		c->stride[0] = mpi->w;
		c->stride[1] = c->stride[2] = mpi->chroma_width;
		c->stride[3] = c->w[3];
		c->background[1] = c->background[2] = 128;

	if (gCpuCaps.hasMMX) c->cpu |= PULLUP_CPU_MMX;
	if (gCpuCaps.hasMMX2) c->cpu |= PULLUP_CPU_MMX2;
	if (gCpuCaps.hasSSE) c->cpu |= PULLUP_CPU_SSE;
	if (gCpuCaps.hasSSE2) c->cpu |= PULLUP_CPU_SSE2;

	pullup_init_context(c);

	vf->priv->init = 1;
	vf->priv->qbuf = malloc(c->w[3]);
}

static struct mp_image *filter(struct vf_instance *vf, struct mp_image *mpi)
{
	struct pullup_context *c = vf->priv->ctx;
	struct pullup_buffer *b;
	struct pullup_frame *f;
	int p;
	int i;
        double pts = mpi->pts;
        struct mp_image *dmpi = NULL;

	if (!vf->priv->init) init_pullup(vf, mpi);

	if (1) {
		b = pullup_get_buffer(c, 2);
		if (!b) {
			mp_msg(MSGT_VFILTER,MSGL_ERR,"Could not get buffer from pullup!\n");
			f = pullup_get_frame(c);
			pullup_release_frame(f);
			goto skip;
		}
		memcpy_pic(b->planes[0], mpi->planes[0], mpi->w, mpi->h,
			c->stride[0], mpi->stride[0]);
			memcpy_pic(b->planes[1], mpi->planes[1],
				mpi->chroma_width, mpi->chroma_height,
				c->stride[1], mpi->stride[1]);
			memcpy_pic(b->planes[2], mpi->planes[2],
				mpi->chroma_width, mpi->chroma_height,
				c->stride[2], mpi->stride[2]);
	}
	if (mpi->qscale) {
		memcpy(b->planes[3], mpi->qscale, c->w[3]);
		memcpy(b->planes[3]+c->w[3], mpi->qscale, c->w[3]);
	}

	p = mpi->fields & MP_IMGFIELD_TOP_FIRST ? 0 :
		(mpi->fields & MP_IMGFIELD_ORDERED ? 1 : 0);

	if (pts == MP_NOPTS_VALUE) {
		pullup_submit_field(c, b, p, MP_NOPTS_VALUE);
		pullup_submit_field(c, b, p^1, MP_NOPTS_VALUE);
		if (mpi->fields & MP_IMGFIELD_REPEAT_FIRST)
			pullup_submit_field(c, b, p, MP_NOPTS_VALUE);
	} else {
		double delta;
		if (vf->priv->lastpts == MP_NOPTS_VALUE)
			delta = 1001.0/60000.0; // delta = field time distance
		else
			delta = (pts - vf->priv->lastpts) / 2;
		if (delta <= 0.0 || delta >= 0.5)
			delta = 0.0;
		vf->priv->lastpts = pts;
		if (mpi->fields & MP_IMGFIELD_REPEAT_FIRST) {
			pullup_submit_field(c, b, p, pts - delta);
			pullup_submit_field(c, b, p^1, pts);
			pullup_submit_field(c, b, p, pts + delta);
		} else {
			pullup_submit_field(c, b, p, pts - delta * 0.5);
			pullup_submit_field(c, b, p^1, pts + delta * 0.5);
		}
	}

	pullup_release_buffer(b, 2);

	f = pullup_get_frame(c);

	/* Fake yes for first few frames (buffer depth) to keep from
	 * breaking A/V sync with G1's bad architecture... */
	//if (!f) return vf->priv->fakecount ? (--vf->priv->fakecount,1) : 0;
	if (!f)
            goto skip;

	if (f->length < 2) {
		pullup_release_frame(f);
		f = pullup_get_frame(c);
		if (!f) goto skip;
		if (f->length < 2) {
			pullup_release_frame(f);
			if (!(mpi->fields & MP_IMGFIELD_REPEAT_FIRST))
				goto skip;
			f = pullup_get_frame(c);
			if (!f) goto skip;
			if (f->length < 2) {
				pullup_release_frame(f);
				goto skip;
			}
		}
	}

#if 0
	/* Average qscale tables from both frames. */
	if (mpi->qscale) {
		for (i=0; i<c->w[3]; i++) {
			vf->priv->qbuf[i] = (f->ofields[0]->planes[3][i]
				+ f->ofields[1]->planes[3][i+c->w[3]])>>1;
		}
	}
#else
	/* Take worst of qscale tables from both frames. */
	if (mpi->qscale) {
		for (i=0; i<c->w[3]; i++) {
			vf->priv->qbuf[i] = MAX(f->ofields[0]->planes[3][i], f->ofields[1]->planes[3][i+c->w[3]]);
		}
	}
#endif

        /* If the frame isn't already exportable... */
        if (!f->buffer)
            pullup_pack_frame(c, f);

        // NOTE: the copy could probably be avoided by changing or using the
        //       pullup internal buffer management. But right now just do the
        //       safe thing and always copy. Code outside the filter might
        //       hold a buffer reference even if the filter chain is destroyed.
        dmpi = vf_alloc_out_image(vf);
        mp_image_copy_attributes(dmpi, mpi);

        struct mp_image data = *dmpi;

	data.planes[0] = f->buffer->planes[0];
	data.planes[1] = f->buffer->planes[1];
	data.planes[2] = f->buffer->planes[2];

	data.stride[0] = c->stride[0];
	data.stride[1] = c->stride[1];
	data.stride[2] = c->stride[2];

        mp_image_copy(dmpi, &data);

        dmpi->pts = f->pts;

        // Warning: entirely bogus memory management of qscale
	if (mpi->qscale) {
		dmpi->qscale = vf->priv->qbuf;
		dmpi->qstride = mpi->qstride;
		dmpi->qscale_type = mpi->qscale_type;
	}
	pullup_release_frame(f);

skip:
        talloc_free(mpi);
	return dmpi;
}

static int query_format(struct vf_instance *vf, unsigned int fmt)
{
	/* FIXME - support more formats */
	switch (fmt) {
	case IMGFMT_420P:
		return vf_next_query_format(vf, fmt);
	}
	return 0;
}

static int config(struct vf_instance *vf,
        int width, int height, int d_width, int d_height,
	unsigned int flags, unsigned int outfmt)
{
	if (height&3) return 0;
	return vf_next_config(vf, width, height, d_width, d_height, flags, outfmt);
}

static void uninit(struct vf_instance *vf)
{
	pullup_free_context(vf->priv->ctx);
	free(vf->priv);
}

static int vf_open(vf_instance_t *vf, char *args)
{
	struct vf_priv_s *p;
	struct pullup_context *c;
	vf->filter = filter;
	vf->config = config;
	vf->query_format = query_format;
	vf->uninit = uninit;
	vf->priv = p = calloc(1, sizeof(struct vf_priv_s));
	p->ctx = c = pullup_alloc_context();
	p->fakecount = 1;
	c->verbose = verbose>0;
	c->junk_left = c->junk_right = 1;
	c->junk_top = c->junk_bottom = 4;
	c->strict_breaks = 0;
	c->metric_plane = 0;
	if (args) {
		sscanf(args, "%d:%d:%d:%d:%d:%d", &c->junk_left, &c->junk_right, &c->junk_top, &c->junk_bottom, &c->strict_breaks, &c->metric_plane);
	}
	return 1;
}

const vf_info_t vf_info_pullup = {
    "pullup (from field sequence to frames)",
    "pullup",
    "Rich Felker",
    "",
    vf_open,
    NULL
};
