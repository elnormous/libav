#include <stdio.h>
#include <inttypes.h>

#include "libavutil/internal.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

typedef struct BlurryContext {
    const AVClass *class;
    int bamount;          ///< blurry amount
    int bthresh;          ///< blurry threshold
    unsigned int frame;   ///< frame number
    unsigned int nblurry;  ///< number of blurry pixels counted so far
} BlurryFrameContext;

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

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    BlurryFrameContext *s = ctx->priv;
    int x, i;
    int pblurry = 0;
    uint8_t *p = frame->data[0];

    for (i = 0; i < frame->height; i++) {
        for (x = 0; x < inlink->w; x++)
            s->nblurry += p[x] < s->bthresh;
        p += frame->linesize[0];
    }

    pblurry = s->nblurry * 100 / (inlink->w * inlink->h);
    if (pblurry >= s->bamount)
        av_log(ctx, AV_LOG_INFO, "frame:%u pblurry:%u pts:%"PRId64" t:%f\n",
               s->frame, pblurry, frame->pts,
               frame->pts == AV_NOPTS_VALUE ? -1 : frame->pts * av_q2d(inlink->time_base));

    s->frame++;
    s->nblurry = 0;
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(BlurryFrameContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM
static const AVOption options[] = {
    { "amount", "Percentage of the pixels that have to be below the threshold "
        "for the frame to be considered blurry.", OFFSET(bamount), AV_OPT_TYPE_INT, { .i64 = 98 }, 0, 100,     FLAGS },
    { "threshold", "threshold below which a pixel value is considered blurry",
                                                 OFFSET(bthresh), AV_OPT_TYPE_INT, { .i64 = 32 }, 0, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass blurry_class = {
    .class_name = "blurry",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_vf_blurry_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_VIDEO,
        .get_video_buffer = ff_null_get_video_buffer,
        .filter_frame     = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_blurry_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO
    },
    { NULL }
};

AVFilter ff_vf_blurry = {
    .name        = "blurry",
    .description = NULL_IF_CONFIG_SMALL("Detect blurry frames."),

    .priv_size = sizeof(BlurryContext),
    .priv_class = &blurry_class,

    .query_formats = query_formats,

    .inputs    = avfilter_vf_blurry_inputs,

    .outputs   = avfilter_vf_blurry_outputs,
};
