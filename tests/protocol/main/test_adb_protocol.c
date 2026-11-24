#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "adb_internal.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "unity.h"
#include "unity_test_runner.h"

typedef struct {
    int listen_fd;
    SemaphoreHandle_t done;
    esp_err_t result;
} fake_adb_server_t;

typedef struct {
    uint8_t key[2048];
    size_t length;
    bool present;
} test_key_store_t;

typedef struct {
    char data[32];
    size_t length;
} shell_stream_capture_t;

static fake_adb_server_t s_fake_adb_server;

static bool socket_send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent_total = 0;
    while (sent_total < length) {
        int sent = send(socket_fd, data + sent_total, length - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool socket_receive_all(int socket_fd, uint8_t *data, size_t length)
{
    size_t received_total = 0;
    while (received_total < length) {
        int received = recv(socket_fd, data + received_total, length - received_total, 0);
        if (received > 0) {
            received_total += (size_t)received;
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static esp_err_t socket_receive_packet(
    int socket_fd,
    adb_packet_header_t *header,
    uint8_t *payload,
    size_t payload_capacity)
{
    uint8_t encoded_header[ADB_PACKET_HEADER_SIZE];
    if (!socket_receive_all(socket_fd, encoded_header, sizeof(encoded_header))) {
        return ESP_FAIL;
    }

    esp_err_t err = adb_protocol_decode_header(encoded_header, header);
    if (err != ESP_OK) {
        return err;
    }
    if (header->data_length > payload_capacity
        || (header->data_length != 0 && payload == NULL)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (header->data_length != 0
        && !socket_receive_all(socket_fd, payload, header->data_length)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t socket_send_packet(
    int socket_fd,
    uint32_t command,
    uint32_t arg0,
    uint32_t arg1,
    const void *payload,
    size_t payload_length)
{
    if (payload_length > UINT32_MAX || (payload_length != 0 && payload == NULL)) {
        return ESP_ERR_INVALID_ARG;
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
    if (!socket_send_all(socket_fd, encoded_header, sizeof(encoded_header))
        || (payload_length != 0
            && !socket_send_all(socket_fd, payload, payload_length))) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t fake_server_expect_open(
    int socket_fd,
    uint32_t expected_local_id,
    const char *service)
{
    adb_packet_header_t header = {0};
    uint8_t payload[64] = {0};
    esp_err_t err = socket_receive_packet(socket_fd, &header, payload, sizeof(payload));
    if (err != ESP_OK) {
        return err;
    }
    if (header.command != ADB_CMD_OPEN
        || header.arg0 != expected_local_id
        || header.arg1 != 0
        || header.data_length != strlen(service) + 1
        || memcmp(payload, service, header.data_length) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fake_server_expect_okay(
    int socket_fd,
    uint32_t local_id,
    uint32_t remote_id)
{
    adb_packet_header_t header = {0};
    esp_err_t err = socket_receive_packet(socket_fd, &header, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (header.command != ADB_CMD_OKAY
        || header.arg0 != local_id
        || header.arg1 != remote_id
        || header.data_length != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fake_adb_server_run(int socket_fd)
{
    adb_packet_header_t header = {0};
    uint8_t payload[128] = {0};
    esp_err_t err = socket_receive_packet(socket_fd, &header, payload, sizeof(payload));
    if (err != ESP_OK) {
        return err;
    }
    if (header.command != ADB_CMD_CNXN) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    static const char device_banner[] = "device::";
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_CNXN,
        ADB_PROTOCOL_VERSION,
        ESP_ADB_DEFAULT_MAX_PAYLOAD_SIZE,
        device_banner,
        sizeof(device_banner));
    if (err != ESP_OK) {
        return err;
    }

    const uint32_t first_remote_id = 100;
    const uint32_t second_remote_id = 101;
    const uint32_t stream_remote_id = 102;
    const uint32_t first_local_id = 1;
    const uint32_t second_local_id = 2;
    const uint32_t stream_local_id = 3;
    err = fake_server_expect_open(socket_fd, first_local_id, "shell:first");
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_OKAY,
        first_remote_id,
        first_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }
    static const char first_output[] = "first-output";
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_WRTE,
        first_remote_id,
        first_local_id,
        first_output,
        sizeof(first_output) - 1);
    if (err != ESP_OK) {
        return err;
    }
    err = fake_server_expect_okay(socket_fd, first_local_id, first_remote_id);
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_CLSE,
        first_remote_id,
        first_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }

    err = fake_server_expect_open(socket_fd, second_local_id, "shell:second");
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_CLSE,
        first_remote_id,
        first_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_OKAY,
        second_remote_id,
        second_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }
    static const char second_output[] = "second-output";
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_WRTE,
        second_remote_id,
        second_local_id,
        second_output,
        sizeof(second_output) - 1);
    if (err != ESP_OK) {
        return err;
    }
    err = fake_server_expect_okay(socket_fd, second_local_id, second_remote_id);
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_CLSE,
        second_remote_id,
        second_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }

    err = fake_server_expect_open(socket_fd, stream_local_id, "shell:stream");
    if (err != ESP_OK) {
        return err;
    }
    err = socket_send_packet(
        socket_fd,
        ADB_CMD_OKAY,
        stream_remote_id,
        stream_local_id,
        NULL,
        0);
    if (err != ESP_OK) {
        return err;
    }
    static const char stream_chunks[][8] = { "chunk-", "output" };
    for (size_t index = 0; index < 2; ++index) {
        err = socket_send_packet(
            socket_fd,
            ADB_CMD_WRTE,
            stream_remote_id,
            stream_local_id,
            stream_chunks[index],
            strlen(stream_chunks[index]));
        if (err != ESP_OK) {
            return err;
        }
        err = fake_server_expect_okay(socket_fd, stream_local_id, stream_remote_id);
        if (err != ESP_OK) {
            return err;
        }
    }
    return socket_send_packet(
        socket_fd,
        ADB_CMD_CLSE,
        stream_remote_id,
        stream_local_id,
        NULL,
        0);
}

static void fake_adb_server_task(void *argument)
{
    fake_adb_server_t *server = argument;
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(server->listen_fd, &read_set);
    struct timeval accept_timeout = {
        .tv_sec = 5,
        .tv_usec = 0,
    };
    int ready = select(
        server->listen_fd + 1,
        &read_set,
        NULL,
        NULL,
        &accept_timeout);
    if (ready <= 0) {
        server->result = ready == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    } else {
        int client_fd = accept(server->listen_fd, NULL, NULL);
        if (client_fd < 0) {
            server->result = ESP_FAIL;
        } else {
            struct timeval timeout = {
                .tv_sec = 3,
                .tv_usec = 0,
            };
            if (setsockopt(
                    client_fd,
                    SOL_SOCKET,
                    SO_RCVTIMEO,
                    &timeout,
                    sizeof(timeout)) < 0
                || setsockopt(
                    client_fd,
                    SOL_SOCKET,
                    SO_SNDTIMEO,
                    &timeout,
                    sizeof(timeout)) < 0) {
                server->result = ESP_FAIL;
            } else {
                server->result = fake_adb_server_run(client_fd);
            }
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
        }
    }
    close(server->listen_fd);
    server->listen_fd = -1;
    xSemaphoreGive(server->done);
    vTaskDelete(NULL);
}

static esp_err_t test_load_key(
    void *context,
    uint8_t *key_buffer,
    size_t key_buffer_size,
    size_t *key_length)
{
    test_key_store_t *store = context;
    if (!store->present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (store->length > key_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(key_buffer, store->key, store->length);
    *key_length = store->length;
    return ESP_OK;
}

static esp_err_t test_store_key(
    void *context,
    const uint8_t *key,
    size_t key_length)
{
    test_key_store_t *store = context;
    if (key_length > sizeof(store->key)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(store->key, key, key_length);
    store->length = key_length;
    store->present = true;
    return ESP_OK;
}

static esp_err_t capture_shell_stream(
    void *context,
    const uint8_t *data,
    size_t data_length)
{
    shell_stream_capture_t *capture = context;
    if (capture == NULL || data == NULL
        || capture->length >= sizeof(capture->data)
        || data_length > sizeof(capture->data) - capture->length - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(capture->data + capture->length, data, data_length);
    capture->length += data_length;
    capture->data[capture->length] = '\0';
    return ESP_OK;
}

TEST_CASE("ADB packet header is encoded little-endian", "[adb][protocol]")
{
    const adb_packet_header_t header = {
        .command = ADB_CMD_CNXN,
        .arg0 = 0x01020304,
        .arg1 = 0x11223344,
        .data_length = 0x00001000,
        .data_checksum = 0xA1B2C3D4,
    };
    const uint8_t expected[ADB_PACKET_HEADER_SIZE] = {
        0x43, 0x4E, 0x58, 0x4E,
        0x04, 0x03, 0x02, 0x01,
        0x44, 0x33, 0x22, 0x11,
        0x00, 0x10, 0x00, 0x00,
        0xD4, 0xC3, 0xB2, 0xA1,
        0xBC, 0xB1, 0xA7, 0xB1,
    };
    uint8_t encoded[ADB_PACKET_HEADER_SIZE] = {0};

    TEST_ASSERT_EQUAL(ESP_OK, adb_protocol_encode_header(&header, encoded));
    TEST_ASSERT_EQUAL_MEMORY(expected, encoded, sizeof(expected));
}

TEST_CASE("ADB packet header round-trips fields", "[adb][protocol]")
{
    const adb_packet_header_t input = {
        .command = ADB_CMD_WRTE,
        .arg0 = 17,
        .arg1 = 29,
        .data_length = 4096,
        .data_checksum = 0,
    };
    adb_packet_header_t output = {0};
    uint8_t encoded[ADB_PACKET_HEADER_SIZE] = {0};

    TEST_ASSERT_EQUAL(ESP_OK, adb_protocol_encode_header(&input, encoded));
    TEST_ASSERT_EQUAL(ESP_OK, adb_protocol_decode_header(encoded, &output));
    TEST_ASSERT_EQUAL_UINT32(input.command, output.command);
    TEST_ASSERT_EQUAL_UINT32(input.arg0, output.arg0);
    TEST_ASSERT_EQUAL_UINT32(input.arg1, output.arg1);
    TEST_ASSERT_EQUAL_UINT32(input.data_length, output.data_length);
    TEST_ASSERT_EQUAL_UINT32(input.data_checksum, output.data_checksum);
    TEST_ASSERT_EQUAL_UINT32(input.command ^ UINT32_MAX, output.magic);
}

TEST_CASE("ADB packet decoder rejects a bad magic word", "[adb][protocol]")
{
    uint8_t encoded[ADB_PACKET_HEADER_SIZE] = {0};
    adb_packet_header_t header = {
        .command = ADB_CMD_CNXN,
    };
    adb_packet_header_t decoded = {0};

    TEST_ASSERT_EQUAL(ESP_OK, adb_protocol_encode_header(&header, encoded));
    encoded[20] ^= 0x01;
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_RESPONSE,
        adb_protocol_decode_header(encoded, &decoded));
}

TEST_CASE("ADB packet helpers reject null arguments", "[adb][protocol]")
{
    uint8_t encoded[ADB_PACKET_HEADER_SIZE] = {0};

    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        adb_protocol_encode_header(NULL, encoded));
    TEST_ASSERT_EQUAL(
        ESP_ERR_INVALID_ARG,
        adb_protocol_decode_header(encoded, NULL));
}

TEST_CASE("Delayed CLOSE for an inactive stream is identified as stale", "[adb][protocol]")
{
    const adb_packet_header_t delayed_close = {
        .command = ADB_CMD_CLSE,
        .arg0 = 11,
        .arg1 = 2,
    };
    const adb_packet_header_t active_close = {
        .command = ADB_CMD_CLSE,
        .arg0 = 11,
        .arg1 = 3,
    };
    const adb_packet_header_t old_data = {
        .command = ADB_CMD_WRTE,
        .arg0 = 11,
        .arg1 = 2,
    };

    TEST_ASSERT_TRUE(adb_packet_is_stale_close(&delayed_close, 3));
    TEST_ASSERT_FALSE(adb_packet_is_stale_close(&active_close, 3));
    TEST_ASSERT_FALSE(adb_packet_is_stale_close(&old_data, 3));
    TEST_ASSERT_FALSE(adb_packet_is_stale_close(NULL, 3));
}

TEST_CASE("Delayed CLOSE does not abort buffered or callback shell streams over TCP", "[adb][integration]")
{
    TEST_ASSERT_EQUAL(ESP_OK, esp_netif_init());

    esp_adb_config_t config = ESP_ADB_CONFIG_DEFAULT();
    config.connect_timeout_ms = 3000;
    config.io_timeout_ms = 3000;
    esp_adb_client_handle_t client = NULL;
    esp_err_t client_err = esp_adb_client_create(&config, &client);
    TEST_ASSERT_EQUAL(ESP_OK, client_err);

    memset(&s_fake_adb_server, 0, sizeof(s_fake_adb_server));
    s_fake_adb_server.listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_fake_adb_server.listen_fd < 0) {
        esp_adb_client_destroy(client);
        TEST_FAIL_MESSAGE("Could not create fake ADB server socket");
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = 0,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(
            s_fake_adb_server.listen_fd,
            (const struct sockaddr *)&address,
            sizeof(address)) < 0
        || listen(s_fake_adb_server.listen_fd, 1) < 0) {
        close(s_fake_adb_server.listen_fd);
        s_fake_adb_server.listen_fd = -1;
        esp_adb_client_destroy(client);
        TEST_FAIL_MESSAGE("Could not bind fake ADB server to loopback");
    }

    socklen_t address_length = sizeof(address);
    if (getsockname(
            s_fake_adb_server.listen_fd,
            (struct sockaddr *)&address,
            &address_length) < 0) {
        close(s_fake_adb_server.listen_fd);
        s_fake_adb_server.listen_fd = -1;
        esp_adb_client_destroy(client);
        TEST_FAIL_MESSAGE("Could not get fake ADB server port");
    }

    s_fake_adb_server.done = xSemaphoreCreateBinary();
    if (s_fake_adb_server.done == NULL
        || xTaskCreate(
               fake_adb_server_task,
               "adb_fake_server",
               4096,
               &s_fake_adb_server,
               5,
               NULL) != pdPASS) {
        if (s_fake_adb_server.done != NULL) {
            vSemaphoreDelete(s_fake_adb_server.done);
            s_fake_adb_server.done = NULL;
        }
        close(s_fake_adb_server.listen_fd);
        s_fake_adb_server.listen_fd = -1;
        esp_adb_client_destroy(client);
        TEST_FAIL_MESSAGE("Could not start fake ADB server task");
    }

    char first_response[32] = {0};
    size_t first_response_length = 0;
    char second_response[32] = {0};
    size_t second_response_length = 0;
    shell_stream_capture_t stream_capture = {0};
    size_t stream_response_length = 0;
    client_err = esp_adb_connect(
        client,
        "127.0.0.1",
        ntohs(address.sin_port));
    if (client_err == ESP_OK) {
        client_err = esp_adb_shell(
            client,
            "first",
            first_response,
            sizeof(first_response),
            &first_response_length);
    }
    if (client_err == ESP_OK) {
        client_err = esp_adb_shell(
            client,
            "second",
            second_response,
            sizeof(second_response),
            &second_response_length);
    }
    if (client_err == ESP_OK) {
        client_err = esp_adb_shell_stream(
            client,
            "stream",
            capture_shell_stream,
            &stream_capture,
            &stream_response_length);
    }
    esp_adb_client_destroy(client);

    BaseType_t server_finished = xSemaphoreTake(
        s_fake_adb_server.done,
        pdMS_TO_TICKS(7000));
    if (server_finished != pdTRUE && s_fake_adb_server.listen_fd >= 0) {
        shutdown(s_fake_adb_server.listen_fd, SHUT_RDWR);
        close(s_fake_adb_server.listen_fd);
        s_fake_adb_server.listen_fd = -1;
    }
    if (server_finished == pdTRUE) {
        vSemaphoreDelete(s_fake_adb_server.done);
        s_fake_adb_server.done = NULL;
    }

    TEST_ASSERT_EQUAL(ESP_OK, client_err);
    TEST_ASSERT_EQUAL(pdTRUE, server_finished);
    TEST_ASSERT_EQUAL(ESP_OK, s_fake_adb_server.result);
    TEST_ASSERT_EQUAL_UINT32(strlen("first-output"), first_response_length);
    TEST_ASSERT_EQUAL_STRING("first-output", first_response);
    TEST_ASSERT_EQUAL_UINT32(strlen("second-output"), second_response_length);
    TEST_ASSERT_EQUAL_STRING("second-output", second_response);
    TEST_ASSERT_EQUAL_UINT32(strlen("chunk-output"), stream_response_length);
    TEST_ASSERT_EQUAL_UINT32(strlen("chunk-output"), stream_capture.length);
    TEST_ASSERT_EQUAL_STRING("chunk-output", stream_capture.data);
}

TEST_CASE("RSA identity exports an Android-compatible ADB public key", "[adb][auth]")
{
    esp_adb_config_t config = ESP_ADB_CONFIG_DEFAULT();
    static test_key_store_t store;
    memset(&store, 0, sizeof(store));
    config.load_private_key = test_load_key;
    config.store_private_key = test_store_key;
    config.key_store_context = &store;

    adb_auth_t auth = {0};
    uint8_t encoded[ADB_PUBLIC_KEY_TEXT_MAX] = {0};
    size_t encoded_length = sizeof(encoded);

    TEST_ASSERT_EQUAL(ESP_OK, adb_auth_init(&auth, &config, "unit-test"));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        adb_auth_get_public_key(&auth, encoded, sizeof(encoded), &encoded_length));

    const uint8_t *suffix = memchr(encoded, ' ', encoded_length);
    TEST_ASSERT_NOT_NULL(suffix);
    size_t base64_length = (size_t)(suffix - encoded);
    uint8_t decoded[ADB_PUBLIC_KEY_BYTES] = {0};
    size_t decoded_length = 0;
    TEST_ASSERT_EQUAL(
        0,
        mbedtls_base64_decode(
            decoded,
            sizeof(decoded),
            &decoded_length,
            encoded,
            base64_length));
    TEST_ASSERT_EQUAL_UINT32(ADB_PUBLIC_KEY_BYTES, decoded_length);
    TEST_ASSERT_EQUAL_UINT8(ADB_RSA_WORDS, decoded[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)" unit-test", suffix, 10);
    TEST_ASSERT_EQUAL_UINT8(0, suffix[10]);

    uint32_t n0 = (uint32_t)decoded[8]
        | ((uint32_t)decoded[9] << 8)
        | ((uint32_t)decoded[10] << 16)
        | ((uint32_t)decoded[11] << 24);
    uint32_t n0inv = (uint32_t)decoded[4]
        | ((uint32_t)decoded[5] << 8)
        | ((uint32_t)decoded[6] << 16)
        | ((uint32_t)decoded[7] << 24);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, n0 * n0inv);
    TEST_ASSERT_EQUAL_UINT8(1, decoded[520]);
    TEST_ASSERT_EQUAL_UINT8(0, decoded[521]);
    TEST_ASSERT_EQUAL_UINT8(1, decoded[522]);
    TEST_ASSERT_EQUAL_UINT8(0, decoded[523]);

    adb_auth_deinit(&auth);

    adb_auth_t restored_auth = {0};
    uint8_t restored[ADB_PUBLIC_KEY_TEXT_MAX] = {0};
    size_t restored_length = sizeof(restored);
    TEST_ASSERT_EQUAL(ESP_OK, adb_auth_init(&restored_auth, &config, "unit-test"));
    TEST_ASSERT_EQUAL(
        ESP_OK,
        adb_auth_get_public_key(
            &restored_auth,
            restored,
            sizeof(restored),
            &restored_length));
    TEST_ASSERT_EQUAL_UINT32(encoded_length, restored_length);
    TEST_ASSERT_EQUAL_MEMORY(encoded, restored, encoded_length);
    adb_auth_deinit(&restored_auth);
    mbedtls_platform_zeroize(&store, sizeof(store));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
