# TT Fusion Binary Container Format Documentation
### Introduction
Many of the binary files used in TT Fusion games are encapsulated within an unnamed container format, which divides the file into segments, which are then further divided into data blocks to be processed. Files using this format tend to have their file extensions prefixed with 'b' (btga, bwav, bxls, bfnmdl, etc.). The format seems to predate TT Fusion, being used in some of Warthog Plc's later games. Within TT Fusion games, there are two variants of container format, one used in LSW:TCS and LIJ1 and matching the format used in Warhog's games, and the other used in all games after that. The only actual difference between these two formats is that the older format has a 12 byte header. The information from this file has been reverse engineered from TT Fusion's DS games, but should apply to all other platforms as well, as the container format seems to have been consistent.

### Header (TCS and LIJ1 Only)
| Offset                | Data Type            | Note / Significance |
| --------------------- | -------------------- | ------------------- |
| `0x00`                | `uint8_t[12]`        | Unknown, not read   |

Despite the header existing, its values are never read from. The function to start reading a file using the container format sets the file offset to 0x0C immediately, skipping the header entirely. Despite never being read, header values are still nonzero. It's possible that it had significance in Warthog's games and TT Fusion simply didn't get around to changing their tools until LB1.

### Segment Format
| Offset                | Data Type            | Note / Significance |
| --------------------- | -------------------- | ------------------- |
| `0x00`                | `uint16_t`           | Number of blocks in segment |
| `0x02`                | `uint8_t[2]`         | Padding                     |
| `0x04`                | `uint32_t`           | Length of segment data      |
| `0x08`                | `uint32_t[blocks]`   | Size in bytes of each block |
| `0x08 + (blocks * 4)` | `uint8_t[dataLength]`| Segment data                |

Files (after the header, if it exists) may be seen as simple arrays of segments, with the first segment starting at offset 0 (or 0x0C for TCS and LIJ1) of the file. The segment data is then divided into 1 or more blocks. Blocks are simply read sequentially, starting at the beginning of the segment data. All multi-byte values are native endian.

The end of segment data for one segment is immediately followed by the next segment (or the end of the file). Data is always read only in blocks, with segments only being 'visible' to the parser. However, there is some significance of segments outside the parser, as segment data is read (and allocced) all at once. The blocks the parser returns are simply generated from offsets within this allocated buffer, so only the first block within a segment may be used with a free call, which will then also free all other blocks within the segment. The upside of this is that it means that specific file formats have to have consistent segment-block divisions, at least within a specific game, which makes it easier to recognize them when working without filenames. 
