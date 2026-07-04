#ifndef RANGE_BACKEND_H
#define RANGE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

void range_param_init(void);
void range_set(uint8_t channel);
uint8_t range_get(void);
void range_process_cmd(const char *cmd, uint16_t len);
void range_update_manual(void);
#endif
