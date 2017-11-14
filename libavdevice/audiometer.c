/**
 * @file
 * Audio meter
 *
 */

#include "libavutil/internal.h"

#include "libavformat/avformat.h"
#include <amqp_tcp_socket.h>
#include <amqp.h>
#include <amqp_framing.h>

typedef struct AudioMeterContext {
    AVClass *class;
    int channels;
    AVCodecContext *avctx;
    amqp_socket_t *socket;
    amqp_connection_state_t conn;
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

    if (sample_rate != st->codecpar->sample_rate) {
        av_log(s1, AV_LOG_ERROR,
               "sample rate %d not available, nearest is %d\n",
               st->codecpar->sample_rate, sample_rate);
        goto fail;
    }

    s->conn = amqp_new_connection();
    s->socket = amqp_tcp_socket_new(s->conn);
    if (!s->socket) {
        av_log(s1, AV_LOG_ERROR,
               "failed to connect to RabbitMQ\n");
        goto fail;
    }

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

    size /= s->frame_size;
    if (s->reorder_func) {
        if (size > s->reorder_buf_size)
            //if (reorder(s, size))
            // TODO: reorder
                return AVERROR(ENOMEM);
        s->reorder_func(buf, s->reorder_buf, size);
        buf = s->reorder_buf;
    }
    //while ((res = write(s->h, buf, size)) < 0) {
    while (0) { // TODO: write to RabbitMQ
    }

    return 0;
}

static int audiometer_close()
{
    // TODO: close RabbitMQ connection
}

AVOutputFormat ff_audiometer_muxer = {
    .name           = "audiometer",
    .long_name      = NULL_IF_CONFIG_SMALL("Audio meter"),
    .priv_data_size = sizeof(AudioMeterContext),
    .audio_codec    = DEFAULT_CODEC_ID,
    .video_codec    = AV_CODEC_ID_NONE,
    .write_header   = audiometer_write_header,
    .write_packet   = audiometer_write_packet,
    .write_trailer  = audiometer_close,
    .flags          = AVFMT_NOFILE,
};
