/*
 * FLV muxer
 * Copyright (c) 2003 The Libav Project
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/dict.h"
#include "libavutil/intfloat.h"
#include "libavutil/mathematics.h"
#include "avc.h"
#include "avformat.h"
#include "flv.h"
#include "internal.h"
#include "metadata.h"
#include "libavutil/opt.h"

#undef NDEBUG
#include <assert.h>
#include <sys/time.h>
#include <stdint.h>
#include <inttypes.h>

static const AVCodecTag flv_video_codec_ids[] = {
    { AV_CODEC_ID_FLV1,     FLV_CODECID_H263 },
    { AV_CODEC_ID_FLASHSV,  FLV_CODECID_SCREEN },
    { AV_CODEC_ID_FLASHSV2, FLV_CODECID_SCREEN2 },
    { AV_CODEC_ID_VP6F,     FLV_CODECID_VP6 },
    { AV_CODEC_ID_VP6A,     FLV_CODECID_VP6A },
    { AV_CODEC_ID_H264,     FLV_CODECID_H264 },
    { AV_CODEC_ID_NONE,     0 }
};

static const AVCodecTag flv_audio_codec_ids[] = {
    { AV_CODEC_ID_MP3,        FLV_CODECID_MP3        >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_PCM_U8,     FLV_CODECID_PCM        >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_PCM_S16BE,  FLV_CODECID_PCM        >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_PCM_S16LE,  FLV_CODECID_PCM_LE     >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_ADPCM_SWF,  FLV_CODECID_ADPCM      >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_AAC,        FLV_CODECID_AAC        >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_NELLYMOSER, FLV_CODECID_NELLYMOSER >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_PCM_MULAW,  FLV_CODECID_PCM_MULAW  >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_PCM_ALAW,   FLV_CODECID_PCM_ALAW   >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_SPEEX,      FLV_CODECID_SPEEX      >> FLV_AUDIO_CODECID_OFFSET },
    { AV_CODEC_ID_NONE,       0 }
};

typedef struct FLVContext {
    int     reserved;
    int64_t duration_offset;
    int64_t filesize_offset;
    int64_t duration;
    int64_t delay;      ///< first dts delay (needed for AVC & Speex)

    AVCodecParameters *audio_par;
    AVCodecParameters *video_par;
    double framerate;
    AVCodecContext *data_enc;
    int pts_mod;
    int64_t stop_time;
    AVCodecParameters *data_par;
} FLVContext;

typedef struct FLVStreamContext {
    int64_t last_ts;    ///< last timestamp for each stream
} FLVStreamContext;

static int get_audio_flags(AVFormatContext *s, AVCodecParameters *par)
{
    int flags = (par->bits_per_coded_sample == 16) ? FLV_SAMPLESSIZE_16BIT
                                                   : FLV_SAMPLESSIZE_8BIT;

    if (par->codec_id == AV_CODEC_ID_AAC) // specs force these parameters
        return FLV_CODECID_AAC | FLV_SAMPLERATE_44100HZ |
               FLV_SAMPLESSIZE_16BIT | FLV_STEREO;
    else if (par->codec_id == AV_CODEC_ID_SPEEX) {
        if (par->sample_rate != 16000) {
            av_log(s, AV_LOG_ERROR,
                   "flv only supports wideband (16kHz) Speex audio\n");
            return -1;
        }
        if (par->channels != 1) {
            av_log(s, AV_LOG_ERROR, "flv only supports mono Speex audio\n");
            return -1;
        }
        return FLV_CODECID_SPEEX | FLV_SAMPLERATE_11025HZ | FLV_SAMPLESSIZE_16BIT;
    } else {
        switch (par->sample_rate) {
        case 44100:
            flags |= FLV_SAMPLERATE_44100HZ;
            break;
        case 22050:
            flags |= FLV_SAMPLERATE_22050HZ;
            break;
        case 11025:
            flags |= FLV_SAMPLERATE_11025HZ;
            break;
        case 16000: // nellymoser only
        case  8000: // nellymoser only
        case  5512: // not MP3
            if (par->codec_id != AV_CODEC_ID_MP3) {
                flags |= FLV_SAMPLERATE_SPECIAL;
                break;
            }
        default:
            av_log(s, AV_LOG_ERROR,
                   "flv does not support that sample rate, "
                   "choose from (44100, 22050, 11025).\n");
            return -1;
        }
    }

    if (par->channels > 1)
        flags |= FLV_STEREO;

    switch (par->codec_id) {
    case AV_CODEC_ID_MP3:
        flags |= FLV_CODECID_MP3    | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_PCM_U8:
        flags |= FLV_CODECID_PCM    | FLV_SAMPLESSIZE_8BIT;
        break;
    case AV_CODEC_ID_PCM_S16BE:
        flags |= FLV_CODECID_PCM    | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_PCM_S16LE:
        flags |= FLV_CODECID_PCM_LE | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_ADPCM_SWF:
        flags |= FLV_CODECID_ADPCM  | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_NELLYMOSER:
        if (par->sample_rate == 8000)
            flags |= FLV_CODECID_NELLYMOSER_8KHZ_MONO  | FLV_SAMPLESSIZE_16BIT;
        else if (par->sample_rate == 16000)
            flags |= FLV_CODECID_NELLYMOSER_16KHZ_MONO | FLV_SAMPLESSIZE_16BIT;
        else
            flags |= FLV_CODECID_NELLYMOSER            | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_PCM_MULAW:
        flags = FLV_CODECID_PCM_MULAW | FLV_SAMPLERATE_SPECIAL | FLV_SAMPLESSIZE_16BIT;
        break;
    case AV_CODEC_ID_PCM_ALAW:
        flags = FLV_CODECID_PCM_ALAW  | FLV_SAMPLERATE_SPECIAL | FLV_SAMPLESSIZE_16BIT;
        break;
    case 0:
        flags |= par->codec_tag << 4;
        break;
    default:
        av_log(s, AV_LOG_ERROR, "codec not compatible with flv\n");
        return -1;
    }

    return flags;
}

static void put_amf_string(AVIOContext *pb, const char *str)
{
    size_t len = strlen(str);
    avio_wb16(pb, len);
    avio_write(pb, str, len);
}

static void put_avc_eos_tag(AVIOContext *pb, unsigned ts)
{
    avio_w8(pb, FLV_TAG_TYPE_VIDEO);
    avio_wb24(pb, 5);               /* Tag Data Size */
    avio_wb24(pb, ts);              /* lower 24 bits of timestamp in ms */
    avio_w8(pb, (ts >> 24) & 0x7F); /* MSB of ts in ms */
    avio_wb24(pb, 0);               /* StreamId = 0 */
    avio_w8(pb, 23);                /* ub[4] FrameType = 1, ub[4] CodecId = 7 */
    avio_w8(pb, 2);                 /* AVC end of sequence */
    avio_wb24(pb, 0);               /* Always 0 for AVC EOS. */
    avio_wb32(pb, 16);              /* Size of FLV tag */
}

