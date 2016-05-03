/*
 * subtitle-rbt.c
 *  by Mike Melanson (mike -at- multimedia.cx)
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

/*********************************************************************/

/* Bit readers and writers */

typedef struct
{
    uint8_t *bytestream;
    int bytestream_size;
    int index;
    uint32_t bits;
    int bits_in_buffer;
} get_bits_context;

static inline void reload_bits(get_bits_context *gb)
{
    while (gb->bits_in_buffer <= 24)
    {
        gb->bits |= (gb->bytestream[gb->index++] << (24 - gb->bits_in_buffer));
        gb->bits_in_buffer += 8;
        if (gb->index >= gb->bytestream_size)
            return;
    }
}

static void init_get_bits(get_bits_context *gb, uint8_t *bytestream, int size)
{
    gb->bytestream = malloc(size);
    memcpy(gb->bytestream, bytestream, size);
    gb->bytestream_size = size;
    gb->index = 0;
    gb->bits = 0;
    gb->bits_in_buffer = 0;

    reload_bits(gb);
}

/* read bits without consuming them from the stream */
static int view_bits(get_bits_context *gb, int count)
{
    if (count >= 24)
        return -1;
    if (gb->bits_in_buffer < count)
        reload_bits(gb);
    return (gb->bits >> (32 - count));
}

/* read and consume bits from the stream */
static int read_bits(get_bits_context *gb, int count)
{
    int value;

    if (count >= 24)
        return -1;

    value = view_bits(gb, count);
    gb->bits <<= count;
    gb->bits_in_buffer -= count;

    return value;
}

static void delete_get_bits(get_bits_context *gb)
{
    free(gb->bytestream);
}

/*********************************************************************/

/* RBT functions */

#define PALETTE_COUNT 256
#define RBT_HEADER_SIZE 60
#define UNKNOWN_TABLE_SIZE (1024+512)

/* VLC table */
#define VLC_SIZE 4
static struct
{
    int count;
    int value;
} lzs_vlc_table[] =
{
    /* code length = 2 bits; value = 2 */
    /* 0000 */ { 2, 2 },
    /* 0001 */ { 2, 2 },
    /* 0010 */ { 2, 2 },
    /* 0011 */ { 2, 2 },

    /* code length = 2 bits; value = 3 */
    /* 0100 */ { 2, 3 },
    /* 0101 */ { 2, 3 },
    /* 0110 */ { 2, 3 },
    /* 0111 */ { 2, 3 },

    /* code length = 2 bits; value = 4 */
    /* 1000 */ { 2, 4 },
    /* 1001 */ { 2, 4 },
    /* 1010 */ { 2, 4 },
    /* 1011 */ { 2, 4 },

    /* code length = 4 bits; value = 5 */
    /* 1100 */ { 4, 5 },

    /* code length = 4 bits; value = 6 */
    /* 1101 */ { 4, 6 },

    /* code length = 4 bits; value = 7 */
    /* 1110 */ { 4, 7 },

    /* special case */
    /* 1111 */ { 4, 8 }
};

