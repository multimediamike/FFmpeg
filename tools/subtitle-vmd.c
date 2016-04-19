#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ass/ass.h>

#define LE_16(x) ((((uint8_t*)(x))[1] <<  8) | ((uint8_t*)(x))[0])

#define LE_32(x) (((uint32_t)(((uint8_t*)(x))[3]) << 24) |  \
                             (((uint8_t*)(x))[2]  << 16) |  \
                             (((uint8_t*)(x))[1]  <<  8) |  \
                              ((uint8_t*)(x))[0])

#define AV_RL16 LE_16

#define VMD_HEADER_SIZE 0x330
#define BLOCK_RECORD_SIZE 6
#define FRAME_RECORD_SIZE 16
#define PALETTE_COUNT 768
#define SUBTITLE_THRESHOLD 0x70

typedef struct
{
    uint16_t unknown_b0_b1;
    uint32_t offset;
} block_record;

typedef struct
{
    uint8_t type;
    uint8_t unknown_b1;
    uint32_t length;
    uint16_t leftedge;
    uint16_t topedge;
    uint16_t rightedge;
    uint16_t bottomedge;
    uint8_t unknown_b14;
    uint8_t video_flags;
} frame_record;

typedef struct 
{
    uint8_t header[VMD_HEADER_SIZE];
    int width;
    int height;
    int block_count;
    int frames_per_block;
    block_record *blocks;
    frame_record *frames;

    uint8_t *buf;
    int size;

    uint8_t *frame;
    uint8_t *prev_frame;
    int frame_size;

    uint8_t fg_index;
    uint8_t bg_index;

    ASS_Library *ass_lib;
    ASS_Renderer *ass_renderer;
    ASS_Track *ass_track;
} vmd_dec_context;

/* compute Euclidean distance between an RGB color and the desired target */
static int compute_rgb_distance(int r1, int r2, int g1, int g2, int b1, int b2)
{
    return sqrt((r1 - r2) * (r1 - r2) +
                (g1 - g2) * (g1 - g2) +
                (b1 - b2) * (b1 - b2));
}

/* This function finds the colors in the palette closest to black and white.
 * The palette parameter needs to point to 768 bytes. */
static void find_subtitle_colors(vmd_dec_context *vmd, uint8_t *palette)
{
    int i;
    uint8_t r, g, b;
    int distance;
    int closest_fg_distance;
    int closest_bg_distance;

    closest_fg_distance = 999999999;
    closest_bg_distance = 999999999;

    for (i = 0; i < PALETTE_COUNT; i++)
    {
        r = palette[i * 3 + 0];
        g = palette[i * 3 + 1];
        b = palette[i * 3 + 2];

        /* find closest foreground match (63, 63, 63) */
        distance = compute_rgb_distance(r, 63, g, 63, b, 63);
        if (distance < closest_fg_distance)
        {
            closest_fg_distance = distance;
            vmd->fg_index = i;
        }

        /* find closest background match (0, 0, 0) */
        distance = compute_rgb_distance(r, 0, g, 0, b, 0);
        if (distance < closest_bg_distance)
        {
            closest_bg_distance = distance;
            vmd->bg_index = i;
        }
    }
}

