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
 #include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavformat/internal.h"

#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <pthread.h>

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

// 64 MiB
#define MEMORY_SIZE (64 * 1024 * 1024)
#define METADATA_OFFSET (NAME_MAX + 1)
#define VIDEO_OFFSET (METADATA_OFFSET + 128)
// 40 MiB
#define AUDIO_OFFSET (VIDEO_OFFSET + 40 * 1024 * 1024)

typedef struct {
    const AVClass   *class;    /**< Class for private options. */
    char            *memory_name;
    int             shared_memory_fd;
    void            *shared_memory;
    uint8_t         *meta_data;
    uint8_t         *video_data;
    uint8_t         *audio_data;
    sem_t           *sem;
    PacketQueue     q;
    AVStream        *audio_st;
    AVStream        *video_st;
    uint64_t        video_ts;
    uint64_t        audio_ts;

    pthread_t       thread;
    int             done;
    // config

    uint32_t pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t frame_duration;
    uint32_t time_scale;
    uint32_t field_dominance;

    uint32_t audio_sample_rate;
    uint32_t audio_sample_depth;
    uint32_t audio_channels;
} BMDMemoryContext;

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

static AVStream *add_audio_stream(AVFormatContext *oc)
{
    AVCodecParameters *c;
    AVStream *st;
    BMDMemoryContext *ctx = oc->priv_data;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    st->time_base  = (AVRational){1, ctx->audio_sample_rate};

    c              = st->codecpar;
    c->codec_type  = AVMEDIA_TYPE_AUDIO;
    c->sample_rate = ctx->audio_sample_rate;
    c->channels    = ctx->audio_channels;

    switch (ctx->audio_sample_depth) {
    case 16:
        c->codec_id   = AV_CODEC_ID_PCM_S16LE;
    break;
    case 32:
        c->codec_id   = AV_CODEC_ID_PCM_S32LE;
    break;
    default:
        av_log(oc, AV_LOG_ERROR,
               "%dbit audio is not supported\n",
               ctx->audio_sample_depth);
        return NULL;
    }

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc)
{
    AVCodecParameters *c;
    AVStream *st;
    BMDMemoryContext *ctx = oc->priv_data;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    c                = st->codecpar;
    c->codec_type    = AVMEDIA_TYPE_VIDEO;

    c->width         = ctx->width;
    c->height        = ctx->height;

    st->time_base.den = ctx->time_scale;
    st->time_base.num = ctx->frame_duration;

    st->avg_frame_rate.num = ctx->time_scale;
    st->avg_frame_rate.den = ctx->frame_duration;

    switch (ctx->field_dominance) {
    case 1:
        c->field_order = AV_FIELD_TT;
        break;
    case 2:
        c->field_order = AV_FIELD_BB;
        break;
    case 3:
        c->field_order = AV_FIELD_PROGRESSIVE;
        break;
    default:
    case 0:
        c->field_order = AV_FIELD_UNKNOWN;
        break;
    }

    switch (ctx->pixel_format) {
    // YUV first
    case 0:
        c->codec_id  = AV_CODEC_ID_RAWVIDEO;
        c->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_UYVY422);
    break;
    case 1:
        c->codec_id            = AV_CODEC_ID_V210;
        c->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_YUV422P10);
    break;
    // RGB later
    default:
        av_log(oc, AV_LOG_ERROR, "Pixel format is not supported\n");
        return NULL;
    }

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
        if (munmap(ctx->shared_memory, MEMORY_SIZE) == -1) {
            av_log(s, AV_LOG_ERROR, "Failed to unmap shared memory\n");
        }
        ctx->shared_memory = MAP_FAILED;
        ctx->meta_data = NULL;
        ctx->video_data = NULL;
        ctx->audio_data = NULL;
    }

    if (ctx->shared_memory_fd != -1) {
        if (close(ctx->shared_memory_fd) == -1) {
            av_log(s, AV_LOG_ERROR, "Failed to close memory\n");
        }
    }

    packet_queue_end(&ctx->q);

    return 0;
}

