#include "navigation_protocol.h"

#include <stdbool.h>
#include <string.h>

static bool is_utf8_continuation(uint8_t byte)
{
    return (byte & 0xC0U) == 0x80U;
}

static size_t utf8_safe_prefix_len(const uint8_t *data, size_t data_len)
{
    size_t offset = 0;

    while (offset < data_len) {
        const uint8_t lead = data[offset];
        size_t char_len = 0;

        if (lead <= 0x7FU) {
            char_len = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            char_len = 2;
            if ((offset + char_len) > data_len || !is_utf8_continuation(data[offset + 1])) {
                break;
            }
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            char_len = 3;
            if ((offset + char_len) > data_len ||
                !is_utf8_continuation(data[offset + 1]) ||
                !is_utf8_continuation(data[offset + 2])) {
                break;
            }
            if ((lead == 0xE0U && data[offset + 1] < 0xA0U) ||
                (lead == 0xEDU && data[offset + 1] >= 0xA0U)) {
                break;
            }
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            char_len = 4;
            if ((offset + char_len) > data_len ||
                !is_utf8_continuation(data[offset + 1]) ||
                !is_utf8_continuation(data[offset + 2]) ||
                !is_utf8_continuation(data[offset + 3])) {
                break;
            }
            if ((lead == 0xF0U && data[offset + 1] < 0x90U) ||
                (lead == 0xF4U && data[offset + 1] >= 0x90U)) {
                break;
            }
        } else {
            break;
        }

        offset += char_len;
    }

    return offset;
}

