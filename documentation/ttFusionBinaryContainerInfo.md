# TT Fusion Binary Container Format Documentation
### Introduction
Many of the binary files used in TT Fusion games are encapsulated within an unnamed container format, which divides the file into segments, which are then further divided into data blocks to be processed. Files using this format tend to have their file extensions prefixed with 'b' (btga, bwav, bxls, bfnmdl, etc.). The documentation here was reverse engineered from the DS version of LEGO Harry Potter Years 1-4, but the format doesn't seem to change in other TT Fusion games. The format itself is rather simple.
### Segment Format
| Offset                | Data Type            | Note / Significance |
| --------------------- | -------------------- | ------------------- |
| `0x00`                | `uint16_t`           | Number of blocks in segment |
| `0x02`                | `uint8_t[2]`         | Padding                     |
| `0x04`                | `uint32_t`           | Length of segment data      |
| `0x08`                | `uint32_t[blocks]`   | Size in bytes of each block |
| `0x08 + (blocks * 4)` | `uint8_t[dataLength]`| Segment data                |

Files may be seen as an simple arrays of segments, with the first segment starting at offset 0 of the file. The segment data is then divided into 1 or more blocks. Blocks are simply read sequentially, starting at the beginning of the segment data. All multi-byte values are little endian.

The end of segment data for one segment is immediately followed by the next segment (or the end of the file). Data is always read only in blocks, with segments only being 'visible' to the parser. However, there is some significance of segments outside the parser, as segment data is read (and allocced) all at once. The blocks the parser returns are simply generated from offsets within this allocated buffer, so only the first block within a segment may be used with a free call, which will then also free all other blocks within the segment. The upside of this is that it means that specific file formats have to have consistent segment-block divisions, which makes it easier to recognize them when working without filenames. 