static void put_amf_double(AVIOContext *pb, double d)
{
    avio_w8(pb, AMF_DATA_TYPE_NUMBER);
    avio_wb64(pb, av_double2int(d));
}

static void put_amf_bool(AVIOContext *pb, int b)
{
    avio_w8(pb, AMF_DATA_TYPE_BOOL);
    avio_w8(pb, !!b);
}

static void write_metadata(AVFormatContext *s, unsigned int ts)
{
    AVIOContext *pb = s->pb;
    FLVContext *flv = s->priv_data;
    int metadata_count = 0;
    int64_t metadata_size_pos, data_size, metadata_count_pos;
    AVDictionaryEntry *tag = NULL;
    int i;

    /* write meta_tag */
    avio_w8(pb, 18);            // tag type META
    metadata_size_pos = avio_tell(pb);
    avio_wb24(pb, 0);           // size of data part (sum of all parts below)
    avio_wb24(pb, ts);          // timestamp
    avio_wb32(pb, 0);           // reserved

    /* now data of data_size size */

    /* first event name as a string */
    avio_w8(pb, AMF_DATA_TYPE_STRING);
    put_amf_string(pb, "onMetaData"); // 12 bytes

    /* mixed array (hash) with size and string/type/data tuples */
    avio_w8(pb, AMF_DATA_TYPE_MIXEDARRAY);
    metadata_count_pos = avio_tell(pb);
    metadata_count = 4 * !!flv->video_par +
                     5 * !!flv->audio_par +
                     1 * !!flv->data_par  +
                     2; // +2 for duration and file size

    avio_wb32(pb, metadata_count);

    put_amf_string(pb, "duration");
    flv->duration_offset = avio_tell(pb);

    // fill in the guessed duration, it'll be corrected later if incorrect
    put_amf_double(pb, s->duration / AV_TIME_BASE);

    if (flv->video_par) {
        put_amf_string(pb, "width");
        put_amf_double(pb, flv->video_par->width);

        put_amf_string(pb, "height");
        put_amf_double(pb, flv->video_par->height);

        put_amf_string(pb, "videodatarate");
        put_amf_double(pb, flv->video_par->bit_rate / 1024.0);

        if (flv->framerate != 0.0) {
            put_amf_string(pb, "framerate");
            put_amf_double(pb, flv->framerate);
            metadata_count++;

            put_amf_string(pb, "fps");
            put_amf_double(pb, flv->framerate);
            metadata_count++;
        }

        put_amf_string(pb, "videocodecid");
        put_amf_double(pb, flv->video_par->codec_tag);
    }

    if (flv->audio_par) {
        put_amf_string(pb, "audiodatarate");
        put_amf_double(pb, flv->audio_par->bit_rate / 1024.0);

        put_amf_string(pb, "audiosamplerate");
        put_amf_double(pb, flv->audio_par->sample_rate);

        put_amf_string(pb, "audiosamplesize");
        put_amf_double(pb, flv->audio_par->codec_id == AV_CODEC_ID_PCM_U8 ? 8 : 16);

        put_amf_string(pb, "stereo");
        put_amf_bool(pb, flv->audio_par->channels == 2);

        put_amf_string(pb, "audiocodecid");
        put_amf_double(pb, flv->audio_par->codec_tag);
    }

    if (flv->data_par) {
        put_amf_string(pb, "datastream");
        put_amf_double(pb, 0.0);
    }

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecContext *enc = s->streams[i]->codec;
        if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            put_amf_string(pb, "gopsize");
            put_amf_double(pb, enc->gop_size);
            metadata_count++;
        }
    }

    while ((tag = av_dict_get(s->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        put_amf_string(pb, tag->key);
        avio_w8(pb, AMF_DATA_TYPE_STRING);
        put_amf_string(pb, tag->value);
        metadata_count++;
    }

    put_amf_string(pb, "filesize");
    flv->filesize_offset = avio_tell(pb);
    put_amf_double(pb, 0); // delayed write

    put_amf_string(pb, "");
    avio_w8(pb, AMF_END_OF_OBJECT);

    /* write total size of tag */
    data_size = avio_tell(pb) - metadata_size_pos - 10;

    avio_seek(pb, metadata_count_pos, SEEK_SET);
    avio_wb32(pb, metadata_count);

    avio_seek(pb, metadata_size_pos, SEEK_SET);
    avio_wb24(pb, data_size);
    avio_skip(pb, data_size + 10 - 3);
    avio_wb32(pb, data_size + 11);
}

static int unsupported_codec(AVFormatContext *s,
                             const char* type, int codec_id)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec_id);
    av_log(s, AV_LOG_ERROR,
           "%s codec %s not compatible with flv\n",
            type,
            desc ? desc->name : "unknown");
    return AVERROR(ENOSYS);
}

