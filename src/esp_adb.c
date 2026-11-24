#include "adb_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/platform_util.h"
#include "unistd.h"

static const char *TAG = "esp_adb";

struct esp_adb_client {
    int socket_fd;
    bool connected;
    uint32_t next_local_id;
    uint32_t connect_timeout_ms;
    uint32_t io_timeout_ms;
    size_t max_payload_size;
    char identity[ESP_ADB_IDENTITY_MAX_LENGTH];
    uint8_t *payload_buffer;
    SemaphoreHandle_t mutex;
    adb_auth_t auth;
    esp_adb_config_t config;
};

static uint32_t checksum_bytes(const uint8_t *data, size_t length)
{
    uint32_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum += data[i];
    }
    return checksum;
}

static esp_err_t send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent_total = 0;
    while (sent_total < length) {
        int sent = send(socket_fd, data + sent_total, length - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t receive_all(int socket_fd, uint8_t *data, size_t length)
{
    size_t received_total = 0;
    while (received_total < length) {
        int received = recv(socket_fd, data + received_total, length - received_total, 0);
        if (received > 0) {
            received_total += (size_t)received;
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return ESP_ERR_TIMEOUT;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_packet(
    esp_adb_client_handle_t client,
    uint32_t command,
    uint32_t arg0,
    uint32_t arg1,
    const uint8_t *payload,
    size_t payload_length)
{
    if (payload_length > client->max_payload_size
        || (payload_length != 0 && payload == NULL)) {
        return ESP_ERR_INVALID_SIZE;
    }

    adb_packet_header_t header = {
        .command = command,
        .arg0 = arg0,
        .arg1 = arg1,
        .data_length = (uint32_t)payload_length,
        .data_checksum = 0,
    };
    uint8_t encoded_header[ADB_PACKET_HEADER_SIZE];
    esp_err_t err = adb_protocol_encode_header(&header, encoded_header);
    if (err != ESP_OK) {
        return err;
    }

    err = send_all(client->socket_fd, encoded_header, sizeof(encoded_header));
    if (err == ESP_OK && payload_length != 0) {
        err = send_all(client->socket_fd, payload, payload_length);
    }
    return err;
}

static esp_err_t receive_packet(
    esp_adb_client_handle_t client,
    adb_packet_header_t *header)
{
    uint8_t encoded_header[ADB_PACKET_HEADER_SIZE];
    esp_err_t err = receive_all(client->socket_fd, encoded_header, sizeof(encoded_header));
    if (err != ESP_OK) {
        return err;
    }

    err = adb_protocol_decode_header(encoded_header, header);
    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Invalid ADB packet header: command bytes %02x %02x %02x %02x, "
            "magic bytes %02x %02x %02x %02x",
            encoded_header[0],
            encoded_header[1],
            encoded_header[2],
            encoded_header[3],
            encoded_header[20],
            encoded_header[21],
            encoded_header[22],
            encoded_header[23]);
        return err;
    }
    if (header->data_length > client->max_payload_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->data_length == 0) {
        return ESP_OK;
    }

    err = receive_all(client->socket_fd, client->payload_buffer, header->data_length);
    if (err != ESP_OK) {
        return err;
    }

    if (header->data_checksum != 0
        && header->data_checksum != checksum_bytes(
            client->payload_buffer,
            header->data_length)) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static void close_connection(esp_adb_client_handle_t client)
{
    if (client->socket_fd >= 0) {
        shutdown(client->socket_fd, SHUT_RDWR);
        close(client->socket_fd);
        client->socket_fd = -1;
    }
    client->connected = false;
}

static esp_err_t connect_socket(
    esp_adb_client_handle_t client,
    const struct addrinfo *address)
{
    int socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd < 0) {
        return ESP_FAIL;
    }

    int original_flags = fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        close(socket_fd);
        return ESP_FAIL;
    }

    int result = connect(socket_fd, address->ai_addr, address->ai_addrlen);
    if (result < 0 && errno != EINPROGRESS) {
        close(socket_fd);
        return ESP_FAIL;
    }

    if (result < 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);
        struct timeval timeout = {
            .tv_sec = (long)(client->connect_timeout_ms / 1000),
            .tv_usec = (long)((client->connect_timeout_ms % 1000) * 1000),
        };
        result = select(socket_fd + 1, NULL, &write_set, NULL, &timeout);
        if (result <= 0) {
            close(socket_fd);
            return result == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (getsockopt(
                socket_fd,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length) < 0
            || socket_error != 0) {
            close(socket_fd);
            return ESP_FAIL;
        }
    }

    if (fcntl(socket_fd, F_SETFL, original_flags) < 0) {
        close(socket_fd);
        return ESP_FAIL;
    }

    struct timeval io_timeout = {
        .tv_sec = (long)(client->io_timeout_ms / 1000),
        .tv_usec = (long)((client->io_timeout_ms % 1000) * 1000),
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) < 0
        || setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) < 0) {
        close(socket_fd);
        return ESP_FAIL;
    }

    client->socket_fd = socket_fd;
    return ESP_OK;
}

