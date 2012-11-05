/*
 * X11 Xv interface
 *
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
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <libavutil/common.h>

#include "config.h"

#ifdef HAVE_SHM
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#endif

// Note: depends on the inclusion of X11/extensions/XShm.h
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>

#include "options.h"
#include "talloc.h"
#include "mp_msg.h"
#include "video_out.h"
#include "libmpcodecs/vfcap.h"
#include "libmpcodecs/mp_image.h"
#include "x11_common.h"
#include "fastmemcpy.h"
#include "sub/sub.h"
#include "aspect.h"
#include "csputils.h"
#include "subopt-helper.h"

static const vo_info_t info = {
    "X11/Xv",
    "xv",
    "Gerd Knorr <kraxel@goldbach.in-berlin.de> and others",
    ""
};

struct xvctx {
    XvAdaptorInfo *ai;
    XvImageFormatValues *fo;
    unsigned int formats, adaptors, xv_format;
    int current_buf;
    int current_ip_buf;
    int num_buffers;
    int total_buffers;
    bool have_image_copy;
    bool unchanged_image;
    int visible_buf;
    XvImage *xvimage[2 + 1];
    uint32_t image_width;
    uint32_t image_height;
    uint32_t image_format;
    struct mp_csp_details cached_csp;
    int is_paused;
    struct mp_rect src_rect;
    struct mp_rect dst_rect;
    uint32_t max_width, max_height; // zero means: not set
    int mode_switched;
#ifdef HAVE_SHM
    XShmSegmentInfo Shminfo[2 + 1];
    int Shmem_Flag;
#endif
};

static void allocate_xvimage(struct vo *, int);
static void deallocate_xvimage(struct vo *vo, int foo);

static void read_xv_csp(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;
    struct vo_x11_state *x11 = vo->x11;
    struct mp_csp_details *cspc = &ctx->cached_csp;
    *cspc = (struct mp_csp_details) MP_CSP_DETAILS_DEFAULTS;
    int bt709_enabled;
    if (vo_xv_get_eq(vo, x11->xv_port, "bt_709", &bt709_enabled))
        cspc->format = bt709_enabled == 100 ? MP_CSP_BT_709 : MP_CSP_BT_601;
}

static void resize(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;

    // Can't be used, because the function calculates screen-space coordinates,
    // while we need video-space.
    struct mp_osd_res unused;

    vo_get_src_dst_rects(vo, &ctx->src_rect, &ctx->dst_rect, &unused);

    struct mp_rect *dst = &ctx->dst_rect;
    int dw = dst->x1 - dst->x0, dh = dst->y1 - dst->y0;
    vo_x11_clearwindow_part(vo, vo->x11->window, dw, dh);
    vo_xv_draw_colorkey(vo, dst->x0, dst->y0, dw, dh);
    read_xv_csp(vo);
}

/*
 * connect to server, create and map window,
 * allocate colors and (shared) memory
 */
