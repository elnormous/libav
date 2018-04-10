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
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

typedef struct CoruptContext {
    const AVClass *class;
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

static int filter_frame(AVFilterLink *link, AVFrame *frame)
{
    AVFilterContext *ctx = link->dst;
//    CoruptContext *s = ctx->priv;

    if ((rand() & 0xF) == 1) {
        memset(frame->data[0], 0, frame->linesize[0] * frame->height);
        memset(frame->data[1], 127, frame->linesize[1] * frame->height / 2);
        memset(frame->data[2], 127, frame->linesize[2] * frame->height / 2);
    }

    return ff_filter_frame(link->dst->outputs[0], frame);
}

static const AVClass corupt_class = {
    .class_name = "crop",
    .item_name  = av_default_item_name,
//    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_vf_corupt_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .filter_frame     = filter_frame,
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

AVFilter ff_vf_corupt = {
    .name      = "corupt",
    .description = NULL_IF_CONFIG_SMALL("Corupt video - currently randomly inserts black frames."),

    .priv_size = sizeof(CoruptContext),
    .priv_class = &corupt_class,

    .query_formats = query_formats,
//    .uninit        = uninit,

    .inputs    = avfilter_vf_corupt_inputs,
    .outputs   = avfilter_vf_corupt_outputs,
};
