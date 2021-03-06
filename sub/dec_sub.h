#ifndef MPLAYER_DEC_SUB_H
#define MPLAYER_DEC_SUB_H

#include <stdbool.h>
#include <stdint.h>

#include "sub/sub.h"

struct sh_sub;
struct ass_track;
struct MPOpts;
struct demux_packet;
struct ass_library;
struct ass_renderer;

struct dec_sub;
struct sd;

struct dec_sub *sub_create(struct MPOpts *opts);
void sub_destroy(struct dec_sub *sub);

void sub_set_video_res(struct dec_sub *sub, int w, int h);
void sub_set_extradata(struct dec_sub *sub, void *data, int data_len);
void sub_set_ass_renderer(struct dec_sub *sub, struct ass_library *ass_library,
                          struct ass_renderer *ass_renderer);
void sub_init_from_sh(struct dec_sub *sub, struct sh_sub *sh);

bool sub_is_initialized(struct dec_sub *sub);

bool sub_accept_packets_in_advance(struct dec_sub *sub);
void sub_decode(struct dec_sub *sub, struct demux_packet *packet);
void sub_get_bitmaps(struct dec_sub *sub, struct mp_osd_res dim, double pts,
                     struct sub_bitmaps *res);
bool sub_has_get_text(struct dec_sub *sub);
char *sub_get_text(struct dec_sub *sub, double pts);
void sub_reset(struct dec_sub *sub);

struct sd *sub_get_last_sd(struct dec_sub *sub);

#ifdef CONFIG_ASS
struct ass_track *sub_get_ass_track(struct dec_sub *sub);
#endif

#endif