static int config(struct vo *vo, uint32_t width, uint32_t height,
                  uint32_t d_width, uint32_t d_height, uint32_t flags,
                  uint32_t format)
{
    struct vo_x11_state *x11 = vo->x11;
    XVisualInfo vinfo;
    XSetWindowAttributes xswa;
    XWindowAttributes attribs;
    unsigned long xswamask;
    int depth;
    struct xvctx *ctx = vo->priv;
    int i;

    ctx->image_height = height;
    ctx->image_width = width;
    ctx->image_format = format;

    if ((ctx->max_width != 0 && ctx->max_height != 0)
        && (ctx->image_width > ctx->max_width
            || ctx->image_height > ctx->max_height)) {
        mp_tmsg(MSGT_VO, MSGL_ERR, "Source image dimensions are too high: %ux%u (maximum is %ux%u)\n",
               ctx->image_width, ctx->image_height, ctx->max_width,
               ctx->max_height);
        return -1;
    }

    ctx->visible_buf = -1;
    ctx->have_image_copy = false;

    /* check image formats */
    ctx->xv_format = 0;
    for (i = 0; i < ctx->formats; i++) {
        mp_msg(MSGT_VO, MSGL_V, "Xvideo image format: 0x%x (%4.4s) %s\n",
               ctx->fo[i].id, (char *) &ctx->fo[i].id,
               (ctx->fo[i].format == XvPacked) ? "packed" : "planar");
        if (ctx->fo[i].id == format)
            ctx->xv_format = ctx->fo[i].id;
    }
    if (!ctx->xv_format)
        return -1;

    {
#ifdef CONFIG_XF86VM
        int vm = flags & VOFLAG_MODESWITCHING;
        if (vm) {
            vo_vm_switch(vo);
            ctx->mode_switched = 1;
        }
#endif
        XGetWindowAttributes(x11->display, DefaultRootWindow(x11->display),
                             &attribs);
        depth = attribs.depth;
        if (depth != 15 && depth != 16 && depth != 24 && depth != 32)
            depth = 24;
        XMatchVisualInfo(x11->display, x11->screen, depth, TrueColor, &vinfo);

        xswa.border_pixel = 0;
        xswamask = CWBorderPixel;
        if (x11->xv_ck_info.method == CK_METHOD_BACKGROUND) {
            xswa.background_pixel = x11->xv_colorkey;
            xswamask |= CWBackPixel;
        }

        vo_x11_create_vo_window(vo, &vinfo, vo->dx, vo->dy, vo->dwidth,
                                vo->dheight, flags, CopyFromParent, "xv");
        XChangeWindowAttributes(x11->display, x11->window, xswamask, &xswa);

#ifdef CONFIG_XF86VM
        if (vm) {
            /* Grab the mouse pointer in our window */
            if (vo_grabpointer)
                XGrabPointer(x11->display, x11->window, True, 0, GrabModeAsync,
                             GrabModeAsync, x11->window, None, CurrentTime);
            XSetInputFocus(x11->display, x11->window, RevertToNone,
                           CurrentTime);
        }
#endif
    }

    mp_msg(MSGT_VO, MSGL_V, "using Xvideo port %d for hw scaling\n",
           x11->xv_port);

    // In case config has been called before
    for (i = 0; i < ctx->total_buffers; i++)
        deallocate_xvimage(vo, i);

    ctx->num_buffers = 2;
    ctx->total_buffers = ctx->num_buffers + 1;

    for (i = 0; i < ctx->total_buffers; i++)
        allocate_xvimage(vo, i);

    ctx->current_buf = 0;
    ctx->current_ip_buf = 0;


    resize(vo);

    return 0;
}

static void allocate_xvimage(struct vo *vo, int foo)
{
    struct xvctx *ctx = vo->priv;
    struct vo_x11_state *x11 = vo->x11;
    /*
     * allocate XvImages.  FIXME: no error checking, without
     * mit-shm this will bomb... trzing to fix ::atmos
     */
#ifdef HAVE_SHM
    if (x11->display_is_local && XShmQueryExtension(x11->display))
        ctx->Shmem_Flag = 1;
    else {
        ctx->Shmem_Flag = 0;
        mp_tmsg(MSGT_VO, MSGL_INFO, "[VO_XV] Shared memory not supported\nReverting to normal Xv.\n");
    }
    if (ctx->Shmem_Flag) {
        ctx->xvimage[foo] =
            (XvImage *) XvShmCreateImage(x11->display, x11->xv_port,
                                         ctx->xv_format, NULL,
                                         ctx->image_width, ctx->image_height,
                                         &ctx->Shminfo[foo]);

        ctx->Shminfo[foo].shmid = shmget(IPC_PRIVATE,
                                         ctx->xvimage[foo]->data_size,
                                         IPC_CREAT | 0777);
        ctx->Shminfo[foo].shmaddr = (char *) shmat(ctx->Shminfo[foo].shmid, 0,
                                                   0);
        ctx->Shminfo[foo].readOnly = False;

        ctx->xvimage[foo]->data = ctx->Shminfo[foo].shmaddr;
        XShmAttach(x11->display, &ctx->Shminfo[foo]);
        XSync(x11->display, False);
        shmctl(ctx->Shminfo[foo].shmid, IPC_RMID, 0);
    } else
#endif
    {
        ctx->xvimage[foo] =
            (XvImage *) XvCreateImage(x11->display, x11->xv_port,
                                      ctx->xv_format, NULL, ctx->image_width,
                                      ctx->image_height);
        ctx->xvimage[foo]->data = malloc(ctx->xvimage[foo]->data_size);
        XSync(x11->display, False);
    }
    memset(ctx->xvimage[foo]->data, 128, ctx->xvimage[foo]->data_size);
    return;
}

