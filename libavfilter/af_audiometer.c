/**
 * @file
 * Audio meter
 *
 */

#include <assert.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "libavutil/bswap.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavformat/avformat.h"

typedef struct AudiometerContext {
    AVClass *class;
    const char *address;
    int port;
    int fd;
    double sum_samples;
    double max_sample;
    uint32_t count_samples;
    int64_t last_pts;
} AudiometerContext;

static av_cold int init(AVFilterContext *ctx)
{
    AudiometerContext *s = ctx->priv_data;
    struct addrinfo* info;
    struct sockaddr_in address;
    int ret;
    char port[12];

    // connect to server
    s->fd = -1;

    sprintf(port, "%d", s->port);

    ret = getaddrinfo(s->address, port, NULL, &info);

    if (ret != 0)
    {
        av_log(ctx, AV_LOG_ERROR,
               "failed to resolve the address %s\n",
               s->address);
        goto fail;
    }

    s->fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (s->fd == -1)
    {
        av_log(ctx, AV_LOG_ERROR,
               "failed to create socket\n");
        goto fail;
    }

#ifdef __APPLE__
    {
        int set = 1;
        if (setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(int)) != 0)
        {
            av_log(ctx, AV_LOG_ERROR,
               "failed to set socket option\n");
            goto fail;
        }
    }
#endif

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = ((struct sockaddr_in*)info->ai_addr)->sin_port;
    address.sin_addr.s_addr = ((struct sockaddr_in*)info->ai_addr)->sin_addr.s_addr;

    if (connect(s->fd, (const struct sockaddr*)&address, sizeof(address)) < 0)
    {
        av_log(ctx, AV_LOG_ERROR,
               "failed to connect to %s\n",
               ctx->filename);
        goto fail;
    }

    freeaddrinfo(info);

    return 0;

fail:
    if (info) freeaddrinfo(info);
    if (s->fd != -1) close(s->fd);
    return AVERROR(EIO);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AudiometerContext *s  = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    int samples           = frame->nb_samples;
    const int16_t *buf    = (int16_t *)frame->data[0];
    int channel_count     = av_get_channel_layout_nb_channels(frame->channel_layout);
    int64_t pts;

    for (int i = 0; i < samples * channel_count; ++i)
    {
        int16_t sample = buf[i];
        double normalized;
        normalized = sample / 32768.0;
        s->sum_samples += normalized * normalized;
        if (normalized > s->max_sample) s->max_sample = normalized;
        ++s->count_samples;
    }

    pts = av_rescale_q(frame->pts, inlink->time_base, AV_TIME_BASE_Q);

    if (pts - s->last_pts > AV_TIME_BASE / 25) // 25 FPS
    {
        double rms = sqrt(s->sum_samples / s->count_samples); // root mean square
        float dB;
        int size;
        int flags  = 0;

#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

        dB = 1.5 + 0.5 * log10(rms);
        //dB = 2.0 + log10(s->max_sample);
        
        size = send(s->fd, (const char*)&dB, sizeof(dB), flags);

        if (size < 0)
        {
            if (errno != EAGAIN)
            {
                av_log(inlink->src, AV_LOG_ERROR,
                    "failed to send data\n");
                return AVERROR(EIO);
            }
        }

        s->sum_samples = 0;
        s->max_sample = 0;
        s->count_samples = 0;
        s->last_pts = pts;
    }

    return ff_filter_frame(outlink, frame);
}

static void uninit(AVFilterContext *ctx)
{
    AudiometerContext *s = ctx->priv_data;

    if (s->fd != -1) close(s->fd);
}

#define OFFSET(x) offsetof(VolumeContext, x)

static const AVOption options[] = {
    { "address", "IP address.",
            OFFSET(address), AV_OPT_TYPE_STRING, { .str = "127.0.0.1" }, 0, 0x7fffff, AV_OPT_FLAG_AUDIO_PARAM },
    { "port", "Port number.",
            OFFSET(port), AV_OPT_TYPE_INT, { .i64 = 7777 }, 0, 0x7fffff, AV_OPT_FLAG_AUDIO_PARAM },
    { NULL },
};

static const AVClass audiometer_class = {
    .class_name = "audiometer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad avfilter_af_audiometer_inputs[] = {
    {
        .name             = "default",
        .type             = AVMEDIA_TYPE_AUDIO,
        .filter_frame     = filter_frame,
    },
    { NULL }
};

static const AVFilterPad avfilter_af_audiometer_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_AUDIO,
    },
    { NULL }
};

AVFilter ff_af_audiometer = {
    .name           = "audiometer",
    .description    = NULL_IF_CONFIG_SMALL("Audio meter"),
    .priv_size      = sizeof(AudioMeterContext),
    .priv_class     = &audiometer_class,
    .init           = init,
    .uninit         = uninit,
    .inputs         = avfilter_af_audiometer_inputs,
    .outputs        = avfilter_af_audiometer_outputs,
};