typedef struct
{
    int version;
    int width;
    int height;
    int frame_count;
    int audio_chunk_size;
    uint8_t palette[PALETTE_COUNT * 3];
    uint16_t *frame_sizes;
    uint8_t *frame_load_buffer;
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
    int max_frame_size;
    int first_palette_index;
    int palette_count;
    int palette_type;
    int palette_index;

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
    rbt->audio_chunk_size = LE_16(&header[8]);
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
    /* load the palette into the internal context */
    memset(rbt->palette, 0, PALETTE_COUNT * 3);
    first_palette_index = palette_chunk[25];
    palette_count = LE_16(&palette_chunk[29]);
    palette_type = palette_chunk[32];
    palette_index = (palette_type == 0) ? 38 : 37;
    for (i = first_palette_index; i < first_palette_index + palette_count; i++)
    {
        rbt->palette[i*3+0] = palette_chunk[palette_index++];
        rbt->palette[i*3+1] = palette_chunk[palette_index++];
        rbt->palette[i*3+2] = palette_chunk[palette_index++];
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
    max_frame_size = 0;
    for (i = 0; i < rbt->frame_count; i++)
    {
        rbt->frame_sizes[i] = LE_16(&frame_info[i*2]);
        if (rbt->frame_sizes[i] > max_frame_size)
            max_frame_size = rbt->frame_sizes[i];
    }
    rbt->frame_load_buffer = malloc(max_frame_size);
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
    padding_size = 0x800 - (ftell(inrbt_file) & 0x7FF);
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

static int get_lzs_back_ref_length(get_bits_context *gb)
{
    int vlc;
    int count;
    int value;

    vlc = view_bits(gb, VLC_SIZE);
    count = lzs_vlc_table[vlc].count;
    value = lzs_vlc_table[vlc].value;

    read_bits(gb, count);
    if (value == 8)
    {
        while (vlc == 0xF)
        {
            vlc = read_bits(gb, VLC_SIZE);
            value += vlc;
        }
    }

    return value;
}

static int copy_frames(rbt_dec_context *rbt, FILE *inrbt_file, FILE *outrbt_file)
{
    int i;
    int j;
    int scale;
    int width;
    int height;
    int x;
    int y;
    int compressed_size;
    int fragment_count;
    int decoded_size;
    uint8_t *decoded_frame;
    int fragment;
    int fragment_compressed_size;
    int fragment_decompressed_size;
    int compression_type;
    int index;
    int out_index;
    get_bits_context gb;
    int back_ref_offset_type;
    int back_ref_offset;
    int back_ref_length;
    int back_ref_start;
    int back_ref_end;

    for (i = 0; i < rbt->frame_count; i++)
    {
        /* read the entire frame (includes audio and video) */
        if (fread(rbt->frame_load_buffer, rbt->frame_sizes[i], 1, inrbt_file) != 1)
        {
            printf("problem reading frame %d\n", i);
            return 0;
        }

        scale = rbt->frame_load_buffer[3];
        width = LE_16(&rbt->frame_load_buffer[4]);
        height = LE_16(&rbt->frame_load_buffer[6]);
        x = LE_16(&rbt->frame_load_buffer[12]);
        y = LE_16(&rbt->frame_load_buffer[14]);
        compressed_size = LE_16(&rbt->frame_load_buffer[16]);
        fragment_count = LE_16(&rbt->frame_load_buffer[18]);
        decoded_size = width * height;
printf("frame %d: %d, %dx%d, (%d, %d), %d, %d\n", i, scale, width, height, x, y, compressed_size, fragment_count);

        /* decode the frame */
        decoded_frame = malloc(decoded_size);
        index = 24;
        out_index = 0;
        for (fragment = 0; fragment < fragment_count; fragment++)
        {
            fragment_compressed_size = LE_32(&rbt->frame_load_buffer[index]);
            index += 4;
            fragment_decompressed_size = LE_32(&rbt->frame_load_buffer[index]);
            index += 4;
            compression_type = LE_16(&rbt->frame_load_buffer[index]);
            index += 2;
printf(" fragment %d: %d, %d, %d\n", fragment, fragment_compressed_size, fragment_decompressed_size, compression_type);

            if (compression_type == 0)
            {
                init_get_bits(&gb, &rbt->frame_load_buffer[index],
                    fragment_compressed_size);

                while (out_index < fragment_decompressed_size)
                {
                    if (read_bits(&gb, 1))
                    {
                        /* decode back reference offset type */
                        back_ref_offset_type = read_bits(&gb, 1);

                        /* back reference offset is 7 or 11 bits */
                        back_ref_offset = read_bits(&gb,
                            (back_ref_offset_type) ? 7 : 11);

                        /* get the length of the back reference */
                        back_ref_length = get_lzs_back_ref_length(&gb);
                        back_ref_start = out_index - back_ref_offset;
                        back_ref_end = back_ref_start + back_ref_length;

                        /* copy the back reference, byte by byte */
//printf("  copy %d pixels from offset -%d; out_index = %d\n", back_ref_length, back_ref_offset, out_index + back_ref_length);
                        for (j = back_ref_start; j < back_ref_end; j++)
                            decoded_frame[out_index++] = decoded_frame[j];
                    }
                    else
                    {
uint8_t b = read_bits(&gb, 8);
//printf("  single byte: %02X, out_index = %d\n", b, out_index + 1);
                        /* read raw pixel byte */
//                        decoded_frame[out_index++] = read_bits(&gb, 8) & 0xFF;
                        decoded_frame[out_index++] = b;
                    }
                }

                if (out_index > fragment_decompressed_size)
                    printf("Help! frame decode overflow\n");

                delete_get_bits(&gb);
            }
        }

if (0)
{
  FILE *outfile;
  char filename[20];
  uint8_t bytes[3];
  int p;
  uint8_t pixel;

  sprintf(filename, "frame-%03d.pnm", i);
  outfile = fopen(filename, "wb");
  fprintf(outfile, "P6\n%d %d\n255\n", width, height);
  for (p = 0; p < fragment_decompressed_size; p++)
  {
    pixel = decoded_frame[p];
    bytes[0] = rbt->palette[pixel*3+0];
    bytes[1] = rbt->palette[pixel*3+1];
    bytes[2] = rbt->palette[pixel*3+2];
    fwrite(bytes, 3, 1, outfile);
  }
  fclose(outfile);
}

        free(decoded_frame);

        /* write the entire frame (for now) */
        if (fwrite(rbt->frame_load_buffer, rbt->frame_sizes[i], 1, outrbt_file) != 1)
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

    /* testing the bit functions */
#if 0
    int i;
    uint8_t bytestream[] = { 0x55, 0xAA, 0x00, 0xAA, 0x55, 0x77, 0xFF, 0x00 };
    get_bits_context gb;
    init_get_bits(&gb, bytestream, 8);
    for (i = 1; i < 8; i++)
    {
        printf("view %d bits: %d\n", i, view_bits(&gb, i));
        printf("read %d bits: %d\n", i, read_bits(&gb, i));
    }
    delete_get_bits(&gb);
#endif

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

    /* clean up */
    free(rbt.frame_load_buffer);
    free(rbt.frame_sizes);

    return 0;
}
