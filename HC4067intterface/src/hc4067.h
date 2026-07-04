/*
 * hc4067.h - Library for HC4067 Multiplexer
 */

#ifndef HC4067_H
#define HC4067_H

#include <zephyr/kernel.h>
#include <stdint.h>

/**
 * @brief Initialize the HC4067 GPIOs.
 *
 * @return 0 on success, negative errno code on failure.
 */
int hc4067_init(void);

/**
 * @brief Set the HC4067 multiplexer channel.
 *
 * @param channel The channel index (0-15).
 */
void hc4067_set_channel(uint8_t channel);

#endif /* HC4067_H */
