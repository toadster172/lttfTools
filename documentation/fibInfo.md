# FIB (FUSE1.00) Archive Format Documentation 
### Introduction
The following document contains documentation of the FIB archive format used to store assets on many LEGO games for the DS, PSP, 3DS, and PS Vita, as well as a few non-LEGO games made by TT Fusion. Fibfiles are the successor to the .hog archive format used by Warthog Games before many of their developers left to form TT Fusion. 

There are 5 variants of the FIB format, which differ slightly from each other. Unfortunately, it's not possible to definitively identify a fibfile variant from the file alone, so I have instead listed out the chronological release spans of each variant, as well as what changed in brief.

Fib version 1: LSW:TCS - GBWRTVG

Fib version 2 (Slightly different refpack format): LIJ2 (DS, PSP) - LHP2 (DS ports only)

Fib version 2.5 (Added in deflate compression option): LHP1 - LHP2 (Non DS ports)

Fib version 3 (Changed length/compression bitfield): LB2 - LEGO Friends (DS ports only)

Fib version 3.5 (Added in deflate compression option): LB2 - LSW:TFA (Non DS ports)

DS ports never got deflate support, which is present in all other systems from LHP1 onward. Exact version differences will be noted when relevant. This document has been compiled from reverse engineering and checking a range of DS, PSP, 3DS, and Vita ports, as well as a handful of Android ports and aims to be both correct and complete, so it will document all of the FIB quirks and implementation details across each version, alongside the more core elements. However, when implementation quirks are noted, they will be mentioned as such.

### Header Info
| Offset | Data Type | Note / Significance          |
| ------ | --------- | ---------------------------- |
| `0x00` | `char[8]` | Magic number: `FUSE1.00`     |
| `0x08` | `uint32_t`| Hashed filetable entries     |
| `0x0C` | `uint32_t`| Named filetable entries      |
| `0x10` | `uint32_t`| Filetable offset             |

The header is the first 0x14 bytes of the fibfile. All fields in fibfiles outside of some compression fields noted later are stored in native byte order (though the only game on a big endian system that uses fibfiles is the Wii port of Guinness Book of World Records: The Videogame). All offsets are relative to the start of the file. Only the first 4 bytes of the magic number are actually checked in-game to verify that a file is actually a fibfile. There do not seem to be any fibfiles shipped in retail games that have any named filetable entries. Regardless, they are fully supported within the game executables, so they will be documented here for completeness.

### Filetable Info
The filetable consists of hashed filetable entries, followed by named filetable entries, followed by paths.

#### Hashed Filetable
| Offset | Data Type | Note / Significance |
| ------ | ---------- | ------------------ |
| `0x00` | `uint32_t` | Filename Hash      |
| `0x04` | `uint32_t` | File offset        |
| `0x08` | `uint32_t` | Flag \| Filesize   |

Each filetable entry has a length of 0x0C bytes, and there is one entry for each file in the archive.

 The original filenames of hashed filetable entries are not stored anywhere within the archive. However, some games ship with CSV files alongside their fibfiles that specify original filenames. Unfortunately, for games that do not have a CSV, this makes it difficult to find the original name. Already known filenames may still be matched to their hashes to recover the names of used files, even in this case.

The method used of hashing a filename is a CRC32 calculated over the path string, with all uppercase lettering first converted to lowercase. The specific CRC32 format used is simply a bitwise not applied to a standard CRC32. For example, the standard CRC32 of `localisation/gametext_us.loc` is `0xBA65F54A`. The fibfile hash for this filename is the bitwise not of this, `0x459A0AB5`. Filetable entries are stored in order of ascending hash, so that they may be binary searched for file lookups. 

