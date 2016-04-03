/*
 * Sierra VMD video decoder
 * Copyright (c) 2004 The FFmpeg Project
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * Sierra VMD video decoder
 * by Vladimir "VAG" Gneushev (vagsoft at mail.ru)
 * for more information on the Sierra VMD format, visit:
 *   http://www.pcisys.net/~melanson/codecs/
 *
 * The video decoder outputs PAL8 colorspace data. The decoder expects
 * a 0x330-byte VMD file header to be transmitted via extradata during
 * codec initialization. Each encoded frame that is sent to this decoder
 * is expected to be prepended with the appropriate 16-byte frame
 * information record from the VMD file.
 */

#include <string.h>

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/tree.h"

#include "avcodec.h"
#include "internal.h"
#include "bytestream.h"

#define VMD_HEADER_SIZE 0x330
#define PALETTE_COUNT 256

typedef struct VmdVideoContext {

    AVCodecContext *avctx;
    AVFrame *prev_frame;

    const unsigned char *buf;
    int size;

    unsigned char palette[PALETTE_COUNT * 4];
    unsigned char *unpack_buffer;
    int unpack_buffer_size;

    int x_off, y_off;
} VmdVideoContext;

#define QUEUE_SIZE 0x1000
#define QUEUE_MASK 0x0FFF

static int lz_unpack(const unsigned char *src, int src_len,
                      unsigned char *dest, int dest_len)
{
    unsigned char *d;
    unsigned char *d_end;
    unsigned char queue[QUEUE_SIZE];
    unsigned int qpos;
    unsigned int dataleft;
    unsigned int chainofs;
    unsigned int chainlen;
    unsigned int speclen;
    unsigned char tag;
    unsigned int i, j;
    GetByteContext gb;

    bytestream2_init(&gb, src, src_len);
    d = dest;
    d_end = d + dest_len;
    dataleft = bytestream2_get_le32(&gb);
    memset(queue, 0x20, QUEUE_SIZE);
    if (bytestream2_get_bytes_left(&gb) < 4)
        return AVERROR_INVALIDDATA;
    if (bytestream2_peek_le32(&gb) == 0x56781234) {
        bytestream2_skipu(&gb, 4);
        qpos = 0x111;
        speclen = 0xF + 3;
    } else {
        qpos = 0xFEE;
        speclen = 100;  /* no speclen */
    }

    while (dataleft > 0 && bytestream2_get_bytes_left(&gb) > 0) {
        tag = bytestream2_get_byteu(&gb);
        if ((tag == 0xFF) && (dataleft > 8)) {
            if (d_end - d < 8 || bytestream2_get_bytes_left(&gb) < 8)
                return AVERROR_INVALIDDATA;
            for (i = 0; i < 8; i++) {
                queue[qpos++] = *d++ = bytestream2_get_byteu(&gb);
                qpos &= QUEUE_MASK;
            }
            dataleft -= 8;
        } else {
            for (i = 0; i < 8; i++) {
                if (dataleft == 0)
                    break;
                if (tag & 0x01) {
                    if (d_end - d < 1 || bytestream2_get_bytes_left(&gb) < 1)
                        return AVERROR_INVALIDDATA;
                    queue[qpos++] = *d++ = bytestream2_get_byteu(&gb);
                    qpos &= QUEUE_MASK;
                    dataleft--;
                } else {
                    chainofs = bytestream2_get_byte(&gb);
                    chainofs |= ((bytestream2_peek_byte(&gb) & 0xF0) << 4);
                    chainlen = (bytestream2_get_byte(&gb) & 0x0F) + 3;
                    if (chainlen == speclen) {
                        chainlen = bytestream2_get_byte(&gb) + 0xF + 3;
                    }
                    if (d_end - d < chainlen)
                        return AVERROR_INVALIDDATA;
                    for (j = 0; j < chainlen; j++) {
                        *d = queue[chainofs++ & QUEUE_MASK];
                        queue[qpos++] = *d++;
                        qpos &= QUEUE_MASK;
                    }
                    dataleft -= chainlen;
                }
                tag >>= 1;
            }
        }
    }
    return d - dest;
}
static int rle_unpack(const unsigned char *src, unsigned char *dest,
                      int src_count, int src_size, int dest_len)
{
    unsigned char *pd;
    int i, l, used = 0;
    unsigned char *dest_end = dest + dest_len;
    GetByteContext gb;
    uint16_t run_val;

    bytestream2_init(&gb, src, src_size);
    pd = dest;
    if (src_count & 1) {
        if (bytestream2_get_bytes_left(&gb) < 1)
            return 0;
        *pd++ = bytestream2_get_byteu(&gb);
        used++;
    }

    do {
        if (bytestream2_get_bytes_left(&gb) < 1)
            break;
        l = bytestream2_get_byteu(&gb);
        if (l & 0x80) {
            l = (l & 0x7F) * 2;
            if (dest_end - pd < l || bytestream2_get_bytes_left(&gb) < l)
                return bytestream2_tell(&gb);
            bytestream2_get_bufferu(&gb, pd, l);
            pd += l;
        } else {
            if (dest_end - pd < 2*l || bytestream2_get_bytes_left(&gb) < 2)
                return bytestream2_tell(&gb);
            run_val = bytestream2_get_ne16(&gb);
            for (i = 0; i < l; i++) {
                AV_WN16(pd, run_val);
                pd += 2;
            }
            l *= 2;
        }
        used += l;
    } while (used < src_count);

    return bytestream2_tell(&gb);
}

