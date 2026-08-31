/*
 * Continuous UART0 test pattern for the external RS-485/discrete-I/O bus.
 *
 * This is intentionally kept in the ESP-IDF application layer.  It is a
 * bring-up aid and does not pretend to be a Modbus transaction yet.
 */
#ifndef RS485_TEST_H
#define RS485_TEST_H

void rs485_test_init(void);

#endif /* RS485_TEST_H */