static void deallocate_xvimage(struct vo *vo, int foo)
{
    struct xvctx *ctx = vo->priv;
#ifdef HAVE_SHM
    if (ctx->Shmem_Flag) {
        XShmDetach(vo->x11->display, &ctx->Shminfo[foo]);
        shmdt(ctx->Shminfo[foo].shmaddr);
    } else
#endif
    {
        free(ctx->xvimage[foo]->data);
    }
    XFree(ctx->xvimage[foo]);

    XSync(vo->x11->display, False);
    return;
}

static inline void put_xvimage(struct vo *vo, XvImage *xvi)
{
    struct xvctx *ctx = vo->priv;
    struct vo_x11_state *x11 = vo->x11;
    struct mp_rect *src = &ctx->src_rect;
    struct mp_rect *dst = &ctx->dst_rect;
    int dw = dst->x1 - dst->x0, dh = dst->y1 - dst->y0;
    int sw = src->x1 - src->x0, sh = src->y1 - src->y0;
#ifdef HAVE_SHM
    if (ctx->Shmem_Flag) {
        XvShmPutImage(x11->display, x11->xv_port, x11->window, x11->vo_gc, xvi,
                      src->x0, src->y0, sw, sh,
                      dst->x0, dst->y0, dw, dh,
                      False);
    } else
#endif
    {
        XvPutImage(x11->display, x11->xv_port, x11->window, x11->vo_gc, xvi,
                   src->x0, src->y0, sw, sh,
                   dst->x0, dst->y0, dw, dh);
    }
}

static struct mp_image get_xv_buffer(struct vo *vo, int buf_index)
{
    struct xvctx *ctx = vo->priv;
    XvImage *xv_image = ctx->xvimage[buf_index];

    struct mp_image img = {0};
    img.w = img.width = xv_image->width;
    img.h = img.height = xv_image->height;
    mp_image_setfmt(&img, ctx->image_format);

    bool swapuv = ctx->image_format == IMGFMT_YV12;
    for (int n = 0; n < img.num_planes; n++) {
        int sn = n > 0 &&  swapuv ? (n == 1 ? 2 : 1) : n;
        img.planes[n] = xv_image->data + xv_image->offsets[sn];
        img.stride[n] = xv_image->pitches[sn];
    }

    mp_image_set_colorspace_details(&img, &ctx->cached_csp);

    return img;
}

static void copy_backup_image(struct vo *vo, int dest, int src)
{
    struct mp_image img_dest = get_xv_buffer(vo, dest);
    struct mp_image img_src = get_xv_buffer(vo, src);

    copy_mpi(&img_dest, &img_src);
}

static void check_events(struct vo *vo)
{
    int e = vo_x11_check_events(vo);

    if (e & VO_EVENT_EXPOSE || e & VO_EVENT_RESIZE) {
        resize(vo);
        vo->want_redraw = true;
    }
}

static void draw_osd(struct vo *vo, struct osd_state *osd)
{
    struct xvctx *ctx = vo->priv;

    struct mp_image img = get_xv_buffer(vo, ctx->current_buf);

    struct mp_rect *src = &ctx->src_rect;
    struct mp_rect *dst = &ctx->dst_rect;
    int dw = dst->x1 - dst->x0, dh = dst->y1 - dst->y0;
    int sw = src->x1 - src->x0, sh = src->y1 - src->y0;
    double xvpar = (double)dw / dh * sh / sw;

    struct mp_osd_res res = {
        .w = ctx->image_width,
        .h = ctx->image_height,
        .display_par = vo->monitor_par / xvpar,
        .video_par = vo->aspdat.par,
    };

    if (osd_draw_on_image(osd, res, osd->vo_pts, 0, &img))
        ctx->unchanged_image = false;
}

