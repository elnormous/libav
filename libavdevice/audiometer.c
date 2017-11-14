/**
 * @file
 * Audio meter
 *
 */

#include "libavutil/bswap.h"
#include "libavutil/internal.h"
#include "libavutil/mathematics.h"
#include "libavformat/avformat.h"


typedef struct AudioMeterContext {
    AVClass *class;
    AVCodecContext *avctx;
    enum AVCodecID codec_id;
    int channels;
    AVRational time_base;
    int16_t max_volume;
    int64_t last_pts;
} AudioMeterContext;

static av_cold int audiometer_write_header(AVFormatContext *s1)
{
    AudioMeterContext *s = s1->priv_data;
    AVStream *st;
    unsigned int sample_rate;
    enum AVCodecID codec_id;
    int res;

    st = s1->streams[0];
    //sample_rate = st->codecpar->sample_rate;
    s->codec_id = codec_id = st->codecpar->codec_id;
    s->channels = st->codecpar->channels;
    s->time_base = st->time_base;

    // TODO connect socket
    //s1->filename;
    printf("ADDRESS: %s, channels: %d\n", s1->filename, s->channels);

    return res;

fail:
    // TODO: close connection
    return AVERROR(EIO);
}

static int audiometer_write_packet(AVFormatContext *s1, AVPacket *pkt)
{
    AudioMeterContext *s = s1->priv_data;
    int res;
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

        if (sample > s->max_volume) s->max_volume = sample;
    }

    pts = av_rescale_q(pkt->pts, s->time_base, AV_TIME_BASE_Q);

    if (pts - s->last_pts > AV_TIME_BASE / 25) // 25 FPS
    {
        printf("Volume: %d\n", s->max_volume);
        s->max_volume = 0;
        s->last_pts = pts;
    }

    return 0;
}

static int audiometer_close()
{
    // TODO: close connection
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
