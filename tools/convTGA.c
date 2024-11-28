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
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

typedef struct _segmentHeader {
    uint16_t totalBlocks;
    uint8_t padding[2];
    uint32_t segmentLength;

    uint32_t *blockSizes;
    uint8_t *segmentData;
} segmentHeader;

typedef struct _ttfTGAHeader {
    uint32_t clobbered0;
    uint32_t bodyLength;
    uint32_t clobbered1;
    uint32_t paletteLength;
    uint32_t clobbered2;
    uint32_t paletteIndexLength;
    uint8_t textureFormat;
    uint8_t color0Transparent;
    uint8_t hwidth;
    uint8_t hheight;

    // Values generated from header values
    uint8_t bpp;
    uint32_t hres; // 8 << hwidth
    uint32_t vres; // 8 << hheight
    uint8_t indexBits;
    const uint8_t *alphaConvTable;
} ttfTGAHeader;

// Stores all allocated buffers by the program to limit number of free calls
typedef struct _ttfTGAFile {
    segmentHeader *headerSegment;
    segmentHeader *bodySegment;
    segmentHeader *paletteSegment;
    segmentHeader *paletteIndexSegment;

    ttfTGAHeader *header;
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

segmentHeader *readSegment(FILE *inFile, long fileLength);
void freeSegment(segmentHeader *seg);
void freeAll(ttfTGAFile segments);

ttfTGAHeader *readHeader(segmentHeader *source);

uint8_t verifyColors(uint8_t *bodyData, ttfTGAHeader *header);
uint8_t verifyPalettes(uint16_t *indexData, ttfTGAHeader *header);

uint32_t *genBasePalette(segmentHeader *source, uint8_t color0Transparent);
uint32_t *genA5I3Palette(uint32_t *basePalette, uint8_t numColors);
uint32_t *genA3I5Palette(uint32_t *basePalette, uint8_t numColors);
uint32_t blend888(const uint32_t color0, const uint32_t color1, const int mix0, const int mix1);

uint32_t *convBodyDataDC(uint16_t *bodyData, uint32_t res);
uint32_t *convBodyDataPalette(uint8_t *bodyData, uint32_t *palette, uint32_t res, uint8_t bpp);
uint32_t *convBodyDataCompressed(uint32_t *bodyData, ttfTGAFile *segments, uint32_t *palette);

char *tryTGAConv(char *path);

int main(int argc, char *argv[]) {
    if(argc != 2) {
        printf("Format: convTGA [input directory]\n");
        return -1;
    }

    DIR *inputDir = opendir(argv[1]);

    if(!inputDir) {
        printf("Unable to open input directory!\n");
        return -1;
    }

    int inputDirLen = strlen(argv[1]);

    struct dirent *currentEntry;

    while(1) {
        currentEntry = readdir(inputDir);

        if(!currentEntry) {
            break;
        }

        int entryNameLen = strlen(currentEntry->d_name);

        char *subfilePath = malloc(inputDirLen + entryNameLen + 1);

        strcpy(subfilePath, argv[1]);
        strcat(subfilePath, currentEntry->d_name);

        tryTGAConv(subfilePath);

        free(subfilePath);
    }

    closedir(inputDir);

    return 0;
}

segmentHeader *readSegment(FILE *inFile, long fileLength) {
    long currentPos = ftell(inFile);

    if(currentPos + 0x0C > fileLength) {
        return NULL;
    }

    segmentHeader *segment = malloc(sizeof(segmentHeader));
    
    size_t readLen = fread(&segment->totalBlocks, 0x04, 0x01, inFile);

    if(readLen != 0x01 || !segment->totalBlocks) {
        free(segment);
        return NULL;
    }

    readLen = fread(&segment->segmentLength, 0x04, 0x01, inFile);

    if(readLen != 0x01 || !segment->segmentLength ||
        currentPos + 0x08 + segment->totalBlocks * 4 + segment->segmentLength > fileLength) {
        free(segment);
        return NULL;
    }

    segment->blockSizes = malloc(segment->totalBlocks * 4);

    fread(segment->blockSizes, 4, segment->totalBlocks, inFile);

    // BTGAs always use one block to a segment
    if(segment->blockSizes[0] != segment->segmentLength) {
        free(segment->blockSizes);
        free(segment);
        return NULL;
    }

    segment->segmentData = malloc(segment->segmentLength);
    fread(segment->segmentData, 1, segment->segmentLength, inFile);

    return segment;
}

void freeSegment(segmentHeader *seg) {
    if(!seg) {
        return;
    }

    free(seg->blockSizes);
    free(seg->segmentData);
    free(seg);
}

void freeAll(ttfTGAFile segments) {
    freeSegment(segments.headerSegment);
    freeSegment(segments.bodySegment);
    freeSegment(segments.paletteSegment);
    freeSegment(segments.paletteIndexSegment);

    free(segments.header);
}

ttfTGAHeader *readHeader(segmentHeader *source) {
    ttfTGAHeader *header = malloc(sizeof(ttfTGAHeader));

    memcpy(&header->clobbered0, source->segmentData, 4);
    memcpy(&header->bodyLength, source->segmentData + 0x04, 4);
    memcpy(&header->clobbered1, source->segmentData + 0x08, 4);
    memcpy(&header->paletteLength, source->segmentData + 0x0C, 4);
    memcpy(&header->clobbered2, source->segmentData + 0x10, 4);
    memcpy(&header->paletteIndexLength, source->segmentData + 0x14, 4);
    memcpy(&header->textureFormat, source->segmentData + 0x18, 1);
    memcpy(&header->color0Transparent, source->segmentData + 0x19, 1);
    memcpy(&header->hwidth, source->segmentData + 0x1A, 1);
    memcpy(&header->hheight, source->segmentData + 0x1B, 1);

    // Paletted texture missing palette
    if(!header->paletteLength && header->textureFormat != 0x07) {
        free(header);
        return NULL;
    }

    // Compressed texture missing segment
    if(!header->paletteIndexLength && header->textureFormat == 0x05) {
        free(header);
        return NULL;
    }

    switch(header->textureFormat) {
        case 0x00: // No texture
            free(header);
            return NULL;
            break;
        case 0x01: // A3I5 texture
            header->bpp = 8;
            header->indexBits = 5;
            header->alphaConvTable = colorConv3;
            break;
        case 0x02: // 2-bit palette texture
            header->bpp = 2;
            header->indexBits = 2;
            break;
        case 0x03: // 4-bit palette texture
            header->bpp = 4;
            header->indexBits = 4;
            break;
        case 0x04: // 8-bit palette texture
            header->bpp = 8;
            header->indexBits = 8;
            break;
        case 0x05: // Compressed texture
            if(header->paletteIndexLength != header->bodyLength / 2) {
                free(header);
                return NULL;
            }

            header->bpp = 2;

            break;
        case 0x06: // A5I3 texture
            header->bpp = 8;
            header->indexBits = 3;
            header->alphaConvTable = colorConv5;
            break;
        case 0x07: // Direct color texture
            header->bpp = 16;
            break;
        default:
            free(header);
            return NULL;
    }

    header->hres = 8 << header->hwidth;
    header->vres = 8 << header->hheight;

    // Body length not matching resolution
    if(header->hres * header->vres * header->bpp != header->bodyLength * 8) {
        free(header);
        return NULL;
    }

    return header;
}

// Return 0 if an invalid palette index is used
uint8_t verifyColors(uint8_t *bodyData, ttfTGAHeader *header) {
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
uint8_t verifyPalettes(uint16_t *indexData, ttfTGAHeader *header) {
    const uint32_t indexEntries = header->paletteIndexLength / 2;

    for(int i = 0; i < indexEntries; i++) {
        if((indexData[i] & 0x3FFF) * 4 > header->paletteLength) {
            return 0;
        }
    }

    return 1;
}

// Convert 16-bit DS palettes to true color BGRA
uint32_t *genBasePalette(segmentHeader *source, uint8_t color0Transparent) {
    int paletteSize = source->segmentLength / 2;

    uint32_t *palette = malloc(paletteSize * 4);

    uint16_t *sourcePalette = (uint16_t *) source->segmentData;

    if(color0Transparent) {
        palette[0] = CONVRGB555(sourcePalette[0]) & 0x00FFFFFF;
    } else {
        palette[0] = CONVRGB555(sourcePalette[0]);
    }

    for(int i = 1; i < paletteSize; i++) {
        palette[i] = CONVRGB555(sourcePalette[i]);
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

uint32_t *convBodyDataCompressed(uint32_t *bodyData, ttfTGAFile *segments, uint32_t *palette) {
    const uint32_t blocks = segments->header->bodyLength / 4;
    const uint32_t width = segments->header->hres;
    const uint32_t hBlocks = width / 4;
    const uint16_t *indexTable = (uint16_t *) segments->paletteIndexSegment->segmentData;
    uint32_t *imageData = malloc(sizeof(uint32_t) * width * segments->header->vres);

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

char *tryTGAConv(char *path) {
    FILE *inputFile = fopen(path, "rb");

    if(!inputFile) {
        return "Couldn't open input file!\n";
    }

    fseek(inputFile, 0, SEEK_END);
    long fileLength = ftell(inputFile);
    fseek(inputFile, 0, SEEK_SET);

    // Minimum header length
    if(fileLength < 0x28) {
        fclose(inputFile);
        return "Requested file is too short to possibly be a TTF TGA!\n";
    }

    ttfTGAFile fileInfo;
    memset(&fileInfo, 0, sizeof(fileInfo));

    segmentHeader *headerSeg = readSegment(inputFile, fileLength);
    
    if(!headerSeg) {
        fclose(inputFile);
        return "Malformed header segment descriptor!\n";
    }

    if(headerSeg->blockSizes[0] != 0x1C) {
        fclose(inputFile);
        freeSegment(headerSeg);
        return "Reported header block segment is incorrect length";
    }

    fileInfo.headerSegment = headerSeg;

    ttfTGAHeader *header = readHeader(headerSeg);

    if(!header) {
        freeAll(fileInfo);
        fclose(inputFile);
        return "Issue relating to header!\n";
    }

    fileInfo.header = header;

    segmentHeader *bodySeg = readSegment(inputFile, fileLength);

    if(!bodySeg) {
        freeAll(fileInfo);
        fclose(inputFile);
        return "Malformed body segment descriptor!\n";
    }

    fileInfo.bodySegment = bodySeg;

    if(bodySeg->segmentLength != header->bodyLength) {
        freeAll(fileInfo);
        fclose(inputFile);
        return "Body's length does not match what is reported in header!\n";
    }

    uint32_t *imageData;
    uint32_t totalRes = header->hres * header->vres;

    if(header->textureFormat == 7) {
        fclose(inputFile);
        imageData = convBodyDataDC((uint16_t *) bodySeg->segmentData, totalRes);
    } else if(header->textureFormat == 5) {
        segmentHeader *paletteSeg = readSegment(inputFile, fileLength);

        if(!paletteSeg) {
            freeAll(fileInfo);
            fclose(inputFile);
            return "Malformed palette segment descriptor!\n";
        }

        fileInfo.paletteSegment = paletteSeg;

        if(paletteSeg->segmentLength != header->paletteLength) {
            freeAll(fileInfo);
            fclose(inputFile);
            return "Palette's length does not match what is reported in header!\n";
        }

        segmentHeader *paletteIndexSeg = readSegment(inputFile, fileLength);

        fclose(inputFile);

        if(!paletteIndexSeg) {
            freeAll(fileInfo);
            return "Malformed palette index segment descriptor!\n";
        }

        fileInfo.paletteIndexSegment = paletteIndexSeg;

        if(paletteIndexSeg->segmentLength != header->paletteIndexLength) {
            freeAll(fileInfo);
            fclose(inputFile);
            return "Palette index's length does not match what is reported in header!\n";
        }

        if(!verifyPalettes((uint16_t *)paletteIndexSeg->segmentData, header)) {
            freeAll(fileInfo);
            return "Invalid palette index used!\n";
        }

        uint32_t *palette = genBasePalette(paletteSeg, 0);

        imageData = convBodyDataCompressed((uint32_t *) bodySeg->segmentData, &fileInfo, palette);

        free(palette);
    } else {
        if(!verifyColors(bodySeg->segmentData, header)) {
            freeAll(fileInfo);
            fclose(inputFile);
            return "Invalid color index used!\n";
        }

        segmentHeader *paletteSeg = readSegment(inputFile, fileLength);

        fclose(inputFile);

        if(!paletteSeg) {
            freeAll(fileInfo);
            return "Malformed palette segment descriptor!\n";
        }

        fileInfo.paletteSegment = paletteSeg;

        if(paletteSeg->segmentLength != header->paletteLength) {
            freeAll(fileInfo);
            return "Palette's length does not match what is reported in header!\n";
        }

        uint32_t *palette = genBasePalette(paletteSeg, header->color0Transparent);

        if(header->textureFormat == 1) {
            palette = genA3I5Palette(palette, header->paletteLength / 2);
        } else if(header->textureFormat == 6) {
            palette = genA5I3Palette(palette, header->paletteLength / 2);
        }

        imageData = convBodyDataPalette(bodySeg->segmentData, palette, totalRes, header->bpp);

        free(palette);
    }

    int pathLen = strlen(path);

    char *outputPath = malloc(pathLen + 5);
    strcpy(outputPath, path);
    strcat(outputPath, ".tga");

    FILE *outFile = fopen(outputPath, "wb");

    free(outputPath);

    if(!outFile) {
        freeAll(fileInfo);
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

    fwrite(&header->hres, 2, 1, outFile);
    fwrite(&header->vres, 2, 1, outFile);
    
    fputc(32, outFile);
    fputc(0b00111000, outFile);

    fwrite(imageData, 4, totalRes, outFile);
    free(imageData);

    fclose(outFile);

    freeAll(fileInfo);

    return NULL;
}