static int flv_write_header(AVFormatContext *s)
{
    int i;
    AVIOContext *pb = s->pb;
    FLVContext *flv = s->priv_data;
    int64_t data_size;

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        FLVStreamContext *sc;
        switch (par->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (s->streams[i]->avg_frame_rate.den &&
                s->streams[i]->avg_frame_rate.num) {
                flv->framerate = av_q2d(s->streams[i]->avg_frame_rate);
            }
            if (flv->video_par) {
                av_log(s, AV_LOG_ERROR,
                       "at most one video stream is supported in flv\n");
                return AVERROR(EINVAL);
            }
            flv->video_par = par;
            if (!ff_codec_get_tag(flv_video_codec_ids, par->codec_id))
                return unsupported_codec(s, "Video", par->codec_id);
            break;
        case AVMEDIA_TYPE_AUDIO:
            if (flv->audio_par) {
                av_log(s, AV_LOG_ERROR,
                       "at most one audio stream is supported in flv\n");
                return AVERROR(EINVAL);
            }
            flv->audio_par = par;
            if (get_audio_flags(s, par) < 0)
                return unsupported_codec(s, "Audio", par->codec_id);
            break;
        case AVMEDIA_TYPE_DATA:
            if (par->codec_id != AV_CODEC_ID_TEXT)
                return unsupported_codec(s, "Data", par->codec_id);
            flv->data_par = par;
            break;
        default:
            av_log(s, AV_LOG_ERROR, "codec not compatible with flv\n");
            return -1;
        }
        avpriv_set_pts_info(s->streams[i], 32, 1, 1000); /* 32 bit pts in ms */

        sc = av_mallocz(sizeof(FLVStreamContext));
        if (!sc)
            return AVERROR(ENOMEM);
        s->streams[i]->priv_data = sc;
        sc->last_ts = -1;
    }

    flv->delay = AV_NOPTS_VALUE;

    avio_write(pb, "FLV", 3);
    avio_w8(pb, 1);
    avio_w8(pb, FLV_HEADER_FLAG_HASAUDIO * !!flv->audio_par +
                FLV_HEADER_FLAG_HASVIDEO * !!flv->video_par);
    avio_wb32(pb, 9);
    avio_wb32(pb, 0);

    for (i = 0; i < s->nb_streams; i++)
        if (s->streams[i]->codecpar->codec_tag == 5) {
            avio_w8(pb, 8);     // message type
            avio_wb24(pb, 0);   // include flags
            avio_wb24(pb, 0);   // time stamp
            avio_wb32(pb, 0);   // reserved
            avio_wb32(pb, 11);  // size
            flv->reserved = 5;
        }

    write_metadata(s, 0);

    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        if (par->codec_id == AV_CODEC_ID_AAC || par->codec_id == AV_CODEC_ID_H264) {
            int64_t pos;
            avio_w8(pb, par->codec_type == AVMEDIA_TYPE_VIDEO ?
                    FLV_TAG_TYPE_VIDEO : FLV_TAG_TYPE_AUDIO);
            avio_wb24(pb, 0); // size patched later
            avio_wb24(pb, 0); // ts
            avio_w8(pb, 0);   // ts ext
            avio_wb24(pb, 0); // streamid
            pos = avio_tell(pb);
            if (par->codec_id == AV_CODEC_ID_AAC) {
                avio_w8(pb, get_audio_flags(s, par));
                avio_w8(pb, 0); // AAC sequence header
                avio_write(pb, par->extradata, par->extradata_size);
            } else {
                avio_w8(pb, par->codec_tag | FLV_FRAME_KEY); // flags
                avio_w8(pb, 0); // AVC sequence header
                avio_wb24(pb, 0); // composition time
                ff_isom_write_avcc(pb, par->extradata, par->extradata_size);
            }
            data_size = avio_tell(pb) - pos;
            avio_seek(pb, -data_size - 10, SEEK_CUR);
            avio_wb24(pb, data_size);
            avio_skip(pb, data_size + 10 - 3);
            avio_wb32(pb, data_size + 11); // previous tag size
        }
    }

    return 0;
}

