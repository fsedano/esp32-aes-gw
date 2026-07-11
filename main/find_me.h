/**
  ******************************************************************************
  * @file    find_me.h
  * @brief   FIND_ME locator: blink the board LED (when one is configured)
  *          for a number of seconds; always logs.
  ******************************************************************************
  */

#ifndef MAIN_FIND_ME_H
#define MAIN_FIND_ME_H

#include <stdint.h>

void find_me_init(void);
void find_me_start(uint32_t duration_s);

#endif /* MAIN_FIND_ME_H */
