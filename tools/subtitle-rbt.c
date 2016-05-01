/*
 * subtitle-rbt.c
 *
 * build with this command:
 *   gcc -g -Wall subtitle-rbt.c -o subtitle-rbt -lm -lass
 */

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

#define PALETTE_COUNT 256
#define RBT_HEADER_SIZE 60
#define UNKNOWN_TABLE_SIZE (1024+512)

typedef struct
{
    int version;
    int width;
    int height;
    int frame_count;
    uint8_t palette[PALETTE_COUNT * 3];
    uint16_t *frame_sizes;
} rbt_dec_context;

static int load_and_copy_rbt_header(rbt_dec_context *rbt, FILE *inrbt_file, FILE *outrbt_file)
{
    uint8_t header[RBT_HEADER_SIZE];
    int palette_data_size;
    uint8_t *palette_chunk;
    int unknown_chunk_size;
    uint8_t *unknown_chunk;
    uint8_t *frame_info;
    uint8_t unknown_table[UNKNOWN_TABLE_SIZE];
    off_t padding_size;
    uint8_t *padding;
    int i;

    fseek(inrbt_file, 0, SEEK_SET);
    fseek(outrbt_file, 0, SEEK_SET);

    /* load the header */
    if (fread(header, RBT_HEADER_SIZE, 1, inrbt_file) != 1)
    {
        printf("problem reading initial RBT header\n");
        return 0;
    }

    /* copy header to the output */
    if (fwrite(header, RBT_HEADER_SIZE, 1, outrbt_file) != 1)
    {
        printf("problem writing initial RBT header\n");
        return 0;
    }

    rbt->version = LE_16(&header[6]);
    rbt->frame_count = LE_16(&header[14]);

    /* transfer the unknown data, if it's there */
    unknown_chunk_size = LE_16(&header[18]);
    if (unknown_chunk_size > 0)
    {
        unknown_chunk = malloc(unknown_chunk_size);
        if (fread(unknown_chunk, unknown_chunk_size, 1, inrbt_file) != 1)
        {
            printf("problem reading unknown data\n");
            return 0;
        }
        if (fwrite(unknown_chunk, unknown_chunk_size, 1, outrbt_file) != 1)
        {
            printf("problem writing unknown data\n");
            return 0;
        }
        free(unknown_chunk);
    }

    /* transfer the palette chunk */
    palette_data_size = LE_16(&header[16]);
    palette_chunk = malloc(palette_data_size);
    if (fread(palette_chunk, palette_data_size, 1, inrbt_file) != 1)
    {
        printf("problem reading palette\n");
        return 0;
    }
    if (fwrite(palette_chunk, palette_data_size, 1, outrbt_file) != 1)
    {
        printf("problem writing palette\n");
        return 0;
    }
    free(palette_chunk);

    frame_info = malloc(rbt->frame_count * sizeof(uint16_t));

    /* load the first frame table (2 bytes per frame) */
    if (fread(frame_info, rbt->frame_count * sizeof(uint16_t), 1, inrbt_file) != 1)
    {
        printf("problem reading frame table\n");
        return 0;
    }
    if (fwrite(frame_info, rbt->frame_count * sizeof(uint16_t), 1, outrbt_file) != 1)
    {
        printf("problem writing frame table\n");
        return 0;
    }

    /* load the second frame table (2 bytes per frame) */
    if (fread(frame_info, rbt->frame_count * sizeof(uint16_t), 1, inrbt_file) != 1)
    {
        printf("problem reading frame table\n");
        return 0;
    }
    if (fwrite(frame_info, rbt->frame_count * sizeof(uint16_t), 1, outrbt_file) != 1)
    {
        printf("problem writing frame table\n");
        return 0;
    }

    /* load the frame sizes */
    rbt->frame_sizes = malloc(rbt->frame_count * sizeof(uint16_t));
    for (i = 0; i < rbt->frame_count; i++)
    {
        rbt->frame_sizes[i] = LE_16(&frame_info[i*2]);
    }
    free(frame_info);

    /* transfer the unknown table(s) */
    if (fread(unknown_table, UNKNOWN_TABLE_SIZE, 1, inrbt_file) != 1)
    {
        printf("problem reading unknown table\n");
        return 0;
    }
    if (fwrite(unknown_table, UNKNOWN_TABLE_SIZE, 1, outrbt_file) != 1)
    {
        printf("problem writing unknown table\n");
        return 0;
    }

    /* copy over padding */
    padding_size = 0x800 - (ftell(inrbt_file) & 0x4CC);
    if (padding_size)
    {
        padding = malloc(padding_size);
        if (fread(padding, padding_size, 1, inrbt_file) != 1)
        {
            printf("problem reading padding\n");
            return 0;
        }
        if (fwrite(padding, padding_size, 1, outrbt_file) != 1)
        {
            printf("problem writing padding\n");
            return 0;
        }
        free(padding);
    }

    return 1;
}

static int copy_frames(rbt_dec_context *rbt, FILE *inrbt_file, FILE *outrbt_file)
{
    int i;
    uint8_t frame[64000];  /* this should be the largest seen frame size */

    for (i = 0; i < rbt->frame_count; i++)
    {
        if (fread(frame, rbt->frame_sizes[i], 1, inrbt_file) != 1)
        {
            printf("problem reading frame %d\n", i);
            return 0;
        }
        if (fwrite(frame, rbt->frame_sizes[i], 1, outrbt_file) != 1)
        {
            printf("problem writing frame %d\n", i);
            return 0;
        }
    }

    return 1;
}

int main(int argc, char *argv[])
{
    char *subtitle_filename;
    FILE *subtitle_file;
    char *inrbt_filename;
    FILE *inrbt_file;
    char *outrbt_filename;
    FILE *outrbt_file;
    rbt_dec_context rbt;

    /* validate the number of arguments */
    if (argc != 4)
    {
        printf("USAGE: subtitle-rbt <subtitles.ass> <in.rbt> <out.rbt>\n");
        return 1;
    }
    subtitle_filename = argv[1];
    inrbt_filename = argv[2];
    outrbt_filename = argv[3];

    /* verify that the specified input files are valid */
    subtitle_file = fopen(subtitle_filename, "r");
    if (!subtitle_file)
    {
        perror(subtitle_filename);
        return 1;
    }
    fclose(subtitle_file);
    inrbt_file = fopen(inrbt_filename, "rb");
    if (!inrbt_file)
    {
        perror(inrbt_filename);
        return 1;
    }

    /* open the output file */
    outrbt_file = fopen(outrbt_filename, "wb");
    if (!outrbt_file)
    {
        perror(outrbt_filename);
        return 1;
    }

    /* transfer header from input to output */
    if (!load_and_copy_rbt_header(&rbt, inrbt_file, outrbt_file))
        return 1;

    /* rewrite the frames */
    if (!copy_frames(&rbt, inrbt_file, outrbt_file))
        return 1;

    /* finished with files */
    fclose(inrbt_file);
    fclose(outrbt_file);

    return 0;
}