static int flv_write_trailer(AVFormatContext *s)
{
    int64_t file_size;

    AVIOContext *pb = s->pb;
    FLVContext *flv = s->priv_data;
    int i;

    /* Add EOS tag */
    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *par = s->streams[i]->codecpar;
        FLVStreamContext *sc = s->streams[i]->priv_data;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            par->codec_id == AV_CODEC_ID_H264)
            put_avc_eos_tag(pb, sc->last_ts);
    }

    file_size = avio_tell(pb);

    /* update information */
    if (avio_seek(pb, flv->duration_offset, SEEK_SET) < 0)
        av_log(s, AV_LOG_WARNING, "Failed to update header with correct duration.\n");
    else
        put_amf_double(pb, flv->duration / (double)1000);
    if (avio_seek(pb, flv->filesize_offset, SEEK_SET) < 0)
        av_log(s, AV_LOG_WARNING, "Failed to update header with correct filesize.\n");
    else
        put_amf_double(pb, file_size);

    avio_seek(pb, file_size, SEEK_SET);
    return 0;
}

static const uint32_t FNV_OFFSET_32 = 2166136261U;
static const uint32_t FNV_PRIME_32 = 16777619;
static uint32_t hash(const char* str)
{
    uint32_t hash = FNV_OFFSET_32;
    for (size_t i = 0; i < strlen(str); i++)
    {
        hash = hash ^ (uint32_t)str[i];
        hash = hash * FNV_PRIME_32;
    }

    return hash;
}

