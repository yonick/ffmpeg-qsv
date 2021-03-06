/*
 * Intel MediaSDK QSV based H.264 decoder
 *
 * copyright (c) 2013 Luca Barbato
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


#include <stdint.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "avcodec.h"
#include "internal.h"
#include "h264.h"
#include "qsv.h"
#include "qsvdec.h"

typedef struct QSVDecH264Context {
    AVClass *class;
    QSVDecOptions options;
    QSVDecContext mem;
    QSVDecContext *qsv;
    AVBitStreamFilterContext *bsf;
    uint8_t *extradata;
    int extradata_size;
} QSVDecH264Context;

static const uint8_t fake_idr[] = { 0x00, 0x00, 0x01, 0x65 };

static av_cold int qsv_dec_init(AVCodecContext *avctx)
{
    QSVDecH264Context *q = avctx->priv_data;
    uint8_t *extradata   = NULL;
    int extradata_size   = 0;
    int ret;
    mfxBitstream bs;

    memset(&bs, 0, sizeof(bs));

    q->qsv = &q->mem;

    q->qsv->options = q->options;

    if (avctx->pix_fmt == AV_PIX_FMT_NONE)
        q->qsv->options.async_depth = 1;

    avctx->pix_fmt = AV_PIX_FMT_NV12;

    ret = ff_qsv_dec_init_mfx(avctx, q->qsv);
    if (ret < 0)
        return ret;

    if (avctx->extradata_size > 0) {
        if (avctx->extradata[0] == 1) {
            AVBitStreamFilterContext *bsf;

            if (!(extradata = av_malloc(avctx->extradata_size)))
                goto fail;
            memcpy(extradata, avctx->extradata, avctx->extradata_size);
            extradata_size = avctx->extradata_size;

            if (!(bsf = av_bitstream_filter_init("h264_mp4toannexb")))
                goto fail;

            // Data and DataLength passed as dummy pointers
            av_bitstream_filter_filter(bsf, avctx, NULL,
                                       &bs.Data, &bs.DataLength,
                                       NULL, 0, 0);

            av_bitstream_filter_close(bsf);
        }

        //FIXME feed it a fake IDR directly
        if (!(bs.Data = av_malloc(avctx->extradata_size + sizeof(fake_idr))))
            goto fail;

        memcpy(bs.Data, avctx->extradata, avctx->extradata_size);
        bs.DataLength += avctx->extradata_size;
        memcpy(bs.Data + bs.DataLength, fake_idr, sizeof(fake_idr));
        bs.DataLength += sizeof(fake_idr);
        bs.MaxLength = bs.DataLength;

        ret = ff_qsv_dec_init_decoder(avctx, q->qsv, &bs);
        if (ret < 0)
            goto fail;

        if (extradata) {
            av_free(avctx->extradata);
            avctx->extradata      = extradata;
            avctx->extradata_size = extradata_size;
        }

        av_free(bs.Data);
    }

    return ret;

fail:
    ff_qsv_dec_close(q->qsv);
    av_free(bs.Data);
    if (extradata) {
        av_free(avctx->extradata);
        avctx->extradata      = extradata;
        avctx->extradata_size = extradata_size;
    }

    return (ret < 0) ? ret : AVERROR(ENOMEM);
}

static int qsv_dec_frame(AVCodecContext *avctx, void *data,
                         int *got_frame, AVPacket *avpkt)
{
    QSVDecH264Context *q = avctx->priv_data;
    AVFrame *frame       = data;
    uint8_t *p           = NULL;
    int size             = 0;
    int ret;

    if (!q->bsf && avctx->extradata_size > 0 && avctx->extradata[0] == 1) {
        if (!(q->extradata = av_malloc(avctx->extradata_size)))
            return AVERROR(ENOMEM);
        memcpy(q->extradata, avctx->extradata, avctx->extradata_size);
        q->extradata_size = avctx->extradata_size;

        if (!(q->bsf = av_bitstream_filter_init("h264_mp4toannexb"))) {
            av_freep(&q->extradata);
            return AVERROR(ENOMEM);
        }
    }

    if (q->bsf)
        av_bitstream_filter_filter(q->bsf, avctx, NULL,
                                   &p, &size,
                                   avpkt->data, avpkt->size, 0);

    if (size && p && p != avpkt->data) {
        AVPacket pkt = { 0 };
        if (!(ret = av_packet_from_data(&pkt, p, size))) {
            if (!(ret = av_packet_copy_props(&pkt, avpkt)))
                ret = ff_qsv_dec_frame(avctx, q->qsv, frame, got_frame, &pkt);
            av_packet_unref(&pkt);
        }
    } else {
        ret = ff_qsv_dec_frame(avctx, q->qsv, frame, got_frame, avpkt);
    }

    return ret;
}

static int qsv_dec_close(AVCodecContext *avctx)
{
    QSVDecH264Context *q = avctx->priv_data;
    int ret              = 0;

    if (!avctx->internal->is_copy) {
        ret = ff_qsv_dec_close(q->qsv);
        if (q->bsf)
            av_bitstream_filter_close(q->bsf);
        if (q->extradata) {
            av_free(avctx->extradata);
            avctx->extradata      = q->extradata;
            avctx->extradata_size = q->extradata_size;
        }
    }

    return ret;
}

static void qsv_dec_flush(AVCodecContext *avctx)
{
    QSVDecH264Context *q = avctx->priv_data;

    ff_qsv_dec_flush(q->qsv);
}

#define OFFSET(x) offsetof(QSVDecH264Context, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "async_depth", "Number which limits internal frame buffering", OFFSET(options.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { "timeout", "Maximum timeout in milliseconds when the device has been busy", OFFSET(options.timeout), AV_OPT_TYPE_INT, { .i64 = TIMEOUT_DEFAULT }, 0, INT_MAX, VD },
    { NULL },
};

static const AVClass class = {
    .class_name = "h264_qsv decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_qsv_decoder = {
    .name           = "h264_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVDecH264Context),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .init           = qsv_dec_init,
    .decode         = qsv_dec_frame,
    .flush          = qsv_dec_flush,
    .close          = qsv_dec_close,
    .capabilities   = CODEC_CAP_DELAY | CODEC_CAP_PKT_TS | CODEC_CAP_DR1,
    .priv_class     = &class,
};