static int load_and_copy_vmd_header(vmd_dec_context *vmd, FILE *invmd_file, FILE *outvmd_file)
{
    int i;
    uint32_t toc_offset;
    unsigned char buf[FRAME_RECORD_SIZE];
    uint32_t max_length;
//    uint32_t max_buffer_size;

    fseek(invmd_file, 0, SEEK_SET);
    fseek(outvmd_file, 0, SEEK_SET);

    /* load the header */
    if (fread(vmd->header, VMD_HEADER_SIZE, 1, invmd_file) != 1)
    {
        printf("problem reading initial VMD header\n");
        return 0;
    }

    /* load some interesting pieces */
    vmd->width = LE_16(&vmd->header[12]);
    vmd->height = LE_16(&vmd->header[14]);
    vmd->block_count = LE_16(&vmd->header[6]);
    vmd->frames_per_block = LE_16(&vmd->header[18]);
    toc_offset = LE_32(&vmd->header[812]);
    max_length = 0;

    /* copy to the output */
    if (fwrite(vmd->header, VMD_HEADER_SIZE, 1, outvmd_file) != 1)
    {
        printf("problem writing initial VMD header\n");
        return 0;
    }

    /* find the best colors to use for subtitles */
    find_subtitle_colors(vmd, &vmd->header[28]);

    /* load the ToC */
    fseek(invmd_file, toc_offset, SEEK_SET);
    vmd->blocks = malloc(sizeof(block_record) * vmd->block_count);
    vmd->frames = malloc(sizeof(frame_record) * vmd->block_count * vmd->frames_per_block);

    /* block table */
    for (i = 0; i < vmd->block_count; i++)
    {
        if (fread(buf, BLOCK_RECORD_SIZE, 1, invmd_file) != 1)
        {
            printf("failed to read block record\n");
            return 0;
        }
        vmd->blocks[i].unknown_b0_b1 = LE_16(&buf[0]);
        vmd->blocks[i].offset = LE_32(&buf[2]);
    }
    /* frame table */
    for (i = 0; i < vmd->block_count * vmd->frames_per_block; i++)
    {
        if (fread(buf, FRAME_RECORD_SIZE, 1, invmd_file) != 1)
        {
            printf("failed to read frame record\n");
            return 0;
        }
        vmd->frames[i].type = buf[0];
        vmd->frames[i].unknown_b1 = buf[1];
        vmd->frames[i].length = LE_32(&buf[2]);
        vmd->frames[i].leftedge = LE_16(&buf[6]);
        vmd->frames[i].topedge = LE_16(&buf[8]);
        vmd->frames[i].rightedge = LE_16(&buf[10]);
        vmd->frames[i].bottomedge = LE_16(&buf[12]);
        vmd->frames[i].unknown_b14 = buf[14];
        vmd->frames[i].video_flags = buf[15];

        if (vmd->frames[i].length > max_length)
            max_length = vmd->frames[i].length;
    }

    /* allocate stuff based on new information */
    vmd->buf = malloc(max_length);
    vmd->frame_size = vmd->width * vmd->height;
    vmd->prev_frame = malloc(vmd->frame_size);
    vmd->frame = malloc(vmd->frame_size);

    /* success */
    return 1;
}