static size_t copy_road_name_prefix(char *out_buffer,
                                    size_t out_capacity,
                                    const uint8_t *data,
                                    size_t data_len)
{
    size_t bounded_len = data_len;
    size_t copy_len = 0;

    if (out_buffer == NULL || out_capacity == 0U) {
        return 0;
    }

    if (bounded_len > (out_capacity - 1U)) {
        bounded_len = out_capacity - 1U;
    }
    copy_len = utf8_safe_prefix_len(data, bounded_len);

    if (copy_len > 0U) {
        memcpy(out_buffer, data, copy_len);
    }
    out_buffer[copy_len] = '\0';
    return copy_len;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

esp_err_t navigation_protocol_parse_packet(const uint8_t *packet,
                                           size_t packet_len,
                                           navigation_packet_t *out_packet)
{
    if (packet == NULL || out_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len < NAVIGATION_PACKET_MIN_SIZE || packet_len > NAVIGATION_PACKET_MAX_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t road_name_len = packet[7];
    if (packet[0] > NAV_TURN_ARRIVE || road_name_len > NAVIGATION_ROAD_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len != (size_t)NAVIGATION_PACKET_MIN_SIZE + road_name_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out_packet, 0, sizeof(*out_packet));
    out_packet->turn_type = packet[0];
    out_packet->step_distance_m = read_le16(&packet[1]);
    out_packet->total_distance_m = read_le16(&packet[3]);
    out_packet->remain_time_s = read_le16(&packet[5]);
    out_packet->road_name_len = (uint8_t)copy_road_name_prefix(out_packet->road_name,
                                                               sizeof(out_packet->road_name),
                                                               &packet[8],
                                                               road_name_len);
    return ESP_OK;
}

esp_err_t navigation_protocol_encode_packet(const navigation_packet_t *packet,
                                            uint8_t *out_buffer,
                                            size_t out_capacity,
                                            size_t *out_packet_len)
{
    if (packet == NULL || out_buffer == NULL || out_packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet->turn_type > NAV_TURN_ARRIVE || packet->road_name_len > NAVIGATION_ROAD_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t road_name_len = (uint8_t)utf8_safe_prefix_len((const uint8_t *)packet->road_name, packet->road_name_len);
    const size_t packet_len = (size_t)NAVIGATION_PACKET_MIN_SIZE + road_name_len;
    if (out_capacity < packet_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_buffer[0] = packet->turn_type;
    write_le16(&out_buffer[1], packet->step_distance_m);
    write_le16(&out_buffer[3], packet->total_distance_m);
    write_le16(&out_buffer[5], packet->remain_time_s);
    out_buffer[7] = road_name_len;

    if (road_name_len > 0U) {
        memcpy(&out_buffer[8], packet->road_name, road_name_len);
    }

    *out_packet_len = packet_len;
    return ESP_OK;
}

esp_err_t navigation_protocol_encode_fragment_frames(
    const navigation_packet_t *packet,
    uint8_t message_id,
    uint8_t out_frames[NAVIGATION_FRAGMENT_COUNT_MAX][NAVIGATION_FRAGMENT_HEADER_SIZE + NAVIGATION_FRAGMENT_PAYLOAD_MAX],
    size_t out_frame_lengths[NAVIGATION_FRAGMENT_COUNT_MAX],
    size_t *out_frame_count)
{
    uint8_t encoded_packet[NAVIGATION_PACKET_MAX_SIZE] = {0};
    size_t encoded_len = 0;
    size_t fragment_count = 0;
    size_t offset = 0;
    size_t index = 0;
    esp_err_t encode_err = ESP_OK;

    if (packet == NULL || out_frames == NULL || out_frame_lengths == NULL || out_frame_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    encode_err = navigation_protocol_encode_packet(packet,
                                                   encoded_packet,
                                                   sizeof(encoded_packet),
                                                   &encoded_len);
    if (encode_err != ESP_OK) {
        return encode_err;
    }

    fragment_count = (encoded_len + NAVIGATION_FRAGMENT_PAYLOAD_MAX - 1U) / NAVIGATION_FRAGMENT_PAYLOAD_MAX;
    if (fragment_count == 0U || fragment_count > NAVIGATION_FRAGMENT_COUNT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (index = 0; index < fragment_count; ++index) {
        const size_t remaining = encoded_len - offset;
        const uint8_t chunk_len = (uint8_t)(remaining > NAVIGATION_FRAGMENT_PAYLOAD_MAX
                                                ? NAVIGATION_FRAGMENT_PAYLOAD_MAX
                                                : remaining);

        out_frames[index][0] = NAVIGATION_FRAGMENT_MARKER;
        out_frames[index][1] = message_id;
        out_frames[index][2] = (uint8_t)index;
        out_frames[index][3] = (uint8_t)fragment_count;
        out_frames[index][4] = chunk_len;
        memcpy(&out_frames[index][NAVIGATION_FRAGMENT_HEADER_SIZE], &encoded_packet[offset], chunk_len);
        out_frame_lengths[index] = (size_t)NAVIGATION_FRAGMENT_HEADER_SIZE + chunk_len;
        offset += chunk_len;
    }

    *out_frame_count = fragment_count;
    return ESP_OK;
}

esp_err_t navigation_protocol_set_road_name(navigation_packet_t *packet, const char *road_name)
{
    size_t input_len = 0;

    if (packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    packet->road_name_len = 0;
    packet->road_name[0] = '\0';
    if (road_name == NULL) {
        return ESP_OK;
    }

    while (road_name[input_len] != '\0' && input_len < NAVIGATION_ROAD_NAME_MAX) {
        input_len++;
    }
    if (road_name[input_len] != '\0' && input_len == NAVIGATION_ROAD_NAME_MAX) {
        while (road_name[input_len] != '\0') {
            input_len++;
        }
    }

    packet->road_name_len = (uint8_t)copy_road_name_prefix(packet->road_name,
                                                           sizeof(packet->road_name),
                                                           (const uint8_t *)road_name,
                                                           input_len);
    return ESP_OK;
}

const char *navigation_protocol_turn_to_string(uint8_t turn_type)
{
    switch ((navigation_turn_type_t)turn_type) {
    case NAV_TURN_STRAIGHT:
        return "straight";
    case NAV_TURN_LEFT:
        return "left";
    case NAV_TURN_RIGHT:
        return "right";
    case NAV_TURN_LEFT_FRONT:
        return "left_front";
    case NAV_TURN_RIGHT_FRONT:
        return "right_front";
    case NAV_TURN_U_TURN:
        return "u_turn";
    case NAV_TURN_ARRIVE:
        return "arrive";
    default:
        return "unknown";
    }
}
