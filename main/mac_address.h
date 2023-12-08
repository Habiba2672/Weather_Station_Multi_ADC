/*
 * mac_address.h
 *
 *  Created on: Dec 3, 2023
 *      Author: alex technology
 */


#ifndef MAIN_MAC_ADDRESS_H_
#define MAIN_MAC_ADDRESS_H_

#include <stdlib.h>
#include "freertos/FreeRTOS.h"

typedef union{
	uint8_t as_bytes[6];
	uint64_t as_long;
}mac_address_t;

BaseType_t mac_get_address(char *buffer, size_t buffer_len);



#endif /* MAIN_MAC_ADDRESS_H_ */