static int copy_blocks(vmd_dec_context *vmd, FILE *invmd_file, FILE *raw_file,
    FILE *outvmd_file, int raw_frame_count)
{
    int b, f, i;
    int x, y;
    ASS_Image *subtitles;
    int detect_change;
    int subtitle_count;
    uint8_t subtitle_pixel;
    uint8_t *frame_ptr;
    uint8_t *subtitle_ptr;

    i = 0;
    for (b = 0; b < vmd->block_count; b++)
    {
        /* seek to the start of the block in the input VMD */
        fseek(invmd_file, vmd->blocks[b].offset, SEEK_SET);
        /* this block starts where the output VMD is currently pointing */
        vmd->blocks[b].offset = ftell(outvmd_file);
        for (f = 0; f < vmd->frames_per_block; f++, i++)
        {
            if (!vmd->frames[i].length)
                continue;

            if (fread(vmd->buf, vmd->frames[i].length, 1, invmd_file) != 1)
            {
                printf("failed to read frame\n");
                return 0;
            }

#if 0
/* phase 1: straight copy */
            /* copy the frame */
            if (fwrite(vmd->buf, vmd->frames[i].length, 1, outvmd_file) != 1)
            {
                printf("failed to write frame\n");
                return 0;
            }
#else
/* phase 2: grab the corresponding frame from the side channel file */
            if (vmd->frames[i].type == 2)
            {
                /* stretch the change window */
                vmd->frames[i].leftedge = 0;
                vmd->frames[i].topedge = 0;
                vmd->frames[i].rightedge = vmd->width - 1;
                vmd->frames[i].bottomedge = vmd->height - 1;

                /* frame accounting */
                raw_frame_count--;
                if (raw_frame_count < 0)
                {
                    printf("ran out of raw frames\n");
                    return 1;
                }

                /* if the frame includes a palette, write that first
                 * (2 info bytes + 768 palette bytes) */
                vmd->frames[i].length = 0;
                if (vmd->frames[i].video_flags & 0x02)
                {
                    vmd->frames[i].length = 2 + PALETTE_COUNT * 3;
                    fwrite(vmd->buf, vmd->frames[i].length, 1, outvmd_file);
                    find_subtitle_colors(vmd, &vmd->buf[2]);
                }

                /* raw encoding method */
                fputc(2, outvmd_file);
                vmd->frames[i].length++;

                /* grab the corresponding frame from the side channel file */
                fread(vmd->frame, vmd->frame_size, 1, raw_file);

#if 1
                /* ask library for the subtitle for this timestamp */
                subtitles = ass_render_frame(vmd->ass_renderer, vmd->ass_track,
                    b * 100, &detect_change);

                /* render the list of subtitles onto the decoded frame */
                subtitle_count = 0;
                while (subtitles)
                {
                    if (subtitle_count == 0)
                        subtitle_pixel = vmd->bg_index;
                    else if (subtitle_count == 1)
                        subtitle_pixel = vmd->fg_index;
                    else
                        printf("HELP! more than 2 subtitle layers\n");
                    subtitle_count++;
                    for (y = 0; y < subtitles->h; y++)
                    {
                        subtitle_ptr = &subtitles->bitmap[y * subtitles->stride];
                        frame_ptr = &vmd->frame[(subtitles->dst_y + y) * vmd->width + subtitles->dst_x];
                        for (x = 0; x < subtitles->w; x++, frame_ptr++, subtitle_ptr++)
                        {
                            if (*subtitle_ptr >= SUBTITLE_THRESHOLD)
                                *frame_ptr = subtitle_pixel;
                        }
                    }
                    subtitles = subtitles->next;
                }
#endif

                /* write the raw frame to the output file */
                fwrite(vmd->frame, vmd->frame_size, 1, outvmd_file);

                /* adjust length */
                vmd->frames[i].length += vmd->frame_size;
            }
            else
            {
                /* copy the frame from the original file */
                if (fwrite(vmd->buf, vmd->frames[i].length, 1, outvmd_file) != 1)
                {
                    printf("failed to write frame\n");
                    return 0;
                }
            }
#endif
        }
    }

    /* success */
    return 1;
}

static int write_new_toc(vmd_dec_context *vmd, FILE *outvmd_file)
{
    uint32_t toc_offset;
    int i;
    uint8_t buf[FRAME_RECORD_SIZE];

    /* save current position so it can be recorded in the header */
    toc_offset = ftell(outvmd_file);

    /* write the block table */
    for (i = 0; i < vmd->block_count; i++)
    {
        buf[0] = vmd->blocks[i].unknown_b0_b1 & 0xFF;
        buf[1] = (vmd->blocks[i].unknown_b0_b1 >> 8) & 0xFF;
        buf[2] = (vmd->blocks[i].offset >>  0) & 0xFF;
        buf[3] = (vmd->blocks[i].offset >>  8) & 0xFF;
        buf[4] = (vmd->blocks[i].offset >> 16) & 0xFF;
        buf[5] = (vmd->blocks[i].offset >> 24) & 0xFF;
        if (fwrite(buf, BLOCK_RECORD_SIZE, 1, outvmd_file) != 1)
        {
            printf("failed to write block record\n");
            return 0;
        }
    }

    /* write the frame table */
    for (i = 0; i < vmd->block_count * vmd->frames_per_block; i++)
    {
        buf[0] = vmd->frames[i].type;
        buf[1] = vmd->frames[i].unknown_b1;
        buf[2] = (vmd->frames[i].length >> 0) & 0xFF;
        buf[3] = (vmd->frames[i].length >> 8) & 0xFF;
        buf[4] = (vmd->frames[i].length >> 16) & 0xFF;
        buf[5] = (vmd->frames[i].length >> 24) & 0xFF;
        buf[6] = (vmd->frames[i].leftedge >> 0) & 0xFF;
        buf[7] = (vmd->frames[i].leftedge >> 8) & 0xFF;
        buf[8] = (vmd->frames[i].topedge >> 0) & 0xFF;
        buf[9] = (vmd->frames[i].topedge >> 8) & 0xFF;
        buf[10] = (vmd->frames[i].rightedge >> 0) & 0xFF;
        buf[11] = (vmd->frames[i].rightedge >> 8) & 0xFF;
        buf[12] = (vmd->frames[i].bottomedge >> 0) & 0xFF;
        buf[13] = (vmd->frames[i].bottomedge >> 8) & 0xFF;
        buf[14] = vmd->frames[i].unknown_b14;
        buf[15] = vmd->frames[i].video_flags;
        if (fwrite(buf, FRAME_RECORD_SIZE, 1, outvmd_file) != 1)
        {
            printf("failed to write frame record\n");
            return 0;
        }
    }

    /* record the new ToC */
    fseek(outvmd_file, 812, SEEK_SET);
    buf[0] = (toc_offset >>  0) & 0xFF;
    buf[1] = (toc_offset >>  8) & 0xFF;
    buf[2] = (toc_offset >> 16) & 0xFF;
    buf[3] = (toc_offset >> 24) & 0xFF;
    if (fwrite(buf, sizeof(uint32_t), 1, outvmd_file) != 1)
    {
        printf("failed to new ToC offset\n");
        return 0;
    }

    /* success */
    return 1;
}

