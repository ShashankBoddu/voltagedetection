#ifndef STORAGE_BACKEND_H
#define STORAGE_BACKEND_H

#include "../common.h"

/**
 * @brief Initialize the storage subsystem (Settings/NVS).
 *        Loads values from flash into provided structure or sets defaults.
 *
 * @param thresholds Pointer to threshold structure to populate.
 * @return 0 on success, negative error code otherwise.
 */
int storage_init(thresholds_t *thresholds);

/**
 * @brief Save the current thresholds to Flash.
 *
 * @param thresholds Pointer to the threshold structure to save.
 * @return 0 on success, negative error code otherwise.
 */
int storage_save_thresholds(const thresholds_t *thresholds);

#endif // STORAGE_BACKEND_H
