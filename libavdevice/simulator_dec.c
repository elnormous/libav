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
#include "libavutil/time.h"
#include "libavutil/internal.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#include <inttypes.h>
#include <pthread.h>
#include <emmintrin.h>
#include <smmintrin.h>

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
        av_packet_unref(&pkt->pkt);
        av_free(pkt);
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

static int packet_queue_put(PacketQueue *q, AVPacket *pkt, int64_t queue_size)
{
    AVPacketList *pkt_entry;
    int ret = 0;
    pthread_mutex_lock(&q->mutex);

    if (queue_size > 0 && q->nb_packets >= queue_size) {
        ret = AVERROR(ENOBUFS);

        pkt_entry = q->first_pkt;

        if (pkt_entry) {
            q->first_pkt = pkt_entry->next;

            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            av_packet_unref(&pkt_entry->pkt);
            av_free(pkt_entry);
        }
    }

    pkt_entry = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt_entry) {
        pthread_mutex_unlock(&q->mutex);
        return AVERROR(ENOMEM);
    }
    pkt_entry->pkt  = *pkt;
    pkt_entry->next = NULL;

    if (!q->last_pkt) {
        q->first_pkt = pkt_entry;
    } else {
        q->last_pkt->next = pkt_entry;
    }

    q->last_pkt = pkt_entry;
    q->nb_packets++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt_entry;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;;) {
        pkt_entry = q->first_pkt;
        if (pkt_entry) {
            q->first_pkt = pkt_entry->next;
            if (!q->first_pkt) {
                q->last_pkt = NULL;
            }
            q->nb_packets--;
            *pkt = pkt_entry->pkt;
            av_free(pkt_entry);
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

    char*           frame_file;
    uint8_t*        raw_frame;
    int             frame_size;

    int             max_frame_cnt;

    pthread_t       v_thread;
//    pthread_t       a_thread;
    int             stop_threads;


    PacketQueue     q;
    AVStream        *audio_st;
    AVStream        *video_st;
    AVStream        *data_st;
    int             wallclock;
    int64_t         timeout;
    int64_t         queue_size;
    int64_t         start_time;
} SContext;

static AVStream *add_audio_stream(AVFormatContext *oc)
{
//    AVCodecParameters *c;
//    AVStream *st;
//
//    st = avformat_new_stream(oc, NULL);
//    if (!st)
//        return NULL;
//
//    st->time_base  = (AVRational){1, 48000};
//
//    c              = st->codecpar;
//    c->codec_type  = AVMEDIA_TYPE_AUDIO;
//    c->sample_rate = 48000;
//    c->channels    = conf->audio_channels;
//
//    switch (conf->audio_sample_depth) {
//        case 16:
//            c->format   = AV_SAMPLE_FMT_S16;
//            c->codec_id = AV_CODEC_ID_PCM_S16LE;
//            break;
//        case 32:
//            c->format   = AV_SAMPLE_FMT_S32;
//            c->codec_id = AV_CODEC_ID_PCM_S32LE;
//            break;
//        default:
//            av_log(oc, AV_LOG_ERROR,
//                   "%dbit audio is not supported.\n",
//                   conf->audio_sample_depth);
//            return NULL;
//    }

//    return st;

    return NULL;
}

static AVStream *add_data_stream(AVFormatContext *oc)
{
    AVCodecParameters *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st) {
        fprintf(stderr, "Could not alloc stream\n");
        exit(1);
    }

    c = st->codecpar;
    c->codec_id   = AV_CODEC_ID_TEXT;
    c->codec_type = AVMEDIA_TYPE_DATA;

    st->time_base.den = 25; //conf->tb_den; // 24000
    st->time_base.num = 1; //conf->tb_num; // 1001

    st->avg_frame_rate.num = 25; // conf->tb_den;
    st->avg_frame_rate.den = 1; // conf->tb_num;

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc)
{
    AVCodecParameters *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    c                = st->codecpar;
    c->codec_type    = AVMEDIA_TYPE_VIDEO;

    c->width         = 1920;
    c->height        = 1080;

    st->time_base.den = 25;
    st->time_base.num = 1;

    st->avg_frame_rate.num = 25;
    st->avg_frame_rate.den = 1;

    c->field_order = AV_FIELD_PROGRESSIVE;

    c->format    = AV_PIX_FMT_YUV420P;
    c->codec_id  = AV_CODEC_ID_RAWVIDEO;
    c->codec_tag = avcodec_pix_fmt_to_codec_tag(c->format);

    return st;
}

static int simulator_read_close(AVFormatContext *s)
{
    SContext *ctx = s->priv_data;

    ctx->stop_threads = 1;

    pthread_join(ctx->v_thread, NULL);

    packet_queue_end(&ctx->q);

    return 0;
}

