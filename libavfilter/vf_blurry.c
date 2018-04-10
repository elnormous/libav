#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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
} BlurryContext;

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

double laplacian3x3[3][3] = {
    {0.0, 1.0, 0.0},
    {1.0, -4.0, 1.0},
    {0.0, 1.0, 0.0}
};

static int convolution_filter(const uint8_t *sourceBitmap,
                              uint32_t width, uint32_t height,
                              double filterMatrix[3][3],
                              double factor,
                              int bias,
                              uint8_t *laplace)
{
    double value = 0.0;

    int filterWidth = 3;
    int filterHeight = 3;

    int filterOffset = (filterWidth - 1) / 2;
    int calcOffset = 0;

    int offset = 0;

    memset(laplace, 0, width * height);

    for (int offsetY = filterOffset; offsetY < height - filterOffset; offsetY++)
    {
        for (int offsetX = filterOffset; offsetX < width - filterOffset; offsetX++)
        {
            value = 0;

            offset = offsetY * width + offsetX;

            for (int filterY = -filterOffset; filterY <= filterOffset; filterY++)
            {
                for (int filterX = -filterOffset; filterX <= filterOffset; filterX++)
                {
                    calcOffset = offset + filterX + filterY * width;

                    value += (double)sourceBitmap[calcOffset] * filterMatrix[filterY + filterOffset][filterX + filterOffset];
                }
            }

            value = factor * value + bias;

            if (value > 255) value = 255;
            else if (value < 0) value = 0;

            laplace[offset] = (uint8_t)value;
        }
    }

    return 1;
}

static double calculate_variance(const uint8_t *bitmap, uint32_t size)
{
    double mean = 0.0;
    double variance = 0.0;

    for (uint32_t i = 0; i < size; ++i) mean += (double)bitmap[i];

    mean /= (double)size;

    for (uint32_t i = 0; i < size; ++i)
    {
        double difference = (double)bitmap[i] - mean;
        variance += difference * difference;
    }

    variance /= (double)size;

    return variance;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    BlurryContext *s = ctx->priv;
    int pblurry = 0;
    uint8_t *grayscale;
    uint8_t *laplace;

    grayscale = av_malloc(inlink->w * frame->height);

    if (frame->format == AV_PIX_FMT_YUV420P)
    {
        int x, i;

        for (i = 0; i < frame->height; i++)
            for (x = 0; x < inlink->w; x++)
                grayscale[i * inlink->w + x] = frame->data[0][i * frame->linesize[0] + x];
    }
    else
    {
        av_free(grayscale);
        return AVERROR(EINVAL);
    }

    laplace = av_malloc(inlink->w * frame->height);
    convolution_filter(grayscale, inlink->w, frame->height, laplacian3x3, 1.0, 0, laplace);

    if (calculate_variance(laplace, inlink->w * frame->height) < s->bthresh)
    {
        av_log(ctx, AV_LOG_INFO, "frame:%u pblurry:%u pts:%"PRId64" t:%f\n",
               s->frame, pblurry, frame->pts,
               frame->pts == AV_NOPTS_VALUE ? -1 : frame->pts * av_q2d(inlink->time_base));

    }

    av_free(laplace);
    av_free(grayscale);

    s->frame++;
    s->nblurry = 0;

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

#define OFFSET(x) offsetof(BlurryContext, x)
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
