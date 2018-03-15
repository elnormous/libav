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
#include "libavutil/rational.h"
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

    // options
    char*           vfile;
    int             wallclock;
    int64_t         queue_size;

    // decoded
    AVStream*       decoded_v_stream;
    AVStream*       decoded_a_stream;
    AVFrame**       v_frames;
    int             v_buf_size;
    int             v_frame_cnt;
    AVFrame**       a_frames;
    int             a_buf_size;
    int             a_frame_cnt;

    pthread_t       video_thread;
    int             stop_threads;

    PacketQueue     q;
    AVStream        *audio_st;
    AVStream        *video_st;
    AVStream        *data_st;

    int64_t         start_time;
} SContext;

static enum AVCodecID av_get_pcm_codec(enum AVSampleFormat fmt, int be)
{
     static const enum AVCodecID map[AV_SAMPLE_FMT_NB][2] = {
         [AV_SAMPLE_FMT_U8  ] = { AV_CODEC_ID_PCM_U8,    AV_CODEC_ID_PCM_U8    },
         [AV_SAMPLE_FMT_S16 ] = { AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_PCM_S16BE },
         [AV_SAMPLE_FMT_S32 ] = { AV_CODEC_ID_PCM_S32LE, AV_CODEC_ID_PCM_S32BE },
         [AV_SAMPLE_FMT_FLT ] = { AV_CODEC_ID_PCM_F32LE, AV_CODEC_ID_PCM_F32BE },
         [AV_SAMPLE_FMT_DBL ] = { AV_CODEC_ID_PCM_F64LE, AV_CODEC_ID_PCM_F64BE },
         [AV_SAMPLE_FMT_U8P ] = { AV_CODEC_ID_PCM_U8,    AV_CODEC_ID_PCM_U8    },
         [AV_SAMPLE_FMT_S16P] = { AV_CODEC_ID_PCM_S16LE, AV_CODEC_ID_PCM_S16BE },
         [AV_SAMPLE_FMT_S32P] = { AV_CODEC_ID_PCM_S32LE, AV_CODEC_ID_PCM_S32BE },
         [AV_SAMPLE_FMT_FLTP] = { AV_CODEC_ID_PCM_F32LE, AV_CODEC_ID_PCM_F32BE },
         [AV_SAMPLE_FMT_DBLP] = { AV_CODEC_ID_PCM_F64LE, AV_CODEC_ID_PCM_F64BE },
     };
     if (fmt < 0 || fmt >= AV_SAMPLE_FMT_NB)
         return AV_CODEC_ID_NONE;
     if (be < 0 || be > 1)
         be = AV_NE(1, 0);
     return map[fmt][be];
}

static AVStream *add_audio_stream(AVFormatContext *oc)
{
    SContext *ctx = oc->priv_data;
    AVCodecParameters *dc = ctx->decoded_a_stream->codecpar;
    AVCodecParameters *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    st->time_base  = (AVRational){1, ctx->decoded_a_stream->time_base.den};

    av_log(NULL, AV_LOG_INFO, "ATB: %d %d | SR: %d \n", ctx->decoded_a_stream->time_base.num, ctx->decoded_a_stream->time_base.den, dc->sample_rate);

    c              = st->codecpar;
    c->codec_type  = AVMEDIA_TYPE_AUDIO;

    c->sample_rate = dc->sample_rate / dc->channels;
    c->channels    = dc->channels;

    c->format   = dc->format;
    c->codec_id = av_get_pcm_codec(dc->format, 0);

    av_log(NULL, AV_LOG_INFO, "Format: %d\n", dc->format);

    return st;
}

static AVStream *add_data_stream(AVFormatContext *oc)
{
    SContext *ctx = oc->priv_data;
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

    st->time_base = ctx->video_st->time_base;
    st->avg_frame_rate = ctx->video_st->avg_frame_rate;

    return st;
}

static AVStream *add_video_stream(AVFormatContext *oc)
{
    SContext *ctx = oc->priv_data;
    AVCodecParameters *dc = ctx->decoded_v_stream->codecpar;

    AVCodecParameters *c;
    AVStream *st;

    st = avformat_new_stream(oc, NULL);
    if (!st)
        return NULL;

    c                = st->codecpar;
    c->codec_type    = AVMEDIA_TYPE_VIDEO;

    c->width         = dc->width;
    c->height        = dc->height;

    st->time_base    = ctx->decoded_v_stream->time_base;
    st->avg_frame_rate = ctx->decoded_v_stream->avg_frame_rate;

    c->field_order = dc->field_order;
    c->format    = dc->format;
    c->codec_id  = AV_CODEC_ID_RAWVIDEO;
    c->codec_tag = avcodec_pix_fmt_to_codec_tag(c->format);

    return st;
}

