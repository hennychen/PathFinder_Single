#include "obd_protocol.h"

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

esp_err_t obd_protocol_parse_packet(const uint8_t *packet, size_t packet_len, obd_packet_t *out_packet)
{
    if (packet == NULL || out_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (packet_len != OBD_PACKET_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    out_packet->speed_kmh = read_le16(&packet[0]);
    out_packet->rpm = read_le16(&packet[2]);
    out_packet->coolant_temp_c = (int8_t)packet[4];
    out_packet->throttle_percent = packet[5];
    out_packet->fuel_percent = packet[6];
    out_packet->connected = (packet[7] & OBD_PACKET_FLAG_CONNECTED) != 0U;
    return ESP_OK;
}

esp_err_t obd_protocol_encode_packet(const obd_packet_t *packet,
                                     uint8_t *out_buffer,
                                     size_t out_capacity,
                                     size_t *out_packet_len)
{
    if (packet == NULL || out_buffer == NULL || out_packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_capacity < OBD_PACKET_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    write_le16(&out_buffer[0], packet->speed_kmh);
    write_le16(&out_buffer[2], packet->rpm);
    out_buffer[4] = (uint8_t)packet->coolant_temp_c;
    out_buffer[5] = packet->throttle_percent;
    out_buffer[6] = packet->fuel_percent;
    out_buffer[7] = packet->connected ? OBD_PACKET_FLAG_CONNECTED : 0U;
    *out_packet_len = OBD_PACKET_SIZE;
    return ESP_OK;
}
