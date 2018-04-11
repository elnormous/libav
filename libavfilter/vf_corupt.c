//
//  vf_corupt.c
//  libavfilter
//
//  Created by Agris Dūmiņš on 10/04/2018.
//  Copyright © 2018 Agris Dūmiņš. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/eval.h"
#include "libavutil/common.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

typedef struct CoruptContext {
    const AVClass *class;
    char *blackframe, *blur;

    uint8_t *temp[2];
} CoruptContext;

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));

    return 0;
}

// blur functionality taken from boxblur filter
static inline void blur(uint8_t *dst, int dst_step, const uint8_t *src, int src_step,
                        int len, int radius)
{
    /* Naive boxblur would sum source pixels from x-radius .. x+radius
     * for destination pixel x. That would be O(radius*width).
     * If you now look at what source pixels represent 2 consecutive
     * output pixels, then you see they are almost identical and only
     * differ by 2 pixels, like:
     * src0       111111111
     * dst0           1
     * src1        111111111
     * dst1            1
     * src0-src1  1       -1
     * so when you know one output pixel you can find the next by just adding
     * and subtracting 1 input pixel.
     * The following code adopts this faster variant.
     */
    const int length = radius*2 + 1;
    const int inv = ((1<<16) + length/2)/length;
    int x, sum = 0;

    for (x = 0; x < radius; x++)
        sum += src[x*src_step]<<1;
    sum += src[radius*src_step];

    for (x = 0; x <= radius; x++) {
        sum += src[(radius+x)*src_step] - src[(radius-x)*src_step];
        dst[x*dst_step] = (sum*inv + (1<<15))>>16;
    }

    for (; x < len-radius; x++) {
        sum += src[(radius+x)*src_step] - src[(x-radius-1)*src_step];
        dst[x*dst_step] = (sum*inv + (1<<15))>>16;
    }

    for (; x < len; x++) {
        sum += src[(2*len-radius-x-1)*src_step] - src[(x-radius-1)*src_step];
        dst[x*dst_step] = (sum*inv + (1<<15))>>16;
    }
}

static inline void blur_power(uint8_t *dst, int dst_step, const uint8_t *src, int src_step,
                              int len, int radius, int power, uint8_t *temp[2])
{
    uint8_t *a = temp[0], *b = temp[1];

    if (radius && power) {
        blur(a, 1, src, src_step, len, radius);
        for (; power > 2; power--) {
            uint8_t *c;
            blur(b, 1, a, 1, len, radius);
            c = a; a = b; b = c;
        }
        if (power > 1) {
            blur(dst, dst_step, a, 1, len, radius);
        } else {
            int i;
            for (i = 0; i < len; i++)
                dst[i*dst_step] = a[i];
        }
    } else {
        int i;
        for (i = 0; i < len; i++)
            dst[i*dst_step] = src[i*src_step];
    }
}

static void hblur(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2])
{
    int y;

    if (radius == 0 && dst == src)
        return;

    for (y = 0; y < h; y++)
        blur_power(dst + y*dst_linesize, 1, src + y*src_linesize, 1,
                   w, radius, power, temp);
}

static void vblur(uint8_t *dst, int dst_linesize, const uint8_t *src, int src_linesize,
                  int w, int h, int radius, int power, uint8_t *temp[2])
{
    int x;

    if (radius == 0 && dst == src)
        return;

    for (x = 0; x < w; x++)
        blur_power(dst + x, dst_linesize, src + x, src_linesize,
                   h, radius, power, temp);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    CoruptContext *s = ctx->priv;

    if (s->blackframe) {
        if ((rand() & 0xF) == 1) {
            memset(frame->data[0], 0, frame->linesize[0] * frame->height);
            memset(frame->data[1], 127, frame->linesize[1] * frame->height / 2);
            memset(frame->data[2], 127, frame->linesize[2] * frame->height / 2);
        }
    }

    if (s->blur) {
        if ((rand() & 0xF) == 1)
        {
            AVFilterLink *outlink = inlink->dst->outputs[0];
            AVFrame *out;
            int plane;
            int cw = inlink->w >> 1, ch = frame->height >> 1;
            int w[4] = { inlink->w, cw, cw, inlink->w };
            int h[4] = { frame->height, ch, ch, frame->height };

            out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
            if (!out) {
                av_frame_free(&frame);
                return AVERROR(ENOMEM);
            }
            av_frame_copy_props(out, frame);

            for (plane = 0; frame->data[plane] && plane < 4; plane++)
                hblur(out->data[plane], out->linesize[plane],
                      frame ->data[plane], frame ->linesize[plane],
                      w[plane], h[plane], plane == 0 ? 5 : 3, 2,
                      s->temp);

            for (plane = 0; frame->data[plane] && plane < 4; plane++)
                vblur(out->data[plane], out->linesize[plane],
                      out->data[plane], out->linesize[plane],
                      w[plane], h[plane], plane == 0 ? 5 : 3, 2,
                      s->temp);

            av_frame_free(&frame);
            frame = out;
        }
    }

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(CoruptContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "blackframe", "Output video width",  OFFSET(blackframe), AV_OPT_TYPE_STRING, { .str = NULL },                 .flags = FLAGS },
    { "blur", "Output video height", OFFSET(blur), AV_OPT_TYPE_STRING, { .str = NULL },                 .flags = FLAGS },
    { NULL },
};

static const AVClass corupt_class = {
    .class_name = "crop",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext    *ctx = inlink->dst;
    CoruptContext *s = ctx->priv;
    int w = inlink->w, h = inlink->h;

    av_freep(&s->temp[0]);
    av_freep(&s->temp[1]);
    if (!(s->temp[0] = av_malloc(FFMAX(w, h))))
        return AVERROR(ENOMEM);
    if (!(s->temp[1] = av_malloc(FFMAX(w, h)))) {
        av_freep(&s->temp[0]);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static const AVFilterPad avfilter_vf_corupt_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .filter_frame     = filter_frame,
        .config_props     = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_corupt_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

static av_cold void uninit(AVFilterContext *ctx)
{
    CoruptContext *s = ctx->priv;

    av_freep(&s->temp[0]);
    av_freep(&s->temp[1]);
}

AVFilter ff_vf_corupt = {
    .name      = "corupt",
    .description = NULL_IF_CONFIG_SMALL("Corupt video - currently randomly inserts black frames."),

    .priv_size = sizeof(CoruptContext),
    .priv_class = &corupt_class,

    .query_formats = query_formats,
    .uninit        = uninit,

    .inputs    = avfilter_vf_corupt_inputs,
    .outputs   = avfilter_vf_corupt_outputs,
};
