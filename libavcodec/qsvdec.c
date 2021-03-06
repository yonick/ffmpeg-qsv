/*
 * Intel MediaSDK QSV decoder utility functions
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

#include <string.h>
#include <sys/types.h>
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/time.h"
#include "internal.h"
#include "avcodec.h"
#include "qsv.h"
#include "qsvdec.h"


static int realloc_ts(QSVDecContext *q, int old_nmemb, int new_nmemb)
{
    QSVDecTimeStamp *tmp = av_realloc_array(q->ts, new_nmemb, sizeof(*q->ts));
    if (!tmp)
        return AVERROR(ENOMEM);

    q->ts = tmp;
    q->nb_ts = new_nmemb;

    for (int i = old_nmemb; i < q->nb_ts; i++)
        q->ts[i].pts = q->ts[i].dts = AV_NOPTS_VALUE;

    return 0;
}

static int get_dts(QSVDecContext *q, int64_t pts, int64_t *dts)
{
    int i;

    if (q->ts_by_qsv) {
        *dts = pts;
        return 0;
    }

    if (pts == AV_NOPTS_VALUE) {
        *dts = AV_NOPTS_VALUE;
        return 0;
    }

    for (i = 0; i < q->nb_ts; i++)
        if (q->ts[i].pts == pts)
            break;

    if (i == q->nb_ts) {
        av_log(q, AV_LOG_ERROR,
               "Requested pts %"PRId64" does not match any dts\n", pts);
        return AVERROR_BUG;
    }

    *dts = q->ts[i].dts;

    q->ts[i].pts = AV_NOPTS_VALUE;

    return 0;
}

static int put_dts(QSVDecContext *q, int64_t pts, int64_t dts)
{
    int i, ret;

    if (q->ts_by_qsv) {
        q->ts_cnt++;
        return 0;
    }

    if (pts == AV_NOPTS_VALUE)
        return 0;

    for (i = 0; i < q->nb_ts; i++)
        if (q->ts[i].pts == AV_NOPTS_VALUE)
            break;

    if (i == q->nb_ts) {
        ret = realloc_ts(q, q->nb_ts,
                         q->nb_ts ? q->nb_ts * 2 : q->req.NumFrameSuggested);
        if (ret < 0)
            return ret;
    }

    q->ts[i].pts = pts;
    q->ts[i].dts = dts;
    q->ts_cnt++;

    return 0;
}

static int set_bitstream_data(QSVDecContext *q, QSVDecBitstreamList *list, AVPacket *pkt)
{
    mfxBitstream *bs = &list->bs;
    int ret;

    if (list->pkt.data)
        av_packet_unref(&list->pkt);

    ret = av_packet_ref(&list->pkt, pkt);
    if (ret < 0)
        return ret;

    bs->Data       = list->pkt.data;
    bs->DataOffset = 0;
    bs->DataLength = list->pkt.size;
    bs->MaxLength  = list->pkt.size;
    bs->TimeStamp  = list->pkt.pts;

    // QSV calculates TimeStamp of output based on the first
    //  TimeStamp when q->bs.TimeStamp = MFX_TIMESTAMP_UNKNOWN
    if (q->ts_by_qsv)
        if (q->ts_cnt > 1 || list->pkt.pts == AV_NOPTS_VALUE)
            bs->TimeStamp = MFX_TIMESTAMP_UNKNOWN;

    return 0;
}

static mfxBitstream *get_bitstream_from_packet(QSVDecContext *q,
                                               AVPacket *pkt)
{
    QSVDecBitstreamList **pool = &q->bs_pool;
    QSVDecBitstreamList *list;

    while (*pool && (*pool)->bs.MaxLength)
        pool = &(*pool)->pool;

    if (!(*pool))
        if (!(*pool = av_mallocz(sizeof(QSVDecBitstreamList))))
            return NULL;

    list = *pool;

    if (set_bitstream_data(q, list, pkt) < 0)
        return NULL;

    return &list->bs;
}

static void put_pending_bitstream(QSVDecContext *q, mfxBitstream *bs)
{
    QSVDecBitstreamList *list = q->bs_pool;

    while (list && &list->bs != bs)
        list = list->pool;

    if (list) {
        if (q->pending_dec_end)
            q->pending_dec_end->next = list;
        else
            q->pending_dec = list;

        q->pending_dec_end = list;
    }
}

static mfxBitstream *get_pending_bitstream(QSVDecContext *q)
{
    QSVDecBitstreamList *list = q->pending_dec;

    q->pending_dec = list->next;

    if (!q->pending_dec)
        q->pending_dec_end = NULL;

    list->next = NULL;

    return &list->bs;
}

static void free_bitstream_pool(QSVDecContext *q)
{
    QSVDecBitstreamList **pool = &q->bs_pool;
    QSVDecBitstreamList *list;

    while (*pool) {
        list = *pool;
        *pool = list->pool;
        if (list->pkt.data)
            av_packet_unref(&list->pkt);
        av_freep(&list);
    }
}

static int set_surface_data(AVCodecContext *avctx, QSVDecContext *q,
                            mfxFrameSurface1 *surf)
{
    AVFrame *frame = surf->Data.MemId;
    int ret;

    if (!frame) {
        frame = av_frame_alloc();
        if (!frame) {
            av_log(avctx, AV_LOG_ERROR, "av_frame_alloc() failed\n");
            return AVERROR(ENOMEM);
        }
    }

    if (!frame->data[0]) {
        ret = ff_get_buffer(avctx, frame, 0);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_get_buffer() failed\n");
            av_frame_free(&frame);
            return ret;
        }
    }

    surf->Data.MemId = frame;
    surf->Data.Y     = frame->data[0];
    surf->Data.UV    = frame->data[1];
    surf->Data.Pitch = frame->linesize[0];
    surf->Info       = q->param.mfx.FrameInfo;

    return 0;
}

static mfxFrameSurface1 *get_surface(AVCodecContext *avctx, QSVDecContext *q)
{
    QSVDecSurfaceList **pool = &q->surf_pool;
    QSVDecSurfaceList *list;
    mfxFrameSurface1 *surf;

    while (*pool && ((*pool)->surface.Data.Locked || (*pool)->sync))
        pool = &(*pool)->pool;

    if (!(*pool))
        if (!(*pool = av_mallocz(sizeof(QSVDecSurfaceList))))
            return NULL;

    list = *pool;
    list->next = NULL;

    surf = &list->surface;
    if (set_surface_data(avctx, q, surf) < 0)
        return NULL;

    return surf;
}

static void release_surface(QSVDecContext *q, mfxFrameSurface1 *surf)
{
    QSVDecSurfaceList *list = q->surf_pool;

    while (list && &list->surface != surf)
        list = list->pool;

    if (list)
        list->sync = 0;
}

static void free_surface_pool(QSVDecContext *q)
{
    QSVDecSurfaceList **pool = &q->surf_pool;
    QSVDecSurfaceList *list;

    while (*pool) {
        list = *pool;
        *pool = list->pool;
        av_frame_free((AVFrame **)&list->surface.Data.MemId);
        av_freep(&list);
    }
}

static void put_sync(QSVDecContext *q, mfxFrameSurface1 *surf,
                     mfxSyncPoint sync)
{
    QSVDecSurfaceList *list = q->surf_pool;

    while (list && &list->surface != surf)
        list = list->pool;

    if (list) {
        list->sync = sync;
        list->next = NULL;

        if (q->pending_sync_end)
            q->pending_sync_end->next = list;
        else
            q->pending_sync = list;

        q->pending_sync_end = list;

        q->nb_sync++;
    }
}

static void get_sync(QSVDecContext *q, mfxFrameSurface1 **surf,
                     mfxSyncPoint *sync)
{
    QSVDecSurfaceList *list = q->pending_sync;

    *surf = &list->surface;
    *sync = list->sync;

    q->pending_sync = list->next;

    if (!q->pending_sync)
        q->pending_sync_end = NULL;

    q->nb_sync--;

    list->next = NULL;
}

static void free_sync(QSVDecContext *q)
{
    q->pending_sync     = NULL;
    q->pending_sync_end = NULL;
    q->nb_sync          = 0;
}

int ff_qsv_dec_init_mfx(AVCodecContext *avctx, QSVDecContext *q)
{
    mfxIMPL impl   = MFX_IMPL_AUTO_ANY;
    mfxVersion ver = { { QSV_VERSION_MINOR, QSV_VERSION_MAJOR } };
    int ret;

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0)
        return ret;

    q->param.mfx.CodecId = ret;

    ret = MFXInit(impl, &ver, &q->session);
    av_log(avctx, AV_LOG_DEBUG, "MFXInit(): %d\n", ret);
    if (ret < 0)
        return ff_qsv_error(ret);

    MFXQueryIMPL(q->session, &impl);

    if (impl & MFX_IMPL_SOFTWARE)
        av_log(avctx, AV_LOG_INFO,
               "Using Intel QuickSync decoder software implementation.\n");
    else if (impl & MFX_IMPL_HARDWARE)
        av_log(avctx, AV_LOG_INFO,
               "Using Intel QuickSync decoder hardware accelerated implementation.\n");
    else
        av_log(avctx, AV_LOG_INFO,
               "Unknown Intel QuickSync decoder implementation %d.\n", impl);

    q->param.IOPattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    q->param.AsyncDepth = q->options.async_depth;

    return 0;
}

int ff_qsv_dec_init_decoder(AVCodecContext *avctx, QSVDecContext *q,
                            mfxBitstream *bs)
{
    int ret;

    ret = MFXVideoDECODE_DecodeHeader(q->session, bs, &q->param);
    av_log(avctx, AV_LOG_DEBUG, "MFXVideoDECODE_DecodeHeader(): %d\n", ret);
    if (ret < 0)
        return ff_qsv_error(ret);

    avctx->width                   = q->param.mfx.FrameInfo.CropW;
    avctx->height                  = q->param.mfx.FrameInfo.CropH;
    avctx->coded_width             = q->param.mfx.FrameInfo.Width;
    avctx->coded_height            = q->param.mfx.FrameInfo.Height;
    avctx->time_base.den           = q->param.mfx.FrameInfo.FrameRateExtN;
    avctx->time_base.num           = q->param.mfx.FrameInfo.FrameRateExtD /
                                     avctx->ticks_per_frame;
    avctx->sample_aspect_ratio.num = q->param.mfx.FrameInfo.AspectRatioW;
    avctx->sample_aspect_ratio.den = q->param.mfx.FrameInfo.AspectRatioH;

    //bs->DataFlag = MFX_BITSTREAM_COMPLETE_FRAME; // can't decode PAFF

    memset(&q->req, 0, sizeof(q->req));
    ret = MFXVideoDECODE_QueryIOSurf(q->session, &q->param, &q->req);
    av_log(avctx, AV_LOG_DEBUG, "MFXVideoDECODE_QueryIOSurf(): %d\n", ret);
    if (ret < 0)
        return ff_qsv_error(ret);

    ret = MFXVideoDECODE_Init(q->session, &q->param);
    av_log(avctx, AV_LOG_DEBUG, "MFXVideoDECODE_Init(): %d\n", ret);
    if (ret)
        ret = ff_qsv_error(ret);

    q->last_ret    = MFX_ERR_MORE_DATA;
    q->initialized = 1;

    return ret;
}

static void reinit_decoder(AVCodecContext *avctx, QSVDecContext *q)
{
    if (q->initialized)
        MFXVideoDECODE_Close(q->session);

    q->initialized = 0;

    free_surface_pool(q);

    free_sync(q);

    ff_qsv_dec_init_decoder(avctx, q, q->bs);

    q->reinit = NULL;
}

static void init_decoder(AVCodecContext *avctx, QSVDecContext *q,
                         AVPacket *avpkt)
{
    if (avpkt->size) {
        mfxBitstream *bs = get_bitstream_from_packet(q, avpkt);
        if (bs) {
            ff_qsv_dec_init_decoder(avctx, q, bs);
            bs->MaxLength = 0;
        }
    }
}

int ff_qsv_dec_frame(AVCodecContext *avctx, QSVDecContext *q,
                     AVFrame *frame, int *got_frame, AVPacket *avpkt)
{
    mfxBitstream *inbs         = NULL;
    mfxBitstream *curbs        = NULL;
    mfxFrameSurface1 *worksurf = NULL;
    mfxFrameSurface1 *outsurf  = NULL;
    mfxFrameSurface1 *surf     = NULL;
    mfxSyncPoint outsync       = 0;
    mfxSyncPoint sync          = 0;
    int size                   = avpkt->size;
    int busymsec               = 0;
    int flush                  = 0;
    int ret;

    *got_frame = 0;

    if (q->reinit && q->last_ret == MFX_ERR_MORE_DATA && !q->nb_sync)
        reinit_decoder(avctx, q);

    if (!q->initialized)
        init_decoder(avctx, q, avpkt);

    if (!q->initialized)
        return size;

    if (size) {
        ret = put_dts(q, avpkt->pts, avpkt->dts);
        if (ret < 0)
            return ret;

        curbs = get_bitstream_from_packet(q, avpkt);
        if (!curbs)
            return AVERROR(ENOMEM);
    }

    // (2) Flush cached frames before reinit
    if (q->reinit)
        flush = 1;

    ret  = q->last_ret;
    inbs = q->bs;

    do {
        if (inbs && !inbs->DataLength) {
            inbs->MaxLength = 0;
            inbs = NULL;
        }

        if (ret == MFX_ERR_MORE_DATA) {
            if (flush) {
                break;
            } else if (inbs && inbs->DataLength > 0) {

            } else if (q->pending_dec) {
                inbs = get_pending_bitstream(q);
            } else if (curbs) {
                inbs = curbs;
                curbs = NULL;
            } else if (!size) {
                // Flush cached frames when EOF
                flush = 1;
            } else {
                break;
            }
        } else if (ret == MFX_WRN_VIDEO_PARAM_CHANGED) {
            // Detected new seaquence header has compatible video parameter
            // Automatically bitstream move forward next time
        } else if (ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM) {
            // Detected new seaquence header has incompatible video parameter
            av_log(avctx, AV_LOG_INFO,
                   "Detected new video parameters in the bitstream\n");
            if (flush)
                break;
            // (1) Flush cached frames before reinit
            flush = 1;
            q->reinit = inbs;
        }

        if (flush)
            inbs = NULL;

        worksurf = get_surface(avctx, q);
        if (!worksurf)
            break;

        ret = MFXVideoDECODE_DecodeFrameAsync(q->session, inbs,
                                              worksurf, &outsurf, &outsync);
        av_log(avctx, AV_LOG_DEBUG,
               "MFXVideoDECODE_DecodeFrameAsync(): %d\n", ret);

        if (ret == MFX_WRN_DEVICE_BUSY) {
            if (busymsec > q->options.timeout) {
                av_log(avctx, AV_LOG_WARNING, "Timeout, device is so busy\n");
                break;
            }
            av_usleep(1000);
            busymsec++;
        } else {
            busymsec = 0;
        }
    } while (ret == MFX_ERR_MORE_SURFACE ||
             ret == MFX_ERR_MORE_DATA ||
             ret == MFX_WRN_DEVICE_BUSY ||
             ret == MFX_WRN_VIDEO_PARAM_CHANGED ||
             ret == MFX_ERR_INCOMPATIBLE_VIDEO_PARAM);

    q->last_ret = ret;
    q->bs       = inbs;

    if (curbs)
        put_pending_bitstream(q, curbs);

    if (outsync)
        put_sync(q, outsurf, outsync);

    ret = ret == MFX_ERR_MORE_DATA ? 0 : ff_qsv_error(ret);

    if (q->pending_sync &&
        (q->nb_sync >= q->req.NumFrameMin || !size || q->reinit)) {
        int64_t pts, dts;

        get_sync(q, &surf, &sync);

        ret = MFXVideoCORE_SyncOperation(q->session, sync, SYNC_TIME_DEFAULT);
        av_log(avctx, AV_LOG_DEBUG,
               "MFXVideoCORE_SyncOperation(): %d\n", ret);
        if (ret < 0)
            return ff_qsv_error(ret);

        pts = surf->Data.TimeStamp;
        ret = get_dts(q, pts, &dts);
        if (ret < 0)
            return ret;

        av_frame_move_ref(frame, surf->Data.MemId);

        frame->pkt_pts = frame->pts = pts;
        frame->pkt_dts = dts;

        frame->repeat_pict =
            surf->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            surf->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            surf->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            !!(surf->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF);
        frame->interlaced_frame =
            !(surf->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        frame->sample_aspect_ratio.num = surf->Info.AspectRatioW;
        frame->sample_aspect_ratio.den = surf->Info.AspectRatioH;

        release_surface(q, surf);

        *got_frame = 1;
    }

    return (ret < 0) ? ret : size;
}

int ff_qsv_dec_flush(QSVDecContext *q)
{
    int ret = MFXVideoDECODE_Reset(q->session, &q->param);

    q->last_ret       = MFX_ERR_MORE_DATA;
    q->bs->DataOffset = q->bs->DataLength = 0;

    free_surface_pool(q);

    free_sync(q);

    av_freep(&q->ts);

    free_bitstream_pool(q);

    return ff_qsv_error(ret);
}

int ff_qsv_dec_close(QSVDecContext *q)
{
    if (q->initialized)
        MFXVideoDECODE_Close(q->session);

    MFXClose(q->session);

    free_surface_pool(q);

    av_freep(&q->ts);

    free_bitstream_pool(q);

    return 0;
}

