/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _LINUX_VIRTIO_GPIO_H
#define _LINUX_VIRTIO_GPIO_H

#include "standard-headers/linux/types.h"

/* Virtio GPIO request types */
#define VIRTIO_GPIO_MSG_GET_NAMES		0x0001
#define VIRTIO_GPIO_MSG_GET_DIRECTION		0x0002
#define VIRTIO_GPIO_MSG_SET_DIRECTION		0x0003
#define VIRTIO_GPIO_MSG_GET_VALUE		0x0004
#define VIRTIO_GPIO_MSG_SET_VALUE		0x0005

/* Possible values of the status field */
#define VIRTIO_GPIO_STATUS_OK			0x0
#define VIRTIO_GPIO_STATUS_ERR			0x1

/* Direction types */
#define VIRTIO_GPIO_DIRECTION_NONE		0x00
#define VIRTIO_GPIO_DIRECTION_OUT		0x01
#define VIRTIO_GPIO_DIRECTION_IN		0x02

struct virtio_gpio_config {
	uint16_t ngpio;
	uint8_t padding[2];
	uint32_t gpio_names_size;
} QEMU_PACKED;

/* Virtio GPIO Request / Response */
struct virtio_gpio_request {
	uint16_t type;
	uint16_t gpio;
	uint32_t value;
};

struct virtio_gpio_response {
	uint8_t status;
	uint8_t value;
};

struct virtio_gpio_response_get_names {
	uint8_t status;
	uint8_t value[];
};

#endif /* _LINUX_VIRTIO_GPIO_H */
