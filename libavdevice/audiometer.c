/**
 * @file
 * Audio meter
 *
 */

#include "libavutil/internal.h"
#include "libavformat/avformat.h"


typedef struct AudioMeterContext {
    AVClass *class;
    int channels;
    AVCodecContext *avctx;
} AudioMeterContext;

static av_cold int audiometer_write_header(AVFormatContext *s1)
{
    AudioMeterContext *s = s1->priv_data;
    AVStream *st;
    unsigned int sample_rate;
    enum AVCodecID codec_id;
    int res;

    st = s1->streams[0];
    sample_rate = st->codecpar->sample_rate;
    codec_id    = st->codecpar->codec_id;

    s1->filename;

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

    /*size /= s->frame_size;
    if (s->reorder_func) {
        if (size > s->reorder_buf_size)
            //if (reorder(s, size))
            // TODO: reorder
                return AVERROR(ENOMEM);
        s->reorder_func(buf, s->reorder_buf, size);
        buf = s->reorder_buf;
    }*/
    printf("received\n");

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
