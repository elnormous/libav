/**
 * @file
 * Audio meter
 *
 */

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "libavutil/bswap.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavformat/avformat.h"


typedef struct AudioMeterContext {
    AVClass *class;
    int fd;
    AVCodecContext *avctx;
    enum AVCodecID codec_id;
    int channels;
    AVRational time_base;
    float sum_volume;
    uint32_t count_volume;
    int64_t last_pts;
} AudioMeterContext;

static av_cold int audiometer_write_header(AVFormatContext *s1)
{
    AudioMeterContext *s = s1->priv_data;
    AVStream *st;
    enum AVCodecID codec_id;
    struct addrinfo* info;
    struct sockaddr_in address;

    s->fd = -1;

    st = s1->streams[0];
    s->codec_id = codec_id = st->codecpar->codec_id;
    s->channels = st->codecpar->channels;
    s->time_base = st->time_base;

    // connect to server
    int ret = getaddrinfo(s1->filename, "7777", NULL, &info);

    if (ret != 0)
    {
        av_log(s1, AV_LOG_ERROR,
               "failed to resolve the address %s\n",
               s1->filename);
        goto fail;
    }

    s->fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (s->fd == -1)
    {
        av_log(s1, AV_LOG_ERROR,
               "failed to create socket\n");
        goto fail;
    }

#ifdef __APPLE__
    {
        int set = 1;
        if (setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(int)) != 0)
        {
            av_log(s1, AV_LOG_ERROR,
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
        av_log(s1, AV_LOG_ERROR,
               "failed to connect to %s\n",
               s1->filename);
        goto fail;
    }

    freeaddrinfo(info);

    return 0;

fail:
    if (info) freeaddrinfo(info);
    if (s->fd != -1) close(s->fd);
    return AVERROR(EIO);
}

static int audiometer_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    AudioMeterContext *s = s1->priv_data;
    int size     = pkt->size;
    uint8_t *buf = pkt->data;
    int swap = 0;
    int64_t pts;

    switch (s->codec_id)
    {
        case AV_CODEC_ID_PCM_S16BE:
            swap = !HAVE_BIGENDIAN;
            break;
        case AV_CODEC_ID_PCM_S16LE:
            swap = HAVE_BIGENDIAN;
            break;
    }

    for (int i = 0; i < size; i += sizeof(uint16_t) * s->channels)
    {
        int16_t sample = *(int16_t*)(buf + i);
        if (swap) sample = (int16_t)av_bswap16((uint16_t)sample);

        float volume = sample / 32767.0f;
        s->sum_volume += volume;
        ++s->count_volume;
    }

    pts = av_rescale_q(pkt->pts, s->time_base, AV_TIME_BASE_Q);

#if defined(__linux__)
    int flags = MSG_NOSIGNAL;
#else
    int flags = 0;
#endif

    if (pts - s->last_pts > AV_TIME_BASE / 25) // 25 FPS
    {
        float volume = s->sum_volume / s->count_volume;
        int size = send(s->fd, (const char*)&volume, sizeof(volume), flags);

        if (size < 0)
        {
            if (errno != EAGAIN)
            {
                av_log(s1, AV_LOG_ERROR,
                    "failed to send data\n");
                return AVERROR(EIO);
            }
        }

        s->sum_volume = 0;
        s->count_volume = 0;
        s->last_pts = pts;
    }

    return 0;
}

static int audiometer_close(struct AVFormatContext *s1)
{
    AudioMeterContext *s = s1->priv_data;

    if (s->fd != -1) close(s->fd);

    return 0;
}

AVOutputFormat ff_audiometer_muxer = {
    .name           = "audiometer",
    .long_name      = NULL_IF_CONFIG_SMALL("Audio meter"),
    .priv_data_size = sizeof(AudioMeterContext),
    .audio_codec    = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE), // accept only PCM data
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = audiometer_write_header,
    .write_packet   = audiometer_write_packet,
    .write_trailer  = audiometer_close,
    .flags          = AVFMT_NOFILE
};
