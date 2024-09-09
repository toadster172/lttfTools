# BTGA (DS) Documentation
### Introduction
BTGA seems to be the standard file extension for files storing images and textures in TT Fusion's games. However, despite the shared file extension, the actual format of the BTGA varies greatly from system to system and even from game to game. DS BTGAs are strongly coupled with the DS hardware. [Scarlet has two different headers used in the 3DS games](https://github.com/xdanieldzd/Scarlet/blob/master/Scarlet.IO.ImageFormats/BTGA.cs), both of them having a system-specific field. A quick glance at function names in the Android version of LHP1 reveals that its BTGAs have yet another completely different format (but also that it seems to support standard TGAs as well). I haven't looked at PSP or Vita BTGAs (yet), and there doesn't seem to be any documentation for them, but I'd imagine that they are similarly completely incompatible. One common theme is that, confusingly, none of these formats seem to have anything in common with standard TGAs outside of both being bitmap formats. The information here was reverse engineered from the DS version of LHP1 and tested against LIJ2. 

All multi-byte fields in BTGAs are little endian. For the purposes of this documentation, I won't go into detail on how the DS processes its hardware-supported texture formats—for documentation there see [Nocash's DS documentation](https://www.problemkaputt.de/gbatek.htm#dsiomaps).
 
### [Segment/Block Format](ttFusionBinaryContainerInfo.md)

1. Header Segment (1 block)
    1. Header block (0x1C bytes)
2. Bitmap Segment (1 block)
    1. Bitmap block (variable length)
3. Palette Segment (optional) (1 block)
    1. Palette block (variable length)
4. Compressed Palette Index Segment (optional) (1 block)
    1. Compressed palette index block (variable length)

On the DS, BTGAs consist of a 28 (0x1C) byte header block followed by 1 or more blocks of data that can (almost always) be loaded straight into VRAM and be interpreted as a valid texture. All blocks are stored in their own segment. Which optional blocks are present is determined by the texture format byte stored in the header.

### BTGA Header
| Offset                | Data Type            | Note / Significance             |
| --------------------- | -------------------- | ------------------------------- |
| `0x00`                | `uint32_t`           | Clobbered without being saved   |
| `0x04`                | `uint32_t`           | Bitmap length                   |
| `0x08`                | `uint32_t`           | Clobbered without being saved   |
| `0x0C`                | `uint32_t`           | Palette length                  |
| `0x10`                | `uint32_t`           | Clobbered without being saved   |
| `0x14`                | `uint32_t`           | Compressed palette index length |
| `0x18`                | `uint8_t`            | Texture format                  |
| `0x19`                | `uint8_t`            | Color 0 transparency flag       |
| `0x1A`                | `uint8_t`            | Horizontal resolution           |
| `0x1B`                | `uint8_t`            | Vertical resolution             |

The values listed as clobbered generally have nonzero values when the following length field is nonzero, but in program execution, they are overwritten without ever being stored. 

Offset 0x18 corresponds with one of the 7 following formats:

1. A3I5 Texture
2. 2 bpp Paletted Texture
3. 4 bpp Paletted Texture
4. 8 bpp Paletted Texture
5. Compressed Texture
6. A5I3 Texture
7. Direct Colored Texture

All other values are invalid. Offset 0x19, when nonzero, sets palette index 0 to be transparent in texture formats 2, 3, and 4. For other formats, it has no effect. The horizontal and vertical resolutions of the image may be obtained by bit shifting 8 left by offset 0x1A and 0x1B, respectively. These values are used to generate the `TEXIMAGE_PARAM` command for the texture.

Palette data is not present for (and only for) texture format 7 (direct colored textures). The compressed palette index is present for (and only for) texture format 5 (compressed textures).

With the image parameters taken from the header, the remaining blocks are trivial to interpret, since they are simply raw texture data to be loaded into DS VRAM.

8 (and possibly 4) bpp paletted images used for backgrounds or sprites on the 2D UI screen are sometimes stored as a tileset, rather than final data. There is no indicator of this in the BTGA, but images where this is the case will appear scrambled in 8x8 pixel groups when viewed. The tilemap to reconstruct the final image can be found in NSC files (documentation coming soon). When working with filenames, there is a convention that the NSC's path is simply the BTGA's path with the file extension replaced with ".nsc". When working without filenames, NSC files may be identified by their consistent magic (`NCSC` at offset 0) and the correct file may be found through brute force trying tilemaps until the correct one is found, since there aren't that many NSC files (just 13 in LHP1). Tilesets are the one exception to data being loaded straight into VRAM, since tiles in the BTGA are arranged in 2D space (8x8) in the image, but must be converted to linear space to be used as tiles.