The formatting of the field at offset 0x08 depends on version. For versions 1, 2, and 2.5, the lower 30 bits contain the final (uncompressed) filesize for the file. The upper two bits are a flag indicating compression. 0 is supposed to indicate no compression, 1 is supposed to indicate refpack compression, and 3 (version 2.5 only)is supposed to indicate deflate compression, and all shipped fibfiles follow this scheme. However, the only thing the flag actually does in code is select for uncompressed (0) or compressed (nonzero) data interpretation, with compression type actually being decided per-chunk (more on that later). In versions 3 and 3.5, the lower 2 bits indicate the compression type, the next 3 bits select the chunk size (more on that later), and the upper 27 bits are the size. All subfields are accessed by shifting, so a subfield's placement within the integer doesn't affect its actual value. Version 3 and 3.5 fibs use their compression flags with the same values as 1 / 2 / 2.5, but they decide compression on a file, rather than a chunk basis, so this value actually does matter beyond 0 / nonzero.

For uncompressed files, raw filedata simply starts at the specified offset and continues for the length of the file.

#### Named Filetable
| Offset | Data Type | Note / Significance    |
| ------ | ---------- | --------------------- |
| `0x00` | `uint32_t` | Path string size      |
| `0x04` | `uint32_t` | File offset           |
| `0x08` | `uint32_t` | Flag \| Filesize      |

Named filetable entries immediately follow the hashed filetable entries, and follow the same general format, with the hash field being switched out for a string size field, which includes the space for a null terminator. Immediately following the end of the named filetable entries is the paths themselves, ordered sequentially and separated by a single null terminator.

Unlike with the hashed filetable entries, there does not seem to be any guaranteed ordering for paths, as a linear rather than a binary search is used for them on file lookup. Named filetable entries are searched before hashed entries on file lookup if they exist, so significant amounts of named filetable entries will slow down all file lookups.

### Compression
Fibfiles use [refpack compression](http://wiki.niotso.org/RefPack) (all versions) and [deflate compression](https://www.w3.org/Graphics/PNG/RFC-1951) (2.5 / 3.5). Compressed data comes in chunks with a maximum length of 32 KiB (1 / 2 / 2.5) or [32 KiB << chunk size field] (3 / 3.5). The first uint32_t of the chunk stores the length of compressed data in the following chunk, using the same format as offset 0x08 of a filetable entry. In version 1 / 2 / 2.5 fibfiles, this is where the compression type for the chunk is stored in practice. In version 3 / 3.5 fibfiles, this uint32_t is only used for length, with filetable offset 0x08 always being used to determine max chunk size and compression type. For invalid compression type values, the chunk is simply interpreted as entirely literal in all fibfile versions. Of course, in shipped fibfiles, there are almost certainly no cases in reality of mismatch between chunk compression type and the compression listed in the filetable entry, or of an invalid compression type value being used. As the refpack compression used is version dependent and sometimes nonstandard, it is explained below. The deflate compression is standard, so the above link should be used for documentation on that.

#### Refpack

Refpack compression has slight variation between version 1 and later versions, with version 1 matching standard refpack compression, as well as the form that hog archives used (at least those with magic `WART3.00`). Version 2 and later uses nonstandard placement for a few bits. Both forms are listed below.

There are 5 possible command formats, determined by the position of the first 0 bit in the first byte. They work as follows:

`0PPC CCLL PPPP PPPP` (FIB version 1)

`0CCC LLPP PPPP PPPP` (FIB version 2 and later)

Copy literal data of length LL. Then copy previous data of length CCC + 3.

`10CC CCCC LLPP PPPP PPPP PPPP`

Copy literal data of length LL. Then copy previous data of length CCCCCC + 4.

`110P CCLL PPPP PPPP PPPP PPPP CCCC CCCC` (FIB version 1)

`110L LCCP PPPP PPPP PPPP PPPP CCCC CCCC` (FIB version 2 and later)

Copy literal data of length LL. Then copy previous data of length CCCCCCCCCC + 5.

Where a field is stored in multiple bytes, it is stored in **big endian byte order**, regardless of native byte order. For dictionary lookups, previously decompressed data is copied from `[current decompression head - (P) - 1]` to the current head. If P is small enough, it is possible for this operation to copy data that was already written earlier in the same operation. This is expected behaviour.

`1111 11LL` (>= 0xFC): Copy literal data of length LL. This immediately followed by the end of the current chunk.

`111L LLLL` (\< 0xFC): Copy literal data of length ((LLLLL << 2) + 4).

The dictionary resets on chunk boundaries.