static const char* RESERVED[] = { "hd", "hi", "hig", "med", "low", "slow", "mob", "hls" };

static uint32_t get_stream_hash(const char* str)
{
    size_t length;
    char* result_str;
    size_t index = 0;
    size_t result_index = 0;
    uint32_t result;

    length = strlen(str);
    result_str = malloc(length);

    for (size_t i = 0; i < length; ++i)
    {
        if (str[length - i - 1] == '\\' || str[length - i - 1] == '/')
        {
            index = length - i;
            break;
        }
    }

    for (size_t i = index; i < length; ++i)
    {
        if (i == length - 1 ||
            str[i + 1] == '-' || str[i + 1] == '_')
        {
            int reserved = 0;

            for (size_t c = 0; c < sizeof(RESERVED) / sizeof(char*); ++c)
            {
                if ((i - index + 1) == strlen(RESERVED[c]) && strncmp(RESERVED[c], str + index, i - index + 1) == 0)
                {
                    reserved = 1;
                    break;
                }
            }

            if (!reserved)
            {
                memcpy(result_str + result_index, str + index, i - index + 1);
                result_index += i - index + 1;
            }

            index = i + 2;
        }
    }

    result_str[result_index] = '\0';

    result = hash(result_str);

    free((void*)result_str);

    return result;
}

