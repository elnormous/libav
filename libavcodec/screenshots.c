
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"
#include "libavutil/internal.h"
#include "libavutil/imgutils.h"

#include "libswscale/swscale.h"

#include "avcodec.h"
#include "internal.h"
#include "avtools/cmdutils.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

typedef struct Screenshot {

    int width;
    int height;
    char* filename;

} Screenshot;

typedef struct ScreenshotsContext {
    AVClass *class;

    unsigned int timeout; // seconds
    char* qualities;
    char* folder;

    Screenshot* screenshots;
    int nb_screenshots;
    int last_generation;

} ScreenshotsContext;

static int save_frame(AVFrame *pFrame, const char* basename, const char* extension, enum AVCodecID codec_id, int compression) {

    AVCodec *codec;
    AVCodecContext *codec_context;
    FILE *file_descriptor;
    char file_name[256];
    AVPacket packet = {.data = NULL, .size = 0};
    int ret;

    if (!(codec = avcodec_find_encoder(codec_id))) {
        av_log(NULL, AV_LOG_WARNING, "save screenshot (%s) >> No codec\n", extension);
        return -1;
    }
    if (!(codec_context = avcodec_alloc_context3(codec))) {
        av_log(NULL, AV_LOG_WARNING, "save screenshot (%s) >> No context\n", extension);
        return -1;
    }

    codec_context->pix_fmt = pFrame->format;
    codec_context->height = pFrame->height;
    codec_context->width = pFrame->width;
    codec_context->time_base = (AVRational){1, 25};
//    codec_context->compression_level = compression;

    if (avcodec_open2(codec_context, codec, NULL) < 0) {
        av_log(NULL, AV_LOG_WARNING, "save screenshot (%s) >> Error on codec open\n", extension);
        goto fail;
    }

    av_init_packet(&packet);

    ret = avcodec_send_frame(codec_context, pFrame);
    if (ret < 0) {
        av_log(NULL, AV_LOG_WARNING, "save screenshot (%s) >> Error submitting a frame for encoding\n", extension);
        goto fail;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_context, &packet);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            av_log(NULL, AV_LOG_WARNING, "save screenshot (%s) >> Error encoding a video frame\n", extension);
            goto fail;
        } else if (ret >= 0) {
            break;
        }
    }

    snprintf(file_name, sizeof(file_name) - 1, "%s.%s", basename, extension);
    file_descriptor = fopen(file_name, "wb");
    fwrite(packet.data, 1, packet.size, file_descriptor);
    fclose(file_descriptor);
fail:
    av_packet_unref(&packet);
    avcodec_free_context(&codec_context);
    return 0;
}

static int screenshots_frame(AVCodecContext *avctx, AVPacket *pkt,
                        const AVFrame *pict, int *got_packet)
{
    ScreenshotsContext *s = avctx->priv_data;

    if (s->last_generation + s->timeout <= time(NULL)) {

        int i = 0;

        for (i = 0; i < s->nb_screenshots; i++) {

            int scaled_w = s->screenshots[i].width, scaled_h = s->screenshots[i].height;
            AVFrame* scaled = av_frame_alloc();
            int scaled_size = 0;
            uint8_t* scaled_buffer;

            if (scaled_w == -1 && scaled_h == -1) {
                scaled_w = pict->width;
                scaled_h = pict->height;
            } else if (scaled_w == -1) {
                scaled_w = scaled_h * ((double)pict->width / pict->height);
            } else if (scaled_h == -1) {
                scaled_h = scaled_w * ((double)pict->height / pict->width);
            }

            scaled_size =  av_image_get_buffer_size(AV_PIX_FMT_YUVJ420P, scaled_w, scaled_h, 1);
            scaled_buffer = (uint8_t *)av_malloc(scaled_size * sizeof(uint8_t));

            av_image_fill_arrays(scaled->data, scaled->linesize,
                                scaled_buffer, AV_PIX_FMT_YUVJ420P, scaled_w, scaled_h, 1);

            scaled->format = AV_PIX_FMT_YUVJ420P;
            scaled->width = scaled_w;
            scaled->height = scaled_h;

            // scale
            {
                struct SwsContext *resize;
                resize = sws_getContext(pict->width, pict->height, pict->format,
                        scaled_w, scaled_h, AV_PIX_FMT_YUVJ420P, SWS_BICUBIC, NULL, NULL, NULL);

                sws_scale(resize, (const uint8_t *const *)pict->data, pict->linesize, 0, pict->height, scaled->data, scaled->linesize);
                sws_freeContext(resize);
            }

            // encode / save to files
            save_frame(scaled, s->screenshots[i].filename, "jpg", AV_CODEC_ID_MJPEG, 2);
//            save_frame(scaled, s->screenshots[i].filename, "jpg", AV_CODEC_ID_JPEG2000, 4);

            av_frame_free(&scaled);
        }

        s->last_generation = time(NULL);
    }

    return 0;
}

