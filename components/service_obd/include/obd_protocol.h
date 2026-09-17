#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    OBD_PACKET_SIZE = 8,
    OBD_PACKET_FLAG_CONNECTED = 1U << 0,
};

typedef struct {
    bool connected;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t throttle_percent;
    uint8_t fuel_percent;
} obd_packet_t;

esp_err_t obd_protocol_parse_packet(const uint8_t *packet, size_t packet_len, obd_packet_t *out_packet);
esp_err_t obd_protocol_encode_packet(const obd_packet_t *packet,
                                     uint8_t *out_buffer,
                                     size_t out_capacity,
                                     size_t *out_packet_len);
