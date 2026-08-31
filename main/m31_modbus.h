/* Ebyte M31-U discrete-I/O backend over Modbus RTU on UART0. */
#ifndef M31_MODBUS_H
#define M31_MODBUS_H

#include <stdint.h>

/* Configure UART0, bind the nonblocking discrete backend and start its
   polling worker. Performs a bounded population probe before returning so
   the self-description can be frozen for this boot. Safe to call once. */
void m31_modbus_init(void);

/* Boot-time-frozen process-image widths discovered during init. */
uint16_t m31_modbus_output_count(void);
uint16_t m31_modbus_input_count(void);

#endif /* M31_MODBUS_H */
