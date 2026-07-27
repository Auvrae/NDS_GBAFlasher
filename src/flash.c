#include <text.h>
#include <audio.h>
#include <flash.h>
#include <input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sfx/soundbank.h>

#define FLASH_BASE   0x08000000   // Start of cartridge ROM space
#define SECTOR_SIZE  (128 * 1024) // S29GL-S uniform 128 KB sector size

// Flash Macro for 16-bit word addressing
#define _FLASH_WRITE(word_offset, data) \
    do { *((volatile uint16_t*)FLASH_BASE + (word_offset)) = (data); __asm("nop"); } while(0)

#define _FLASH_READ(word_offset) \
    (*((volatile uint16_t*)FLASH_BASE + (word_offset)))

bool bitSwapped = false;
uint32_t cartSize = 0;
uint32_t totalSectors = 0;

// Apply bit-swapping logic if CPLD routes D0 and D1 inversely
uint16_t SwapCmd(uint16_t cmd) {
    if (bitSwapped) {
        return (cmd & 0xFFFC) | ((cmd & 1) << 1) | ((cmd & 2) >> 1);
    }
    return cmd;
}

uint16_t ReadWord(uint32_t wordOffset) {
    uint16_t data = _FLASH_READ(wordOffset);
    if (bitSwapped) {
        data = (data & 0xFFFC) | ((data & 1) << 1) | ((data & 2) >> 1);
    }
    return data;
}

// Reset Flash to Read Array Mode
void FlashReset(void) {
    _FLASH_WRITE(0x0000, SwapCmd(0x00F0));
}

// Unlocks Dynamic Sector Protection (DYB) for all sectors
void UnlockDYBProtection(void) {
    _FLASH_WRITE(0x555, SwapCmd(0x00AA));
    _FLASH_WRITE(0x2AA, SwapCmd(0x0055));
    _FLASH_WRITE(0x555, SwapCmd(0x00C0)); // Enter ASP ASO

    // Clear all DYB bits to 0xFF (Unprotected)
    _FLASH_WRITE(0x000, SwapCmd(0x00A0));
    _FLASH_WRITE(0x000, 0x00FF);

    FlashReset(); // Exit ASO Mode
}

// Query the ROM chip using CFI
bool QueryCFI(void) {
    FlashReset();
    
    // Try Standard (No Bit Swap)
    bitSwapped = false;
    _FLASH_WRITE(0x55, SwapCmd(0x0098));

    uint16_t Q = ReadWord(0x10);
    uint16_t R = ReadWord(0x11);
    uint16_t Y = ReadWord(0x12);

    if (Q == 'Q' && R == 'R' && Y == 'Y') {
        bitSwapped = false;
    } else {
        // Try Bit Swapped
        bitSwapped = true;
        _FLASH_WRITE(0x55, SwapCmd(0x0098));
        Q = ReadWord(0x10);
        R = ReadWord(0x11);
        Y = ReadWord(0x12);

        if (Q != 'Q' || R != 'R' || Y != 'Y') {
            FlashReset();
            return false;
        }
    }

    // Read Device Size Power from CFI offset 0x27
    uint8_t sizePower = (uint8_t)(ReadWord(0x27) & 0x00FF);

    // 0x18 = 16MB (128Mb), 0x19 = 32MB (256Mb), 0x1A = 64MB (512Mb), 0x1B = 128MB (1Gb)
    if (sizePower >= 0x18 && sizePower <= 0x1B) {
        cartSize = ((uint32_t)1 << sizePower); 
        totalSectors = ((uint32_t)1 << (sizePower - 17)); // 128KB sectors
    } else {
        FlashReset();
        return false;
    }

    FlashReset();
    UnlockDYBProtection();
    return true;
}

uint16_t DetectChipType(void) {
    if (!QueryCFI()) {
        return 0xFFFF; // Failed CFI
    }

    // Return Megabit capacity to match main.c switch statement
    switch (cartSize) {
        case (8 * 1024 * 1024):   return 64;   // 8 MB / 64 Mb
        case (16 * 1024 * 1024):  return 128;  // 16 MB / 128 Mb
        case (32 * 1024 * 1024):  return 256;  // 32 MB / 256 Mb (S29GL256S)
        case (64 * 1024 * 1024):  return 512;  // 64 MB / 512 Mb
        case (128 * 1024 * 1024): return 1024; // 128 MB / 1 Gb
        default: return 0;
    }
}

// Non-blocking status poll using DQ6 toggle bit with safety timeout
bool WaitUntilReady(uint32_t wordAddress) {
    volatile uint16_t status1, status2;
    uint32_t timeout = 5000000; // Timeout threshold prevents UI freezes

    do {
        status1 = _FLASH_READ(wordAddress);
        status2 = _FLASH_READ(wordAddress);
        
        // DQ6 stops toggling when operation completes
        if ((status1 & 0x0040) == (status2 & 0x0040)) {
            return true; 
        }
    } while (--timeout > 0);

    FlashReset(); // Safety reset on timeout
    return false; 
}

