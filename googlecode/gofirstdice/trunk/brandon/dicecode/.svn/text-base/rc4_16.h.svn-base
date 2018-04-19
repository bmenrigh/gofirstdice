#ifndef RC4_16_H
#define RC4_16_H 1

#include <stdint.h>
#include <stddef.h>

uint16_t rc4_16_state[65536];
uint16_t rc4_16_i;
uint16_t rc4_16_j;

void rc4_16_init(uint16_t (*)[], size_t);
void rc4_16_add(uint16_t (*)[], size_t);
void rc4_16_swap(uint16_t, uint16_t);
uint16_t rc4_16_next();

#endif
