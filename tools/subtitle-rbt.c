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

/* Bit reader stuff */

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

/* Bit writer stuff */

#define MAX_PUT_BITS_BYTES 63000
typedef struct
{
    uint8_t bytes[MAX_PUT_BITS_BYTES];
    int byte_index;
    uint32_t bit_buffer;
    int bits_buffered;
} put_bits_context;

static void reset_put_bits(put_bits_context *pb)
{
    memset(pb->bytes, 0, MAX_PUT_BITS_BYTES);
    pb->byte_index = 0;
    pb->bit_buffer = 0;
    pb->bits_buffered = 0;
}

static put_bits_context *init_put_bits()
{
    put_bits_context *pb;

    pb = malloc(sizeof(put_bits_context));
    reset_put_bits(pb);

    return pb;
}

static void put_bits(put_bits_context *pb, int bits, int count)
{
    pb->bit_buffer <<= count;
    pb->bit_buffer |= (bits & (~(0xFFFFFFFF << count)));
    pb->bits_buffered += count;

    while (pb->bits_buffered >= 8)
    {
        pb->bytes[pb->byte_index++] = pb->bit_buffer >> (pb->bits_buffered - 8);
        pb->bit_buffer &= (~(0xFFFFFFFF << (pb->bits_buffered - 8)));
        pb->bits_buffered -= 8;
    }

    if (pb->byte_index >= MAX_PUT_BITS_BYTES)
    {
        printf("HELP! Bit overflow\n");
        exit(1);
    }
}

static void put_bits_flush(put_bits_context *pb)
{
    if (pb->bits_buffered > 0)
        pb->bytes[pb->byte_index++] = pb->bit_buffer << (8 - pb->bits_buffered);
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
    off_t video_frame_size_table_offset;
    uint8_t *video_frame_size_table;
    off_t frame_size_table_offset;
    uint8_t *frame_size_table;
    uint8_t *frame_load_buffer;
} rbt_dec_context;