static int vmd_decode(VmdVideoContext *s, AVFrame *frame)
{
    int i;
    unsigned int *palette32;
    unsigned char r, g, b;

    GetByteContext gb;

    unsigned char meth;
    unsigned char *dp;   /* pointer to current frame */
    unsigned char *pp;   /* pointer to previous frame */
    unsigned char len;
    int ofs;

    int frame_x, frame_y;
    int frame_width, frame_height;

    frame_x = AV_RL16(&s->buf[6]);
    frame_y = AV_RL16(&s->buf[8]);
    frame_width = AV_RL16(&s->buf[10]) - frame_x + 1;
    frame_height = AV_RL16(&s->buf[12]) - frame_y + 1;

    if ((frame_width == s->avctx->width && frame_height == s->avctx->height) &&
        (frame_x || frame_y)) {

        s->x_off = frame_x;
        s->y_off = frame_y;
    }
    frame_x -= s->x_off;
    frame_y -= s->y_off;

    if (frame_x < 0 || frame_width < 0 ||
        frame_x >= s->avctx->width ||
        frame_width > s->avctx->width ||
        frame_x + frame_width > s->avctx->width) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Invalid horizontal range %d-%d\n",
               frame_x, frame_width);
        return AVERROR_INVALIDDATA;
    }
    if (frame_y < 0 || frame_height < 0 ||
        frame_y >= s->avctx->height ||
        frame_height > s->avctx->height ||
        frame_y + frame_height > s->avctx->height) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Invalid vertical range %d-%d\n",
               frame_x, frame_width);
        return AVERROR_INVALIDDATA;
    }

    /* if only a certain region will be updated, copy the entire previous
     * frame before the decode */
    if (s->prev_frame->data[0] &&
        (frame_x || frame_y || (frame_width != s->avctx->width) ||
        (frame_height != s->avctx->height))) {

        memcpy(frame->data[0], s->prev_frame->data[0],
            s->avctx->height * frame->linesize[0]);
    }

    /* check if there is a new palette */
    bytestream2_init(&gb, s->buf + 16, s->size - 16);
    if (s->buf[15] & 0x02) {
        bytestream2_skip(&gb, 2);
        palette32 = (unsigned int *)s->palette;
        if (bytestream2_get_bytes_left(&gb) >= PALETTE_COUNT * 3) {
            for (i = 0; i < PALETTE_COUNT; i++) {
                r = bytestream2_get_byteu(&gb) * 4;
                g = bytestream2_get_byteu(&gb) * 4;
                b = bytestream2_get_byteu(&gb) * 4;
                palette32[i] = 0xFFU << 24 | (r << 16) | (g << 8) | (b);
                palette32[i] |= palette32[i] >> 6 & 0x30303;
            }
        } else {
            av_log(s->avctx, AV_LOG_ERROR, "Incomplete palette\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (!s->size)
        return 0;

    /* originally UnpackFrame in VAG's code */
    if (bytestream2_get_bytes_left(&gb) < 1)
        return AVERROR_INVALIDDATA;
    meth = bytestream2_get_byteu(&gb);
    if (meth & 0x80) {
        int size;
        if (!s->unpack_buffer_size) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Trying to unpack LZ-compressed frame with no LZ buffer\n");
            return AVERROR_INVALIDDATA;
        }
        size = lz_unpack(gb.buffer, bytestream2_get_bytes_left(&gb),
                         s->unpack_buffer, s->unpack_buffer_size);
        if (size < 0)
            return size;
        meth &= 0x7F;
        bytestream2_init(&gb, s->unpack_buffer, size);
    }

    dp = &frame->data[0][frame_y * frame->linesize[0] + frame_x];
    pp = &s->prev_frame->data[0][frame_y * s->prev_frame->linesize[0] + frame_x];
    switch (meth) {
    case 1:
        for (i = 0; i < frame_height; i++) {
            ofs = 0;
            do {
                len = bytestream2_get_byte(&gb);
                if (len & 0x80) {
                    len = (len & 0x7F) + 1;
                    if (ofs + len > frame_width ||
                        bytestream2_get_bytes_left(&gb) < len)
                        return AVERROR_INVALIDDATA;
                    bytestream2_get_bufferu(&gb, &dp[ofs], len);
                    ofs += len;
                } else {
                    /* interframe pixel copy */
                    if (ofs + len + 1 > frame_width || !s->prev_frame->data[0])
                        return AVERROR_INVALIDDATA;
                    memcpy(&dp[ofs], &pp[ofs], len + 1);
                    ofs += len + 1;
                }
            } while (ofs < frame_width);
            if (ofs > frame_width) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "offset > width (%d > %d)\n",
                       ofs, frame_width);
                return AVERROR_INVALIDDATA;
            }
            dp += frame->linesize[0];
            pp += s->prev_frame->linesize[0];
        }
        break;

    case 2:
        for (i = 0; i < frame_height; i++) {
            bytestream2_get_buffer(&gb, dp, frame_width);
            dp += frame->linesize[0];
            pp += s->prev_frame->linesize[0];
        }
        break;

    case 3:
        for (i = 0; i < frame_height; i++) {
            ofs = 0;
            do {
                len = bytestream2_get_byte(&gb);
                if (len & 0x80) {
                    len = (len & 0x7F) + 1;
                    if (bytestream2_peek_byte(&gb) == 0xFF) {
                        int slen = len;
                        bytestream2_get_byte(&gb);
                        len = rle_unpack(gb.buffer, &dp[ofs],
                                         len, bytestream2_get_bytes_left(&gb),
                                         frame_width - ofs);
                        ofs += slen;
                        bytestream2_skip(&gb, len);
                    } else {
                        if (ofs + len > frame_width ||
                            bytestream2_get_bytes_left(&gb) < len)
                            return AVERROR_INVALIDDATA;
                        bytestream2_get_buffer(&gb, &dp[ofs], len);
                        ofs += len;
                    }
                } else {
                    /* interframe pixel copy */
                    if (ofs + len + 1 > frame_width || !s->prev_frame->data[0])
                        return AVERROR_INVALIDDATA;
                    memcpy(&dp[ofs], &pp[ofs], len + 1);
                    ofs += len + 1;
                }
            } while (ofs < frame_width);
            if (ofs > frame_width) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "offset > width (%d > %d)\n",
                       ofs, frame_width);
                return AVERROR_INVALIDDATA;
            }
            dp += frame->linesize[0];
            pp += s->prev_frame->linesize[0];
        }
        break;
    }
    return 0;
}

