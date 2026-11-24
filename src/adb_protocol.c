#include "adb_internal.h"

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0]
        | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16)
        | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

esp_err_t adb_protocol_encode_header(
    const adb_packet_header_t *header,
    uint8_t output[ADB_PACKET_HEADER_SIZE])
{
    if (header == NULL || output == NULL || header->command == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    write_le32(&output[0], header->command);
    write_le32(&output[4], header->arg0);
    write_le32(&output[8], header->arg1);
    write_le32(&output[12], header->data_length);
    write_le32(&output[16], header->data_checksum);
    write_le32(&output[20], header->command ^ UINT32_MAX);
    return ESP_OK;
}

esp_err_t adb_protocol_decode_header(
    const uint8_t input[ADB_PACKET_HEADER_SIZE],
    adb_packet_header_t *header)
{
    if (input == NULL || header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    header->command = read_le32(&input[0]);
    header->arg0 = read_le32(&input[4]);
    header->arg1 = read_le32(&input[8]);
    header->data_length = read_le32(&input[12]);
    header->data_checksum = read_le32(&input[16]);
    header->magic = read_le32(&input[20]);

    if (header->command == 0 || header->magic != (header->command ^ UINT32_MAX)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

bool adb_packet_is_stale_close(
    const adb_packet_header_t *header,
    uint32_t local_id)
{
    return header != NULL
        && header->command == ADB_CMD_CLSE
        && header->arg1 != local_id;
}
