# FIB (FUSE1.00) Archive Format Documentation 
### Introduction
The following document contains documentation of the FIB archive format used to store assets on many LEGO games for the DS, PSP, 3DS, and PS Vita. Information was taken from the DS version of LEGO Harry Potter: Years 1-4 and tested against FIB files from that game and the DS version of LEGO Indiana Jones 2. Debug strings refer to the archive files as "fibfiles" and I will use that term going forth, since it isn't clear what FIB (or FUSE) are supposed to stand for.
### Header Info
| Offset | Data Type | Note / Significance |
| ------ | --------- | ------------------- |
| `0x00` | `char[8]` | Magic number: `FUSE1.00` |
| `0x08` | `uint32_t`| Number of files     |
| `0x0C` | `uint32_t`| ??? - Zeroed in practice |
| `0x10` | `uint32_t`| Filetable offset    |

The header is the first 0x14 bytes of the fibfile. All fields in fibfiles outside of some compression fields are stored in little endian byte order. All offsets are relative to the start of the file. The uint32_t at offset 0x0C seems to be filled with zeroes in all fibfiles in retail builds, but does have some significance when non-zero. Because it isn't important in practice, its exact behaviour isn't currently documented, but I will try to figure it out for a future version of this file.
### Filetable Info
| Offset | Data Type | Note / Significance |
| ------ | ---------- | ------------------ |
| `0x00` | `uint32_t` | Filename Hash      |
| `0x04` | `uint32_t` | File offset        |
| `0x08` | `uint32_t` | Flag \| Filesize   |

Each filetable entry has a length of 0x0C bytes, and there is one entry for each file in the archive.

Files are identified solely by a hash of their filename. The original filenames are not stored anywhere within the archive. Some games ship with CSV files alongside their fibfiles that specify original filenames. Unfortunately, for games that do not have a CSV, this makes it difficult to find the original name. Already known filenames may still be matched to their hashes to recover the names of used files, even in this case.

It seems that the specific method of hashing the filename changes based off game and/or platform. For the DS version of LHP1, hashes are a CRC32 calculated over the path string, with all uppercase lettering first converted to lowercase. The specific CRC32 format used is simply a bitwise not applied to a standard CRC32. For example, the standard CRC32 of `localisation/gametext_us.loc` is `0xBA65F54A`. The fibfile hash for this filename is the bitwise not of this, `0x459A0AB5`. Filetable entries are stored in order of ascending hash, so that it may be binary searched for file lookups. 

For the flag/filesize field at offset 0x08, the lower 30 bits contain the final (uncompressed) filesize for the file. The upper two bits are a flag indicating compression, where 00 indicates no compression, and 01 indicates compression. Both bits are checked, even though there is no behaviour for if the top bit isn't zero.

For uncompressed files, raw filedata simply starts at the specified offset and continues for the length of the file.
### Compression
Fibfiles use a dictionary-based compression similar to LZSS and its variants. Compressed data comes in chunks with a maximum length of 32 KiB. The first uint32_t stores the length of compressed data in the following chunk, using the same format as offset 0x08 of a filetable entry. It is in theory possible for the top two bits of this uint32_t to be 00, indicating that the chunk is entirely literal, but in practice this didn't happen in any of the fibfiles I tested on.

There are 5 possible command formats, determined by the position of the first 0 bit in the first byte. They work as follows:

`0CCC LLPP PPPP PPPP`: Copy literal data of length LL. Then copy previous data of length CCC + 3.

`10CC CCCC LLPP PPPP PPPP PPPP`: Copy literal data of length LL. Then copy previous data of length CCCCCC + 4.

`110L LCCP PPPP PPPP PPPP PPPP CCCC CCCC`: Copy literal data of length LL. Then copy previous data of length CCCCCCCCCC + 5.

Where a field is stored in multiple bytes, it is stored in **big endian byte order**. For dictionary lookups, previously decompressed data is copied from `[current decompression head - (P) - 1]` to the current head. If P is small enough, it is possible for this operation to copy data that was already written earlier in the same operation. This is expected behaviour.

`1111 11LL` (>= 0xFC): Copy literal data of length LL. This immediately followed by the end of the current chunk.

`111L LLLL` (\< 0xFC): Copy literal data of length ((LLLLL << 2) + 4).

The dictionary resets on chunk boundaries.