static int flv_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb      = s->pb;
    AVCodecParameters *par = s->streams[pkt->stream_index]->codecpar;
    FLVContext *flv      = s->priv_data;
    FLVStreamContext *sc = s->streams[pkt->stream_index]->priv_data;
    unsigned ts;
    int size = pkt->size;
    uint8_t *data = NULL;
    int flags = 0, flags_size;

    if (par->codec_id == AV_CODEC_ID_VP6F || par->codec_id == AV_CODEC_ID_VP6A ||
        par->codec_id == AV_CODEC_ID_AAC)
        flags_size = 2;
    else if (par->codec_id == AV_CODEC_ID_H264)
        flags_size = 5;
    else
        flags_size = 1;

    if (flv->delay == AV_NOPTS_VALUE)
    {
        if (flv->pts_mod)
        {
            const uint32_t first_timewrap = 457200; // January 6, 1970, 7:00 UTC
            const uint32_t three_weeks = 3 * 7 * 24 * 60 * 60;
            uint32_t stream_hash;
            time_t prev_wrap_timestamp;
            time_t next_wrap_timestamp;
            struct tm* timeinfo;
            int64_t *stream_start, timestamp;

            stream_start = av_stream_get_side_data(s->streams[pkt->stream_index], AV_PKT_DATA_STREAM_START_TIME, NULL);

            if (stream_start)
            {
                timestamp = *stream_start / 1000;
            
                stream_hash = get_stream_hash(s->filename) % 18;

                prev_wrap_timestamp = first_timewrap + ((timestamp - first_timewrap - stream_hash * 10 * 60) / three_weeks * three_weeks) + stream_hash * 10 * 60;
                next_wrap_timestamp = prev_wrap_timestamp + three_weeks;

                timeinfo = localtime(&timestamp);
                av_log(s, AV_LOG_INFO, "Current timestamp: %zu, %s", timestamp, asctime(timeinfo));

                timeinfo = localtime(&prev_wrap_timestamp);
                av_log(s, AV_LOG_INFO, "Previous stop on: %zu, %s", prev_wrap_timestamp, asctime(timeinfo));

                timeinfo = localtime(&next_wrap_timestamp);
                av_log(s, AV_LOG_INFO, "Next stop on: %zu, %s", next_wrap_timestamp, asctime(timeinfo));

                flv->delay = (*stream_start - (int64_t)prev_wrap_timestamp * 1000) % 0x7FFFFFFF;
                flv->stop_time = next_wrap_timestamp;
            }
        }
        else
        {
            flv->delay = -pkt->dts;
            flv->stop_time = 0;
        }
    }

    if (pkt->dts < -flv->delay) {
        av_log(s, AV_LOG_WARNING,
               "Packets are not in the proper order with respect to DTS\n");
        return AVERROR(EINVAL);
    }

    ts = pkt->dts + flv->delay; // add delay to force positive dts

    if (flv->stop_time)
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        if (tv.tv_sec >= flv->stop_time)
        {
            av_log(s, AV_LOG_INFO, "Stop reached! Timestamp: %"PRId64", pts: %u\n", (int64_t)tv.tv_sec, ts);
            exit(1);
        }
    }

    if (ts >= 0x7FFFFFFF)
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        av_log(s, AV_LOG_INFO, "Invalid pts! This shouldn't happen! Timestamp: %"PRId64", pts: %u\n", (int64_t)tv.tv_sec, ts);
        exit(1);
    }

    if (s->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
        write_metadata(s, ts);
        s->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
    }

    avio_write_marker(pb, av_rescale(ts, AV_TIME_BASE, 1000),
                      pkt->flags & AV_PKT_FLAG_KEY && (flv->video_par ? par->codec_type == AVMEDIA_TYPE_VIDEO : 1) ? AVIO_DATA_MARKER_SYNC_POINT : AVIO_DATA_MARKER_BOUNDARY_POINT);

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        avio_w8(pb, FLV_TAG_TYPE_VIDEO);

        flags = ff_codec_get_tag(flv_video_codec_ids, par->codec_id);

        flags |= pkt->flags & AV_PKT_FLAG_KEY ? FLV_FRAME_KEY : FLV_FRAME_INTER;
        break;
    case AVMEDIA_TYPE_AUDIO:
        flags = get_audio_flags(s, par);

        assert(size);

        avio_w8(pb, FLV_TAG_TYPE_AUDIO);
        break;
    case AVMEDIA_TYPE_DATA:
        avio_w8(pb, FLV_TAG_TYPE_META);
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (par->codec_id == AV_CODEC_ID_H264)
        /* check if extradata looks like MP4 */
        if (par->extradata_size > 0 && *(uint8_t*)par->extradata != 1)
            if (ff_avc_parse_nal_units_buf(pkt->data, &data, &size) < 0)
                return -1;

    /* check Speex packet duration */
    if (par->codec_id == AV_CODEC_ID_SPEEX && ts - sc->last_ts > 160)
        av_log(s, AV_LOG_WARNING, "Warning: Speex stream has more than "
                                  "8 frames per packet. Adobe Flash "
                                  "Player cannot handle this!\n");

    if (sc->last_ts < ts)
        sc->last_ts = ts;

    avio_wb24(pb, size + flags_size);
    avio_wb24(pb, ts);
    avio_w8(pb, (ts >> 24) & 0x7F); // timestamps are 32 bits _signed_
    avio_wb24(pb, flv->reserved);

    if (par->codec_type == AVMEDIA_TYPE_DATA) {
        int data_size;
        int64_t metadata_size_pos = avio_tell(pb);
        avio_w8(pb, AMF_DATA_TYPE_STRING);
        put_amf_string(pb, "onTextData");
        avio_w8(pb, AMF_DATA_TYPE_MIXEDARRAY);
        avio_wb32(pb, 2);
        put_amf_string(pb, "type");
        avio_w8(pb, AMF_DATA_TYPE_STRING);
        put_amf_string(pb, "Text");
        put_amf_string(pb, "text");
        avio_w8(pb, AMF_DATA_TYPE_STRING);
        put_amf_string(pb, pkt->data);
        put_amf_string(pb, "");
        avio_w8(pb, AMF_END_OF_OBJECT);
        /* write total size of tag */
        data_size = avio_tell(pb) - metadata_size_pos;
        avio_seek(pb, metadata_size_pos - 10, SEEK_SET);
        avio_wb24(pb, data_size);
        avio_seek(pb, data_size + 10 - 3, SEEK_CUR);
        avio_wb32(pb, data_size + 11);
    } else {
        avio_w8(pb,flags);
        if (par->codec_id == AV_CODEC_ID_VP6F || par->codec_id == AV_CODEC_ID_VP6A) {
            if (par->extradata_size)
                avio_w8(pb, par->extradata[0]);
            else
                avio_w8(pb, ((FFALIGN(par->width,  16) - par->width) << 4) |
                             (FFALIGN(par->height, 16) - par->height));
        } else if (par->codec_id == AV_CODEC_ID_AAC)
            avio_w8(pb, 1); // AAC raw
        else if (par->codec_id == AV_CODEC_ID_H264) {
            avio_w8(pb, 1); // AVC NALU
            avio_wb24(pb, pkt->pts - pkt->dts);
        }

        avio_write(pb, data ? data : pkt->data, size);

        avio_wb32(pb, size + flags_size + 11); // previous tag size
        flv->duration = FFMAX(flv->duration,
                              pkt->pts + flv->delay + pkt->duration);
    }

    av_free(data);

    return pb->error;
}