static esp_err_t send_public_key(esp_adb_client_handle_t client)
{
    uint8_t public_key[ADB_PUBLIC_KEY_TEXT_MAX];
    size_t public_key_length = sizeof(public_key);
    esp_err_t err = adb_auth_get_public_key(
        &client->auth,
        public_key,
        sizeof(public_key),
        &public_key_length);
    if (err == ESP_OK) {
        err = send_packet(
            client,
            ADB_CMD_AUTH,
            ADB_AUTH_RSAPUBLICKEY,
            0,
            public_key,
            public_key_length);
    }
    mbedtls_platform_zeroize(public_key, sizeof(public_key));
    return err;
}

static esp_err_t authenticate(esp_adb_client_handle_t client)
{
    static const uint8_t version_string[] = ADB_LOCAL_VERSION_STRING;
    esp_err_t err = send_packet(
        client,
        ADB_CMD_CNXN,
        ADB_PROTOCOL_VERSION,
        (uint32_t)client->max_payload_size,
        version_string,
        sizeof(version_string));
    if (err != ESP_OK) {
        return err;
    }

    bool signature_sent = false;
    bool public_key_sent = false;
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
        adb_packet_header_t header = {0};
        err = receive_packet(client, &header);
        if (err != ESP_OK) {
            return err;
        }
        if (header.command == ADB_CMD_CNXN) {
            client->connected = true;
            return ESP_OK;
        }
        if (header.command != ADB_CMD_AUTH || header.arg0 != ADB_AUTH_TOKEN
            || header.data_length != ADB_AUTH_TOKEN_BYTES) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (!signature_sent) {
            uint8_t signature[ADB_RSA_KEY_BYTES];
            size_t signature_length = sizeof(signature);
            err = adb_auth_sign(
                &client->auth,
                client->payload_buffer,
                header.data_length,
                signature,
                &signature_length);
            if (err == ESP_OK) {
                err = send_packet(
                    client,
                    ADB_CMD_AUTH,
                    ADB_AUTH_SIGNATURE,
                    0,
                    signature,
                    signature_length);
            }
            mbedtls_platform_zeroize(signature, sizeof(signature));
            if (err != ESP_OK) {
                return err;
            }
            signature_sent = true;
        } else if (!public_key_sent) {
            err = send_public_key(client);
            if (err != ESP_OK) {
                return err;
            }
            public_key_sent = true;
        } else {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t esp_adb_client_create(
    const esp_adb_config_t *config,
    esp_adb_client_handle_t *out_client)
{
    if (config == NULL || out_client == NULL
        || config->connect_timeout_ms == 0
        || config->io_timeout_ms == 0
        || config->max_payload_size < ADB_PUBLIC_KEY_TEXT_MAX
        || config->max_payload_size > ESP_ADB_MAX_PAYLOAD_SIZE_LIMIT
        || ((config->load_private_key == NULL) != (config->store_private_key == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_client = NULL;

    const char *identity = config->identity != NULL ? config->identity : "esp-idf-adb";
    size_t identity_length = strnlen(identity, ESP_ADB_IDENTITY_MAX_LENGTH);
    if (identity_length == 0 || identity_length >= ESP_ADB_IDENTITY_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < identity_length; ++i) {
        char character = identity[i];
        bool allowed = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_' || character == '-';
        if (!allowed) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_adb_client_handle_t client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    client->socket_fd = -1;
    client->connect_timeout_ms = config->connect_timeout_ms;
    client->io_timeout_ms = config->io_timeout_ms;
    client->max_payload_size = config->max_payload_size;
    client->next_local_id = 1;
    client->config = *config;
    memcpy(client->identity, identity, identity_length + 1);
    client->config.identity = client->identity;

    client->mutex = xSemaphoreCreateMutex();
    client->payload_buffer = malloc(client->max_payload_size);
    if (client->mutex == NULL || client->payload_buffer == NULL) {
        if (client->mutex != NULL) {
            vSemaphoreDelete(client->mutex);
        }
        free(client->payload_buffer);
        free(client);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = adb_auth_init(&client->auth, &client->config, client->identity);
    if (err != ESP_OK) {
        vSemaphoreDelete(client->mutex);
        free(client->payload_buffer);
        free(client);
        return err;
    }

    *out_client = client;
    return ESP_OK;
}

esp_err_t esp_adb_client_destroy(esp_adb_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(client->mutex, portMAX_DELAY);
    close_connection(client);
    xSemaphoreGive(client->mutex);
    adb_auth_deinit(&client->auth);
    vSemaphoreDelete(client->mutex);
    mbedtls_platform_zeroize(client->payload_buffer, client->max_payload_size);
    free(client->payload_buffer);
    free(client);
    return ESP_OK;
}

esp_err_t esp_adb_connect(
    esp_adb_client_handle_t client,
    const char *host,
    uint16_t port)
{
    if (client == NULL || host == NULL || host[0] == '\0' || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(client->mutex, portMAX_DELAY);
    close_connection(client);

    char port_string[6];
    int port_length = snprintf(port_string, sizeof(port_string), "%u", (unsigned int)port);
    if (port_length <= 0 || (size_t)port_length >= sizeof(port_string)) {
        xSemaphoreGive(client->mutex);
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    struct addrinfo *addresses = NULL;
    int result = getaddrinfo(host, port_string, &hints, &addresses);
    if (result != 0 || addresses == NULL) {
        xSemaphoreGive(client->mutex);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ESP_FAIL;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        err = connect_socket(client, address);
        if (err == ESP_OK) {
            break;
        }
    }
    freeaddrinfo(addresses);

    if (err == ESP_OK) {
        err = authenticate(client);
    }
    if (err != ESP_OK) {
        close_connection(client);
    }
    xSemaphoreGive(client->mutex);
    return err;
}

esp_err_t esp_adb_disconnect(esp_adb_client_handle_t client)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(client->mutex, portMAX_DELAY);
    close_connection(client);
    xSemaphoreGive(client->mutex);
    return ESP_OK;
}

esp_err_t esp_adb_is_connected(
    esp_adb_client_handle_t client,
    bool *connected)
{
    if (client == NULL || connected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(client->mutex, portMAX_DELAY);
    *connected = client->connected;
    xSemaphoreGive(client->mutex);
    return ESP_OK;
}

static esp_err_t adb_shell_internal(
    esp_adb_client_handle_t client,
    const char *command,
    char *response,
    size_t response_size,
    size_t *response_length,
    esp_adb_shell_stream_fn on_data,
    void *context)
{
    if (client == NULL || command == NULL || command[0] == '\0'
        || (response == NULL && response_size != 0)
        || (on_data != NULL && response_size != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response_length != NULL) {
        *response_length = 0;
    }
    if (response_size != 0) {
        response[0] = '\0';
    }

    size_t command_length = strlen(command);
    if (command_length > client->max_payload_size - sizeof("shell:")) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(client->mutex, portMAX_DELAY);
    if (!client->connected) {
        xSemaphoreGive(client->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *service = malloc(command_length + sizeof("shell:"));
    if (service == NULL) {
        xSemaphoreGive(client->mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(service, "shell:", sizeof("shell:") - 1);
    memcpy(service + sizeof("shell:") - 1, command, command_length);
    service[command_length + sizeof("shell:") - 1] = '\0';

    uint32_t local_id = client->next_local_id++;
    if (local_id == 0) {
        local_id = client->next_local_id++;
    }

    esp_err_t err = send_packet(
        client,
        ADB_CMD_OPEN,
        local_id,
        0,
        service,
        command_length + sizeof("shell:"));
    free(service);
    if (err != ESP_OK) {
        close_connection(client);
        xSemaphoreGive(client->mutex);
        return err;
    }

    adb_packet_header_t header = {0};
    for (;;) {
        err = receive_packet(client, &header);
        if (err != ESP_OK) {
            close_connection(client);
            xSemaphoreGive(client->mutex);
            return err;
        }
        if (adb_packet_is_stale_close(&header, local_id)) {
            ESP_LOGD(
                TAG,
                "Ignoring CLOSE for inactive stream during OPEN: "
                "remote_id=0x%08" PRIx32 " local_id=0x%08" PRIx32,
                header.arg0,
                header.arg1);
            continue;
        }
        if (header.command == ADB_CMD_CLSE) {
            xSemaphoreGive(client->mutex);
            return ESP_ERR_NOT_FOUND;
        }
        break;
    }
    if (header.command != ADB_CMD_OKAY || header.arg1 != local_id || header.arg0 == 0) {
        ESP_LOGE(
            TAG,
            "Unexpected shell OPEN response: command=0x%08" PRIx32
            " arg0=0x%08" PRIx32 " arg1=0x%08" PRIx32
            " expected local_id=0x%08" PRIx32,
            header.command,
            header.arg0,
            header.arg1,
            local_id);
        close_connection(client);
        xSemaphoreGive(client->mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t remote_id = header.arg0;
    size_t total_length = 0;
    size_t copied_length = 0;
    bool overflow = false;
    esp_err_t callback_error = ESP_OK;

    for (;;) {
        err = receive_packet(client, &header);
        if (err != ESP_OK) {
            break;
        }
        if (adb_packet_is_stale_close(&header, local_id)) {
            ESP_LOGD(
                TAG,
                "Ignoring CLOSE for inactive stream: "
                "remote_id=0x%08" PRIx32 " local_id=0x%08" PRIx32,
                header.arg0,
                header.arg1);
            continue;
        }
        if (header.arg0 != remote_id || header.arg1 != local_id) {
            ESP_LOGE(
                TAG,
                "Shell stream ID mismatch: command=0x%08" PRIx32
                " arg0=0x%08" PRIx32 " arg1=0x%08" PRIx32
                " expected remote_id=0x%08" PRIx32
                " local_id=0x%08" PRIx32,
                header.command,
                header.arg0,
                header.arg1,
                remote_id,
                local_id);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (header.command == ADB_CMD_WRTE) {
            if (header.data_length > SIZE_MAX - total_length) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            total_length += header.data_length;
            if (on_data != NULL && callback_error == ESP_OK && header.data_length != 0) {
                callback_error = on_data(context, client->payload_buffer, header.data_length);
            }
            if (response_size != 0 && copied_length < response_size - 1) {
                size_t available = response_size - 1 - copied_length;
                size_t copied = header.data_length < available
                    ? header.data_length
                    : available;
                memcpy(response + copied_length, client->payload_buffer, copied);
                copied_length += copied;
                if (copied != header.data_length) {
                    overflow = true;
                }
                response[copied_length] = '\0';
            } else if (response != NULL && response_size != 0 && header.data_length != 0) {
                overflow = true;
            }
            err = send_packet(client, ADB_CMD_OKAY, local_id, remote_id, NULL, 0);
            if (err != ESP_OK) {
                break;
            }
        } else if (header.command == ADB_CMD_CLSE) {
            if (response_length != NULL) {
                *response_length = total_length;
            }
            break;
        } else {
            ESP_LOGE(
                TAG,
                "Unexpected shell stream packet: command=0x%08" PRIx32
                " arg0=0x%08" PRIx32 " arg1=0x%08" PRIx32,
                header.command,
                header.arg0,
                header.arg1);
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }

    if (err != ESP_OK || (header.command != ADB_CMD_CLSE)) {
        close_connection(client);
    }
    xSemaphoreGive(client->mutex);
    if (err == ESP_OK && overflow) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && callback_error != ESP_OK) {
        return callback_error;
    }
    return err;
}

esp_err_t esp_adb_shell(
    esp_adb_client_handle_t client,
    const char *command,
    char *response,
    size_t response_size,
    size_t *response_length)
{
    return adb_shell_internal(
        client,
        command,
        response,
        response_size,
        response_length,
        NULL,
        NULL);
}

esp_err_t esp_adb_shell_stream(
    esp_adb_client_handle_t client,
    const char *command,
    esp_adb_shell_stream_fn on_data,
    void *context,
    size_t *response_length)
{
    if (on_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return adb_shell_internal(
        client,
        command,
        NULL,
        0,
        response_length,
        on_data,
        context);
}

esp_err_t esp_adb_send_key(
    esp_adb_client_handle_t client,
    uint16_t keycode)
{
    if (client == NULL || keycode == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char command[32];
    int length = snprintf(
        command,
        sizeof(command),
        "input keyevent %u",
        (unsigned int)keycode);
    if (length <= 0 || (size_t)length >= sizeof(command)) {
        return ESP_FAIL;
    }
    return esp_adb_shell(client, command, NULL, 0, NULL);
}
