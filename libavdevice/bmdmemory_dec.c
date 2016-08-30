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
 * BMD memory decoder
 */

#include "config.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#include <sys/mman.h>
#include <semaphore.h>
#include <pthread.h>

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

typedef struct {
    uint8_t meta_data[128];
    uint8_t video_data[1024 * 1024 * 40]; // 40MiB
    uint8_t audio_data[1024 * 1024 * 40]; // 40MiB
} Memory;

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
    char            *memory_name;
    int             shared_memory_fd;
    Memory          *shared_memory;
    sem_t           *sem;
    PacketQueue     q;
    AVStream        *audio_st;
    AVStream        *video_st;
    uint64_t        video_pts;
    uint64_t        audio_pts;

    pthread_t       thread;
    int             done;
    // config

    uint32_t pixel_format;
    long width;
    long height;
    int64_t frame_duration;
    int64_t time_scale;
    uint32_t field_dominance;

    uint32_t audio_sample_rate;
    uint32_t audio_sample_depth;
    uint32_t audio_channels;
} BMDMemoryContext;

static AVStream *add_audio_stream(AVFormatContext *oc)
{
    AVCodecContext *c;
    AVStream *st;
    BMDMemoryContext *ctx = oc->priv_data;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    st->time_base  = (AVRational){1, ctx->audio_sample_rate};

    c              = st->codec;
    c->codec_type  = AVMEDIA_TYPE_AUDIO;
    c->sample_rate = 48000;
    c->channels    = ctx->audio_channels;

    switch (ctx->audio_sample_depth) {
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
               "%dbit audio is not supported\n",
               ctx->audio_sample_depth);
        return NULL;
    }

    c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc)
{
    AVCodecContext *c;
    AVStream *st;
    BMDMemoryContext *ctx = oc->priv_data;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    c                = st->codec;
    c->codec_type    = AVMEDIA_TYPE_VIDEO;

    c->width         = ctx->width;
    c->height        = ctx->height;

    st->time_base.num = ctx->frame_duration;
    st->time_base.den = ctx->time_scale;

    st->avg_frame_rate.den = ctx->frame_duration;
    st->avg_frame_rate.num = ctx->time_scale;

    switch (ctx->field_dominance) {
    case 'lowr':
        c->field_order = AV_FIELD_TT;
        break;
    case 'uppr':
        c->field_order = AV_FIELD_BB;
        break;
    case 'prog':
        c->field_order = AV_FIELD_PROGRESSIVE;
        break;
    default:
    case 0:
        c->field_order = AV_FIELD_UNKNOWN;
        break;
    }

    switch (ctx->pixel_format) {
    // YUV first
    case '2vuy':
        c->pix_fmt   = AV_PIX_FMT_UYVY422;
        c->codec_id  = AV_CODEC_ID_RAWVIDEO;
        c->codec_tag = avcodec_pix_fmt_to_codec_tag(c->pix_fmt);
    break;
    case 'v210':
        c->pix_fmt             = AV_PIX_FMT_YUV422P10;
        c->codec_id            = AV_CODEC_ID_V210;
        c->bits_per_raw_sample = 10;
    break;
    // RGB later
    default:
        av_log(oc, AV_LOG_ERROR, "Pixel format is not supported\n");
        return NULL;
    }

    c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    return st;
}

static int bmd_read_close(AVFormatContext *s)
{
    BMDMemoryContext *ctx = s->priv_data;

    ctx->done = 1;

    if (ctx->thread) {
        pthread_join(ctx->thread, NULL);
    }

    if (ctx->sem != SEM_FAILED) {
        if (sem_close(ctx->sem) == -1) {
            av_log(s, AV_LOG_ERROR, "Failed to close semaphore\n");
        }
    }

    if (ctx->shared_memory != MAP_FAILED) {
        if (munmap(ctx->shared_memory, sizeof(Memory)) == -1) {
            av_log(s, AV_LOG_ERROR, "Failed to unmap shared memory\n");
        }
        ctx->shared_memory = (Memory*)MAP_FAILED;
    }

    if (shared_memory_fd != -1) {
        if (close(shared_memory_fd) == -1) {
            av_log(s, AV_LOG_ERROR, "Failed to close memory\n");
        }
    }

    packet_queue_end(&ctx->q);

    return 0;
}

