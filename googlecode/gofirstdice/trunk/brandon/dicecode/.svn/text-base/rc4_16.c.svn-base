#include "rc4_16.h"

void rc4_16_swap(uint16_t i, uint16_t j) {

  uint16_t temp;
  temp = rc4_16_state[i];
  rc4_16_state[i] = rc4_16_state[j];
  rc4_16_state[j] = temp;
}


void rc4_16_init(uint16_t (*key)[], size_t len) {

  int i;
  uint16_t junk;

  /* setup array */
  for (i = 0; i < 65536; i++) {
    rc4_16_state[i] = i;
  }

  rc4_16_i = 0;
  rc4_16_j = 0;
  for (i = 0; i < 65536; i++) {
    rc4_16_j = (rc4_16_j + (*key)[i % len] + rc4_16_state[i]) & 0xFFFF;
    rc4_16_swap(i, rc4_16_j);
  }

  rc4_16_i = 0;
  rc4_16_j = 0;

  /* Drop 12 times the state size */
  for (i = 0; i < (65536 * 12); i++) {
    junk = rc4_16_next();
  }

}


void rc4_16_add(uint16_t (*key)[], size_t len) {

  int i;
  uint16_t junk;

  rc4_16_i = 0;
  rc4_16_j = 0;
  for (i = 0; i < 65536; i++) {
    rc4_16_j = (rc4_16_j + (*key)[i % len] + rc4_16_state[i]) & 0xFFFF;
    rc4_16_swap(i, rc4_16_j);
  }

  rc4_16_i = 0;
  rc4_16_j = 0;

  /* Drop 12 times the state size */
  for (i = 0; i < (65536 * 12); i++) {
    junk = rc4_16_next();
  }

}


uint16_t rc4_16_next() {
  rc4_16_i = (rc4_16_i + 1) & 0xFFFF;
  rc4_16_j = (rc4_16_j + rc4_16_state[rc4_16_i]) & 0xFFFF;

  rc4_16_swap(rc4_16_i, rc4_16_j);

  return rc4_16_state[(rc4_16_state[rc4_16_i] +
		       rc4_16_state[rc4_16_j]) & 0xFFFF];

}


