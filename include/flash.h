#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <stdbool.h>

#define CHUNK_SIZE (128 * 1024)

#ifdef __cplusplus
extern "C" {
#endif

// External variables provided by flash.c
extern bool bitSwapped;
extern uint32_t cartSize;
extern uint32_t totalSectors;

// Core Initialization & CFI Functions
void FlashReset(void);
void UnlockDYBProtection(void);
bool QueryCFI(void);
uint16_t DetectChipType(void);

// Flash Operations
bool WaitUntilReady(uint32_t wordAddress);
bool EraseSector(uint32_t byteAddress);
bool WriteBuffer(uint32_t byteAddress, const uint16_t* bufferWords, uint16_t wordCount);
bool WriteData(uint32_t byteAddress, const uint8_t* data, uint32_t size);
bool VerifyData(uint32_t address, const uint8_t* data, uint32_t size);

// High-level Operations
bool LoadAndFlashROM(const char* filename);

#ifdef __cplusplus
}
#endif

#endif // FLASH_H