static int redraw_frame(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;

    if (ctx->have_image_copy)
        copy_backup_image(vo, ctx->visible_buf, ctx->num_buffers);
    else if (ctx->unchanged_image) {
        copy_backup_image(vo, ctx->num_buffers, ctx->visible_buf);
        ctx->have_image_copy = true;
    }  else
        return false;
    ctx->current_buf = ctx->visible_buf;
    return true;
}

static void flip_page(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;
    put_xvimage(vo, ctx->xvimage[ctx->current_buf]);

    /* remember the currently visible buffer */
    ctx->visible_buf = ctx->current_buf;

    ctx->current_buf = (ctx->current_buf + 1) % ctx->num_buffers;
    XFlush(vo->x11->display);
    return;
}

static int draw_slice(struct vo *vo, uint8_t *image[], int stride[], int w,
                      int h, int x, int y)
{
    struct xvctx *ctx = vo->priv;
    uint8_t *dst;
    XvImage *current_image = ctx->xvimage[ctx->current_buf];

    dst = current_image->data + current_image->offsets[0]
        + current_image->pitches[0] * y + x;
    memcpy_pic(dst, image[0], w, h, current_image->pitches[0], stride[0]);

    x /= 2;
    y /= 2;
    w /= 2;
    h /= 2;

    dst = current_image->data + current_image->offsets[1]
        + current_image->pitches[1] * y + x;
    if (ctx->image_format != IMGFMT_YV12)
        memcpy_pic(dst, image[1], w, h, current_image->pitches[1], stride[1]);
    else
        memcpy_pic(dst, image[2], w, h, current_image->pitches[1], stride[2]);

    dst = current_image->data + current_image->offsets[2]
        + current_image->pitches[2] * y + x;
    if (ctx->image_format == IMGFMT_YV12)
        memcpy_pic(dst, image[1], w, h, current_image->pitches[1], stride[1]);
    else
        memcpy_pic(dst, image[2], w, h, current_image->pitches[1], stride[2]);

    return 0;
}

static mp_image_t *get_screenshot(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;

    // try to get an image without OSD
    int id = ctx->have_image_copy ? ctx->num_buffers : ctx->visible_buf;
    struct mp_image img = get_xv_buffer(vo, id);
    img.display_w = vo->aspdat.prew;
    img.display_h = vo->aspdat.preh;

    return talloc_memdup(NULL, &img, sizeof(img));
}

static uint32_t draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct xvctx *ctx = vo->priv;

    ctx->have_image_copy = false;

    if (mpi->flags & MP_IMGFLAG_DRAW_CALLBACK)
        ; // done
    else if (mpi->flags & MP_IMGFLAG_PLANAR)
        draw_slice(vo, mpi->planes, mpi->stride, mpi->w, mpi->h, 0, 0);
    else if (mpi->flags & MP_IMGFLAG_YUV)
        // packed YUV:
        memcpy_pic(ctx->xvimage[ctx->current_buf]->data +
                   ctx->xvimage[ctx->current_buf]->offsets[0], mpi->planes[0],
                   mpi->w * (mpi->bpp / 8), mpi->h,
                   ctx->xvimage[ctx->current_buf]->pitches[0], mpi->stride[0]);
    else
          return false;

    if (ctx->is_paused) {
        copy_backup_image(vo, ctx->num_buffers, ctx->current_buf);
        ctx->have_image_copy = true;
    }
    ctx->unchanged_image = true;
    return true;
}

static int query_format(struct xvctx *ctx, uint32_t format)
{
    uint32_t i;
    int flag = VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW | VFCAP_HWSCALE_UP | VFCAP_HWSCALE_DOWN | VFCAP_OSD | VFCAP_ACCEPT_STRIDE;       // FIXME! check for DOWN

    /* check image formats */
    for (i = 0; i < ctx->formats; i++) {
        if (ctx->fo[i].id == format)
            return flag;        //xv_format = fo[i].id;
    }
    return 0;
}

