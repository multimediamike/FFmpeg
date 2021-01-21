#!/usr/bin/python

import os
import struct
import sys

HEADER_SIZE = 0x330
PALETTE_SIZE = 768
BLOCK_RECORD_SIZE = 6
FRAME_RECORD_SIZE = 16

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("USAGE: remux-vmd.py <original.vmd> <intermediate.vmd> <final.vmd>")
        sys.exit(1)

    original_vmd = sys.argv[1]
    intermediate_vmd = sys.argv[2]
    final_vmd = sys.argv[3]

    # validate the input files
    if not os.path.exists(original_vmd):
        print("can't find " + original_vmd)
        sys.exit(1)
    if not os.path.exists(intermediate_vmd):
        print("can't find " + intermediate_vmd)
        sys.exit(1)

    # open the files, read header, and copy it
    orig_f = open(original_vmd, "rb")
    inter_f = open(intermediate_vmd, "rb")
    final_f = open(final_vmd, "wb")
    header = orig_f.read(HEADER_SIZE)
    final_f.write(header)

    # get the table of contents
    block_count = struct.unpack("<H", header[6:8])[0]
    frames_per_block = struct.unpack("<H", header[18:20])[0]
    toc_offset = struct.unpack("<I", header[812:816])[0]
    orig_f.seek(toc_offset, 0)
    block_table_bytes = orig_f.read(BLOCK_RECORD_SIZE * block_count)
    frame_table_bytes = orig_f.read(FRAME_RECORD_SIZE * block_count * frames_per_block)
    orig_f.seek(HEADER_SIZE, 0)

    # parse the block table
    block_table = []
    for i in xrange(block_count):
        index = i * BLOCK_RECORD_SIZE
        block = {}
        block['b0_1'] = block_table_bytes[index:index+2]
        block['offset'] = struct.unpack("<I", block_table_bytes[index+2:index+6])[0]
        block_table.append(block)

    # parse the frame table
    frame_table = []
    for i in xrange(block_count * frames_per_block):
        index = i * FRAME_RECORD_SIZE
        frame = {}
        frame['type'] = struct.unpack("B", frame_table_bytes[index])[0]
        frame['b1'] = frame_table_bytes[index+1]
        frame['length'] = struct.unpack("<I", frame_table_bytes[index+2:index+6])[0]
        frame['leftedge'] = struct.unpack("<H", frame_table_bytes[index+6:index+8])[0]
        frame['topedge'] = struct.unpack("<H", frame_table_bytes[index+8:index+10])[0]
        frame['rightedge'] = struct.unpack("<H", frame_table_bytes[index+10:index+12])[0]
        frame['bottomedge'] = struct.unpack("<H", frame_table_bytes[index+12:index+14])[0]
        frame['b14'] = frame_table_bytes[index+14]
        frame['palette'] = struct.unpack("B", frame_table_bytes[index+15])[0]
        frame_table.append(frame)

    # get the frame count and the palette from the intermediate file
    inter_f.seek(0x18, 0)
    data = inter_f.read(4 + PALETTE_SIZE)
    intermediate_frame_count = struct.unpack("<I", data[0:4])[0]

    # rewind the final file and write the new palette
    final_f.seek(28, 0)
    final_f.write(data[4:])
    final_f.seek(HEADER_SIZE, 0)

    # iterate through the blocks
    f = 0
    palette_offset = 28  # initial palette
    palette = None
    for b in xrange(len(block_table)):
        # update the block's offset
        block_table[b]['offset'] = final_f.tell()
        # iterate through the frames in the blocks
        for x in xrange(frames_per_block):
            if frame_table[f]['type'] == 2:
                # skip the video frame from the original file
                orig_f.seek(frame_table[f]['length'], 1)

                # if this frame has a new palette, rewind to the last known
                # palette offset and dump the most recent palette before
                # loading the palette from this frame
                palette_size = 0
                if frame_table[f]['palette']:
                    current_offset = final_f.tell()
                    final_f.seek(palette_offset, 0)
                    final_f.write(palette)
                    final_f.seek(current_offset, 0)
                    # write new placeholder palette
                    final_f.write(struct.pack(">H", 0x00FF))
                    palette_offset = final_f.tell()
                    final_f.write(palette)
                    palette_size = PALETTE_SIZE + 2

                # read this frame's palette
                palette = inter_f.read(PALETTE_SIZE)

                # read the frame parameters
                frame_table[f]['leftedge'] = struct.unpack("<H", inter_f.read(2))[0]
                frame_table[f]['topedge'] = struct.unpack("<H", inter_f.read(2))[0]
                frame_table[f]['rightedge'] = struct.unpack("<H", inter_f.read(2))[0]
                frame_table[f]['bottomedge'] = struct.unpack("<H", inter_f.read(2))[0]
                frame_size = struct.unpack("<I", inter_f.read(4))[0]

                # copy the new video frame from the intermediate file
                frame_data = inter_f.read(frame_size)
                final_f.write(frame_data)

                # update the frame size
                frame_table[f]['length'] = frame_size + palette_size
            else:
                # copy the data from the original file
                frame_data = orig_f.read(frame_table[f]['length'])
                final_f.write(frame_data)
            f += 1

    # record the final palette
    current_offset = final_f.tell()
    final_f.seek(palette_offset, 0)
    final_f.write(palette)
    final_f.seek(current_offset, 0)

    # record the ToC offset
    toc_offset = final_f.tell()
    final_f.seek(812, 0)
    final_f.write(struct.pack("<I", toc_offset))
    final_f.seek(toc_offset, 0)

    # write the modified block table
    for block in block_table:
        final_f.write(block['b0_1'])
        final_f.write(struct.pack("<I", block['offset']))

    # write the modified frame table
    for frame in frame_table:
        final_f.write(struct.pack("B", frame['type']))
        final_f.write(frame['b1'])
        final_f.write(struct.pack("<I", frame['length']))
        final_f.write(struct.pack("<H", frame['leftedge']))
        final_f.write(struct.pack("<H", frame['topedge']))
        final_f.write(struct.pack("<H", frame['rightedge']))
        final_f.write(struct.pack("<H", frame['bottomedge']))
        final_f.write(frame['b14'])
        final_f.write(struct.pack("B", frame['palette']))

    final_f.close()
