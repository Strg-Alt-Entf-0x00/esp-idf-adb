#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_adb.h"
#include "esp_err.h"
#include "psa/crypto.h"

#define ADB_PACKET_HEADER_SIZE 24
#define ADB_RSA_KEY_BYTES 256
#define ADB_RSA_WORDS 64
#define ADB_PUBLIC_KEY_BYTES 524
#define ADB_PUBLIC_KEY_TEXT_MAX 1024
#define ADB_AUTH_TOKEN_BYTES 20
#define ADB_PROTOCOL_VERSION 0x01000001
#define ADB_LOCAL_VERSION_STRING "host::"

#define ADB_CMD_SYNC 0x434e5953
#define ADB_CMD_CNXN 0x4e584e43
#define ADB_CMD_AUTH 0x48545541
#define ADB_CMD_OPEN 0x4e45504f
#define ADB_CMD_OKAY 0x59414b4f
#define ADB_CMD_CLSE 0x45534c43
#define ADB_CMD_WRTE 0x45545257

#define ADB_AUTH_TOKEN 1
#define ADB_AUTH_SIGNATURE 2
#define ADB_AUTH_RSAPUBLICKEY 3

typedef struct {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_checksum;
    uint32_t magic;
} adb_packet_header_t;

typedef struct {
    psa_key_id_t key_id;
    char identity[ESP_ADB_IDENTITY_MAX_LENGTH];
    bool initialized;
} adb_auth_t;

esp_err_t adb_protocol_encode_header(
    const adb_packet_header_t *header,
    uint8_t output[ADB_PACKET_HEADER_SIZE]);
esp_err_t adb_protocol_decode_header(
    const uint8_t input[ADB_PACKET_HEADER_SIZE],
    adb_packet_header_t *header);
bool adb_packet_is_stale_close(
    const adb_packet_header_t *header,
    uint32_t local_id);

esp_err_t adb_auth_init(
    adb_auth_t *auth,
    const esp_adb_config_t *config,
    const char *identity);
void adb_auth_deinit(adb_auth_t *auth);
esp_err_t adb_auth_sign(
    adb_auth_t *auth,
    const uint8_t *token,
    size_t token_length,
    uint8_t signature[ADB_RSA_KEY_BYTES],
    size_t *signature_length);
esp_err_t adb_auth_get_public_key(
    adb_auth_t *auth,
    uint8_t *output,
    size_t output_size,
    size_t *output_length);
