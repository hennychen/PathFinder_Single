#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "service_ble_protocol.h"

enum {
    NAVIGATION_PACKET_MIN_SIZE = 8,
    NAVIGATION_ROAD_NAME_MAX = 32,
    NAVIGATION_PACKET_MAX_SIZE = NAVIGATION_PACKET_MIN_SIZE + NAVIGATION_ROAD_NAME_MAX,
    NAVIGATION_FRAGMENT_MARKER = 0xFF,
    NAVIGATION_FRAGMENT_HEADER_SIZE = 5,
    NAVIGATION_FRAGMENT_PAYLOAD_MAX = NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX - NAVIGATION_FRAGMENT_HEADER_SIZE,
    NAVIGATION_FRAGMENT_COUNT_MAX =
        (NAVIGATION_PACKET_MAX_SIZE + NAVIGATION_FRAGMENT_PAYLOAD_MAX - 1) / NAVIGATION_FRAGMENT_PAYLOAD_MAX,
};

typedef enum {
    NAV_TURN_STRAIGHT = 0,
    NAV_TURN_LEFT = 1,
    NAV_TURN_RIGHT = 2,
    NAV_TURN_LEFT_FRONT = 3,
    NAV_TURN_RIGHT_FRONT = 4,
    NAV_TURN_U_TURN = 5,
    NAV_TURN_ARRIVE = 6,
} navigation_turn_type_t;

typedef struct {
    uint8_t turn_type;
    uint16_t step_distance_m;
    uint16_t total_distance_m;
    uint16_t remain_time_s;
    uint8_t road_name_len;
    char road_name[NAVIGATION_ROAD_NAME_MAX + 1];
} navigation_packet_t;

esp_err_t navigation_protocol_parse_packet(const uint8_t *packet,
                                           size_t packet_len,
                                           navigation_packet_t *out_packet);
esp_err_t navigation_protocol_encode_packet(const navigation_packet_t *packet,
                                            uint8_t *out_buffer,
                                            size_t out_capacity,
                                            size_t *out_packet_len);
esp_err_t navigation_protocol_encode_fragment_frames(
    const navigation_packet_t *packet,
    uint8_t message_id,
    uint8_t out_frames[NAVIGATION_FRAGMENT_COUNT_MAX][NAVIGATION_FRAGMENT_HEADER_SIZE + NAVIGATION_FRAGMENT_PAYLOAD_MAX],
    size_t out_frame_lengths[NAVIGATION_FRAGMENT_COUNT_MAX],
    size_t *out_frame_count);
esp_err_t navigation_protocol_set_road_name(navigation_packet_t *packet, const char *road_name);
const char *navigation_protocol_turn_to_string(uint8_t turn_type);
