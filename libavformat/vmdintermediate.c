/*
 * Sierra VMD intermediate frame muxer.
 * Copyright (c) 2016  Mike Melanson <mike@multimedia.cx>
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

/*
 * Based on this documentation:
 *  http://wiki.multimedia.cx/index.php?title=VMD
 */

#include "internal.h"

#define PALETTE_SIZE 768
#define VMD_SIDE_DATA_SIZE ((2*4) + 1 + 1 + PALETTE_SIZE)

typedef struct
{
    int64_t frame_count_offset;
    int frame_count;
    uint8_t palette[PALETTE_SIZE];
    int palette_count;
} VmdEncContext;

static int vmd_write_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    VmdEncContext *vmd = (VmdEncContext*)s->priv_data;

    memset(vmd->palette, 0, PALETTE_SIZE);
    vmd->palette_count = 0;

    avio_put_str(pb, "VMD Intermediate Frames");
    vmd->frame_count_offset = avio_tell(pb);
    avio_wl32(pb, 0);  /* frame count; fill in later */
    avio_write(pb, vmd->palette, PALETTE_SIZE);

    return 0;
}

static int vmd_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    VmdEncContext *vmd = (VmdEncContext*)s->priv_data;
    uint8_t *enc_ptr;
    int enc_size;

    /* for parsing the side data */
    int left_coord;
    int top_coord;
    int right_coord;
    int bottom_coord;
    int new_palette;
    int new_palette_entries;
    uint8_t *palette;

    /* parse out the side data */
    enc_ptr = pkt->data;
    enc_size = pkt->size - VMD_SIDE_DATA_SIZE;
    left_coord = (enc_ptr[0] << 8)| enc_ptr[1];
    top_coord = (enc_ptr[2] << 8)| enc_ptr[3];
    right_coord = (enc_ptr[4] << 8)| enc_ptr[5];
    bottom_coord = (enc_ptr[6] << 8)| enc_ptr[7];
    enc_ptr += 8;
    new_palette = *enc_ptr++;
    new_palette_entries = *enc_ptr++;
    palette = enc_ptr;
    enc_ptr += PALETTE_SIZE;

    /* wipe the existing palette if a new palette is incoming */
    if (new_palette)
    {
        memset(vmd->palette, 0, PALETTE_SIZE);
        vmd->palette_count = 0;
    }

    /* first video frame; fetch the palette and rewind to write in header */
    if (new_palette_entries > 0)
    {
        /* copy the new palette entries */
        memcpy(&vmd->palette[vmd->palette_count * 3], palette, new_palette_entries * 3);
        vmd->palette_count += new_palette_entries;
    }

    /* write the frame's palette */
    avio_write(pb, vmd->palette, PALETTE_SIZE);

    /* write the frame's dimension and encoding size and, finally, frame data */
    avio_wl16(pb, left_coord);
    avio_wl16(pb, top_coord);
    avio_wl16(pb, right_coord);
    avio_wl16(pb, bottom_coord);
    avio_wl32(pb, enc_size);
    avio_write(pb, enc_ptr, enc_size);

    vmd->frame_count++;

    return 0;
}

static int vmd_write_trailer(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    VmdEncContext *vmd = (VmdEncContext*)s->priv_data;

    /* go back to write the frame count */
    avio_seek(pb, vmd->frame_count_offset, SEEK_SET);
    avio_wl32(pb, vmd->frame_count);

    return 0;
}

AVOutputFormat ff_vmd_muxer = {
    .name              = "vmd",
    .long_name         = NULL_IF_CONFIG_SMALL("Sierra VMD"),
    .extensions        = "vmd",
    .priv_data_size    = sizeof(VmdEncContext),
    .video_codec       = AV_CODEC_ID_VMDVIDEO,
    .write_header      = vmd_write_header,
    .write_packet      = vmd_write_packet,
    .write_trailer     = vmd_write_trailer,
    .flags             = AVFMT_NOTIMESTAMPS,
};