#define E AV_OPT_FLAG_ENCODING_PARAM
#define OFFSET(x) offsetof(FLVContext, x)
static const struct AVOption options[] = {
    { "pts_mod", "Use non-zero start ts", OFFSET(pts_mod), AV_OPT_TYPE_INT,   { .i64 = 1 },   0, 1,   E },
    { NULL },
};

static const AVClass flv_class = {
    .class_name = "flv muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_flv_muxer = {
    .name           = "flv",
    .long_name      = NULL_IF_CONFIG_SMALL("FLV (Flash Video)"),
    .mime_type      = "video/x-flv",
    .extensions     = "flv",
    .priv_data_size = sizeof(FLVContext),
    .audio_codec    = CONFIG_LIBMP3LAME ? AV_CODEC_ID_MP3 : AV_CODEC_ID_ADPCM_SWF,
    .video_codec    = AV_CODEC_ID_FLV1,
    .write_header   = flv_write_header,
    .write_packet   = flv_write_packet,
    .write_trailer  = flv_write_trailer,
    .codec_tag      = (const AVCodecTag* const []) {
                          flv_video_codec_ids, flv_audio_codec_ids, 0
                      },
    .flags          = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                      AVFMT_TS_NONSTRICT,
    .priv_class     = &flv_class,
};
