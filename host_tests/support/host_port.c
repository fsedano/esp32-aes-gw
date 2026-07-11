/**
  ******************************************************************************
  * @file    host_port.c
  * @brief   Host implementation of the lccore port seam for unit tests.
  ******************************************************************************
  */

#include "lc_port.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t g_tick;

void lc_port_init(void){
    g_tick = 0;
}

uint32_t lc_port_tick_ms(void){
    return g_tick;
}

/* Test control: pin the tick so fixtures can produce exact wire bytes. */
void host_port_set_tick(uint32_t ms){
    g_tick = ms;
}

void lc_port_lock(void){}
void lc_port_unlock(void){}

void lc_port_console(const char *line){
    if(getenv("TEST_VERBOSE")){
        printf("[lc] %s\n", line);
    }
}
