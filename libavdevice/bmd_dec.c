/*
 * Blackmagic Devices Decklink capture
 *
 * This file is part of Libav.
 *
 * Copyright (C) 2013 Luca Barbato
 *
 * Libav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Blackmagic Devices Decklink capture
 */

#include "config.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#include <pthread.h>
#include <libbmd/decklink_capture.h>

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

static int packet_queue_init(PacketQueue *q)
{
    int ret = 0;
    memset(q, 0, sizeof(PacketQueue));
    if ((ret = pthread_mutex_init(&q->mutex, NULL)))
        return AVERROR(ret);
    if ((ret = pthread_cond_init(&q->cond, NULL)))
        return AVERROR(ret);
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt   = NULL;
    q->first_pkt  = NULL;
    q->nb_packets = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    int err;

    /* duplicate the packet */
    if ((err = av_dup_packet(pkt)) < 0) {
        return err;
    }

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1) {
        return AVERROR(ENOMEM);
    }
    pkt1->pkt  = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt) {
        q->first_pkt = pkt1;
    } else {
        q->last_pkt->next = pkt1;
    }

    q->last_pkt = pkt1;
    q->nb_packets++;

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;; ) {
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = pkt->size;
            break;
        } else if (!block) {
            ret = AVERROR(EAGAIN);
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}



typedef struct {
    const AVClass   *class;    /**< Class for private options. */
    DecklinkCapture *capture;
    DecklinkConf    conf;
    PacketQueue     q;
    AVStream        *audio_st;
    AVStream        *video_st;
} BMDCaptureContext;

static AVStream *add_audio_stream(AVFormatContext *oc, DecklinkConf *conf)
{
    AVCodecContext *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    st->time_base  = (AVRational){1, 48000};

    c              = st->codec;
    c->codec_type  = AVMEDIA_TYPE_AUDIO;
    c->sample_rate = 48000;
    c->channels    = conf->audio_channels;

    switch (conf->audio_sample_depth) {
    case 16:
        c->sample_fmt = AV_SAMPLE_FMT_S16;
        c->codec_id   = AV_CODEC_ID_PCM_S16LE;
    break;
    case 32:
        c->sample_fmt = AV_SAMPLE_FMT_S32;
        c->codec_id   = AV_CODEC_ID_PCM_S32LE;
    break;
    default:
        av_log(oc, AV_LOG_ERROR,
               "%dbit audio is not supported.\n",
               conf->audio_sample_depth);
        return NULL;
    }

    c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc, DecklinkConf *conf)
{
    AVCodecContext *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    c                = st->codec;
    c->codec_type    = AVMEDIA_TYPE_VIDEO;

    c->width         = conf->width;
    c->height        = conf->height;

    st->time_base.den = conf->tb_den;
    st->time_base.num = conf->tb_num;

    st->avg_frame_rate.num = conf->tb_den;
    st->avg_frame_rate.den = conf->tb_num;

    switch (conf->pixel_format) {
    // YUV first
    case 0:
        c->pix_fmt   = AV_PIX_FMT_UYVY422;
        c->codec_id  = AV_CODEC_ID_RAWVIDEO;
        c->codec_tag = avcodec_pix_fmt_to_codec_tag(c->pix_fmt);
    break;
    case 1:
        c->pix_fmt             = AV_PIX_FMT_YUV422P10;
        c->codec_id            = AV_CODEC_ID_V210;
        c->bits_per_raw_sample = 10;
    break;
    // RGB later
    default:
        av_log(oc, AV_LOG_ERROR, "pixel format is not supported.\n");
        return NULL;
    }

    c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return st;
}

static int bmd_read_close(AVFormatContext *s)
{
    BMDCaptureContext *ctx = s->priv_data;

    if (ctx->capture) {
        decklink_capture_stop(ctx->capture);
        decklink_capture_free(ctx->capture);
    }

    packet_queue_end(&ctx->q);

    return 0;
}

static int video_callback(void *priv, uint8_t *frame,
                          int width, int height, int stride,
                          int64_t timestamp,
                          int64_t duration,
                          int64_t flags)
{
    BMDCaptureContext *ctx = priv;
    AVPacket pkt;

    av_init_packet(&pkt);

    pkt.pts = pkt.dts = timestamp / ctx->video_st->time_base.num;
    pkt.duration      = duration  / ctx->video_st->time_base.num;

    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = ctx->video_st->index;
    pkt.data          = frame;
    pkt.size          = stride * height;

    return packet_queue_put(&ctx->q, &pkt);
}

static int audio_callback(void *priv, uint8_t *frame,
                          int nb_samples,
                          int64_t timestamp,
                          int64_t flags)
{
    BMDCaptureContext *ctx = priv;
    AVCodecContext *c = ctx->audio_st->codec;
    AVPacket pkt;

    av_init_packet(&pkt);

    pkt.size          = nb_samples * c->channels *
                        (ctx->conf.audio_sample_depth / 8);
    pkt.dts = pkt.pts = timestamp;
    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = ctx->audio_st->index;
    pkt.data          = frame;

    return packet_queue_put(&ctx->q, &pkt);
}

static int bmd_read_header(AVFormatContext *s)
{
    BMDCaptureContext *ctx = s->priv_data;
    int ret;

    if ((ret = packet_queue_init(&ctx->q)) < 0)
        return ret;

    ctx->conf.video_cb = video_callback;
    ctx->conf.audio_cb = audio_callback;
    ctx->conf.priv     = ctx;

    ctx->capture = decklink_capture_alloc(&ctx->conf);

    if (!ctx->capture) {
        ret = AVERROR(EIO);
        goto out;
    }

    ctx->video_st = add_video_stream(s, &ctx->conf);
    ctx->audio_st = add_audio_stream(s, &ctx->conf);

    if (!ctx->video_st || !ctx->audio_st) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    decklink_capture_start(ctx->capture);

    return 0;
out:
    bmd_read_close(s);
    return ret;
}

static int bmd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BMDCaptureContext *ctx = s->priv_data;

    return packet_queue_get(&ctx->q, pkt, 0);
}


#define O(x) offsetof(BMDCaptureContext, conf) + offsetof(DecklinkConf, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "instance",         "Device instance",    O(instance),         AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { "video_mode",       "Video mode",         O(video_mode),       AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { "video_connection", "Video connection",   O(video_connection), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { "video_format",     "Video pixel format", O(pixel_format),     AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { "audio_connection", "Audio connection",   O(audio_connection), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { NULL },
};

static const AVClass bmd_class = {
    .class_name = "bmd indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/** x11 grabber device demuxer declaration */
AVInputFormat ff_bmd_demuxer = {
    .name           = "bmd",
    .long_name      = NULL_IF_CONFIG_SMALL("Decklink capture"),
    .priv_data_size = sizeof(BMDCaptureContext),
    .read_header    = bmd_read_header,
    .read_packet    = bmd_read_packet,
    .read_close     = bmd_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &bmd_class,
};