static int put_wallclock_packet(SContext *ctx, int64_t pts)
{
    AVPacket pkt;
    char buf[21];
    int size;
    int ret;

    size = snprintf(buf, sizeof(buf), "%" PRId64, av_gettime()) + 1;

    ret = av_new_packet(&pkt, size);

    if (ret != 0) {
        return ret;
    }

    memcpy(pkt.buf->data, buf, size);

    pkt.pts = pkt.dts = pts;
    pkt.stream_index  = ctx->data_st->index;

    if (packet_queue_put(&ctx->q, &pkt, ctx->queue_size) != 0) {
        av_log(NULL, AV_LOG_WARNING, "no space in queue, data frame dropped.\n");
        ctx->data_st->dropped_frames++;
    }

    return 0;
}

static void convertRawUYVYtoYUV420P(const uint8_t* srcData,
                             const int W, const int H, const int stride,
                             uint8_t* dst)
{
    const int YStride = W;
    const int UVStride = (W + 1) / 2;
    const int UVRows = (H + 1) / 2;

    uint8_t* Ys = &dst[0];
    uint8_t* Us = Ys + YStride * H;
    uint8_t* Vs = Us + UVStride * UVRows;

    const uint8_t* data = srcData;
    __m128i zeros = _mm_setzero_si128();

    for (int y = 0; y < H; y++)
    {
        int x = 0;
        data = &srcData[y * stride];

        if (y < H - 1) // can't store Us in this way
            while (x + 7 < UVStride)
            {
                __m128i uvs, vs, us;
                __m128i px1 = _mm_loadu_si128((__m128i*)&data[0]);
                __m128i ypx1 = _mm_srli_epi16(px1, 8);
                __m128i px2 = _mm_loadu_si128((__m128i*)&data[16]);
                __m128i ypx2 = _mm_srli_epi16(px2, 8);
                _mm_storeu_si128((__m128i*)(Ys), _mm_packus_epi16(ypx1, ypx2));

                // how to get us & vs..
                ypx1 = _mm_slli_epi16(px1, 8);
                ypx1 = _mm_srli_epi16(ypx1, 8);
                ypx2 = _mm_slli_epi16(px2, 8);
                ypx2 = _mm_srli_epi16(ypx2, 8);

                uvs = _mm_packus_epi16(ypx1, ypx2);
                vs = _mm_srli_epi16(uvs, 8);
                us = _mm_slli_epi16(uvs, 8);
                us = _mm_srli_epi16(us, 8);

                _mm_storeu_si128((__m128i*)(&Us[x]), _mm_packus_epi16(us, zeros));
                _mm_storeu_si128((__m128i*)(&Vs[x]), _mm_packus_epi16(vs, zeros));

                x += 8;
                data += 32;
                Ys += 16;
            }

        for (; x < UVStride; x++, data += 4, Ys += 2)
        {
            Ys[0] = data[1];
            Ys[1] = data[3];
            Us[x] = data[0];
            Vs[x] = data[2];
        }
        //        Ys += YStride;
        Us += UVStride;
        Vs += UVStride;

        if (++y < H)
        {
            int x = 0;
            data = &srcData[y * stride];

            while (x + 7 < UVStride)
            {
                __m128i px1 = _mm_srli_epi16(_mm_loadu_si128((__m128i*)&data[0]), 8);
                __m128i px2 = _mm_srli_epi16(_mm_loadu_si128((__m128i*)&data[16]), 8);
                _mm_storeu_si128((__m128i*)(Ys), _mm_packus_epi16(px1, px2));

                x += 8;
                data += 32;
                Ys += 16;
            }

            for (; x < UVStride; x++, data += 4, Ys += 2)
            {
                Ys[0] = data[1];
                Ys[1] = data[3];
            }
        }
    }
}