static av_cold int vmdvideo_decode_end(AVCodecContext *avctx)
{
    VmdVideoContext *s = avctx->priv_data;

    av_frame_free(&s->prev_frame);
    av_freep(&s->unpack_buffer);
    s->unpack_buffer_size = 0;

    return 0;
}

static av_cold int vmdvideo_decode_init(AVCodecContext *avctx)
{
    VmdVideoContext *s = avctx->priv_data;
    int i;
    unsigned int *palette32;
    int palette_index = 0;
    unsigned char r, g, b;
    unsigned char *vmd_header;
    unsigned char *raw_palette;

    s->avctx = avctx;
    avctx->pix_fmt = AV_PIX_FMT_PAL8;

    /* make sure the VMD header made it */
    if (s->avctx->extradata_size != VMD_HEADER_SIZE) {
        av_log(s->avctx, AV_LOG_ERROR, "expected extradata size of %d\n",
            VMD_HEADER_SIZE);
        return AVERROR_INVALIDDATA;
    }
    vmd_header = (unsigned char *)avctx->extradata;

    s->unpack_buffer_size = AV_RL32(&vmd_header[800]);
    if (s->unpack_buffer_size) {
        s->unpack_buffer = av_malloc(s->unpack_buffer_size);
        if (!s->unpack_buffer)
            return AVERROR(ENOMEM);
    }

    /* load up the initial palette */
    raw_palette = &vmd_header[28];
    palette32 = (unsigned int *)s->palette;
    for (i = 0; i < PALETTE_COUNT; i++) {
        r = raw_palette[palette_index++] * 4;
        g = raw_palette[palette_index++] * 4;
        b = raw_palette[palette_index++] * 4;
        palette32[i] = 0xFFU << 24 | (r << 16) | (g << 8) | (b);
        palette32[i] |= palette32[i] >> 6 & 0x30303;
    }

    s->prev_frame = av_frame_alloc();
    if (!s->prev_frame) {
        vmdvideo_decode_end(avctx);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int vmdvideo_decode_frame(AVCodecContext *avctx,
                                 void *data, int *got_frame,
                                 AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    VmdVideoContext *s = avctx->priv_data;
    AVFrame *frame = data;
    int ret;

    s->buf = buf;
    s->size = buf_size;

    if (buf_size < 16)
        return AVERROR_INVALIDDATA;

    if ((ret = ff_get_buffer(avctx, frame, AV_GET_BUFFER_FLAG_REF)) < 0)
        return ret;

    if ((ret = vmd_decode(s, frame)) < 0)
        return ret;

    /* make the palette available on the way out */
    memcpy(frame->data[1], s->palette, PALETTE_COUNT * 4);

    /* shuffle frames */
    av_frame_unref(s->prev_frame);
    if ((ret = av_frame_ref(s->prev_frame, frame)) < 0)
        return ret;

    *got_frame      = 1;

    /* report that the buffer was completely consumed */
    return buf_size;
}

AVCodec ff_vmdvideo_decoder = {
    .name           = "vmdvideo",
    .long_name      = NULL_IF_CONFIG_SMALL("Sierra VMD video"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VMDVIDEO,
    .priv_data_size = sizeof(VmdVideoContext),
    .init           = vmdvideo_decode_init,
    .close          = vmdvideo_decode_end,
    .decode         = vmdvideo_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
};

#define PALETTE_SIZE (256 * 3)
#define VMD_SIDE_DATA_SIZE ((2*4) + 1 + 1 + PALETTE_SIZE)

typedef struct {
    int rgb;  /* will be used for comparisons; only bottom 24 bits are set */
    uint8_t index;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} PaletteEntry;

typedef struct {
    struct AVTreeNode *palette;
    int palette_count;
    int current_frame;
    uint8_t *frames[2];
    uint8_t *diff;
    int frame_size;
    int keyframe;
} VmdVideoEncContext;

static int palette_compare(const void *key, const void *b)
{
    int key_int;
    PaletteEntry *b_struct;

    key_int = *(const int *)key;
    b_struct = (PaletteEntry*)b;

    return key_int - b_struct->rgb;
}

static int palette_enumerate(void *opaque, void *elem)
{
    uint8_t *palette;
    PaletteEntry *entry;
    int i;

    palette = (uint8_t*)opaque;
    entry = (PaletteEntry*)elem;
    i = entry->index;
    palette[i*3+0] = entry->r;
    palette[i*3+1] = entry->g;
    palette[i*3+2] = entry->b;

    return 0;
}

static int reset_palette(VmdVideoEncContext *const s)
{
    struct AVTreeNode *node;
    PaletteEntry *entry;

    /* free existing tree */
    if (s->palette)
        av_tree_destroy(s->palette);
    s->palette = NULL;
    s->palette_count = 0;

    /* color 0 needs to be black as it's used for background and
     * interlace fill in Sierra game engines */
    node = av_tree_node_alloc();
    entry = av_malloc(sizeof(PaletteEntry));
    if (!node || !entry)
    {
        return AVERROR(ENOMEM);
    }
    entry->r = 0;
    entry->g = 0;
    entry->b = 0;
    entry->rgb = 0;
    entry->index = s->palette_count++;
    av_tree_insert(&s->palette, entry, palette_compare, &node);

    return 0;
}

static av_cold int vmdvideo_encode_init(AVCodecContext *avctx)
{
    VmdVideoEncContext *const s = avctx->priv_data;
    int ret;

    s->palette = NULL;
    ret = reset_palette(s);
    if (ret < 0)
        return 0;

    s->frame_size = avctx->width * avctx->height * sizeof(uint8_t);
    s->current_frame = 0;
    s->frames[0] = av_malloc(s->frame_size);
    s->frames[1] = av_malloc(s->frame_size);
    s->diff = av_malloc(s->frame_size);
    s->keyframe = 1;

    return 0;
}

static int process_colors(VmdVideoEncContext *const s, const AVFrame *pict, uint8_t *cur_frame)
{
    int x, y;
    uint8_t *pixel;
    uint8_t r, g, b;
    int rgb;
    int cur_index;
    struct AVTreeNode *node;
    PaletteEntry *entry, *next[2] = {NULL, NULL};

    /* Iterate over the frame's pixels and build the palette by using a
     * sorted tree. If the color is not already in the tree, create a new
     * palette index. Convert the image at the same time. */
    cur_index = 0;
    for (y = 0; y < pict->height; y++)
    {
        pixel = &pict->data[0][y * pict->linesize[0]];
        for (x = 0; x < pict->width; x++)
        {
            /* fetch the pixel elements, scaling them down in advance */
            b = *pixel++ >> 2;
            g = *pixel++ >> 2;
            r = *pixel++ >> 2;
            rgb = (r << 16) | (g << 8) | b;

            /* check if there is already a palette entry */
            entry = av_tree_find(s->palette, &rgb, palette_compare, (void**)next);
            if (!entry)
            {
                node = av_tree_node_alloc();
                entry = av_malloc(sizeof(PaletteEntry));
                if (!node || !entry)
                {
                    return AVERROR(ENOMEM);
                }
                entry->r = r;
                entry->g = g;
                entry->b = b;
                entry->rgb = rgb;
                entry->index = s->palette_count++;
                av_tree_insert(&s->palette, entry, palette_compare, &node);
            }
            cur_frame[cur_index++] = entry->index;
        }
    }

    return 0;
}

static int vmdvideo_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                 const AVFrame *pict, int *got_packet)
{
    VmdVideoEncContext *const s = avctx->priv_data;
    int ret;
    uint8_t *cur_frame;
    uint8_t *prev_frame;
    uint8_t palette[PALETTE_SIZE];
    uint8_t *enc_ptr;
    int initial_palette_count;
    int x;

static int count = 0;
av_log(NULL, AV_LOG_INFO, "... %d, %dx%d, linesize = %d\n", count++, pict->width, pict->height, pict->linesize[0]);

    cur_frame = s->frames[s->current_frame];
    prev_frame = s->frames[!s->current_frame];

    if ((ret = ff_alloc_packet2(avctx, pkt,
        VMD_SIDE_DATA_SIZE + 1 + s->frame_size, 0)) < 0)
        return ret;
    enc_ptr = pkt->data;

    if (avctx->pix_fmt != AV_PIX_FMT_BGR24) {
        av_log(avctx, AV_LOG_ERROR, "unsupported pixel format\n");
        return -1;
    }

    /* convert the BGR24 frame -> PAL8 frame, expanding the palette as necessary */
    initial_palette_count = s->palette_count;
    ret = process_colors(s, pict, cur_frame);
    if (ret != 0)
        return ret;

    /* if palette overflows, reset the palette and re-process frame */
    if (s->palette_count > 256)
    {
av_log(NULL, AV_LOG_INFO, "  HEY! palette reset\n");
        ret = reset_palette(s);
        if (ret < 0)
            return 0;
        initial_palette_count = 0;
        ret = process_colors(s, pict, cur_frame);
        if (ret != 0)
            return ret;
    }

#if 1
av_log(NULL, AV_LOG_INFO, "%d palette entries\n", s->palette_count);
if (s->palette_count > initial_palette_count)
  av_log(NULL, AV_LOG_INFO, "  **** more colors found!\n");
#endif

    /* diff the the previous frame and the current frame */
    if (!s->keyframe)
    {
        memset(s->diff, 0, s->frame_size);
        for (x = 0; x < s->frame_size; x++)
        {
            if (cur_frame[x] != prev_frame[x])
                s->diff[x] = cur_frame[x];
        }
    }

    /* encode the side channel data at the front of the frame */
    *enc_ptr++ = 0;  /* top left */
    *enc_ptr++ = 0;
    *enc_ptr++ = 0;  /* top right */
    *enc_ptr++ = 0;
    *enc_ptr++ = ((pict->width - 1) >> 8) & 0xFF;  /* width */
    *enc_ptr++ = ((pict->width - 1) >> 0) & 0xFF;
    *enc_ptr++ = ((pict->height - 1) >> 8) & 0xFF;  /* height */
    *enc_ptr++ = ((pict->height - 1) >> 0) & 0xFF;
    if (initial_palette_count == 0)
        *enc_ptr++ = 1;  /* new palette incoming */
    else
        *enc_ptr++ = 0;  /* no new palette */
    *enc_ptr++ = s->palette_count - initial_palette_count;

    /* update the palette sent to the muxer */
    if (s->palette_count > initial_palette_count)
    {
        memset(palette, 0, PALETTE_SIZE);
        av_tree_enumerate(s->palette, palette, NULL, palette_enumerate);
        memcpy(enc_ptr, palette, PALETTE_SIZE);
    }
    enc_ptr += PALETTE_SIZE;

    /* copy whole frame for now */
    *enc_ptr++ = 2;  /* uncompressed, raw video */
    memcpy(enc_ptr, cur_frame, s->frame_size);

    s->current_frame = !s->current_frame;

    if (s->keyframe)
    {
        pkt->flags |= AV_PKT_FLAG_KEY;
        s->keyframe = 0;
    }
    *got_packet = 1;
    return 0;
}

static av_cold int vmdvideo_encode_end(AVCodecContext *avctx)
{
    VmdVideoEncContext *const s = avctx->priv_data;

    av_free(s->frames[0]);
    av_free(s->frames[1]);
    av_free(s->diff);

    return 0;
}

AVCodec ff_vmdvideo_encoder = {
    .name           = "vmdvideo",
    .long_name      = NULL_IF_CONFIG_SMALL("Sierra VMD video"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_VMDVIDEO,
    .priv_data_size = sizeof(VmdVideoEncContext),
    .init           = vmdvideo_encode_init,
    .encode2        = vmdvideo_encode_frame,
    .close          = vmdvideo_encode_end,
    .pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_BGR24,
                                                     AV_PIX_FMT_NONE },
};