static int load_and_copy_rbt_header(rbt_dec_context *rbt, FILE *inrbt_file, FILE *outrbt_file)
{
    uint8_t header[RBT_HEADER_SIZE];
    int palette_data_size;
    uint8_t *palette_chunk;
    int unknown_chunk_size;
    uint8_t *unknown_chunk;
    uint8_t unknown_table[UNKNOWN_TABLE_SIZE];
    off_t padding_size;
    uint8_t *padding;
    int i;
    int frame_size;
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

    /* copy the video frame size table (2 bytes per frame), as a placeholder */
    rbt->video_frame_size_table = malloc(rbt->frame_count * sizeof(uint16_t));
    if (fread(rbt->video_frame_size_table, rbt->frame_count * sizeof(uint16_t), 1, inrbt_file) != 1)
    {
        printf("problem reading frame table\n");
        return 0;
    }
    rbt->video_frame_size_table_offset = ftell(outrbt_file);
    if (fwrite(rbt->video_frame_size_table, rbt->frame_count * sizeof(uint16_t), 1, outrbt_file) != 1)
    {
        printf("problem writing frame table\n");
        return 0;
    }

    /* copy the frame size table (2 bytes per frame), as a placeholder */
    rbt->frame_size_table = malloc(rbt->frame_count * sizeof(uint16_t));
    if (fread(rbt->frame_size_table, rbt->frame_count * sizeof(uint16_t), 1, inrbt_file) != 1)
    {
        printf("problem reading frame table\n");
        return 0;
    }
    rbt->frame_size_table_offset = ftell(outrbt_file);
    if (fwrite(rbt->frame_size_table, rbt->frame_count * sizeof(uint16_t), 1, outrbt_file) != 1)
    {
        printf("problem writing frame table\n");
        return 0;
    }

    /* find the max frame size */
    max_frame_size = 0;
    for (i = 0; i < rbt->frame_count; i++)
    {
        frame_size = LE_16(&rbt->frame_size_table[i*2]);
        if (frame_size > max_frame_size)
            max_frame_size = frame_size;
    }
    rbt->frame_load_buffer = malloc(max_frame_size);

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

static void compress_window(put_bits_context *pb, uint8_t *full_window,
    int full_window_stride,
    int window_top, int window_bottom, int window_left, int window_right)
{
    int last_pixel;
    int run_size;
    int x;
    int y;
    int start_index;
    int end_index;
    int encode_last_run;

    last_pixel = full_window[0];
    run_size = 1;
    for (y = window_top; y <= window_bottom; y++)
    {
        start_index = y * full_window_stride + window_left;
        if (y == window_top)
            start_index += 1;
        end_index = y * full_window_stride + window_right;
        if (y == window_bottom)
            encode_last_run = 1;
        else
            encode_last_run = 0;
        for (x = start_index; x < end_index; x++)
        {
if (y - window_top == 51)
  printf(".... (%d: %d): %02X\n", x % full_window_stride - window_left, y - window_top, full_window[x]);
            if (!encode_last_run && full_window[x] == last_pixel)
                run_size++;
            else
            {
printf("@(%d, %d):", x % full_window_stride - window_left, y - window_top);
                if (run_size == 1)
                {
printf("  encoding single pixel: 0x%02X\n", last_pixel);
                    /* encode a 0 bit followed by raw pixel byte */
                    put_bits(pb, 0, 1);
                    put_bits(pb, last_pixel, 8);
                }
                else if (run_size == 2)
                {
printf("  encoding a double run of pixel: 0x%02X\n", last_pixel);
                    /* encode a 0 bit followed by raw pixel byte */
                    put_bits(pb, 0, 1);
                    put_bits(pb, last_pixel, 8);
                    put_bits(pb, 0, 1);
                    put_bits(pb, last_pixel, 8);
                }
                else
                {
printf("  encoding %d-length run of pixel: 0x%02X\n", run_size, last_pixel);
                    /* encode a 0 bit followed by raw pixel byte */
                    put_bits(pb, 0, 1);
                    put_bits(pb, last_pixel, 8);
                    run_size--;
                    /* encode a run: a 1 bit, followed by a back reference
                     * offset (-1), followed by a length */
                    put_bits(pb, 1, 1);
                    put_bits(pb, 1, 1);  /* 1 = 7-bit offset */
                    put_bits(pb, 1, 7);
                    if (run_size <= 4)
                    {
                        /* lengths 2, 3, and 4 are 2 bits */
                        put_bits(pb, run_size - 2, 2);
                    }
                    else if (run_size <= 7)
                    {
                        /* lengths 5, 6, and 7 are 4 bits */
                        put_bits(pb, run_size + 7, 4);
                    }
                    else
                    {
                        /* arbitrary length; start by encoding 0xF which
                         * stands in for an initial version of 8 */
                        put_bits(pb, 0xF, 4);
                        run_size -= 8;

                        /* encode blocks of 4 bits until run_size is 0 */
                        while (run_size > 0)
                        {
                            if (run_size >= 15)
                            {
                                put_bits(pb, 0xF, 4);
                                run_size -= 0xF;
                            }
                            else
                            {
                                put_bits(pb, run_size, 4);
                                run_size = 0;
                            }
                        }
                    }
                }

                last_pixel = full_window[x];
                run_size = 1;
            }
        }
    }
    put_bits_flush(pb);
}

static int copy_frames(rbt_dec_context *rbt, FILE *inrbt_file, FILE *outrbt_file,
    int origin_x, int origin_y, int window_width, int window_height)
{
    int i;
    int j;
    int scale;
    int width;
    int height;
    int frame_x;
    int frame_y;
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
    int frame_size;
    int video_frame_size;
    int audio_frame_size;

    int back_ref_offset_type;
    int back_ref_offset;
    int back_ref_length;
    int back_ref_start;
    int back_ref_end;

    uint8_t *full_window;
    int full_window_size;
    int y;
    int window_top;
    int window_bottom;
    int window_left;
    int window_right;
    int window_size;

    put_bits_context *pb;

    full_window_size = window_width * window_height;
    full_window = malloc(full_window_size);
    pb = init_put_bits();

    for (i = 0; i < rbt->frame_count; i++)
    {
        /* read the entire frame (includes audio and video) */
        frame_size = LE_16(&rbt->frame_size_table[i*2]);
        video_frame_size = LE_16(&rbt->video_frame_size_table[i*2]);
        audio_frame_size = frame_size - video_frame_size;
printf("frame_size = %d, video = %d, audio = %d\n", frame_size, video_frame_size, audio_frame_size);
        if (fread(rbt->frame_load_buffer, frame_size, 1, inrbt_file) != 1)
        {
            printf("problem reading frame %d\n", i);
            return 0;
        }

        scale = rbt->frame_load_buffer[3];
        width = LE_16(&rbt->frame_load_buffer[4]);
        height = LE_16(&rbt->frame_load_buffer[6]);
        frame_x = LE_16(&rbt->frame_load_buffer[12]);
        frame_y = LE_16(&rbt->frame_load_buffer[14]);
        compressed_size = LE_16(&rbt->frame_load_buffer[16]);
        fragment_count = LE_16(&rbt->frame_load_buffer[18]);
        decoded_size = width * height;
printf("frame %d: %d, %dx%d, (%d, %d), %d, %d\n", i, scale, width, height, frame_x, frame_y, compressed_size, fragment_count);

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

        /* transfer the image onto the frame window */
        memset(full_window, 0xFF, full_window_size);
        index = 0;
        for (y = 0; y < height; y++)
        {
            out_index = window_width * (frame_y + y) + frame_x;
            memcpy(&full_window[out_index], &decoded_frame[index], width);
            index += width;
        }

        /* write the subtitle */

        /* figure out the smallest change window */
        window_top = frame_y;
        window_bottom = frame_y + height;
        window_left = frame_x;
        window_right = frame_x + width;
        window_size = (window_right - window_left) * (window_bottom - window_top);

        /* compress the frame */
        reset_put_bits(pb);
        compress_window(pb, full_window, window_width, window_top,
            window_bottom, window_left, window_right);
        printf(" compressed frame = %d bytes\n", pb->byte_index);

if (1)
{
  FILE *outfile;
  char filename[20];
  uint8_t bytes[3];
  int p;
  uint8_t pixel;

  sprintf(filename, "frame-%03d.pnm", i);
  outfile = fopen(filename, "wb");
#if 0
  fprintf(outfile, "P6\n%d %d\n255\n", window_width, window_height);
  for (p = 0; p < full_window_size; p++)
  {
    pixel = full_window[p];
    bytes[0] = rbt->palette[pixel*3+0];
    bytes[1] = rbt->palette[pixel*3+1];
    bytes[2] = rbt->palette[pixel*3+2];
    fwrite(bytes, 3, 1, outfile);
  }
#else
  fprintf(outfile, "P6\n%d %d\n255\n", width, height);
  for (p = 0; p < width * height; p++)
  {
    pixel = decoded_frame[p];
    bytes[0] = rbt->palette[pixel*3+0];
    bytes[1] = rbt->palette[pixel*3+1];
    bytes[2] = rbt->palette[pixel*3+2];
    fwrite(bytes, 3, 1, outfile);
  }
#endif
  fclose(outfile);
}

        free(decoded_frame);

#if 0
        /* write the entire frame (for now) */
        if (fwrite(rbt->frame_load_buffer, frame_size, 1, outrbt_file) != 1)
        {
            printf("problem writing frame %d\n", i);
            return 0;
        }
#else
        /* update the frame header */
        /* width */
        rbt->frame_load_buffer[4] = (window_right - window_left) & 0xFF;
        rbt->frame_load_buffer[5] = (window_right - window_left) >> 8;
        /* height */
        rbt->frame_load_buffer[6] = (window_bottom - window_top) & 0xFF;
        rbt->frame_load_buffer[7] = (window_bottom - window_top) >> 8;
        /* origin X */
        rbt->frame_load_buffer[12] = window_left & 0xFF;
        rbt->frame_load_buffer[13] = window_left >> 8;
        /* origin Y */
        rbt->frame_load_buffer[14] = window_top & 0xFF;
        rbt->frame_load_buffer[15] = window_top >> 8;
        /* fragment payload size */
        rbt->frame_load_buffer[16] = (pb->byte_index + 10) & 0xFF;
        rbt->frame_load_buffer[17] = (pb->byte_index + 10) >> 8;
        /* fragment count (1) */
        rbt->frame_load_buffer[18] = 1;
        rbt->frame_load_buffer[19] = 0;

        /* update the fragment header */
        /* compressed size */
        rbt->frame_load_buffer[24 + 0] = (pb->byte_index >>  0) & 0xFF;
        rbt->frame_load_buffer[24 + 1] = (pb->byte_index >>  8) & 0xFF;
        rbt->frame_load_buffer[24 + 2] = (pb->byte_index >> 16) & 0xFF;
        rbt->frame_load_buffer[24 + 3] = (pb->byte_index >> 24) & 0xFF;
        /* decompressed size */
        rbt->frame_load_buffer[24 + 4] = (window_size >>  0) & 0xFF;
        rbt->frame_load_buffer[24 + 5] = (window_size >>  8) & 0xFF;
        rbt->frame_load_buffer[24 + 6] = (window_size >> 16) & 0xFF;
        rbt->frame_load_buffer[24 + 7] = (window_size >> 24) & 0xFF;
        /* compression format 0 */
        rbt->frame_load_buffer[24 + 8] = 0;
        rbt->frame_load_buffer[24 + 9] = 0;

        /* write the 24-byte frame header and the 10-byte fragment header */
        if (fwrite(rbt->frame_load_buffer, 24 + 10, 1, outrbt_file) != 1)
        {
            printf("problem writing frame %d\n", i);
            return 0;
        }

        /* write the new compressed frame data */
        if (fwrite(pb->bytes, pb->byte_index, 1, outrbt_file) != 1)
        {
            printf("problem writing frame %d\n", i);
            return 0;
        }

        /* write the audio data */
        if (fwrite(&rbt->frame_load_buffer[video_frame_size], frame_size - video_frame_size, 1, outrbt_file) != 1)
        {
            printf("problem writing frame %d\n", i);
            return 0;
        }

        /* update the table entries */
        video_frame_size = pb->byte_index + 24 + 10;
        frame_size = video_frame_size + audio_frame_size;
        rbt->frame_size_table[i*2+0] = frame_size & 0xFF;
        rbt->frame_size_table[i*2+1] = frame_size >> 8;
        rbt->video_frame_size_table[i*2+0] = video_frame_size & 0xFF;
        rbt->video_frame_size_table[i*2+1] = video_frame_size >> 8;
#endif
    }

    free(full_window);

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
    int origin_x;
    int origin_y;
    int frame_width;
    int frame_height;

    /* testing the bit functions */
#if 0
    int i;
    uint8_t bytestream[] = { 0x55, 0xAA, 0x00, 0xAA, 0x55, 0x77, 0xFF, 0x1B, 0x70, 0x8F };
    int bytestream_size = 10;
    get_bits_context gb;
    put_bits_context *pb;
    int bits;

    init_get_bits(&gb, bytestream, bytestream_size);
    pb = init_put_bits();

    for (i = 1; i <= 12; i++)
    {
        bits = view_bits(&gb, i);
        printf("view %d bits: %d\n", i, bits);
        printf("read %d bits: %d\n", i, read_bits(&gb, i));
        put_bits(pb, bits, i);
    }
    put_bits_flush(pb);

    printf("original bytestream:\n");
    for (i = 0; i < bytestream_size; i++)
        printf(" %02X", bytestream[i]);
    printf("\nnewbytestream:\n");
    for (i = 0; i < pb->byte_index; i++)
        printf(" %02X", pb->bytes[i]);
    printf("\n");

    delete_get_bits(&gb);
    free(pb);
#endif

    /* validate the number of arguments */
    if (argc != 8)
    {
        printf("USAGE: subtitle-rbt <subtitles.ass> <in.rbt> <out.rbt> <origin X> <origin Y> <width> <height>\n");
        return 1;
    }
    subtitle_filename = argv[1];
    inrbt_filename = argv[2];
    outrbt_filename = argv[3];
    origin_x = atoi(argv[4]);
    origin_y = atoi(argv[5]);
    frame_width = atoi(argv[6]);
    frame_height = atoi(argv[7]);

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
    if (!copy_frames(&rbt, inrbt_file, outrbt_file, origin_x, origin_y,
        frame_width, frame_height))
        return 1;

    /* write the modified frame size tables back to the file */
    fseek(outrbt_file, rbt.video_frame_size_table_offset, SEEK_SET);
    fwrite(rbt.video_frame_size_table, rbt.frame_count * sizeof(uint16_t), 1, outrbt_file);
    fseek(outrbt_file, rbt.frame_size_table_offset, SEEK_SET);
    fwrite(rbt.frame_size_table, rbt.frame_count * sizeof(uint16_t), 1, outrbt_file);

    /* finished with files */
    fclose(inrbt_file);
    fclose(outrbt_file);

    /* clean up */
    free(rbt.frame_load_buffer);
    free(rbt.video_frame_size_table);
    free(rbt.frame_size_table);

    return 0;
}