static int simulator_read_close(AVFormatContext *s)
{
    SContext *ctx = s->priv_data;

    ctx->stop_threads = 1;

    pthread_join(ctx->video_thread, NULL);

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

static void* video_thread(void *priv)
{
    SContext *ctx = priv;
    uint64_t v_frame_nr = 0, a_frame_nr = 0;
    int64_t v_pts = 0, a_pts = 0;
    int64_t start_time = av_gettime();
    uint64_t video_prev_pts = 0, audio_prev_pts = 0;

    {
        int64_t* time_data;

        ctx->start_time = av_gettime() / 1000;

        av_log(ctx, AV_LOG_INFO, "simulator start time: %lld\n", ctx->start_time);

        time_data = (int64_t*)av_stream_new_side_data(ctx->video_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
        if (time_data) {
            *time_data = ctx->start_time;
        }
        if (ctx->audio_st) {
            time_data = (int64_t*)av_stream_new_side_data(ctx->audio_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
            if (time_data) {
                *time_data = ctx->start_time;
            }

        }
        if (ctx->data_st) {
            time_data = (int64_t*)av_stream_new_side_data(ctx->data_st, AV_PKT_DATA_STREAM_START_TIME, sizeof(int64_t));
            if (time_data) {
                *time_data = ctx->start_time;
            }
        }
    }

    while (!ctx->stop_threads) {
        int64_t ctime = av_gettime();
        int produced = 0;

        v_pts = (ctime - start_time) * ctx->video_st->time_base.den / ctx->video_st->time_base.num / 1000000;

        // should add video frame
        while (v_frame_nr < ctx->v_frame_cnt && ctx->v_frames[v_frame_nr]->pts <= v_pts)
        {
            AVPacket pkt;
            AVFrame* frame = ctx->v_frames[v_frame_nr++];
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
            int ret, i, psize = 0;
            uint64_t copied = 0;

            for (i = 0; i < 4 && frame->linesize[i]; i++) {
                int h = frame->height;
                if (i == 1 || i == 2) h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);
                psize += frame->linesize[i] * h;
            }

            ret = av_new_packet(&pkt, psize);
            if (ret != 0) {
                av_log(ctx, AV_LOG_ERROR, "No memory?");
                return NULL;
            }

            for (i = 0; i < 4 && frame->linesize[i]; i++) {
                int h = frame->height;
                if (i == 1 || i == 2) h = AV_CEIL_RSHIFT(h, desc->log2_chroma_h);

                memcpy(&pkt.data[copied], frame->data[i], frame->linesize[i] * h);
                copied += frame->linesize[i] * h;
            }

            pkt.pts = pkt.dts = video_prev_pts + frame->pts;
            pkt.duration      = 1;

            pkt.flags        |= AV_PKT_FLAG_KEY;
            pkt.stream_index  = ctx->video_st->index;

            if (ctx->wallclock) {
                put_wallclock_packet(ctx, pkt.pts);
            }

            if (packet_queue_put(&ctx->q, &pkt, ctx->queue_size) != 0) {
                av_log(NULL, AV_LOG_WARNING, "no space in queue, video frame dropped.\n");
                ctx->video_st->dropped_frames++;
            }

            produced = 1;
        }

        if (ctx->audio_st) {
            a_pts = (ctime - start_time) * ctx->audio_st->time_base.den / ctx->audio_st->time_base.num / 1000000;
            while (a_frame_nr < ctx->a_frame_cnt && ctx->a_frames[a_frame_nr]->pts <= a_pts) {
                AVPacket pkt;
                int ret;

                AVFrame* frame = ctx->a_frames[a_frame_nr++];

                int sz = av_get_bytes_per_sample(frame->format) * frame->nb_samples;

                ret = av_new_packet(&pkt, sz);
                if (ret != 0) {
                    av_log(ctx, AV_LOG_ERROR, "No memory?");
                    return NULL;
                }

                memcpy(pkt.buf->data, frame->data[0], sz);

                pkt.dts = pkt.pts = audio_prev_pts + frame->pts;// a_samples;
                pkt.flags        |= AV_PKT_FLAG_KEY;
                pkt.stream_index  = ctx->audio_st->index;

                if (packet_queue_put(&ctx->q, &pkt, ctx->queue_size) != 0) {
                    av_log(NULL, AV_LOG_WARNING, "no space in queue, audio frame dropped.\n");
                    ctx->audio_st->dropped_frames++;
                }

                produced = 1;
            }
        }

        if (v_frame_nr >= ctx->v_frame_cnt)
        {
            video_prev_pts += ctx->v_frames[ctx->v_frame_cnt-1]->pts + // add one frame
                (ctx->video_st->time_base.den * ctx->video_st->avg_frame_rate.den) / (ctx->video_st->time_base.num * ctx->video_st->avg_frame_rate.num);
            if (ctx->audio_st) {
                audio_prev_pts = video_prev_pts * ctx->video_st->time_base.num * ctx->audio_st->time_base.den / ctx->video_st->time_base.den / ctx->audio_st->time_base.num;
            }
            start_time = av_gettime();
            v_frame_nr = 0;
            a_frame_nr = 0;
        }

        if (!produced) {
            av_usleep(5000);
            continue;
        }
    }

    return NULL;
}

static int decode_packet(AVPacket* packet,
                         AVCodecContext* codec_ctx,
                         AVFrame*** frame_buffer,
                         int* buffer_size,
                         int* frame_cnt)
{
    int dret = 0;
    dret = avcodec_send_packet(codec_ctx, packet);
    if (dret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    while (dret >= 0) {
        AVFrame* frame = av_frame_alloc();
        dret = avcodec_receive_frame(codec_ctx, frame);
        if (dret == AVERROR(EAGAIN) || dret == AVERROR_EOF) {
            av_frame_unref(frame);
            break;
        } else if (dret < 0) {
            char errbuf[1024];
            av_strerror(dret, errbuf, sizeof(errbuf));
            av_log(NULL, AV_LOG_ERROR, "Decoding error: %s\n", errbuf);
            exit(1);
        }

        if (*frame_cnt == *buffer_size) {
            AVFrame** new_buffer = av_realloc(*frame_buffer, (*buffer_size + 100) * sizeof(AVFrame*));
            if (!new_buffer) {
                av_freep(new_buffer);
                return AVERROR(ENOMEM);
            }
            *buffer_size += 100;
            *frame_buffer = new_buffer;
        }

        (*frame_buffer)[*frame_cnt] = frame;
        (*frame_cnt)++;
    }

    return 0;
}

static int loadVideoFrames(SContext *ctx)
{
    int video_stream_indx = -1, audio_stream_indx = -1, ret;

    AVCodec *video_codec = NULL, *audio_codec = NULL;
    AVCodecContext *video_codec_ctx = NULL, *audio_codec_ctx = NULL;
    AVStream *video_stream = NULL, *audio_stream = NULL;
    AVPacket packet;
    char errbuf[1024];

    AVFormatContext* fmtCtx = avformat_alloc_context();

    if (ctx->vfile == NULL) return AVERROR(EIO);


    ret = avformat_open_input(&fmtCtx, ctx->vfile, NULL, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(ctx, AV_LOG_ERROR, "error opening input: %s\n", errbuf);
        return AVERROR(EIO);
    }

    ret = avformat_find_stream_info(fmtCtx, NULL);
    if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        av_log(ctx, AV_LOG_ERROR, "failed to find stream info %s\n", errbuf);
        return AVERROR(EIO);
    }

    av_log(ctx, AV_LOG_INFO, "Streams: %d\n", fmtCtx->nb_streams);

    video_stream_indx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &video_codec, 0);
    if (video_stream_indx < 0) {
        if (video_stream_indx == AVERROR_STREAM_NOT_FOUND) {
            av_log(ctx, AV_LOG_ERROR, "video stream could not be found\n");
        } else if (video_stream_indx == AVERROR_DECODER_NOT_FOUND) {
            av_log(ctx, AV_LOG_ERROR, "video decoder could not be found\n");
        }
        return AVERROR(EIO);
    } else {
        video_stream = fmtCtx->streams[video_stream_indx];
        video_codec_ctx = video_stream->codec;
        if (avcodec_open2(video_codec_ctx, video_codec, NULL) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to open decoder for stream #%u in file '%s'\n", video_stream_indx, ctx->vfile);
            return AVERROR(EIO);
        }
        ctx->decoded_v_stream = video_stream;
        av_log(ctx, AV_LOG_INFO, "FPS: %f\n", ((double)video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den));
    }

    audio_stream_indx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_codec, 0);
    if (audio_stream_indx < 0) {
        if (audio_stream_indx == AVERROR_STREAM_NOT_FOUND) {
            av_log(ctx, AV_LOG_ERROR, "audio stream could not be found\n");
        } else if (audio_stream_indx == AVERROR_DECODER_NOT_FOUND) {
            av_log(ctx, AV_LOG_ERROR, "audio decoder could not be found\n");
        }
    } else {
        audio_stream = fmtCtx->streams[audio_stream_indx];
        audio_codec_ctx = audio_stream->codec;
        if (avcodec_open2(audio_codec_ctx, audio_codec, NULL) < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to open decoder for stream #%u in file '%s'\n", audio_stream_indx, ctx->vfile);
            return AVERROR(EIO);
        }
        ctx->decoded_a_stream = audio_stream;
    }

    ctx->v_buf_size = 0;
    ctx->v_frame_cnt = 0;
    ctx->v_frames = NULL;

    ctx->a_buf_size = 0;
    ctx->a_frame_cnt = 0;
    ctx->a_frames = NULL;

    av_init_packet(&packet);

    for (;;) {
        AVPacket* toSend = NULL;
        int ret = av_read_frame(fmtCtx, &packet);

        if (ret == 0) { // go once more to flush decoders
            toSend = &packet;
        }

        if (packet.stream_index == video_stream_indx || toSend == NULL) {
            int r = decode_packet(toSend, video_codec_ctx, &ctx->v_frames, &ctx->v_buf_size, &ctx->v_frame_cnt);
            if (r != 0) return r;
        }

        if (packet.stream_index == audio_stream_indx || (toSend == NULL && audio_codec_ctx)) {
            int r = decode_packet(toSend, audio_codec_ctx, &ctx->a_frames, &ctx->a_buf_size, &ctx->a_frame_cnt);
            if (r != 0) return r;
        }

        av_packet_unref(&packet);

        if (ret < 0) {
            av_strerror(ret, errbuf, sizeof(errbuf));
            av_log(ctx, AV_LOG_INFO, "Decoding finished: %s\n", errbuf);
            break;
        }
    }

    av_log(ctx, AV_LOG_INFO, "Video frames: %d, Audio frames: %d\n", ctx->v_frame_cnt, ctx->a_frame_cnt);

    return 0;
}

