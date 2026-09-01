#ifndef RS485_DISCRETE_H
#define RS485_DISCRETE_H

#include <stdint.h>

void rs485_discrete_init(void);
uint16_t rs485_discrete_output_count(void);
uint16_t rs485_discrete_input_count(void);

#endif /* RS485_DISCRETE_H */