static void* video_callback(void *priv)
{
    int64_t timestamp = 1;

    SContext *ctx = priv;
    int64_t previous = 0; //av_gettime();

    while (!ctx->stop_threads)
    {
        AVPacket pkt;
        int ret;

        if (av_gettime() - previous < 40000)
        {
            av_usleep(20000);
            continue;
        }
        previous = av_gettime();

        ret = av_new_packet(&pkt, 1920 * 1080 * 1.5);

        if (ret != 0) {
            return NULL;
        }

        if (ctx->start_time == AV_NOPTS_VALUE) {
            int64_t* time_data;

            ctx->start_time = av_gettime() / 1000;

            av_log(NULL, AV_LOG_INFO, "simulator start time: %lld\n", ctx->start_time);

            time_data = (int64_t*)av_stream_new_side_data(ctx->video_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
            if (time_data) {
                *time_data = ctx->start_time;
            }
//            time_data = (int64_t*)av_stream_new_side_data(ctx->audio_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
//            if (time_data) {
//                *time_data = ctx->start_time;
//            }
            time_data = (int64_t*)av_stream_new_side_data(ctx->data_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
            if (time_data) {
                *time_data = ctx->start_time;
            }
        }

        convertRawUYVYtoYUV420P(ctx->raw_frame, 1920, 1080, 1920 * 2, pkt.data);

        pkt.pts = pkt.dts = timestamp; //timestamp / ctx->video_st->time_base.num;
        pkt.duration      = 1; //duration  / ctx->video_st->time_base.num;

        pkt.flags        |= AV_PKT_FLAG_KEY;
        pkt.stream_index  = ctx->video_st->index;

        if (ctx->wallclock) {
            put_wallclock_packet(ctx, pkt.pts);
        }

        if (packet_queue_put(&ctx->q, &pkt, ctx->queue_size) != 0) {
            av_log(NULL, AV_LOG_WARNING, "no space in queue, video frame dropped.\n");
            ctx->video_st->dropped_frames++;
        }

        timestamp++;

        if (ctx->max_frame_cnt != 0 && timestamp > ctx->max_frame_cnt) ctx->stop_threads = 1;
    }

    return NULL;
}

static int audio_callback(void *priv, uint8_t *frame,
                          int nb_samples,
                          int64_t timestamp,
                          int64_t flags)
{
//    SContext *ctx = priv;
//    AVCodecParameters *c = ctx->audio_st->codecpar;
//    AVPacket pkt;
//    int ret;
//
//    ret = av_new_packet(&pkt, nb_samples * c->channels * (ctx->conf.audio_sample_depth / 8));
//
//    if (ret != 0) {
//        return ret;
//    }
//
//    memcpy(pkt.buf->data, frame,
//           nb_samples * c->channels * (ctx->conf.audio_sample_depth / 8));
//
//    pkt.dts = pkt.pts = timestamp;
//    pkt.flags        |= AV_PKT_FLAG_KEY;
//    pkt.stream_index  = ctx->audio_st->index;
//
//    if (packet_queue_put(&ctx->q, &pkt, ctx->queue_size) != 0) {
//        av_log(NULL, AV_LOG_WARNING, "no space in queue, audio frame dropped.\n");
//        ctx->audio_st->dropped_frames++;
//    }

    return 0;
}

static int simulator_read_header(AVFormatContext *s)
{
    SContext *ctx = s->priv_data;
    int ret;
    FILE *file;

    ctx->stop_threads = 0;

    if ((ret = packet_queue_init(&ctx->q)) < 0)
        return ret;

    file = fopen(ctx->frame_file, "r");
    if(!file)
    {
        ret = AVERROR(EIO);
        goto out;
    }

    ctx->frame_size = 1920 * 1080 * 2;
    ctx->raw_frame = malloc(ctx->frame_size);
    fread(ctx->raw_frame, ctx->frame_size, 1, file);
    fclose(file);

    ctx->video_st = add_video_stream(s);
//    ctx->audio_st = add_audio_stream(s);
    ctx->data_st  = add_data_stream(s);

    if (!ctx->video_st
//        || !ctx->audio_st
        )
    {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    ctx->start_time = AV_NOPTS_VALUE;

    pthread_create(&ctx->v_thread, NULL, &video_callback, (void*)ctx);

    return 0;
out:
    simulator_read_close(s);
    return ret;
}

static int simulator_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SContext *ctx = s->priv_data;

    if (ctx->stop_threads)
    {
        return AVERROR_EOF;
    }

    return packet_queue_get(&ctx->q, pkt, 0);
}

#define OC(x) offsetof(SContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "max", "File cnt to generate",   OC(max_frame_cnt), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { "video_timeout",    "Video timeout (in seconds), 0 to disable the timeout",      OC(timeout),          AV_OPT_TYPE_INT64, {.i64 = 3}, 0, INT_MAX, D },
    { "queue_size",       "Packet queue size, 0 to disable the queue limit",  OC(queue_size),       AV_OPT_TYPE_INT64, {.i64 = 25}, 0, INT_MAX, D },
    { "frame", "UYVY raw 8bit frame"         , OC(frame_file), AV_OPT_TYPE_STRING, {.str = 0}, 0, 0, D },
    { "wallclock",        "Add the wallclock", OC(wallclock),     AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { NULL },
};

static const AVClass simulator_class = {
    .class_name = "simulator indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/** x11 grabber device demuxer declaration */
AVInputFormat ff_simulator_demuxer = {
    .name           = "simulator",
    .long_name      = NULL_IF_CONFIG_SMALL("Input simulator"),
    .priv_data_size = sizeof(SContext),
    .read_header    = simulator_read_header,
    .read_packet    = simulator_read_packet,
    .read_close     = simulator_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &simulator_class,
};