static int video_callback(BMDMemoryContext *ctx,
                          uint8_t *frame,
                          int width, int height, int stride,
                          int64_t timestamp,
                          int64_t duration,
                          int64_t flags)
{
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

static int audio_callback(BMDMemoryContext *ctx,
                          uint8_t *frame,
                          int nb_samples,
                          int64_t timestamp,
                          int64_t flags)
{
    AVCodecContext *c = ctx->audio_st->codec;
    AVPacket pkt;

    av_init_packet(&pkt);

    pkt.size          = nb_samples * c->channels *
                        (ctx->audio_sample_depth / 8);
    pkt.dts = pkt.pts = timestamp;
    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = ctx->audio_st->index;
    pkt.data          = frame;

    return packet_queue_put(&ctx->q, &pkt);
}

static void thread_proc(BMDMemoryContext *ctx)
{
    uint64_t video_pts;
    uint64_t audio_pts;

    while (!ctx->done) {
        sem_wait(ctx->sem);
        memcpy(&video_pts, ctx->shared_memory, sizeof(video_pts));
        memcpy(&audio_pts, ctx->shared_memory, sizeof(video_pts));
        sem_post(sem);

        if (video_pts > ctx->video_pts) {
            int64_t     duration;
            long        frame_width;
            long        frame_height;
            uint32_t    stride;
            uint32_t    data_size;
            uint8_t     *frame_data;
            uint32_t    offset = sizeof(video_pts);

            sem_wait(ctx->sem);

            memcpy(&duration, offset, sizeof(duration));
            offset += sizeof(duration);

            memcpy(&frame_width, offset, sizeof(frame_width));
            offset += sizeof(frame_width);

            memcpy(&frame_height, offset, sizeof(frame_height));
            offset += sizeof(frame_height);

            memcpy(&stride, offset, sizeof(stride));
            offset += sizeof(stride);

            memcpy(&data_size, offset, sizeof(data_size));
            offset += sizeof(data_size);

            frame_data = av_malloc(data_size);

            memcpy(frame_data, offset, data_size);

            sem_post(sem);

            video_callback(ctx,
                           frame_data,
                           frame_width,
                           frame_height,
                           stride,
                           video_pts,
                           duration,
                           0)
        }

        if (audio_pts > ctx->audio_pts) {
            long sample_frame_count;
            uint32_t    data_size;
            uint8_t     *frame_data;
            uint32_t    offset = sizeof(audio_pts);

            sem_wait(ctx->sem);

            memcpy(&sample_frame_count, offset, sizeof(sample_frame_count));
            offset += sizeof(sample_frame_count);

            memcpy(&data_size, offset, sizeof(data_size));
            offset += sizeof(data_size);

            frame_data = av_malloc(data_size);

            memcpy(frame_data, offset, data_size);

            sem_post(sem);

            audio_callback(ctx,
                           frame_data,
                           sample_frame_count,
                           audio_pts,
                           0)
        }
    }
}

static int bmd_read_header(AVFormatContext *s)
{
    BMDMemoryContext *ctx = s->priv_data;
    char sem_name[256];
    int ret;
    uint32_t offset = 0;

    ctx->shared_memory = (Memory*)MAP_FAILED;
    ctx->sem = SEM_FAILED;

    if ((ctx->shared_memory_fd = shm_open(ctx->memory_name, O_RDONLY , 0)) == -1) {
        av_log(s, AV_LOG_ERROR, "Failed to open shared memory\n");
        ret = AVERROR(EIO);
        goto out;
    }

    ctx->shared_memory = (Memory*)mmap(nullptr, sizeof(Memory), PROT_READ, MAP_SHARED, ctx->shared_memory_fd, 0);

    if (ctx->shared_memory == MAP_FAILED) {
        av_log(s, AV_LOG_ERROR, "Failed to open shared memory\n");
        ret = AVERROR(EIO);
        goto out;
    }

    sprintf(sem_name, sizeof(sem_name), "%s_sem", ctx->memory_name);

    if ((sem = sem_open(sem_name, 0)) == SEM_FAILED) {
        av_log(s, AV_LOG_ERROR, "Failed to open semaphore\n");
        ret = AVERROR(EIO);
        goto out;
    }

    if ((ret = packet_queue_init(&ctx->q)) < 0)
        return ret;

    sem_wait(ctx->sem);

    memcpy(sharedMemory->metaData + offset, &ctx->pixel_format, sizeof(ctx->pixel_format));
    offset += sizeof(pixelFormat);

    memcpy(sharedMemory->metaData + offset, &ctx->width, sizeof(ctx->width));
    offset += sizeof(width);

    memcpy(sharedMemory->metaData + offset, &ctx->height, sizeof(ctx->height));
    offset += sizeof(height);

    memcpy(sharedMemory->metaData + offset, &ctx->frame_duration, sizeof(ctx->frame_duration)); // numerator
    offset += sizeof(frameDuration);

    memcpy(sharedMemory->metaData + offset, &ctx->time_scale, sizeof(ctx->time_scale)); // denumerator
    offset += sizeof(timeScale);

    memcpy(sharedMemory->metaData + offset, &ctx->field_dominance, sizeof(ctx->field_dominance));
    offset += sizeof(fieldDominance);

    memcpy(sharedMemory->metaData + offset, &ctx->audio_sample_rate, sizeof(ctx->audio_sample_rate));
    offset += sizeof(audioSampleRate);

    memcpy(sharedMemory->metaData + offset, &ctx->audio_sample_depth, sizeof(ctx->audio_sample_depth));
    offset += sizeof(audioSampleDepth);

    memcpy(sharedMemory->metaData + offset, &ctx->audio_channels, sizeof(ctx->audio_channels));
    offset += sizeof(audioChannels);

    sem_post(ctx->sem);

    ctx->video_st = add_video_stream(s);
    ctx->audio_st = add_audio_stream(s);

    if (!ctx->video_st || !ctx->audio_st) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    if (pthread_create(&ctx->thread, NULL, thread_proc, ctx) != 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create thread\n");
        ret = AVERROR(ENOMEM);
        goto out;
    }    

    return 0;
out:
    bmd_read_close(s);
    return ret;
}

static int bmd_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    BMDMemoryContext *ctx = s->priv_data;

    return packet_queue_get(&ctx->q, pkt, 0);
}


#define O(x) offsetof(BMDMemoryContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "memory_name", "Memory name", O(memory_name), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, D },
    { NULL },
};

static const AVClass bmdmemory_class = {
    .class_name = "bmdmemory indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_evf_demuxer = {
    .name           = "bmdmemory",
    .long_name      = NULL_IF_CONFIG_SMALL("BMD memory decoder"),
    .priv_data_size = sizeof(BMDMemoryContext),
    .read_header    = bmd_read_header,
    .read_packet    = bmd_read_packet,
    .read_close     = bmd_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &bmd_class,
};
