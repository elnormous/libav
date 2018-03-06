/*
 * Copyright (c) 2010 Stefano Sabatini
 * Copyright (c) 2006 Ivo van Poorten
 * Copyright (c) 2006 Julian Hall
 * Copyright (c) 2002-2003 Brian J. Murrell
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Libav; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * Search for black frames to detect scene transitions.
 * Ported from MPlayer libmpcodecs/vf_blackframe.c.
 */

#include <stdio.h>
#include <inttypes.h>

#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "libavutil/enc_connection.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#include <emmintrin.h>
#include <smmintrin.h>

typedef struct BlackFrameContext {
    const AVClass *class;
    int bamount;          ///< black amount
    int bthresh;          ///< black threshold
    unsigned int frame;   ///< frame number
    unsigned int nblack;  ///< number of black pixels counted so far
} BlackFrameContext;

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21, AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static uint64_t vectorised_count(uint8_t* img, int w, int linesize, int h, uint8_t threshold)
{
    uint64_t cnt = 0;
    int bi, i, x, stores;

    uint8_t* p = img;

    __m128i thresh = _mm_set1_epi8(threshold);
    __m128i maxVal = _mm_set1_epi8(255);
    __m128i thSub = _mm_set1_epi8(254);
    __m128i thAdd = _mm_subs_epi8(maxVal, thresh);
    __m128i zero = _mm_set1_epi8(0);
    __m128i tmpSum = _mm_setzero_si128();

    uint8_t buf[16];
    uint8_t tmp[16];

    for (i = 0; i < h; i++) {
        stores = 0;

        for (x = 0; x < w - 15; x += 16) {
            __m128i p1 = _mm_loadu_si128((__m128i*)&p[x]);
            __m128i ones = _mm_subs_epu8(_mm_adds_epu8(p1, thAdd), thSub);
            tmpSum += ones; //_mm_add_epi8(ones, tmpSum);

            stores++;
            if (stores >= 255) {
                stores = 0;
                _mm_storeu_si128((__m128i*)(&buf[0]), tmpSum);
                for (bi = 0; bi < 16; bi++) {
                    cnt += buf[bi];
                }
                tmpSum = zero;
            }
        }

        for (; x < w; x++) {
            cnt += p[x] >= threshold;
        }

        _mm_storeu_si128((__m128i*)(&buf[0]), tmpSum);
        for (bi = 0; bi < 16; bi++) {
            cnt += buf[bi];
        }
        tmpSum = zero;

        p += linesize;
    }

    return h * w - cnt;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    BlackFrameContext *s = ctx->priv;
    int pblack = 0;

// original counting code
//    int x, i;
//    uint8_t *p = frame->data[0];
//
//    for (i = 0; i < frame->height; i++) {
//        for (x = 0; x < inlink->w; x++)
//            s->nblack += p[x] < s->bthresh;
//        p += frame->linesize[0];
//    }

    s->nblack = vectorised_count(frame->data[0], inlink->w, frame->linesize[0], frame->height, s->bthresh);

    pblack = s->nblack * 100 / (inlink->w * inlink->h);
    if (pblack >= s->bamount) {
        char msg[250];
        sprintf(msg, "frame:%u pblack:%u pts:%"PRId64" t:%f", s->frame, pblack, frame->pts,
                frame->pts == AV_NOPTS_VALUE ? -1 : frame->pts * av_q2d(inlink->time_base));

        av_log(ctx, AV_LOG_INFO, "frame:%u pblack:%u pts:%"PRId64" t:%f\n", s->frame, pblack, frame->pts,
               frame->pts == AV_NOPTS_VALUE ? -1 : frame->pts * av_q2d(inlink->time_base));

        enc_send(ENC_MSG_BLACK_FRAME, msg);
    }

    s->frame++;
    s->nblack = 0;
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(BlackFrameContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "amount", "Percentage of the pixels that have to be below the threshold "
        "for the frame to be considered black.", OFFSET(bamount), AV_OPT_TYPE_INT, { .i64 = 98 }, 0, 100,     FLAGS },
    { "threshold", "threshold below which a pixel value is considered black",
                                                 OFFSET(bthresh), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass blackframe_class = {
    .class_name = "blackframe",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_vf_blackframe_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer = ff_null_get_video_buffer,
        .filter_frame     = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_blackframe_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO
    },
    { NULL }
};

AVFilter ff_vf_blackframe = {
    .name        = "blackframe",
    .description = NULL_IF_CONFIG_SMALL("Detect frames that are (almost) black."),

    .priv_size = sizeof(BlackFrameContext),
    .priv_class = &blackframe_class,

    .query_formats = query_formats,

    .inputs    = avfilter_vf_blackframe_inputs,

    .outputs   = avfilter_vf_blackframe_outputs,
};