bool EraseSector(uint32_t byteAddress) {
    uint32_t sectorWordAddr = byteAddress / 2;

    _FLASH_WRITE(0x555, SwapCmd(0x00AA));
    _FLASH_WRITE(0x2AA, SwapCmd(0x0055));
    _FLASH_WRITE(0x555, SwapCmd(0x0080));
    _FLASH_WRITE(0x555, SwapCmd(0x00AA));
    _FLASH_WRITE(0x2AA, SwapCmd(0x0055));
    _FLASH_WRITE(sectorWordAddr, SwapCmd(0x0030)); // Sector erase command

    return WaitUntilReady(sectorWordAddr);
}

// Write up to 256 words (512 bytes) using Write Buffer Programming
bool WriteBuffer(uint32_t byteAddress, const uint16_t* bufferWords, uint16_t wordCount) {
    if (wordCount == 0 || wordCount > 256) return false;

    uint32_t sectorWordAddr = byteAddress / 2;

    _FLASH_WRITE(0x555, SwapCmd(0x00AA));
    _FLASH_WRITE(0x2AA, SwapCmd(0x0055));
    _FLASH_WRITE(sectorWordAddr, SwapCmd(0x0025));             // Write to Buffer command
    _FLASH_WRITE(sectorWordAddr, SwapCmd(wordCount - 1));       // Word Count (N - 1)

    // Load payload into Write Buffer
    for (uint16_t i = 0; i < wordCount; i++) {
        _FLASH_WRITE(sectorWordAddr + i, SwapCmd(bufferWords[i]));
    }

    _FLASH_WRITE(sectorWordAddr, SwapCmd(0x0029));             // Program Confirm

    return WaitUntilReady(sectorWordAddr);
}

bool WriteData(uint32_t byteAddress, const uint8_t* data, uint32_t size) {
    uint32_t bytesWritten = 0;

    while (bytesWritten < size) {
        uint32_t currentAddr = byteAddress + bytesWritten;
        uint32_t chunkBytes = size - bytesWritten;

        // Align writes to 512-byte buffer boundaries
        uint32_t boundaryOffset = currentAddr % 512;
        uint32_t maxChunk = 512 - boundaryOffset;
        if (chunkBytes > maxChunk) {
            chunkBytes = maxChunk;
        }

        uint16_t wordCount = (chunkBytes + 1) / 2;
        uint16_t wordBuf[256];
        
        memset(wordBuf, 0xFF, sizeof(wordBuf));
        memcpy(wordBuf, data + bytesWritten, chunkBytes);

        if (!WriteBuffer(currentAddr, wordBuf, wordCount)) {
            return false;
        }

        bytesWritten += chunkBytes;
    }
    return true;
}

bool LoadAndFlashROM(const char* filename) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        RenderLine(0, "ERROR OPENING FILE!", 12);
        return false;
    }

    fseek(file, 0, SEEK_END);
    uint32_t romSize = ftell(file);
    rewind(file);

    if (romSize > cartSize) {
        fclose(file);
        RenderLine(0, "ROM TOO BIG FOR CART!", 12);
        PlaySound(SFX_FAIL);
        return false;
    }

    RenderLine(0, "FLASHING ROM...", 12);

    uint8_t* buffer = (uint8_t*)malloc(SECTOR_SIZE);
    if (!buffer) {
        fclose(file);
        RenderLine(0, "MEMORY ALLOCATION FAILED!", 12);
        PlaySound(SFX_FAIL);
        return false;
    }

    uint32_t offset = 0;
    uint32_t sectorIndex = 0;

    while (offset < romSize && sectorIndex < totalSectors) {
        uint32_t sectorAddress = sectorIndex * SECTOR_SIZE;
        uint32_t bytesToRead = (romSize - offset > SECTOR_SIZE) ? SECTOR_SIZE : (romSize - offset);

        memset(buffer, 0xFF, SECTOR_SIZE);
        fread(buffer, 1, bytesToRead, file);

        if (!EraseSector(sectorAddress)) {
            RenderLine(0, "ERASE FAILED / TIMED OUT!", 14);
            PlaySound(SFX_FAIL);
            free(buffer);
            fclose(file);
            return false;
        }

        if (!WriteData(sectorAddress, buffer, bytesToRead)) {
            RenderLine(0, "WRITE FAILED / TIMED OUT!", 14);
            PlaySound(SFX_FAIL);
            free(buffer);
            fclose(file);
            return false;
        }

        offset += bytesToRead;
        sectorIndex++;

        // Render progress
        char progressMsg[32];
        snprintf(progressMsg, sizeof(progressMsg), "Progress: %ld%%", (offset * 100) / romSize);
        RenderLine(0, progressMsg, 14);
    }

    FlashReset();

    RenderLine(0, "FLASH COMPLETE!", 16);
    PlaySound(SFX_SUCCESS);

    free(buffer);
    fclose(file);
    return true;
}
