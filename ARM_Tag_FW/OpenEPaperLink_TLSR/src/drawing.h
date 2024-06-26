#ifndef _DRAWING_H_
#define _DRAWING_H_

#include <stdint.h>
#include <stdbool.h>

void drawOnOffline(uint8_t state);
void drawImageAtAddress(uint32_t addr, uint8_t lut);

#endif