static void uninit(struct vo *vo)
{
    struct xvctx *ctx = vo->priv;
    int i;

    ctx->visible_buf = -1;
    if (ctx->ai)
        XvFreeAdaptorInfo(ctx->ai);
    ctx->ai = NULL;
    if (ctx->fo) {
        XFree(ctx->fo);
        ctx->fo = NULL;
    }
    for (i = 0; i < ctx->total_buffers; i++)
        deallocate_xvimage(vo, i);
#ifdef CONFIG_XF86VM
    if (ctx->mode_switched)
        vo_vm_close(vo);
#endif
    // uninit() shouldn't get called unless initialization went past vo_init()
    vo_x11_uninit(vo);
}

static int preinit(struct vo *vo, const char *arg)
{
    XvPortID xv_p;
    int busy_ports = 0;
    unsigned int i;
    strarg_t ck_src_arg = { 0, NULL };
    strarg_t ck_method_arg = { 0, NULL };
    struct xvctx *ctx = talloc_zero(vo, struct xvctx);
    vo->priv = ctx;
    int xv_adaptor = -1;

    if (!vo_init(vo))
        return -1;

    struct vo_x11_state *x11 = vo->x11;

    const opt_t subopts[] =
    {
      /* name         arg type     arg var         test */
      {  "port",      OPT_ARG_INT, &x11->xv_port,  int_pos },
      {  "adaptor",   OPT_ARG_INT, &xv_adaptor,    int_non_neg },
      {  "ck",        OPT_ARG_STR, &ck_src_arg,    xv_test_ck },
      {  "ck-method", OPT_ARG_STR, &ck_method_arg, xv_test_ckm },
      {  NULL }
    };

    x11->xv_port = 0;

    /* parse suboptions */
    if (subopt_parse(arg, subopts) != 0) {
        return -1;
    }

    /* modify colorkey settings according to the given options */
    xv_setup_colorkeyhandling(vo, ck_method_arg.str, ck_src_arg.str);

    /* check for Xvideo extension */
    unsigned int ver, rel, req, ev, err;
    if (Success != XvQueryExtension(x11->display, &ver, &rel, &req, &ev, &err)) {
        mp_tmsg(MSGT_VO, MSGL_ERR, "[VO_XV] Sorry, Xv not supported by this X11 version/driver\n[VO_XV] ******** Try with  -vo x11 *********\n");
        goto error;
    }

    /* check for Xvideo support */
    if (Success !=
        XvQueryAdaptors(x11->display, DefaultRootWindow(x11->display),
                        &ctx->adaptors, &ctx->ai)) {
        mp_tmsg(MSGT_VO, MSGL_ERR, "[VO_XV] XvQueryAdaptors failed.\n");
        goto error;
    }

    /* check adaptors */
    if (x11->xv_port) {
        int port_found;

        for (port_found = 0, i = 0; !port_found && i < ctx->adaptors; i++) {
            if ((ctx->ai[i].type & XvInputMask)
                && (ctx->ai[i].type & XvImageMask)) {
                for (xv_p = ctx->ai[i].base_id;
                     xv_p < ctx->ai[i].base_id + ctx->ai[i].num_ports;
                     ++xv_p) {
                    if (xv_p == x11->xv_port) {
                        port_found = 1;
                        break;
                    }
                }
            }
        }
        if (port_found) {
            if (XvGrabPort(x11->display, x11->xv_port, CurrentTime))
                x11->xv_port = 0;
        } else {
            mp_tmsg(MSGT_VO, MSGL_WARN, "[VO_XV] Invalid port parameter, overriding with port 0.\n");
            x11->xv_port = 0;
        }
    }

    for (i = 0; i < ctx->adaptors && x11->xv_port == 0; i++) {
        /* check if adaptor number has been specified */
        if (xv_adaptor != -1 && xv_adaptor != i)
            continue;

        if ((ctx->ai[i].type & XvInputMask) && (ctx->ai[i].type & XvImageMask)) {
            for (xv_p = ctx->ai[i].base_id;
                 xv_p < ctx->ai[i].base_id + ctx->ai[i].num_ports; ++xv_p)
                if (!XvGrabPort(x11->display, xv_p, CurrentTime)) {
                    x11->xv_port = xv_p;
                    mp_msg(MSGT_VO, MSGL_V,
                           "[VO_XV] Using Xv Adapter #%d (%s)\n",
                           i, ctx->ai[i].name);
                    break;
                } else {
                    mp_tmsg(MSGT_VO, MSGL_WARN, "[VO_XV] Could not grab port %i.\n",
                           (int) xv_p);
                    ++busy_ports;
                }
        }
    }
    if (!x11->xv_port) {
        if (busy_ports)
            mp_tmsg(MSGT_VO, MSGL_ERR,
                "[VO_XV] Could not find free Xvideo port - maybe another process is already\n"\
                "[VO_XV] using it. Close all video applications, and try again. If that does\n"\
                "[VO_XV] not help, see 'mpv -vo help' for other (non-xv) video out drivers.\n");
        else
            mp_tmsg(MSGT_VO, MSGL_ERR,
                "[VO_XV] It seems there is no Xvideo support for your video card available.\n"\
                "[VO_XV] Run 'xvinfo' to verify its Xv support and read\n"\
                "[VO_XV] DOCS/HTML/en/video.html#xv!\n"\
                "[VO_XV] See 'mpv -vo help' for other (non-xv) video out drivers.\n"\
                "[VO_XV] Try -vo x11.\n");
        goto error;
    }

    if (!vo_xv_init_colorkey(vo)) {
        goto error;             // bail out, colorkey setup failed
    }
    vo_xv_enable_vsync(vo);
    vo_xv_get_max_img_dim(vo, &ctx->max_width, &ctx->max_height);

    ctx->fo = XvListImageFormats(x11->display, x11->xv_port,
                                 (int *) &ctx->formats);

    return 0;

  error:
    uninit(vo);                 // free resources
    return -1;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct xvctx *ctx = vo->priv;
    struct vo_x11_state *x11 = vo->x11;
    switch (request) {
    case VOCTRL_PAUSE:
        return (ctx->is_paused = 1);
    case VOCTRL_RESUME:
        return (ctx->is_paused = 0);
    case VOCTRL_QUERY_FORMAT:
        return query_format(ctx, *((uint32_t *) data));
    case VOCTRL_DRAW_IMAGE:
        return draw_image(vo, data);
    case VOCTRL_GET_PANSCAN:
        return VO_TRUE;
    case VOCTRL_FULLSCREEN:
        vo_x11_fullscreen(vo);
        /* indended, fallthrough to update panscan on fullscreen/windowed switch */
    case VOCTRL_SET_PANSCAN:
        resize(vo);
        return VO_TRUE;
    case VOCTRL_SET_EQUALIZER: {
        vo->want_redraw = true;
        struct voctrl_set_equalizer_args *args = data;
        return vo_xv_set_eq(vo, x11->xv_port, args->name, args->value);
    }
    case VOCTRL_GET_EQUALIZER: {
        struct voctrl_get_equalizer_args *args = data;
        return vo_xv_get_eq(vo, x11->xv_port, args->name, args->valueptr);
    }
    case VOCTRL_SET_YUV_COLORSPACE:;
        struct mp_csp_details* given_cspc = data;
        int is_709 = given_cspc->format == MP_CSP_BT_709;
        vo_xv_set_eq(vo, x11->xv_port, "bt_709", is_709 * 200 - 100);
        read_xv_csp(vo);
        vo->want_redraw = true;
        return true;
    case VOCTRL_GET_YUV_COLORSPACE:;
        struct mp_csp_details* cspc = data;
        read_xv_csp(vo);
        *cspc = ctx->cached_csp;
        return true;
    case VOCTRL_ONTOP:
        vo_x11_ontop(vo);
        return VO_TRUE;
    case VOCTRL_UPDATE_SCREENINFO:
        update_xinerama_info(vo);
        return VO_TRUE;
    case VOCTRL_REDRAW_FRAME:
        return redraw_frame(vo);
    case VOCTRL_SCREENSHOT: {
        struct voctrl_screenshot_args *args = data;
        args->out_image = get_screenshot(vo);
        args->has_osd = !ctx->have_image_copy;
        return true;
    }
    }
    return VO_NOTIMPL;
}

const struct vo_driver video_out_xv = {
    .is_new = 1,
    .info = &info,
    .preinit = preinit,
    .config = config,
    .control = control,
    .draw_slice = draw_slice,
    .draw_osd = draw_osd,
    .flip_page = flip_page,
    .check_events = check_events,
    .uninit = uninit
};