static int simulator_read_header(AVFormatContext *s)
{
    SContext *ctx = s->priv_data;
    int ret = 0;

    ctx->stop_threads = 0;

    if ((ret = packet_queue_init(&ctx->q)) < 0) goto out;

    if ((ret = loadVideoFrames(ctx)) < 0) goto out;

    if (ctx->decoded_v_stream)
        ctx->video_st = add_video_stream(s);
    if (ctx->decoded_a_stream)
        ctx->audio_st = add_audio_stream(s);
    if (ctx->wallclock)
        ctx->data_st  = add_data_stream(s);

    if (!ctx->video_st) {
        ret = AVERROR(ENOMEM);
        goto out;
    }

    ctx->start_time = AV_NOPTS_VALUE;

    pthread_create(&ctx->video_thread, NULL, &video_thread, (void*)ctx);

    return 0;
out:
    simulator_read_close(s);
    return ret;
}

static int simulator_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    SContext *ctx = s->priv_data;

    if (ctx->stop_threads)
        return AVERROR_EOF;

    return packet_queue_get(&ctx->q, pkt, 0);
}

#define OC(x) offsetof(SContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "queue_size",       "Packet queue size, 0 to disable the queue limit",  OC(queue_size),       AV_OPT_TYPE_INT64, {.i64 = 25}, 0, INT_MAX, D },
    { "video",     "Video file to repeat", OC(vfile), AV_OPT_TYPE_STRING, {.str = 0}, 0, 0, D },
    { "wallclock", "Add the wallclock",    OC(wallclock), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, D },
    { NULL },
};

static const AVClass simulator_class = {
    .class_name = "simulator indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_simulator_demuxer = {
    .name           = "simulator",
    .long_name      = NULL_IF_CONFIG_SMALL("Input stream simulator"),
    .priv_data_size = sizeof(SContext),
    .read_header    = simulator_read_header,
    .read_packet    = simulator_read_packet,
    .read_close     = simulator_read_close,
    .flags          = AVFMT_NOFILE,
    .priv_class     = &simulator_class,
};
