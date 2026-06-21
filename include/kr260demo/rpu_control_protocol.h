#ifndef KR260DEMO_RPU_CONTROL_PROTOCOL_H
#define KR260DEMO_RPU_CONTROL_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KR260_RPU_MAGIC 0x4d525055u
#define KR260_RPU_VERSION 1u
#define KR260_RPU_MAX_FRAME_SIZE 256u

enum kr260demo_rpu_msg_type {
	KR260_RPU_MSG_PING = 1,
	KR260_RPU_MSG_PONG = 2,
	KR260_RPU_MSG_GET_STATUS = 3,
	KR260_RPU_MSG_STATUS = 4,
	KR260_RPU_MSG_SET_LED = 5,
	KR260_RPU_MSG_ACK = 6,
	KR260_RPU_MSG_ERROR = 7,
};

enum kr260demo_rpu_status_code {
	KR260_RPU_STATUS_OK = 0,
	KR260_RPU_STATUS_BAD_MAGIC = 1,
	KR260_RPU_STATUS_BAD_VERSION = 2,
	KR260_RPU_STATUS_BAD_LENGTH = 3,
	KR260_RPU_STATUS_BAD_TYPE = 4,
	KR260_RPU_STATUS_BAD_PAYLOAD = 5,
	KR260_RPU_STATUS_INTERNAL_ERROR = 6,
};

enum kr260demo_rpu_led_mode {
	KR260_RPU_LED_OFF = 0,
	KR260_RPU_LED_ON = 1,
	KR260_RPU_LED_TOGGLE = 2,
	KR260_RPU_LED_HEARTBEAT = 3,
};

struct kr260demo_rpu_msg_header {
	uint32_t magic;
	uint8_t version;
	uint8_t type;
	uint16_t payload_len;
	uint32_t sequence;
	uint32_t status;
} __attribute__((packed));

struct kr260demo_rpu_led_payload {
	uint8_t mode;
	uint8_t reserved[3];
} __attribute__((packed));

struct kr260demo_rpu_status_payload {
	uint32_t core_id;
	uint32_t led_mode;
	uint32_t led_on;
	uint32_t heartbeat_count;
	uint32_t uptime_ms;
	uint32_t rx_count;
	uint32_t error_count;
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif /* KR260DEMO_RPU_CONTROL_PROTOCOL_H */
