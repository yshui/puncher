#pragma once

#include <stdint.h>

#define talloc(nmemb, type) ((type *)calloc(nmemb, sizeof(type)))

#define TCP 0
#define UDP 1

#define OPEN_TCP TCP
#define OPEN_UDP UDP
#define TRANSFER_PORT 2
#define CLOSE 3
#define TCP_CONNECTED 4
#define TCP_CLOSED 5
#define CLOSE_CTRL 6

struct control_packet {
	uint8_t action;
	uint32_t seq;
	uint32_t value;
};