static int add_video_packet(BMDMemoryContext *ctx,
                            AVBufferRef *buf,
                            int width, int height, int stride,
                            int64_t timestamp,
                            int64_t duration)
{
    AVPacket pkt;

    av_init_packet(&pkt);

    pkt.buf           = buf;
    pkt.data          = buf->data;
    pkt.size          = buf->size;

    pkt.pts = pkt.dts = timestamp / ctx->video_st->time_base.num;
    pkt.duration      = duration  / ctx->video_st->time_base.num;

    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = ctx->video_st->index;

    return packet_queue_put(&ctx->q, &pkt);
}

static int add_audio_packet(BMDMemoryContext *ctx,
                            AVBufferRef *buf,
                            int64_t timestamp)
{
    AVPacket pkt;

    av_init_packet(&pkt);

    pkt.buf           = buf;
    pkt.data          = buf->data;
    pkt.size          = buf->size;
    pkt.dts = pkt.pts = timestamp;
    pkt.flags        |= AV_PKT_FLAG_KEY;
    pkt.stream_index  = ctx->audio_st->index;

    return packet_queue_put(&ctx->q, &pkt);
}

static void* thread_proc(void *arg)
{
    AVFormatContext *s = arg;
    BMDMemoryContext *ctx = s->priv_data;

    uint32_t    offset = 0;

    uint64_t    video_ts;
    uint32_t    video_duration;
    uint32_t    video_frame_width;
    uint32_t    video_frame_height;
    uint32_t    video_stride;
    uint32_t    video_data_size;
    AVBufferRef *video_buf;

    uint64_t    audio_ts;
    uint32_t    audio_sample_frame_count;
    uint32_t    audio_data_size;
    AVBufferRef *audio_buf;

    while (!ctx->done) {
        video_buf = NULL;
        audio_buf = NULL;

        sem_wait(ctx->sem);

        memcpy(&video_ts, ctx->video_data, sizeof(video_ts));
        
        if (video_ts > ctx->video_ts) {
            ctx->video_ts = video_ts;

            offset = sizeof(video_ts);

            memcpy(&video_duration, ctx->video_data + offset, sizeof(video_duration));
            offset += sizeof(video_duration);

            memcpy(&video_frame_width, ctx->video_data + offset, sizeof(video_frame_width));
            offset += sizeof(video_frame_width);

            memcpy(&video_frame_height, ctx->video_data + offset, sizeof(video_frame_height));
            offset += sizeof(video_frame_height);

            memcpy(&video_stride, ctx->video_data + offset, sizeof(video_stride));
            offset += sizeof(video_stride);

            memcpy(&video_data_size, ctx->video_data + offset, sizeof(video_data_size));
            offset += sizeof(video_data_size);

            video_buf = av_buffer_alloc(video_data_size);

            if (!video_buf) {
                sem_post(ctx->sem);
                av_log(s, AV_LOG_ERROR, "Failed to create buffer\n");
                //return AVERROR(ENOMEM);
                return NULL;
            }

            memcpy(video_buf->data, ctx->video_data + offset, video_data_size);
        }

        memcpy(&audio_ts, ctx->audio_data, sizeof(audio_ts));

        if (audio_ts > ctx->audio_ts) {
            ctx->audio_ts = audio_ts;

            offset = sizeof(audio_ts);

            memcpy(&audio_sample_frame_count, ctx->audio_data + offset, sizeof(audio_sample_frame_count));
            offset += sizeof(audio_sample_frame_count);

            memcpy(&audio_data_size, ctx->audio_data + offset, sizeof(audio_data_size));
            offset += sizeof(audio_data_size);

            audio_buf = av_buffer_alloc(audio_data_size);

            if (!audio_buf) {
                sem_post(ctx->sem);
                av_log(s, AV_LOG_ERROR, "Failed to create buffer\n");
                //return AVERROR(ENOMEM);
                return NULL;
            }

            memcpy(audio_buf->data, ctx->audio_data + offset, audio_data_size);
        }

        sem_post(ctx->sem);

        if (video_buf) {
            add_video_packet(ctx,
                             video_buf,
                             video_frame_width,
                             video_frame_height,
                             video_stride,
                             video_ts,
                             video_duration);
        }

        if (audio_buf) {
            add_audio_packet(ctx,
                             audio_buf,
                             audio_ts);
        }

        // sleep for half the frame interval
        av_usleep(500 * 1000 * ctx->frame_duration / ctx->time_scale);
    }

    return NULL;
}

