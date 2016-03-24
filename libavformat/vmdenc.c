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

typedef struct
{
    int64_t offset;
    uint32_t size;
} FrameTableEntry;

typedef struct
{
    int video_frame_count;
    int video_width;
    int video_height;
    int video_stream;
    int audio_stream;
    FrameTableEntry frame_table[100];
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

    vmd->video_frame_count = 0;
    vmd->video_width = 280;
    vmd->video_height = 218;

    /* placeholder palette until the first frame transports the correct one */
    memset(palette, 0, PALETTE_SIZE);

    /* write the header (with a lot of placeholders) */
    avio_wl16(pb, VMD_HEADER_SIZE);  /* 0-1: header size */
    avio_wl16(pb, 0);  /* 2-3: VMD handle */
    avio_wl16(pb, 0);  /* 4-5: unknown */
    avio_wl16(pb, 0);  /* 6-7: number of blocks in ToC (fill in later) */
    avio_wl16(pb, 0);  /* 8-9: top corner coordinate of video frame */
    avio_wl16(pb, 0);  /* 10-11: left corner coordinate of video frame */
    avio_wl16(pb, vmd->video_width); /* 12-13: width of video frame */
    avio_wl16(pb, vmd->video_height); /* 14-15: height of video frame */
    avio_wl16(pb, 0);  /* 16-17: flags */
    avio_wl16(pb, 1);  /* 18-19: frames per block */
    avio_wl32(pb, 2 + VMD_HEADER_SIZE);  /* 20-23: absolute offset of multimedia data */
    avio_wl32(pb, 0);  /* 24-27: unknown */
    avio_write(pb, palette, PALETTE_SIZE);  /* initially zero palette (fill in later) */
    avio_wl32(pb, 64000); /* bytes 796-799: buffer size for data frame load buffer */
    avio_wl32(pb, 64000); /* bytes 800-803: buffer size needed for decoding */
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
    uint8_t palette[AVPALETTE_SIZE];
    int i;

/* skip non-video streams */
if (pkt->stream_index != vmd->video_stream)
    return 0;

    /* first video frame; fetch the palette and rewind to write in header */
    if (pkt->stream_index == vmd->video_stream && vmd->video_frame_count == 0)
    {
        /* get the current offset before seeking back */
        current_offset = avio_seek(pb, 0, SEEK_CUR);

        /* go back to the palette in the header */
        avio_seek(pb, 28, SEEK_SET);

        /* get the palette from the packet */
        ff_get_packet_palette(s, pkt, CONTAINS_PAL, (uint32_t*)palette);

        /* write the palette; scale down to 0..63 range */
        for (i = 0; i < 256; i++)
        {
            avio_w8(pb, palette[i*4+2] >> 2);
            avio_w8(pb, palette[i*4+1] >> 2);
            avio_w8(pb, palette[i*4+0] >> 2);
        }

        /* go back to the current position */
        avio_seek(pb, current_offset, SEEK_SET);
    }

    /* note the current offset and the frame length */
    vmd->frame_table[vmd->video_frame_count].offset = avio_tell(pb);
    vmd->frame_table[vmd->video_frame_count].size = 1 + vmd->video_width * vmd->video_height;

    avio_w8(pb, 2);  /* uncompressed, raw video */
    avio_write(pb, pkt->data, pkt->size);

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
        avio_wl16(pb, 0);
        avio_wl32(pb, 0);
        avio_wl32(pb, 0);
        avio_wl32(pb, 0);
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
    .video_codec       = AV_CODEC_ID_RAWVIDEO,
    .write_header      = vmd_write_header,
    .write_packet      = vmd_write_packet,
    .write_trailer     = vmd_write_trailer,
//    .codec_tag         = (const AVCodecTag* const []){ ff_voc_codec_tags, 0 },
    .flags             = AVFMT_NOTIMESTAMPS,
};
