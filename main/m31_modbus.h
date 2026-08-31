/* Ebyte M31-U discrete-I/O backend over Modbus RTU on UART0. */
#ifndef M31_MODBUS_H
#define M31_MODBUS_H

/* Configure UART0, bind the nonblocking discrete backend and start its
   polling worker. Safe to call once during application startup. */
void m31_modbus_init(void);

#endif /* M31_MODBUS_H */
