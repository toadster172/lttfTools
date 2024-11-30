/* Copyright (C) 2024 toadster172 <toadster172@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// This code currently makes assumptions about padding and endianness.
// As such, it is non-portable, though will probably work on all modern desktop systems.
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

// typedef struct _segmentHeader {
//     uint16_t totalBlocks;
//     uint8_t padding[2];
//     uint32_t segmentLength;
// 
//     uint32_t *blockSizes;
//     uint8_t *segmentData;
// } segmentHeader;

typedef struct _blockParser {
    bool rereadSizes;
    // Structural info
    uint32_t numSizeEntries;
    int32_t *blockSizes;
    uint32_t sizeIndex;
    // Exposed return info
    uint32_t dataLen;
    uint8_t *blockData;
    // Version-specific return info
    bool newSegmentFlag; // Versions 1-3, set to true when block is the first of its segment
    uint32_t segmentLength; // Versions 1-3, set to total length of segment
    
    uint32_t sizesInBlock; // Version 4, set to the number of size entries making up the block, minus the magic number
    int32_t blockBank; // Version 4, set to block's magic number (requires additional research)
} blockParser;

enum dsTextureFormat {
    NO_TEXTURE,
    A3I5,
    PALETTE_2_BPP,
    PALETTE_4_BPP,
    PALETTE_8_BPP,
    COMPRESSED,
    A5I3,
    DIRECT_TEXTURE
};

typedef struct _dsBTGAHeader {
    uint32_t clobbered0;
    uint32_t bodyLength;
    uint32_t clobbered1;
    uint32_t paletteLength;
    uint32_t clobbered2;
    uint32_t paletteIndexLength;
    enum dsTextureFormat textureFormat;
    uint8_t color0Transparent;
    uint8_t hwidth;
    uint8_t hheight;

    // Values generated from header values
    uint8_t bpp;
    uint32_t hres; // 8 << hwidth
    uint32_t vres; // 8 << hheight
    uint8_t indexBits;
    const uint8_t *alphaConvTable;
} dsBTGAHeader;

// Stores allocated buffers for easier cleanup
typedef struct _ttfTGAFile {
    uint8_t *bodySegment;
    uint16_t *paletteSegment;
    uint16_t *paletteIndexSegment;

    blockParser *parser;
} ttfTGAFile;

// Lookup tables to convert color spaces to 8 bit depth
const uint8_t colorConv5[32] = {0x00, 0x08, 0x10, 0x19, 0x21, 0x29, 0x31, 0x3A,
                                0x42, 0x4A, 0x52, 0x5A, 0x63, 0x6B, 0x73, 0x7B,
                                0x84, 0x8C, 0x94, 0x9C, 0xA5, 0xAD, 0xB5, 0xBD,
                                0xC5, 0xCE, 0xD6, 0xDE, 0xE6, 0xEF, 0xF7, 0xFF};
const uint8_t colorConv3[8]  = {0x00, 0x24, 0x49, 0x6D, 0x92, 0xB6, 0xDB, 0xFF};
const uint8_t colorConv1[2]  = {0xFF, 0xFF}; // For alpha bit

#define CONVRGB555(X) (0xFF000000 | (colorConv5[(X) & 0x001F] << 16) | \
                        (colorConv5[((X) & 0x03E0) >> 5] << 8) | \
                        (colorConv5[((X) & 0x7C00) >> 10]))

#define CONVRGBA5551(X) ((colorConv1[(X) >> 15] << 24) | (colorConv5[(X) & 0x001F] << 16) | \
                        (colorConv5[((X) & 0x03E0) >> 5] << 8) | \
                        (colorConv5[((X) & 0x7C00) >> 10]))

bool readV1Block(blockParser *parser, FILE *inFile, long fileLength);
bool readV3Block(blockParser *parser, FILE *inFile, long fileLength);
bool readV4Block(blockParser *parser, FILE *inFile, long fileLength);

void freeAll(ttfTGAFile *buffers);

// Verifies if current block is a valid BTGA header. Frees block before returning on success
bool processHeader(blockParser *source, dsBTGAHeader *header);

uint8_t verifyColors(uint8_t *bodyData, dsBTGAHeader *header);
uint8_t verifyPalettes(uint16_t *indexData, dsBTGAHeader *header);

uint32_t *genBasePalette(uint16_t *source, uint32_t length, uint8_t color0Transparent);
uint32_t *genA5I3Palette(uint32_t *basePalette, uint8_t numColors);
uint32_t *genA3I5Palette(uint32_t *basePalette, uint8_t numColors);
uint32_t blend888(const uint32_t color0, const uint32_t color1, const int mix0, const int mix1);

uint32_t *convBodyDataDC(uint16_t *bodyData, uint32_t res);
uint32_t *convBodyDataPalette(uint8_t *bodyData, uint32_t *palette, uint32_t res, uint8_t bpp);
uint32_t *convBodyDataCompressed(uint32_t *bodyData, uint32_t *palette, uint16_t *indexTable, dsBTGAHeader *header);

char *tryTGAConv(char *path, bool (*readBlock)(blockParser *, FILE *, long), long startOffset);

int main(int argc, char *argv[]) {
    if(argc != 3) {
        printf("Format: dsConvBTGA version input_directory\n");
        return -1;
    }

    long startOffset = 0;
    bool (*readBlock)(blockParser *, FILE *, long);

    if(!strcmp(argv[1], "1")) {
        readBlock = &readV1Block;
        startOffset = 0x0C;
    } else if(!strcmp(argv[1], "2")) {
        readBlock = &readV1Block;
    } else if(!strcmp(argv[1], "3")) {
        readBlock = &readV3Block;
    } else if(!strcmp(argv[1], "4")) {
        readBlock = &readV4Block;
    } else {
        printf("Format: ./dsConvBTGA version input_directory\n"
               "Where version is one of 1, 2, 3, or 4\n");
    }

    DIR *inputDir = opendir(argv[2]);

    if(!inputDir) {
        printf("Unable to open input directory!\n");
        return -1;
    }

    int inputDirLen = strlen(argv[2]);

    struct dirent *currentEntry;

    int successCount = 0;

    while(1) {
        currentEntry = readdir(inputDir);

        if(!currentEntry) {
            break;
        }

        int entryNameLen = strlen(currentEntry->d_name);

        char *subfilePath = malloc(inputDirLen + entryNameLen + 1);

        strcpy(subfilePath, argv[2]);
        strcat(subfilePath, currentEntry->d_name);

        if(!tryTGAConv(subfilePath, readBlock, startOffset)) {
            successCount++;
        }

        free(subfilePath);
    }

    closedir(inputDir);

    printf("Successfully converted %i files\n", successCount);

    return 0;
}

bool readV1Block(blockParser *parser, FILE *inFile, long fileLength) {
    if(parser->rereadSizes) {
        long currentPos = ftell(inFile);

        if(currentPos + 0x08 > fileLength) {
            return false;
        }

        uint16_t numBlocks;

        size_t readLen = fread(&numBlocks, 0x02, 0x01, inFile);

        if(readLen != 0x01 || !numBlocks) {
            return false;
        }

        parser->numSizeEntries = numBlocks;
        fseek(inFile, 2, SEEK_CUR);

        readLen = fread(&parser->segmentLength, 0x04, 0x01, inFile);

        if(readLen != 0x01 || !parser->segmentLength ||
            currentPos + 0x08 + parser->numSizeEntries * 4 + parser->segmentLength > fileLength) {
            return false;
        }

        parser->blockSizes = malloc(parser->numSizeEntries * 4);

        fread(parser->blockSizes, 4, parser->numSizeEntries, inFile);

        uint32_t totalLen = 0;
        for(int i = 0; i < parser->numSizeEntries; i++) {
            totalLen += parser->blockSizes[i];
        }

        if(totalLen != parser->segmentLength) {
            free(parser->blockSizes);
            parser->blockSizes = NULL;
            parser->blockData = NULL;
            return false;
        }

        parser->sizeIndex = 0;
        parser->newSegmentFlag = true;
        parser->rereadSizes = false;
    } else {
        parser->newSegmentFlag = false;
    }

    parser->dataLen = parser->blockSizes[parser->sizeIndex];
    parser->blockData = malloc(parser->dataLen);
    
    if(!fread(parser->blockData, parser->dataLen, 1, inFile)) {
        free(parser->blockData);
        free(parser->blockSizes);
        parser->blockData = NULL;
        parser->blockSizes = NULL;
        return false;
    }

    parser->sizeIndex++;
    if(parser->sizeIndex == parser->numSizeEntries) {
        free(parser->blockSizes);
        parser->blockSizes = NULL;
        parser->rereadSizes = true;
    }

    return true;
}

bool readV3Block(blockParser *parser, FILE *inFile, long fileLength) {
    unsigned long filePos = ftell(inFile);
    int redirections = 0;

    parser->newSegmentFlag = parser->rereadSizes;

    while(parser->rereadSizes) {
        if(redirections > 5 || filePos + 8 > fileLength) {
            return false;
        }

        uint32_t headerInfo[2];
        if(!fread(&headerInfo, 8, 1, inFile)) {
            return false;
        }
        filePos += 8;

        if(headerInfo[0] & 0xFF) {
            fseek(inFile, headerInfo[1], SEEK_CUR);
            filePos += headerInfo[1];
            redirections++;
            continue;
        }

        parser->numSizeEntries = headerInfo[0] >> 16;
        parser->segmentLength = headerInfo[1];

        if(filePos + parser->numSizeEntries * 4 + parser->segmentLength > fileLength) {
            return false;
        }

        parser->blockSizes = malloc(parser->numSizeEntries * 4);
        fread(parser->blockSizes, 4, parser->numSizeEntries, inFile);

        uint32_t totalLen = 0;
        for(int i = 0; i < parser->numSizeEntries; i++) {
            totalLen += parser->blockSizes[i];
        }

        if(totalLen != parser->segmentLength) {
            free(parser->blockSizes);
            parser->blockSizes = NULL;
            parser->blockData = NULL;
            return false;
        }

        parser->sizeIndex = 0;
        parser->rereadSizes = false;
    }

    parser->dataLen = parser->blockSizes[parser->sizeIndex];
    parser->blockData = malloc(parser->dataLen);
    
    if(!fread(parser->blockData, parser->dataLen, 1, inFile)) {
        free(parser->blockData);
        free(parser->blockSizes);
        parser->blockData = NULL;
        parser->blockSizes = NULL;
        return false;
    }

    parser->sizeIndex++;
    if(parser->sizeIndex == parser->numSizeEntries) {
        free(parser->blockSizes);
        parser->blockSizes = NULL;
        parser->rereadSizes = true;
    }

    return true;
}

bool readV4Block(blockParser *parser, FILE *inFile, long fileLength) {
    unsigned long filePos = ftell(inFile);
    int redirections = 0;

    while(parser->rereadSizes) {
        if(redirections > 5 || filePos + 8 > fileLength) {
            return false;
        }

        uint32_t headerInfo[2];
        if(!fread(&headerInfo, 8, 1, inFile)) {
            return false;
        }
        filePos += 8;

        if(headerInfo[0] & 0xFF) {
            fseek(inFile, headerInfo[1], SEEK_CUR);
            filePos += headerInfo[1];
            redirections++;
            continue;
        }

        parser->numSizeEntries = headerInfo[0] >> 8;
        uint32_t blockSizesLength = headerInfo[1];

        if(parser->numSizeEntries * 4 != blockSizesLength || filePos + parser->numSizeEntries * 4 > fileLength) {
            return false;
        }

        parser->blockSizes = malloc(blockSizesLength);
        fread(parser->blockSizes, blockSizesLength, 1, inFile);
        filePos += blockSizesLength;
        parser->sizeIndex = 0;
        parser->rereadSizes = false;
    }

    if(parser->sizeIndex >= parser->numSizeEntries) {
        free(parser->blockSizes);
        parser->blockSizes = NULL;
        return false;
    }

    int32_t blockMagic = parser->blockSizes[parser->sizeIndex];
    parser->sizeIndex++;
    if(blockMagic < -0x10 || blockMagic > -0x0E) {
        free(parser->blockSizes);
        return false;
    }

    parser->sizesInBlock = 0;
    parser->dataLen = 0;

    while(parser->sizeIndex < parser->numSizeEntries) {
        int32_t blockSize = parser->blockSizes[parser->sizeIndex];
        if(blockSize >= -0x10 && blockSize <= -0x0E) {
            break;
        }
        parser->sizesInBlock++;
        parser->dataLen += blockSize;
        parser->sizeIndex++;
    }

    if(filePos + parser->dataLen > fileLength) {
        free(parser->blockSizes);
        return false;
    }

    parser->blockData = malloc(parser->dataLen);
    fread(parser->blockData, parser->dataLen, 1, inFile);
    return true;
}

void freeAll(ttfTGAFile *buffers) {
    free(buffers->bodySegment);
    free(buffers->paletteSegment);
    free(buffers->paletteIndexSegment);

    free(buffers->parser->blockSizes);
    free(buffers->parser->blockData);
}

bool processHeader(blockParser *source, dsBTGAHeader *header) {
    if(source->dataLen != 0x1C) {
        return false;
    }

    uint8_t formatByte;
    
    memcpy(&header->clobbered0, source->blockData, 4);
    memcpy(&header->bodyLength, source->blockData + 0x04, 4);
    memcpy(&header->clobbered1, source->blockData + 0x08, 4);
    memcpy(&header->paletteLength, source->blockData + 0x0C, 4);
    memcpy(&header->clobbered2, source->blockData + 0x10, 4);
    memcpy(&header->paletteIndexLength, source->blockData + 0x14, 4);
    memcpy(&formatByte, source->blockData + 0x18, 1);
    header->textureFormat = formatByte;
    memcpy(&header->color0Transparent, source->blockData + 0x19, 1);
    memcpy(&header->hwidth, source->blockData + 0x1A, 1);
    memcpy(&header->hheight, source->blockData + 0x1B, 1);

    // Paletted texture missing palette
    if(!header->paletteLength && header->textureFormat != 0x07) {
        return false;
    }

    // Compressed texture missing segment
    if(!header->paletteIndexLength && header->textureFormat == 0x05) {
        return false;
    }

    switch(header->textureFormat) {
        case NO_TEXTURE:
            return false;
            break;
        case A3I5:
            header->bpp = 8;
            header->indexBits = 5;
            header->alphaConvTable = colorConv3;
            break;
        case PALETTE_2_BPP:
            header->bpp = 2;
            header->indexBits = 2;
            break;
        case PALETTE_4_BPP:
            header->bpp = 4;
            header->indexBits = 4;
            break;
        case PALETTE_8_BPP:
            header->bpp = 8;
            header->indexBits = 8;
            break;
        case COMPRESSED:
            if(header->paletteIndexLength != header->bodyLength / 2) {
                return false;
            }

            header->bpp = 2;
            break;
        case A5I3:
            header->bpp = 8;
            header->indexBits = 3;
            header->alphaConvTable = colorConv5;
            break;
        case DIRECT_TEXTURE:
            header->bpp = 16;
            break;
        default:
            return false;
    }

    header->hres = 8 << (header->hwidth & 0x07);
    header->vres = 8 << (header->hheight & 0x07);

    // Body length not matching resolution
    if(header->hres * header->vres * header->bpp != header->bodyLength * 8) {
        return NULL;
    }

    free(source->blockData);
    source->blockData = NULL;

    return header;
}

// Return 0 if an invalid palette index is used
uint8_t verifyColors(uint8_t *bodyData, dsBTGAHeader *header) {
    const uint8_t indexMask = (1 << header->indexBits) - 1;
    const uint8_t bpp = header->bpp;
    const uint8_t ppB = 4 >> (bpp >> 2);
    const uint32_t bodyBytes = header->bodyLength;
    const uint32_t colors = header->paletteLength / 2;

    for(int i = 0; i < bodyBytes; i++) {
        uint8_t currByte = bodyData[i];

        for(int j = 0; j < ppB; j++) {
            if((currByte & indexMask) >= colors) {
                return 0;
            }
            
            currByte >>= bpp;
        }
    }

    return 1;
}

// Return 0 if a compressed texture's palette indexing table points to an invalid palette 
uint8_t verifyPalettes(uint16_t *indexData, dsBTGAHeader *header) {
    const uint32_t indexEntries = header->paletteIndexLength / 2;

    for(int i = 0; i < indexEntries; i++) {
        if((indexData[i] & 0x3FFF) * 4 > header->paletteLength) {
            return 0;
        }
    }

    return 1;
}

// Convert 16-bit DS palettes to true color BGRA
uint32_t *genBasePalette(uint16_t *source, uint32_t length, uint8_t color0Transparent) {
    int paletteSize = length / 2;

    uint32_t *palette = malloc(paletteSize * 4);

    if(color0Transparent) {
        palette[0] = CONVRGB555(source[0]) & 0x00FFFFFF;
    } else {
        palette[0] = CONVRGB555(source[0]);
    }

    for(int i = 1; i < paletteSize; i++) {
        palette[i] = CONVRGB555(source[i]);
    }

    return palette;
}

uint32_t *genA5I3Palette(uint32_t *basePalette, uint8_t numColors) {
    uint32_t *fullPalette = malloc(sizeof(uint32_t) * 256);

    if(numColors > 8) {
        numColors = 8;
    }

    basePalette[0] |= 0xFF000000;

    for(int i = 0; i < numColors; i++) {
        const uint32_t baseColor = basePalette[i];
        for(int j = 0; j < 32; j++) {
            fullPalette[i + j * 8] = baseColor & ((colorConv5[j] << 24) | 0x00FFFFFF);
        }
    }

    free(basePalette);

    return fullPalette;
}

uint32_t *genA3I5Palette(uint32_t *basePalette, uint8_t numColors) {
    uint32_t *fullPalette = malloc(sizeof(uint32_t) * 256);

    if(numColors > 32) {
        numColors = 32;
    }

    basePalette[0] |= 0xFF000000;

    for(int i = 0; i < numColors; i++) {
        const uint32_t baseColor = basePalette[i];
        for(int j = 0; j < 8; j++) {
            fullPalette[i + j * 32] = baseColor & ((colorConv5[j * 4 + j / 2] << 24) | 0x00FFFFFF);
        }
    }

    free(basePalette);

    return fullPalette;
}

uint32_t blend888(const uint32_t color0, const uint32_t color1, const int mix0, const int mix1) {
    const int mixTotal = mix0 + mix1;
    const uint32_t componentOne = (((color0 >> 16) & 0xFF) * mix0 + ((color1 >> 16) & 0xFF) * mix1) / mixTotal;
    const uint32_t componentTwo = (((color0 >> 8) & 0xFF) * mix0 + ((color1 >> 8) & 0xFF) * mix1) / mixTotal;
    const uint32_t componentThree = ((color0 & 0xFF) * mix0 + (color1 & 0xFF) * mix1) / mixTotal;

    return (componentOne << 16) | (componentTwo << 8) | componentThree;
}

uint32_t *convBodyDataDC(uint16_t *bodyData, uint32_t res) {
    uint32_t *imageData = malloc(sizeof(uint32_t) * res);

    for(int i = 0; i < res; i++) {
        imageData[i] = CONVRGBA5551(bodyData[i]);
    }

    return imageData;
}

uint32_t *convBodyDataPalette(uint8_t *bodyData, uint32_t *palette, uint32_t res, uint8_t bpp) {
    uint32_t *imageData = malloc(sizeof(uint32_t) * res);

    const uint8_t pixelMask = (1 << bpp) - 1;
    const uint8_t ppB = 4 >> (bpp >> 2);
    const uint32_t bodyBytes = res >> (ppB >> 1);

    for(int i = 0; i < bodyBytes; i++) {
        uint8_t currByte = bodyData[i];

        for(int j = 0; j < ppB; j++) {
            imageData[i * ppB + j] = palette[currByte & pixelMask];
            currByte >>= bpp;
        }
    }

    return imageData;
}

uint32_t *convBodyDataCompressed(uint32_t *bodyData, uint32_t *palette, uint16_t *indexTable, dsBTGAHeader *header) {
    const uint32_t blocks = header->bodyLength / 4;
    const uint32_t width = header->hres;
    const uint32_t hBlocks = width / 4;
    uint32_t *imageData = malloc(sizeof(uint32_t) * width * header->vres);

    uint32_t blockPalette[4];

    for(int i = 0; i < blocks; i++) {
        uint32_t blockData = bodyData[i];
        uint16_t indexData = indexTable[i];
        const uint32_t *paletteBase = palette + (indexData & 0x3FFF) * 2;

        blockPalette[0] = paletteBase[0];
        blockPalette[1] = paletteBase[1];

        switch(indexData >> 14) {
            case 0:
                blockPalette[2] = paletteBase[2];
                blockPalette[3] = 0;
                break;
            case 1:
                blockPalette[2] = 0xFF000000 | blend888(blockPalette[0], blockPalette[1], 1, 1);
                blockPalette[3] = 0;
                break;
            case 2:
                blockPalette[2] = paletteBase[2];
                blockPalette[3] = paletteBase[3];
                break;
            case 3:
                blockPalette[2] = 0xFF000000 | blend888(blockPalette[0], blockPalette[1], 5, 3);
                blockPalette[3] = 0xFF000000 | blend888(blockPalette[0], blockPalette[1], 3, 5);
                break;
        }

        for(int j = 0; j < 4; j++) {
            for(int k = 0; k < 4; k++) {
                imageData[((i / hBlocks) * width * 4) + ((i % hBlocks) * 4) + j * width + k] = blockPalette[blockData & 0x03];
                blockData >>= 2;
            }
        }
    }

    return imageData;
}

char *tryTGAConv(char *path, bool (*readBlock)(blockParser *, FILE *, long), long startOffset) {
    FILE *inputFile = fopen(path, "rb");

    if(!inputFile) {
        return "Couldn't open input file!\n";
    }

    fseek(inputFile, 0, SEEK_END);
    long fileLength = ftell(inputFile);
    fseek(inputFile, startOffset, SEEK_SET);

    // Minimum header length
    if(fileLength < 0x28) {
        fclose(inputFile);
        return "Requested file is too short to possibly be a TTF TGA!\n";
    }

    ttfTGAFile fileInfo;
    memset(&fileInfo, 0, sizeof(fileInfo));

    blockParser parser;
    parser.blockSizes = NULL;
    parser.rereadSizes = true;
    fileInfo.parser = &parser;

    if(!readBlock(&parser, inputFile, fileLength)) {
        fclose(inputFile);
        return "Malformed header segment descriptor!\n";
    }

    dsBTGAHeader header;

    if(!processHeader(&parser, &header)) {
        freeAll(&fileInfo);
        fclose(inputFile);
        return "Issue relating to header!\n";
    }

    if(!readBlock(&parser, inputFile, fileLength)) {
        freeAll(&fileInfo);
        fclose(inputFile);
        return "Malformed body segment descriptor!\n";
    }

    fileInfo.bodySegment = parser.blockData;
    parser.blockData = NULL;

    if(parser.dataLen != header.bodyLength) {
        freeAll(&fileInfo);
        fclose(inputFile);
        return "Body's length does not match what is reported in header!\n";
    }

    uint32_t *imageData;
    uint32_t totalRes = header.hres * header.vres;

    if(header.textureFormat == DIRECT_TEXTURE) {
        fclose(inputFile);
        imageData = convBodyDataDC((uint16_t *) fileInfo.bodySegment, totalRes);
    } else if(header.textureFormat == COMPRESSED) {
        if(!readBlock(&parser, inputFile, fileLength)) {
            freeAll(&fileInfo);
            fclose(inputFile);
            return "Malformed palette segment descriptor!\n";
        }

        fileInfo.paletteSegment = (uint16_t *) parser.blockData;
        parser.blockData = NULL;

        if(parser.dataLen != header.paletteLength) {
            freeAll(&fileInfo);
            fclose(inputFile);
            return "Palette's length does not match what is reported in header!\n";
        }

        if(!readBlock(&parser, inputFile, fileLength)) {
            freeAll(&fileInfo);
            fclose(inputFile);
            return "Malformed palette index segment descriptor!\n";
        }

        fclose(inputFile);

        fileInfo.paletteIndexSegment = (uint16_t *) parser.blockData;
        parser.blockData = NULL;

        if(parser.dataLen != header.paletteIndexLength) {
            freeAll(&fileInfo);
            return "Palette index's length does not match what is reported in header!\n";
        }

        if(!verifyPalettes(fileInfo.paletteIndexSegment, &header)) {
            freeAll(&fileInfo);
            return "Invalid palette index used!\n";
        }

        uint32_t *palette = genBasePalette(fileInfo.paletteSegment, header.paletteLength, 0);

        imageData = convBodyDataCompressed((uint32_t *) fileInfo.bodySegment, palette, fileInfo.paletteIndexSegment, &header);

        free(palette);
    } else {
        if(!verifyColors(fileInfo.bodySegment, &header)) {
            freeAll(&fileInfo);
            fclose(inputFile);
            return "Invalid color index used!\n";
        }

        if(!readBlock(&parser, inputFile, fileLength)) {
            freeAll(&fileInfo);
            fclose(inputFile);
            return "Malformed palette segment descriptor!\n";
        }

        fclose(inputFile);

        fileInfo.paletteSegment = (uint16_t *) parser.blockData;
        parser.blockData = NULL;

        if(parser.dataLen != header.paletteLength) {
            freeAll(&fileInfo);
            return "Palette's length does not match what is reported in header!\n";
        }

        uint32_t *palette = genBasePalette(fileInfo.paletteSegment, header.paletteLength, header.color0Transparent);

        if(header.textureFormat == A3I5) {
            palette = genA3I5Palette(palette, header.paletteLength / 2);
        } else if(header.textureFormat == A5I3) {
            palette = genA5I3Palette(palette, header.paletteLength / 2);
        }

        imageData = convBodyDataPalette(fileInfo.bodySegment, palette, totalRes, header.bpp);

        free(palette);
    }

    int pathLen = strlen(path);

    char *outputPath = malloc(pathLen + 5);
    strcpy(outputPath, path);
    strcat(outputPath, ".tga");

    FILE *outFile = fopen(outputPath, "wb");

    free(outputPath);

    if(!outFile) {
        freeAll(&fileInfo);
        free(imageData);

        return "Failed to open output file!\n";
    }

    fputc(0, outFile);
    fputc(0, outFile);
    fputc(2, outFile);

    fputc(0, outFile);
    fputc(0, outFile);
    fputc(0, outFile);
    fputc(0, outFile);
    fputc(0, outFile);

    fputc(0, outFile);
    fputc(0, outFile);

    fputc(0, outFile);
    fputc(0, outFile);

    fwrite(&header.hres, 2, 1, outFile);
    fwrite(&header.vres, 2, 1, outFile);
    
    fputc(32, outFile);
    fputc(0b00111000, outFile);

    fwrite(imageData, 4, totalRes, outFile);
    free(imageData);

    fclose(outFile);

    freeAll(&fileInfo);

    return NULL;
}