static int bmd_read_header(AVFormatContext *s)
{
    BMDMemoryContext *ctx = s->priv_data;
    int ret;
    uint32_t offset = 0;

    ctx->shared_memory = MAP_FAILED;
    ctx->sem = SEM_FAILED;

    if ((ret = packet_queue_init(&ctx->q)) < 0)
        return ret;

    if ((ctx->shared_memory_fd = shm_open(ctx->memory_name, O_RDONLY , 0)) == -1) {
        av_log(s, AV_LOG_ERROR, "Failed to open shared memory\n");
        ret = AVERROR(EIO);
        goto out;
    }

    ctx->shared_memory = mmap(NULL, MEMORY_SIZE, PROT_READ, MAP_SHARED, ctx->shared_memory_fd, 0);

    if (ctx->shared_memory == MAP_FAILED) {
        av_log(s, AV_LOG_ERROR, "Failed to open shared memory\n");
        ret = AVERROR(EIO);
        goto out;
    }

    ctx->meta_data = ((uint8_t*)ctx->shared_memory) + METADATA_OFFSET;
    ctx->video_data = ((uint8_t*)ctx->shared_memory) + VIDEO_OFFSET;
    ctx->audio_data = ((uint8_t*)ctx->shared_memory) + AUDIO_OFFSET;

    if ((ctx->sem = sem_open((const char*)ctx->shared_memory, 0)) == SEM_FAILED) {
        av_log(s, AV_LOG_ERROR, "Failed to open semaphore\n");
        ret = AVERROR(EIO);
        goto out;
    }

    if ((ret = packet_queue_init(&ctx->q)) < 0)
        return ret;

    sem_wait(ctx->sem);

    memcpy(&ctx->pixel_format, ctx->meta_data + offset, sizeof(ctx->pixel_format));
    offset += sizeof(ctx->pixel_format);

    memcpy(&ctx->width, ctx->meta_data + offset, sizeof(ctx->width));
    offset += sizeof(ctx->width);

    memcpy(&ctx->height, ctx->meta_data + offset, sizeof(ctx->height));
    offset += sizeof(ctx->height);

    memcpy(&ctx->frame_duration, ctx->meta_data + offset, sizeof(ctx->frame_duration)); // numerator
    offset += sizeof(ctx->frame_duration);

    memcpy(&ctx->time_scale, ctx->meta_data + offset, sizeof(ctx->time_scale)); // denumerator
    offset += sizeof(ctx->time_scale);

    memcpy(&ctx->field_dominance, ctx->meta_data + offset, sizeof(ctx->field_dominance));
    offset += sizeof(ctx->field_dominance);

    memcpy(&ctx->audio_sample_rate, ctx->meta_data + offset, sizeof(ctx->audio_sample_rate));
    offset += sizeof(ctx->audio_sample_rate);

    memcpy(&ctx->audio_sample_depth, ctx->meta_data + offset, sizeof(ctx->audio_sample_depth));
    offset += sizeof(ctx->audio_sample_depth);

    memcpy(&ctx->audio_channels, ctx->meta_data + offset, sizeof(ctx->audio_channels));
    offset += sizeof(ctx->audio_channels);

    sem_post(ctx->sem);

    ctx->video_st = add_video_stream(s);
    ctx->audio_st = add_audio_stream(s);

    if (!ctx->video_st || !ctx->audio_st) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    if (pthread_create(&ctx->thread, NULL, &thread_proc, s) != 0) {
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

AVInputFormat ff_bmdmemory_demuxer = {
    .name           = "bmdmemory",
    .long_name      = NULL_IF_CONFIG_SMALL("BMD memory decoder"),
    .priv_data_size = sizeof(BMDMemoryContext),
    .read_header    = bmd_read_header,
    .read_packet    = bmd_read_packet,
    .read_close     = bmd_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &bmdmemory_class,
};
