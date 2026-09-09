#ifndef AURORA_DVD_H
#define AURORA_DVD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <dolphin/types.h>

/**
 * Open a GC/Wii disc image for use by the DVD API.
 * Must be called before DVDInit().
 * Returns true on success, false on failure.
 */
bool aurora_dvd_open(const char* disc_path);

/**
 * Close the disc image and free all resources.
 */
void aurora_dvd_close(void);

/** Raw FST bytes from the opened disc's DATA partition. */
bool aurora_dvd_get_raw_fst(const uint8_t** out_data, size_t* out_size);

/** Read from the decrypted DATA partition at a byte offset. */
int32_t aurora_dvd_read_partition(void* out, uint32_t length, uint64_t offset);

/** Game code from the opened disc's DiskID (e.g. RMCE01 header). 0 if no disc. */
uint32_t aurora_dvd_get_game_code(void);

#ifdef __cplusplus
}
#endif

#endif
