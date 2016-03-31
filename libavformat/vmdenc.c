/*
 * Sierra VMD File muxer.
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

#define VMD_HEADER_SIZE 0x32E
#define PALETTE_SIZE 768
#define VMD_SIDE_DATA_SIZE ((2*4) + 1 + 1 + PALETTE_SIZE)
#define FRAME_TABLE_INC_SIZE 100

typedef struct
{
    int64_t offset;
    uint32_t size;
} FrameTableEntry;

typedef struct
{
    int video_width;
    int video_height;
    int frame_size;
    int video_stream;
    int audio_stream;
    FrameTableEntry *frame_table;
    int video_frame_table_size;
    int video_frame_count;
    int64_t palette_offset;  /* points to the current base palette offset */
} VmdEncContext;

static int vmd_write_header(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    VmdEncContext *vmd = s->priv_data;
    uint8_t palette[PALETTE_SIZE];
    int i;

    /* iterate through the streams and figure out which is audio and video */
    vmd->video_stream = -1;
    vmd->audio_stream = -1;
    for (i = 0; i < s->nb_streams; i++)
    {
        if (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
            vmd->video_stream = i;
        if (s->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
            vmd->audio_stream = i;
    }

    /* initialize frame table */
    vmd->video_frame_table_size = FRAME_TABLE_INC_SIZE;
    vmd->frame_table = av_malloc(vmd->video_frame_table_size * sizeof(FrameTableEntry));
    if (!vmd->frame_table)
        return -1;

    vmd->video_frame_count = 0;
    vmd->video_width = s->streams[vmd->video_stream]->codec->width;
    vmd->video_height = s->streams[vmd->video_stream]->codec->height;
    vmd->frame_size = vmd->video_width * vmd->video_height;
    vmd->palette_offset = 28;  /* initial offset, found in the main header */

    /* placeholder palette until the first frame transports the correct one */
    memset(palette, 0, PALETTE_SIZE);

    /* write the header (with a lot of placeholders) */
    avio_wl16(pb, VMD_HEADER_SIZE);  /* 0-1: header size */
    avio_wl16(pb, 0);  /* 2-3: VMD handle */
    avio_wl16(pb, 1);  /* 4-5: unknown; real sample use 1 */
    avio_wl16(pb, 0);  /* 6-7: number of blocks in ToC (fill in later) */
    avio_wl16(pb, 0);  /* 8-9: top corner coordinate of video frame */
    avio_wl16(pb, 0);  /* 10-11: left corner coordinate of video frame */
    avio_wl16(pb, vmd->video_width); /* 12-13: width of video frame */
    avio_wl16(pb, vmd->video_height); /* 14-15: height of video frame */
    avio_wl16(pb, 0x4081);  /* 16-17: flags; unknown, except for 0x1000, which indicates audio */
    avio_wl16(pb, 1);  /* 18-19: frames per block */
    avio_wl32(pb, 2 + VMD_HEADER_SIZE);  /* 20-23: absolute offset of multimedia data */
    avio_wl16(pb, 0);  /* 24-25: unknown */
    avio_w8(pb, 0xf7);  /* 26: unknown */
    avio_w8(pb, 0x23);  /* 27: unknown */
    avio_write(pb, palette, PALETTE_SIZE);  /* initially zero palette (fill in later) */
    avio_wl32(pb, vmd->frame_size + 1); /* bytes 796-799: buffer size for data frame load buffer (set equal to max possible; fill in more accurate value later) */
    avio_wl32(pb, vmd->frame_size + 1); /* bytes 800-803: buffer size needed for decoding */
    avio_wl16(pb, 0);  /* bytes 804-805: audio sample rate */
    avio_wl16(pb, 0);  /* bytes 806-807: audio frame length / sample resolution */
    avio_wl16(pb, 0);  /* bytes 808-809: number of sound buffers */
    avio_wl16(pb, 0);  /* bytes 810-811: audio flags */
    avio_wl32(pb, 0);  /* bytes 812-815: absolute offset of ToC (fill in later) */

    return 0;
}

static int vmd_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    VmdEncContext *vmd = s->priv_data;
//    AVCodecContext *enc = s->streams[0]->codec;
    AVIOContext *pb = s->pb;
    int64_t current_offset;
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

/* skip non-video streams for now */
if (pkt->stream_index != vmd->video_stream)
{
av_log(NULL, AV_LOG_INFO, "audio: %d bytes\n", pkt->size);
    return 0;
}

    /* first video frame; fetch the palette and rewind to write in header */
    if (pkt->stream_index == vmd->video_stream && new_palette_entries > 0)
    {
        /* get the current offset before seeking back */
        current_offset = avio_seek(pb, 0, SEEK_CUR);

        /* go back to the palette in the header */
        avio_seek(pb, vmd->palette_offset, SEEK_SET);

        /* copy the new palette entries */
        avio_write(pb, palette, new_palette_entries * 3);
        vmd->palette_offset += (new_palette_entries * 3);

        /* go back to the current position */
        avio_seek(pb, current_offset, SEEK_SET);
    }

    /* allocate more space in the frame table as needed */
    if (vmd->video_frame_count == vmd->video_frame_table_size)
    {
        vmd->video_frame_table_size += FRAME_TABLE_INC_SIZE;
        vmd->frame_table = av_realloc(vmd->frame_table, vmd->video_frame_table_size * sizeof(FrameTableEntry));
        if (!vmd->frame_table)
            return -1;
    }

    /* note the current offset and the frame length */
    vmd->frame_table[vmd->video_frame_count].offset = avio_tell(pb);
    vmd->frame_table[vmd->video_frame_count].size = enc_size;

    avio_write(pb, enc_ptr, enc_size);

    vmd->video_frame_count++;

    return 0;
}

static int vmd_write_trailer(AVFormatContext *s)
{
    AVIOContext *pb = s->pb;
    VmdEncContext *vmd = s->priv_data;
    int i;
    int64_t toc_offset;

    /* note the ToC offset to write into the header */
    toc_offset = avio_tell(pb);

    /* write the block table */
    for (i = 0; i < vmd->video_frame_count; i++)
    {
        avio_wl16(pb, 0);  /* unknown */
        avio_wl32(pb, vmd->frame_table[i].offset);
    }

    /* write the frame table */
    for (i = 0; i < vmd->video_frame_count; i++)
    {
        avio_w8(pb, 2);  /* byte 0: video frame */
        avio_w8(pb, 0);  /* byte 1: unknown */
        avio_wl32(pb, vmd->frame_table[i].size);  /* bytes 2-5: size */
        avio_wl16(pb, 0);  /* bytes 6-7: left coordinate of frame */
        avio_wl16(pb, 0);  /* bytes 8-9: top coordinate of frame */
        avio_wl16(pb, vmd->video_width - 1);  /* bytes 10-11: right coordinate of frame */
        avio_wl16(pb, vmd->video_height - 1);  /* bytes 12-13: bottom coordinate of frame */
        avio_w8(pb, 0);  /* byte 14: unknown */
        avio_w8(pb, 0);  /* byte 15: new palette */

        avio_w8(pb, 1);  /* byte 0: audio frame */
        avio_w8(pb, 0);  /* byte 1: unknown */
        avio_wl32(pb, 0);  /* bytes 2-5: frame length */
        avio_w8(pb, 0);  /* byte 6: audio flags */
        avio_w8(pb, 0);  /* byte 7: unknown */
        avio_wl32(pb, 0);  /* bytes 8-11: unknown */
        avio_wl32(pb, 0);  /* bytes 12-15: unknown */
    }

    /* fill in the missing items in the header */
    avio_seek(pb, 6, SEEK_SET);
    avio_wl16(pb, vmd->video_frame_count);
    avio_seek(pb, 812, SEEK_SET);
    avio_wl32(pb, toc_offset);

    return 0;
}

AVOutputFormat ff_vmd_muxer = {
    .name              = "vmd",
    .long_name         = NULL_IF_CONFIG_SMALL("Sierra VMD"),
    .extensions        = "vmd",
    .priv_data_size    = sizeof(VmdEncContext),
    .audio_codec       = AV_CODEC_ID_PCM_S16LE,
    .video_codec       = AV_CODEC_ID_VMDVIDEO,
    .write_header      = vmd_write_header,
    .write_packet      = vmd_write_packet,
    .write_trailer     = vmd_write_trailer,
    .flags             = AVFMT_NOTIMESTAMPS,
};