int main(int argc, char *argv[])
{
    char *subtitle_filename;
    FILE *subtitle_file;
    char *invmd_filename;
    FILE *invmd_file;
    char *raw_filename;
    FILE *raw_file;
    char *outvmd_filename;
    FILE *outvmd_file;
    vmd_dec_context vmd;
    uint8_t read_buf[6];
    int raw_frame_count;
    int raw_width;
    int raw_height;

    /* validate the number of arguments */
    if (argc != 5)
    {
        printf("USAGE: subtitle-vmd <subtitles.ass> <in.vmd> <raw-vmd-frames> <out.vmd>\n");
        return 1;
    }
    subtitle_filename = argv[1];
    invmd_filename = argv[2];
    raw_filename = argv[3];
    outvmd_filename = argv[4];

    /* verify that the specified input files are valid */
    subtitle_file = fopen(subtitle_filename, "r");
    if (!subtitle_file)
    {
        perror(subtitle_filename);
        return 1;
    }
    fclose(subtitle_file);
    invmd_file = fopen(invmd_filename, "rb");
    if (!invmd_file)
    {
        perror(invmd_filename);
        return 1;
    }
    raw_file = fopen(raw_filename, "rb");
    if (!raw_file)
    {
        perror(raw_filename);
        return 1;
    }
    /* load the short header from the raw frame file */
    fread(read_buf, 6, 1, raw_file);
    raw_frame_count = LE_16(&read_buf[0]);
    raw_width = LE_16(&read_buf[2]);
    raw_height = LE_16(&read_buf[4]);

    /* open the output file */
    outvmd_file = fopen(outvmd_filename, "wb");
    if (!outvmd_file)
    {
        perror(outvmd_filename);
        return 1;
    }

    /* transfer the header and process a copy into memory at the same time */
    if (!load_and_copy_vmd_header(&vmd, invmd_file, outvmd_file))
        return 1;

    /* initialize the ASS library */
    vmd.ass_lib = ass_library_init();
    vmd.ass_renderer = ass_renderer_init(vmd.ass_lib);
    vmd.ass_track = ass_read_file(vmd.ass_lib, subtitle_filename, "UTF-8");
    ass_set_frame_size(vmd.ass_renderer, vmd.width, vmd.height);
    ass_set_fonts(vmd.ass_renderer, NULL, NULL, 1, NULL, 1);

    /* make sure the dimensions match up */
    if ((vmd.width != raw_width) && (vmd.height != raw_height))
    {
        printf("input file's dimensions do not match the raw file's dimensions\n");
        return 1;
    }

    /* go back to the start and transfer the individual blocks and frames */
    fseek(invmd_file, VMD_HEADER_SIZE, SEEK_SET);
    if (!copy_blocks(&vmd, invmd_file, raw_file, outvmd_file, raw_frame_count))
        return 1;

    /* write the new ToC */
    if (!write_new_toc(&vmd, outvmd_file))
        return 1;

    fclose(invmd_file);
    fclose(outvmd_file);

    return 0;
}