static av_cold int screenshots_close(AVCodecContext *avctx)
{
    ScreenshotsContext *s = avctx->priv_data;

    av_freep(&s->screenshots);

    return 0;
}

static av_cold int screenshots_init(AVCodecContext *avctx)
{
    ScreenshotsContext *s = avctx->priv_data;
    char *qualityToken, *sizeToken, *str;
    int cnt = 0;

    if (s->qualities == NULL) {
        av_log(NULL, AV_LOG_ERROR, "Qualities not passed for screenshots\n");
        exit_program(1);
    }

    // count files
    str = s->qualities;
    for (cnt = 0; *str; str++)
        if (*str == ',') cnt++;
    cnt++;

    s->screenshots = av_mallocz_array(sizeof(Screenshot), cnt);
    s->nb_screenshots = cnt;

    // parse filenames and qualities
    cnt = 0;
    while ((qualityToken = strsep(&s->qualities, ","))) {

        if ((sizeToken = strsep(&qualityToken, ":"))) {
            if (strlen(s->folder) > 0) {
                s->screenshots[cnt].filename = av_mallocz(strlen(s->folder) + strlen(sizeToken) + 2);
                memcpy(s->screenshots[cnt].filename, s->folder, strlen(s->folder));
                s->screenshots[cnt].filename[strlen(s->folder)] = '/';
                memcpy(s->screenshots[cnt].filename + strlen(s->folder) + 1, sizeToken, strlen(sizeToken));
            } else {
                s->screenshots[cnt].filename = av_mallocz(strlen(sizeToken) + 1);
                memcpy(s->screenshots[cnt].filename, sizeToken, strlen(sizeToken));
            }
        }

        if ((sizeToken = strsep(&qualityToken, ":"))) {
            s->screenshots[cnt].width = atoi(sizeToken);
        }
        if ((sizeToken = strsep(&qualityToken, ":"))) {
            s->screenshots[cnt].height = atoi(sizeToken);
        }

        cnt++;
    }

    s->last_generation = 0;

    return 0;
}

#define OFFSET(x) offsetof(ScreenshotsContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "timeout",   "seconds between screenshot generation",                             OFFSET(timeout),    AV_OPT_TYPE_INT, { .i64 = 30 },  0, INT_MAX, VE},
    { "qualities", "Qualities to generate - name and size seperated by comma (HD:1920:1080,MED:1280:720,...)",  OFFSET(qualities),  AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE},
    { "folder",    "Folder in which to store screenshots",                              OFFSET(folder),     AV_OPT_TYPE_STRING, { .str="" }, 0, 0, VE},
    { NULL},
};

static const AVClass screenshots_class = {
    .class_name = "screenshots",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};
AVCodec ff_screenshots_encoder = {
    .name           = "screenshots",
    .long_name      = NULL_IF_CONFIG_SMALL("Screenshots"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_SCREENSHOTS,
    .priv_data_size = sizeof(ScreenshotsContext),
    .priv_class     = &screenshots_class,
    .init           = screenshots_init,
    .close          = screenshots_close,
    .encode2        = screenshots_frame,
    .pix_fmts       = (const enum AVPixelFormat[]) {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGBA, AV_PIX_FMT_RGB48,
        AV_PIX_FMT_RGBA64,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY16, AV_PIX_FMT_YA8,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV420P9, AV_PIX_FMT_YUV422P9, AV_PIX_FMT_YUV444P9,
        AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV422P10, AV_PIX_FMT_YUV444P10,
        AV_PIX_FMT_YUV420P16, AV_PIX_FMT_YUV422P16, AV_PIX_FMT_YUV444P16,
        AV_PIX_FMT_NONE
    },
};
