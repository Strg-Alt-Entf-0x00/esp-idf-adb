#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ADB_DEFAULT_PORT 5555
#define ESP_ADB_DEFAULT_TIMEOUT_MS 5000
#define ESP_ADB_DEFAULT_MAX_PAYLOAD_SIZE 4096
#define ESP_ADB_MAX_PAYLOAD_SIZE_LIMIT (1024 * 1024)
#define ESP_ADB_IDENTITY_MAX_LENGTH 64

typedef struct esp_adb_client *esp_adb_client_handle_t;

/* Key callbacks read/store a DER-encoded RSA private key. */
typedef esp_err_t (*esp_adb_key_load_fn)(
    void *context,
    uint8_t *key_buffer,
    size_t key_buffer_size,
    size_t *key_length);

typedef esp_err_t (*esp_adb_key_store_fn)(
    void *context,
    const uint8_t *key,
    size_t key_length);

typedef esp_err_t (*esp_adb_shell_stream_fn)(
    void *context,
    const uint8_t *data,
    size_t data_length);

typedef struct {
    uint32_t connect_timeout_ms;
    uint32_t io_timeout_ms;
    size_t max_payload_size;
    const char *identity;
    esp_adb_key_load_fn load_private_key;
    esp_adb_key_store_fn store_private_key;
    void *key_store_context;
} esp_adb_config_t;

#define ESP_ADB_CONFIG_DEFAULT() \
    { ESP_ADB_DEFAULT_TIMEOUT_MS, ESP_ADB_DEFAULT_TIMEOUT_MS, \
      ESP_ADB_DEFAULT_MAX_PAYLOAD_SIZE, "esp-idf-adb", NULL, NULL, NULL }

/**
 * Create a client and load or generate its RSA identity.
 *
 * Provide both key callbacks to persist an identity across reboots. If neither
 * is provided, a new in-memory identity is generated for each client instance.
 * The identity label may contain ASCII letters, digits, '.', '_' and '-'.
 */
esp_err_t esp_adb_client_create(
    const esp_adb_config_t *config,
    esp_adb_client_handle_t *out_client);

/* Stop all other calls using this handle before destroying it. */
esp_err_t esp_adb_client_destroy(esp_adb_client_handle_t client);

/**
 * Connect to an ADB TCP endpoint and complete classic ADB RSA authentication.
 * The target must have network ADB enabled and authorize this client identity.
 */
esp_err_t esp_adb_connect(
    esp_adb_client_handle_t client,
    const char *host,
    uint16_t port);

esp_err_t esp_adb_disconnect(esp_adb_client_handle_t client);
esp_err_t esp_adb_is_connected(
    esp_adb_client_handle_t client,
    bool *connected);

/**
 * Execute a raw Android shell command.
 *
 * The command is sent as an ADB shell service string. Callers must not pass
 * untrusted text without applying application-appropriate shell escaping.
 * Output is NUL-terminated when response_size is nonzero. If output exceeds
 * the buffer, the stream is drained and ESP_ERR_INVALID_SIZE is returned.
 * response_length receives the full number of output bytes, including when
 * output is discarded or truncated.
 */
esp_err_t esp_adb_shell(
    esp_adb_client_handle_t client,
    const char *command,
    char *response,
    size_t response_size,
    size_t *response_length);

/**
 * Execute a raw shell command and deliver response chunks to a callback.
 *
 * The callback runs synchronously while the ADB client is locked and must not
 * call back into this client. If the callback returns an error, the remaining
 * stream is drained to preserve the ADB connection, then that error is returned.
 */
esp_err_t esp_adb_shell_stream(
    esp_adb_client_handle_t client,
    const char *command,
    esp_adb_shell_stream_fn on_data,
    void *context,
    size_t *response_length);

/**
 * Send a numeric Android key event without constructing a shell command in
 * application code.
 */
esp_err_t esp_adb_send_key(
    esp_adb_client_handle_t client,
    uint16_t keycode);

#ifdef __cplusplus
}
#endif
