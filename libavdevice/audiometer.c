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
    double sum_samples;
    uint32_t count_samples;
    int64_t last_pts;
} AudioMeterContext;

static av_cold int audiometer_write_header(AVFormatContext *s1)
{
    AudioMeterContext *s = s1->priv_data;
    AVStream *st;
    enum AVCodecID codec_id;
    struct addrinfo* info;
    struct sockaddr_in address;
    int ret;

    s->fd = -1;

    st = s1->streams[0];
    s->codec_id = codec_id = st->codecpar->codec_id;
    s->channels = st->codecpar->channels;
    s->time_base = st->time_base;

    if (s->codec_id != AV_CODEC_ID_PCM_S16BE &&
        s->codec_id != AV_CODEC_ID_PCM_S16LE)
    {
        av_log(s1, AV_LOG_ERROR,
           "unsupported codec %x\n", s->codec_id);
    }

    // connect to server
    ret = getaddrinfo(s1->filename, "7777", NULL, &info);

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
    int flags = 0;

    switch (s->codec_id)
    {
        case AV_CODEC_ID_PCM_S16BE:
            swap = !HAVE_BIGENDIAN;
            break;
        case AV_CODEC_ID_PCM_S16LE:
            swap = HAVE_BIGENDIAN;
            break;
    }

    for (int i = 0; i < size; i += sizeof(int16_t) * s->channels)
    {
        int16_t sample = *(int16_t*)(buf + i);
        double normalized;
        if (swap) sample = (int16_t)av_bswap16((uint16_t)sample);
        normalized = sample / 32768.0;
        s->sum_samples += normalized * normalized;
        ++s->count_samples;
    }

    pts = av_rescale_q(pkt->pts, s->time_base, AV_TIME_BASE_Q);

#if defined(__linux__)
    flags = MSG_NOSIGNAL;
#endif

    if (pts - s->last_pts > AV_TIME_BASE / 25) // 25 FPS
    {
        double rms = sqrt(s->sum_samples / s->count_samples); // root mean square
        float dB;
        int size;

        dB = 3.0 + 12.0 * log10(rms);
        
        size = send(s->fd, (const char*)&dB, sizeof(dB), flags);

        if (size < 0)
        {
            if (errno != EAGAIN)
            {
                av_log(s1, AV_LOG_ERROR,
                    "failed to send data\n");
                return AVERROR(EIO);
            }
        }

        s->sum_samples = 0;
        s->count_samples = 0;
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
