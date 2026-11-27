#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

#include "esp_adb.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT pdMS_TO_TICKS(30000)
#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_PASSWORD_MAX_LENGTH 64
#define ADB_RESPONSE_SIZE (16 * 1024)
#define ADB_COMMAND_SIZE 512
#define TV_EXPORT_MAX_BYTES (1024 * 1024)
#define TV_EXPORT_RESPONSE_SIZE 1024
#define TV_EXPORT_REQUEST_ID_LENGTH 32
#define TV_VOLUME_MAX_STEPS 10
#define WIFI_NVS_NAMESPACE "adb_wifi"
#define WIFI_NVS_KEY "credentials"
#define WIFI_CREDENTIALS_VERSION 1
#define ADB_NVS_NAMESPACE "adb_auth"
#define ADB_NVS_KEY "private_key"
#define ADB_PRIVATE_KEY_MAX_SIZE 2048

typedef struct {
    uint8_t version;
    uint8_t ssid_length;
    uint8_t password_length;
    char ssid[WIFI_SSID_MAX_LENGTH + 1];
    char password[WIFI_PASSWORD_MAX_LENGTH + 1];
} wifi_credentials_t;

static const char *TAG = "android_tv_adb";
static EventGroupHandle_t s_wifi_events;
static atomic_uint s_wifi_retries;
static atomic_bool s_wifi_configured;
static atomic_bool s_wifi_reconfiguring;
static wifi_credentials_t s_wifi_credentials;
static esp_adb_client_handle_t s_adb;

static void secure_zero(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

static esp_err_t load_adb_private_key(
    void *context,
    uint8_t *key_buffer,
    size_t key_buffer_size,
    size_t *key_length)
{
    (void)context;
    if (key_buffer == NULL || key_buffer_size == 0 || key_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *key_length = 0;

    nvs_handle_t nvs;
    esp_err_t error = nvs_open(ADB_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error != ESP_OK) {
        return error;
    }

    size_t length = key_buffer_size;
    error = nvs_get_blob(nvs, ADB_NVS_KEY, key_buffer, &length);
    nvs_close(nvs);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        secure_zero(key_buffer, key_buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (error != ESP_OK) {
        secure_zero(key_buffer, key_buffer_size);
        return error;
    }
    if (length == 0) {
        secure_zero(key_buffer, key_buffer_size);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *key_length = length;
    return ESP_OK;
}

static esp_err_t store_adb_private_key(
    void *context,
    const uint8_t *key,
    size_t key_length)
{
    (void)context;
    if (key == NULL || key_length == 0 || key_length > ADB_PRIVATE_KEY_MAX_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t error = nvs_open(ADB_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(nvs, ADB_NVS_KEY, key, key_length);
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static void print_error(esp_err_t error)
{
    if (error != ESP_OK) {
        printf("Error: %s\n", esp_err_to_name(error));
    }
}

static int parse_uint(const char *text, unsigned long maximum, unsigned long *value)
{
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text[0] == '\0' || *end != '\0' || parsed == 0 || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!atomic_load(&s_wifi_configured)) {
            ESP_LOGI(TAG, "Wi-Fi ready; configure credentials with 'wifi setup'");
            return;
        }
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connection start failed: %s", esp_err_to_name(error));
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (atomic_load(&s_wifi_reconfiguring)) {
            return;
        }
        if (!atomic_load(&s_wifi_configured)) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            return;
        }
        unsigned int retries = atomic_load(&s_wifi_retries);
        if (retries < CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY) {
            retries = atomic_fetch_add(&s_wifi_retries, 1) + 1;
            esp_err_t error = esp_wifi_connect();
            if (error == ESP_OK) {
                ESP_LOGI(TAG, "Reconnecting to Wi-Fi (attempt %u)", retries);
            } else {
                ESP_LOGW(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(error));
                xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            }
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            ESP_LOGW(TAG, "Wi-Fi reconnect limit reached; use 'wifi connect' to retry");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        atomic_store(&s_wifi_retries, 0);
        xEventGroupClearBits(s_wifi_events, WIFI_FAILED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi ready: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_storage(WIFI_STORAGE_RAM),
        TAG,
        "RAM-only Wi-Fi credential storage setup failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
        TAG,
        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
        TAG,
        "IP event registration failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode setup failed");
    nvs_handle_t nvs;
    error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (error == ESP_OK) {
        size_t length = sizeof(s_wifi_credentials);
        error = nvs_get_blob(nvs, WIFI_NVS_KEY, &s_wifi_credentials, &length);
        nvs_close(nvs);
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            atomic_store(&s_wifi_configured, false);
            return esp_wifi_start();
        }
        if (error != ESP_OK) {
            secure_zero(&s_wifi_credentials, sizeof(s_wifi_credentials));
            return error;
        }
        if (length != sizeof(s_wifi_credentials)
            || s_wifi_credentials.version != WIFI_CREDENTIALS_VERSION
            || s_wifi_credentials.ssid_length == 0
            || s_wifi_credentials.ssid_length > WIFI_SSID_MAX_LENGTH
            || s_wifi_credentials.password_length > WIFI_PASSWORD_MAX_LENGTH
            || s_wifi_credentials.ssid[s_wifi_credentials.ssid_length] != '\0'
            || s_wifi_credentials.password[s_wifi_credentials.password_length] != '\0') {
            secure_zero(&s_wifi_credentials, sizeof(s_wifi_credentials));
            return ESP_ERR_INVALID_RESPONSE;
        }
        atomic_store(&s_wifi_configured, true);
        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, s_wifi_credentials.ssid, s_wifi_credentials.ssid_length);
        memcpy(
            wifi_config.sta.password,
            s_wifi_credentials.password,
            s_wifi_credentials.password_length);
        wifi_config.sta.threshold.authmode = s_wifi_credentials.password_length == 0
            ? WIFI_AUTH_OPEN
            : WIFI_AUTH_WPA2_PSK;
        error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        secure_zero(&wifi_config, sizeof(wifi_config));
        if (error != ESP_OK) {
            secure_zero(&s_wifi_credentials, sizeof(s_wifi_credentials));
            atomic_store(&s_wifi_configured, false);
            return error;
        }
        ESP_LOGI(TAG, "Loaded saved Wi-Fi credentials from NVS");
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    } else {
        atomic_store(&s_wifi_configured, false);
    }

    return esp_wifi_start();
}

static esp_err_t save_wifi_credentials(const wifi_credentials_t *credentials)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(nvs, WIFI_NVS_KEY, credentials, sizeof(*credentials));
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static esp_err_t erase_wifi_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t error = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_erase_key(nvs, WIFI_NVS_KEY);
    if (error == ESP_OK) {
        error = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return error;
}

static esp_err_t apply_wifi_credentials(const wifi_credentials_t *credentials)
{
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, credentials->ssid, credentials->ssid_length);
    memcpy(wifi_config.sta.password, credentials->password, credentials->password_length);
    wifi_config.sta.threshold.authmode = credentials->password_length == 0
        ? WIFI_AUTH_OPEN
        : WIFI_AUTH_WPA2_PSK;
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    secure_zero(&wifi_config, sizeof(wifi_config));
    return error;
}

static esp_err_t read_console_line(
    const char *prompt,
    char *output,
    size_t capacity,
    bool hidden)
{
    if (prompt == NULL || output == NULL || capacity < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(output, 0, capacity);
    printf("%s", prompt);
    fflush(stdout);

    size_t used = 0;
    bool overflow = false;
    for (;;) {
        uint8_t character = 0;
        if (uart_read_bytes(UART_NUM_0, &character, 1, portMAX_DELAY) != 1) {
            secure_zero(output, capacity);
            return ESP_FAIL;
        }
        if (character == '\r' || character == '\n') {
            printf("\r\n");
            break;
        }
        if (character == 3) {
            printf("\r\nInput cancelled.\n");
            secure_zero(output, capacity);
            return ESP_ERR_INVALID_STATE;
        }
        if (character == '\b' || character == 127) {
            if (used != 0) {
                output[--used] = '\0';
                if (!hidden) {
                    uart_write_bytes(UART_NUM_0, "\b \b", 3);
                }
            }
            continue;
        }
        if (character < 0x20 || character == 0x7f) {
            continue;
        }
        if (used + 1 >= capacity) {
            overflow = true;
            continue;
        }
        output[used++] = (char)character;
        output[used] = '\0';
        if (!hidden) {
            uart_write_bytes(UART_NUM_0, (const char *)&character, 1);
        }
    }

    if (overflow) {
        secure_zero(output, capacity);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool valid_wifi_password(const char *password)
{
    size_t length = strlen(password);
    if (length == 0 || (length >= 8 && length <= 63)) {
        return true;
    }
    if (length != 64) {
        return false;
    }
    for (const unsigned char *character = (const unsigned char *)password;
         *character != '\0';
         ++character) {
        if (!isxdigit(*character)) {
            return false;
        }
    }
    return true;
}

static int configure_wifi_internal(const char *ssid, const char *password_direct)
{
    size_t ssid_length = strlen(ssid);
    if (ssid_length == 0 || ssid_length > WIFI_SSID_MAX_LENGTH) {
        printf("SSID must be 1-%u bytes.\n", WIFI_SSID_MAX_LENGTH);
        return 1;
    }

    wifi_credentials_t credentials = {0};
    credentials.version = WIFI_CREDENTIALS_VERSION;
    credentials.ssid_length = (uint8_t)ssid_length;
    memcpy(credentials.ssid, ssid, ssid_length);

    if (password_direct != NULL) {
        size_t pass_length = strlen(password_direct);
        if (pass_length > WIFI_PASSWORD_MAX_LENGTH) {
            secure_zero(&credentials, sizeof(credentials));
            printf("Password is too long (maximum %u characters).\n", WIFI_PASSWORD_MAX_LENGTH);
            return 1;
        }
        memcpy(credentials.password, password_direct, pass_length);
    } else {
        esp_err_t input_error = read_console_line(
            "Wi-Fi password (hidden; empty for an open network): ",
            credentials.password,
            sizeof(credentials.password),
            true);
        if (input_error != ESP_OK) {
            secure_zero(&credentials, sizeof(credentials));
            printf("Password input failed: %s\n", esp_err_to_name(input_error));
            return 1;
        }
    }

    size_t password_length = strlen(credentials.password);
    if (!valid_wifi_password(credentials.password)) {
        secure_zero(&credentials, sizeof(credentials));
        printf("Password must be empty, 8-63 characters, or a 64-digit hexadecimal PSK.\n");
        return 1;
    }
    credentials.password_length = (uint8_t)password_length;

    bool previous_configured = atomic_load(&s_wifi_configured);
    wifi_credentials_t previous_credentials = s_wifi_credentials;
    bool storage_touched = false;
    atomic_store(&s_wifi_configured, false);
    atomic_store(&s_wifi_reconfiguring, true);
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    atomic_store(&s_wifi_retries, CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY);
    esp_err_t error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        goto failed;
    }

    error = apply_wifi_credentials(&credentials);
    if (error != ESP_OK) {
        goto failed;
    }
    storage_touched = true;
    error = save_wifi_credentials(&credentials);
    if (error != ESP_OK) {
        goto failed;
    }

    memcpy(&s_wifi_credentials, &credentials, sizeof(credentials));
    atomic_store(&s_wifi_configured, true);
    atomic_store(&s_wifi_reconfiguring, false);
    atomic_store(&s_wifi_retries, 0);
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    secure_zero(&credentials, sizeof(credentials));

    error = esp_wifi_connect();
    if (error != ESP_OK) {
        xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        print_error(error);
        printf("Wi-Fi credentials were saved. Use 'wifi connect' to retry.\n");
        secure_zero(&previous_credentials, sizeof(previous_credentials));
        return 1;
    }
    printf("Wi-Fi credentials saved to NVS; connecting.\n");
    secure_zero(&previous_credentials, sizeof(previous_credentials));
    return 0;

failed:
    print_error(error);
    if (storage_touched) {
        esp_err_t storage_error = previous_configured
            ? save_wifi_credentials(&previous_credentials)
            : erase_wifi_credentials();
        if (storage_error != ESP_OK && storage_error != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Could not restore saved Wi-Fi credentials: %s",
                     esp_err_to_name(storage_error));
        }
    }
    esp_err_t restore_error = previous_configured
        ? apply_wifi_credentials(&previous_credentials)
        : apply_wifi_credentials(&(wifi_credentials_t){0});
    if (restore_error == ESP_OK) {
        atomic_store(&s_wifi_configured, previous_configured);
        atomic_store(&s_wifi_reconfiguring, false);
        atomic_store(&s_wifi_retries, 0);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        if (previous_configured) {
            restore_error = esp_wifi_connect();
        }
    }
    if (restore_error != ESP_OK) {
        atomic_store(&s_wifi_reconfiguring, false);
        ESP_LOGE(TAG, "Could not restore previous Wi-Fi configuration: %s",
                 esp_err_to_name(restore_error));
    }
    printf("Could not save/apply Wi-Fi credentials; see the error above.\n");
    secure_zero(&previous_credentials, sizeof(previous_credentials));
    secure_zero(&credentials, sizeof(credentials));
    return 1;
}

static int configure_wifi(const char *ssid)
{
    return configure_wifi_internal(ssid, NULL);
}

static int configure_wifi_direct(const char *ssid, const char *password)
{
    return configure_wifi_internal(ssid, password);
}

static int console_wifi_setup(void)
{
    char ssid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    esp_err_t input_error = read_console_line("SSID: ", ssid, sizeof(ssid), false);
    if (input_error != ESP_OK) {
        secure_zero(ssid, sizeof(ssid));
        printf("SSID input failed: %s\n", esp_err_to_name(input_error));
        return 1;
    }

    int result = configure_wifi(ssid);
    secure_zero(ssid, sizeof(ssid));
    return result;
}

static int forget_wifi_credentials(void)
{
    bool previous_configured = atomic_load(&s_wifi_configured);
    wifi_credentials_t previous_credentials = s_wifi_credentials;
    atomic_store(&s_wifi_configured, false);
    atomic_store(&s_wifi_reconfiguring, true);
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    atomic_store(&s_wifi_retries, CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY);
    esp_err_t error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        print_error(error);
        atomic_store(&s_wifi_configured, previous_configured);
        atomic_store(&s_wifi_reconfiguring, false);
        atomic_store(&s_wifi_retries, 0);
        if (previous_configured) {
            esp_err_t reconnect_error = esp_wifi_connect();
            if (reconnect_error != ESP_OK) {
                ESP_LOGE(TAG, "Could not reconnect to saved Wi-Fi: %s",
                         esp_err_to_name(reconnect_error));
            }
        }
        secure_zero(&previous_credentials, sizeof(previous_credentials));
        return 1;
    }

    error = apply_wifi_credentials(&(wifi_credentials_t){0});
    if (error != ESP_OK) {
        print_error(error);
        atomic_store(&s_wifi_configured, previous_configured);
        atomic_store(&s_wifi_reconfiguring, false);
        atomic_store(&s_wifi_retries, 0);
        if (previous_configured) {
            esp_err_t reconnect_error = apply_wifi_credentials(&previous_credentials);
            if (reconnect_error == ESP_OK) {
                reconnect_error = esp_wifi_connect();
            }
            if (reconnect_error != ESP_OK) {
                ESP_LOGE(TAG, "Could not restore saved Wi-Fi after forget failed: %s",
                         esp_err_to_name(reconnect_error));
            }
        }
        secure_zero(&previous_credentials, sizeof(previous_credentials));
        return 1;
    }
    error = erase_wifi_credentials();
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        print_error(error);
        esp_err_t restore_error = previous_configured
            ? apply_wifi_credentials(&previous_credentials)
            : ESP_OK;
        atomic_store(&s_wifi_reconfiguring, false);
        if (restore_error == ESP_OK && previous_configured) {
            restore_error = esp_wifi_connect();
        }
        if (restore_error != ESP_OK) {
            ESP_LOGE(TAG, "Could not restore Wi-Fi after credential erase failed: %s",
                     esp_err_to_name(restore_error));
        }
        atomic_store(&s_wifi_configured, previous_configured);
        secure_zero(&previous_credentials, sizeof(previous_credentials));
        return 1;
    }
    secure_zero(&s_wifi_credentials, sizeof(s_wifi_credentials));
    atomic_store(&s_wifi_reconfiguring, false);
    secure_zero(&previous_credentials, sizeof(previous_credentials));
    printf("Saved Wi-Fi credentials erased from NVS.\n");
    return 0;
}

static int console_wifi_scan(void)
{
    printf("Starting Wi-Fi scan...\n");
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t error = esp_wifi_scan_start(&scan_config, true);
    if (error != ESP_OK) {
        printf("Wi-Fi scan failed: %s\n", esp_err_to_name(error));
        return 1;
    }
    uint16_t ap_count = 0;
    error = esp_wifi_scan_get_ap_num(&ap_count);
    if (error != ESP_OK) {
        printf("Could not read Wi-Fi scan result count: %s\n", esp_err_to_name(error));
        return 1;
    }
    if (ap_count == 0) {
        printf("Wi-Fi scan complete. No networks found.\n");
        return 0;
    }
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_records == NULL) {
        printf("Out of memory for Wi-Fi scan records.\n");
        return 1;
    }
    error = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (error != ESP_OK) {
        free(ap_records);
        printf("Could not retrieve Wi-Fi scan records: %s\n", esp_err_to_name(error));
        return 1;
    }
    printf("Wi-Fi scan complete. Discovered %u APs:\n", (unsigned)ap_count);
    for (uint16_t i = 0; i < ap_count; i++) {
        const char *auth = "UNKNOWN";
        switch (ap_records[i].authmode) {
        case WIFI_AUTH_OPEN: auth = "OPEN"; break;
        case WIFI_AUTH_WEP: auth = "WEP"; break;
        case WIFI_AUTH_WPA_PSK: auth = "WPA"; break;
        case WIFI_AUTH_WPA2_PSK: auth = "WPA2"; break;
        case WIFI_AUTH_WPA_WPA2_PSK: auth = "WPA/WPA2"; break;
        case WIFI_AUTH_WPA2_ENTERPRISE: auth = "WPA2-ENT"; break;
        case WIFI_AUTH_WPA3_PSK: auth = "WPA3"; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: auth = "WPA2/WPA3"; break;
        case WIFI_AUTH_WAPI_PSK: auth = "WAPI"; break;
        default: auth = "OPEN"; break;
        }
        printf("WIFI_AP: \"%s\" RSSI: %d AUTH: %s CH: %d\n",
               (char *)ap_records[i].ssid,
               (int)ap_records[i].rssi,
               auth,
               (int)ap_records[i].primary);
    }
    free(ap_records);
    return 0;
}

static int console_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("wifi setup | status | connect | forget | scan\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_events);
        printf("Wi-Fi: %s\n", !atomic_load(&s_wifi_configured) ? "not configured"
            : (bits & WIFI_CONNECTED_BIT) ? "connected"
            : (bits & WIFI_FAILED_BIT) ? "reconnect limit reached"
            : "connecting");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        return console_wifi_scan();
    }
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        return forget_wifi_credentials();
    }
    if (argc >= 2 && strcmp(argv[1], "setup") == 0) {
        if (argc == 3 || argc == 4) {
            const char *ssid = argv[2];
            const char *pass = (argc >= 4) ? argv[3] : "";
            return configure_wifi_direct(ssid, pass);
        }
        if (argc == 2) {
            return console_wifi_setup();
        }
        printf("Usage: wifi setup [<ssid> [<password>]]\n");
        return 1;
    }
    if (argc != 2 || strcmp(argv[1], "connect") != 0) {
        printf("Usage: wifi [setup|status|connect|forget|scan]\n");
        return 1;
    }
    if (!atomic_load(&s_wifi_configured)) {
        printf("Wi-Fi is not configured. Use 'wifi setup' first.\n");
        return 1;
    }

    EventBits_t current_bits = xEventGroupGetBits(s_wifi_events);
    if ((current_bits & WIFI_CONNECTED_BIT) != 0) {
        printf("Wi-Fi connected.\n");
        return 0;
    }
    if ((current_bits & WIFI_FAILED_BIT) != 0) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        atomic_store(&s_wifi_retries, 0);
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            print_error(error);
            return 1;
        }
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        WIFI_CONNECT_TIMEOUT);
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        printf("Wi-Fi connected.\n");
        return 0;
    }
    printf("Wi-Fi connection failed or timed out.\n");
    return 1;
}

static int console_tv_connect(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("Usage: tv-connect <host> [port]\n");
        return 1;
    }
    unsigned long port = ESP_ADB_DEFAULT_PORT;
    if (argc == 3 && parse_uint(argv[2], UINT16_MAX, &port) != 0) {
        printf("Invalid port (expected 1-%u).\n", UINT16_MAX);
        return 1;
    }

    esp_err_t error = esp_adb_connect(s_adb, argv[1], (uint16_t)port);
    if (error == ESP_OK) {
        printf("Android TV connected to %s:%lu.\n", argv[1], port);
        return 0;
    }
    print_error(error);
    return 1;
}

static int console_tv_disconnect(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: tv-disconnect\n");
        return 1;
    }
    esp_err_t error = esp_adb_disconnect(s_adb);
    print_error(error);
    return error == ESP_OK ? 0 : 1;
}

static int console_tv_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: tv-status\n");
        return 1;
    }
    bool connected = false;
    esp_err_t error = esp_adb_is_connected(s_adb, &connected);
    if (error != ESP_OK) {
        print_error(error);
        return 1;
    }
    printf("Android TV ADB: %s\n", connected ? "connected" : "disconnected");
    return 0;
}

static int parse_android_key(const char *text, unsigned long *keycode)
{
    static const struct {
        const char *name;
        unsigned long keycode;
    } key_names[] = {
        { "home", 3 },
        { "back", 4 },
        { "up", 19 },
        { "down", 20 },
        { "left", 21 },
        { "right", 22 },
        { "center", 23 },
        { "ok", 23 },
        { "volume-up", 24 },
        { "volume-down", 25 },
        { "power", 26 },
        { "menu", 82 },
        { "search", 84 },
        { "play-pause", 85 },
        { "mute", 164 },
        { "assistant", 219 },
        { "voice-assist", 231 },
    };

    for (size_t index = 0; index < sizeof(key_names) / sizeof(key_names[0]); ++index) {
        if (strcasecmp(text, key_names[index].name) == 0) {
            *keycode = key_names[index].keycode;
            return 0;
        }
    }
    return parse_uint(text, UINT16_MAX, keycode);
}

static int console_tv_key(int argc, char **argv)
{
    unsigned long keycode;
    if (argc != 2 || parse_android_key(argv[1], &keycode) != 0) {
        printf("Usage: tv-key <name|android-keycode>\n");
        printf("Names: home, back, up, down, left, right, center, ok, menu,\n");
        printf("       volume-up, volume-down, mute, power, play-pause, search,\n");
        printf("       assistant, voice-assist\n");
        return 1;
    }
    esp_err_t error = esp_adb_send_key(s_adb, (uint16_t)keycode);
    print_error(error);
    if (error == ESP_OK) {
        printf("ADB shell stream closed after the key request; Android handling is not confirmed.\n");
    }
    return error == ESP_OK ? 0 : 1;
}

static int console_tv_volume(int argc, char **argv)
{
    unsigned long steps = 1;
    if (argc < 2 || argc > 3
        || (strcasecmp(argv[1], "up") != 0 && strcasecmp(argv[1], "down") != 0)
        || (argc == 3 && parse_uint(argv[2], TV_VOLUME_MAX_STEPS, &steps) != 0)) {
        printf("Usage: tv-volume <up|down> [steps 1-%u]\n", TV_VOLUME_MAX_STEPS);
        printf("Android volume changes in device-defined discrete steps.\n");
        return 1;
    }

    uint16_t keycode = strcasecmp(argv[1], "up") == 0 ? 24 : 25;
    for (unsigned long step = 0; step < steps; ++step) {
        esp_err_t error = esp_adb_send_key(s_adb, keycode);
        if (error != ESP_OK) {
            printf("Volume request stopped after %lu of %lu shell stream(s) closed.\n",
                   step,
                   steps);
            print_error(error);
            return 1;
        }
    }

    printf("ADB shell streams closed after %lu volume-%s request(s); "
           "Android handling is not confirmed.\n",
           steps,
           argv[1]);
    return 0;
}

static int run_tv_shell_command(const char *command)
{
    char *response = malloc(ADB_RESPONSE_SIZE);
    if (response == NULL) {
        printf("Error: not enough memory for the shell response buffer.\n");
        return 1;
    }

    size_t response_length = 0;
    esp_err_t error = esp_adb_shell(
        s_adb,
        command,
        response,
        ADB_RESPONSE_SIZE,
        &response_length);
    if (error != ESP_OK) {
        if (error == ESP_ERR_INVALID_SIZE && response[0] != '\0') {
            printf("%s", response);
        }
        print_error(error);
        if (error == ESP_ERR_INVALID_SIZE) {
            printf("Response exceeded the %u-byte example limit; output may be incomplete.\n",
                   (unsigned)ADB_RESPONSE_SIZE - 1);
        }
        free(response);
        return 1;
    }
    printf("%s", response);
    if (response_length != 0
        && response[response_length < ADB_RESPONSE_SIZE
            ? response_length : ADB_RESPONSE_SIZE - 1] != '\n') {
        printf("\n");
    }
    free(response);
    return 0;
}

static esp_err_t run_tv_shell_capture(
    const char *command,
    char *response,
    size_t response_size)
{
    size_t response_length = 0;
    esp_err_t error = esp_adb_shell(
        s_adb,
        command,
        response,
        response_size,
        &response_length);
    if (error != ESP_OK) {
        print_error(error);
        return error;
    }
    if (strstr(response, "ERROR:") != NULL) {
        printf("TV channel export request failed: %s\n", response);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool is_tv_export_path(
    const char *path,
    const char *request_id)
{
    static const char prefix[] = "/sdcard/Download/tv-channel-list-";
    char suffix[TV_EXPORT_REQUEST_ID_LENGTH + 6];
    int suffix_length = snprintf(suffix, sizeof(suffix), "-%s.csv", request_id);
    if (suffix_length < 0 || (size_t)suffix_length >= sizeof(suffix)) {
        return false;
    }

    size_t prefix_length = sizeof(prefix) - 1;
    size_t path_length = strlen(path);
    size_t timestamp_length = 15;
    if (path_length != prefix_length + timestamp_length + (size_t)suffix_length
        || strncmp(path, prefix, prefix_length) != 0) {
        return false;
    }

    const char *timestamp = path + prefix_length;
    for (size_t index = 0; index < 8; ++index) {
        if (!isdigit((unsigned char)timestamp[index])) {
            return false;
        }
    }
    if (timestamp[8] != '-') {
        return false;
    }
    for (size_t index = 9; index < 15; ++index) {
        if (!isdigit((unsigned char)timestamp[index])) {
            return false;
        }
    }
    return memcmp(timestamp + timestamp_length, suffix, (size_t)suffix_length) == 0;
}

static bool find_tv_export_path(
    char *response,
    const char *request_id,
    char *path,
    size_t path_size)
{
    size_t matches = 0;
    char *save = NULL;
    for (char *line = strtok_r(response, "\r\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\r\n", &save)) {
        if (!is_tv_export_path(line, request_id)) {
            continue;
        }
        size_t length = strlen(line);
        if (length >= path_size) {
            return false;
        }
        memcpy(path, line, length + 1);
        matches += 1;
    }
    return matches == 1;
}

static bool parse_tv_export_size(char *response, size_t *size)
{
    char *cursor = response;
    while (isspace((unsigned char)*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(cursor, &end, 10);
    if (errno != 0 || end == cursor || parsed == 0 || parsed > TV_EXPORT_MAX_BYTES) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end += 1;
    }
    if (*end != '\0') {
        return false;
    }
    *size = (size_t)parsed;
    return true;
}

typedef struct {
    size_t expected_size;
    size_t written_size;
} tv_export_stream_context_t;

static esp_err_t write_tv_export_chunk(
    void *context,
    const uint8_t *data,
    size_t data_length)
{
    tv_export_stream_context_t *stream = context;
    if (stream == NULL || data == NULL
        || data_length > stream->expected_size - stream->written_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int written = uart_write_bytes(
        UART_NUM_0,
        (const char *)data,
        data_length);
    if (written < 0 || (size_t)written != data_length) {
        return ESP_FAIL;
    }
    stream->written_size += data_length;
    return ESP_OK;
}

static int console_tv_export(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: tv-export\n");
        return 1;
    }

    bool connected = false;
    esp_err_t error = esp_adb_is_connected(s_adb, &connected);
    if (error != ESP_OK) {
        print_error(error);
        return 1;
    }
    if (!connected) {
        printf("Connect to the intended Android TV first with tv-connect.\n");
        return 1;
    }

    uint8_t random_bytes[TV_EXPORT_REQUEST_ID_LENGTH / 2];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    static const char hex[] = "0123456789abcdef";
    char request_id[TV_EXPORT_REQUEST_ID_LENGTH + 1];
    for (size_t index = 0; index < sizeof(random_bytes); ++index) {
        request_id[index * 2] = hex[random_bytes[index] >> 4];
        request_id[index * 2 + 1] = hex[random_bytes[index] & 0x0f];
    }
    request_id[TV_EXPORT_REQUEST_ID_LENGTH] = '\0';

    char command[ADB_COMMAND_SIZE];
    char response[TV_EXPORT_RESPONSE_SIZE] = {0};
    int command_length = snprintf(
        command,
        sizeof(command),
        "am broadcast -a de.esp32adb.companion.action.EXPORT_CHANNELS "
        "-n de.esp32adb.companion/.AdbExportReceiver --es request_id %s",
        request_id);
    if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
        printf("TV export request command is too long.\n");
        return 1;
    }
    error = run_tv_shell_capture(command, response, sizeof(response));
    if (error != ESP_OK) {
        return 1;
    }

    command_length = snprintf(
        command,
        sizeof(command),
        "ls -1 /sdcard/Download/tv-channel-list-*-%s.csv 2>/dev/null",
        request_id);
    if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
        printf("TV export lookup command is too long.\n");
        return 1;
    }

    char export_path[160] = {0};
    bool found = false;
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        memset(response, 0, sizeof(response));
        error = run_tv_shell_capture(command, response, sizeof(response));
        if (error != ESP_OK) {
            return 1;
        }
        if (find_tv_export_path(response, request_id, export_path, sizeof(export_path))) {
            found = true;
            break;
        }
        if (attempt + 1 < 10) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (!found) {
        printf("The companion export file was not found. Check Companion 0.8.0 and READ_TV_LISTINGS access.\n");
        return 1;
    }

    command_length = snprintf(command, sizeof(command), "wc -c < %s", export_path);
    if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
        printf("TV export size command is too long.\n");
        return 1;
    }
    memset(response, 0, sizeof(response));
    error = run_tv_shell_capture(command, response, sizeof(response));
    if (error != ESP_OK) {
        return 1;
    }
    size_t csv_size = 0;
    if (!parse_tv_export_size(response, &csv_size)) {
        printf("TV export is empty, invalid, or exceeds the %u-byte transfer limit.\n",
               (unsigned)TV_EXPORT_MAX_BYTES);
        return 1;
    }

    command_length = snprintf(command, sizeof(command), "cat %s", export_path);
    if (command_length < 0 || (size_t)command_length >= sizeof(command)) {
        printf("TV export read command is too long.\n");
        return 1;
    }

    esp_log_level_set("*", ESP_LOG_NONE);
    fflush(stdout);
    char marker[96];
    int marker_length = snprintf(
        marker,
        sizeof(marker),
        "\r\nTVCSV_BEGIN:%s:%lu\r\n",
        request_id,
        (unsigned long)csv_size);
    if (marker_length < 0 || (size_t)marker_length >= sizeof(marker)
        || uart_write_bytes(UART_NUM_0, marker, marker_length) != marker_length) {
        esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);
        printf("Could not start the serial CSV transfer.\n");
        return 1;
    }

    tv_export_stream_context_t stream = {
        .expected_size = csv_size,
    };
    size_t response_length = 0;
    error = esp_adb_shell_stream(
        s_adb,
        command,
        write_tv_export_chunk,
        &stream,
        &response_length);
    bool transfer_ok = error == ESP_OK
        && stream.written_size == csv_size
        && response_length == csv_size;
    if (transfer_ok) {
        marker_length = snprintf(marker, sizeof(marker), "\r\nTVCSV_END:%s\r\n", request_id);
    } else {
        marker_length = snprintf(marker, sizeof(marker), "\r\nTVCSV_ABORT:%s\r\n", request_id);
    }
    if (marker_length > 0 && (size_t)marker_length < sizeof(marker)) {
        uart_write_bytes(UART_NUM_0, marker, marker_length);
    }
    esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL);

    if (!transfer_ok) {
        if (error != ESP_OK) {
            printf("TV export transfer failed: %s\n", esp_err_to_name(error));
        } else {
            printf("TV export size mismatch: expected %lu bytes, streamed %lu.\n",
                   (unsigned long)csv_size,
                   (unsigned long)stream.written_size);
        }
        return 1;
    }
    printf("TV channel list streamed (%lu bytes).\n", (unsigned long)csv_size);
    return 0;
}

static int console_tv_app_list(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: tv-app-list\n");
        return 1;
    }
    return run_tv_shell_command("pm list packages");
}

static int valid_android_package_name(const char *package)
{
    if (package[0] == '\0') {
        return 0;
    }
    for (const char *character = package; *character != '\0'; ++character) {
        if (!((*character >= 'a' && *character <= 'z')
              || (*character >= 'A' && *character <= 'Z')
              || (*character >= '0' && *character <= '9')
              || *character == '.' || *character == '_')) {
            return 0;
        }
    }
    return 1;
}

static int console_tv_app_launch(int argc, char **argv)
{
    if (argc != 2 || !valid_android_package_name(argv[1])) {
        printf("Usage: tv-app-launch <package.name>\n");
        printf("Use 'tv-app-list' to discover installed package names.\n");
        return 1;
    }

    char command[ADB_COMMAND_SIZE];
    int length = snprintf(
        command,
        sizeof(command),
        "monkey -p %s -c android.intent.category.LAUNCHER 1",
        argv[1]);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        printf("Launch command is too long.\n");
        return 1;
    }
    int result = run_tv_shell_command(command);
    printf("The ADB shell stream cannot confirm whether Android launched the app.\n");
    return result;
}

static int console_tv_app_stop(int argc, char **argv)
{
    if (argc != 2 || !valid_android_package_name(argv[1])) {
        printf("Usage: tv-app-stop <package.name>\n");
        printf("Use 'tv-app-list' to discover installed package names.\n");
        return 1;
    }

    char command[ADB_COMMAND_SIZE];
    int length = snprintf(command, sizeof(command), "am force-stop %s", argv[1]);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        printf("Stop command is too long.\n");
        return 1;
    }
    int result = run_tv_shell_command(command);
    printf("The ADB shell stream cannot confirm whether Android stopped the app.\n");
    return result;
}

static int console_tv_device_info(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: tv-device-info\n");
        return 1;
    }
    return run_tv_shell_command(
        "getprop ro.product.manufacturer; getprop ro.product.model; "
        "getprop ro.build.version.release");
}

static int console_tv_shell(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tv-shell <command>\n");
        return 1;
    }

    char command[ADB_COMMAND_SIZE];
    size_t command_length = 0;
    for (int index = 1; index < argc; ++index) {
        size_t argument_length = strlen(argv[index]);
        size_t separator_length = index == 1 ? 0 : 1;
        if (command_length + separator_length >= sizeof(command)
            || argument_length > sizeof(command) - command_length - separator_length - 1) {
            printf("Command is too long (maximum %u bytes).\n", (unsigned)sizeof(command) - 1);
            return 1;
        }
        if (separator_length != 0) {
            command[command_length++] = ' ';
        }
        memcpy(command + command_length, argv[index], argument_length);
        command_length += argument_length;
    }
    command[command_length] = '\0';

    return run_tv_shell_command(command);
}

static esp_err_t console_init(void)
{
    static const esp_console_cmd_t commands[] = {
        { .command = "wifi", .help = "Configure Wi-Fi or show/connect/scan: wifi [setup|status|connect|forget|scan]", .func = console_wifi },
        { .command = "tv-connect", .help = "Connect to an Android TV: tv-connect <host> [port]", .func = console_tv_connect },
        { .command = "tv-disconnect", .help = "Disconnect from the Android TV", .func = console_tv_disconnect },
        { .command = "tv-status", .help = "Show Android TV ADB status", .func = console_tv_status },
        { .command = "tv-key", .help = "Request a named or numeric Android key event", .func = console_tv_key },
        { .command = "tv-volume", .help = "Request 1-10 discrete volume changes", .func = console_tv_volume },
        { .command = "tv-app-list", .help = "List installed packages reported by the Android device", .func = console_tv_app_list },
        { .command = "tv-app-launch", .help = "Attempt to launch a package by its discovered name", .func = console_tv_app_launch },
        { .command = "tv-app-stop", .help = "Request force-stop for an app package", .func = console_tv_app_stop },
        { .command = "tv-device-info", .help = "Read manufacturer, model and Android version", .func = console_tv_device_info },
        { .command = "tv-export", .help = "Read the companion channel CSV and stream it to the serial client", .func = console_tv_export },
        { .command = "tv-shell", .help = "Run a raw Android shell command", .func = console_tv_shell },
    };
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    repl_config.task_stack_size = 12 * 1024;
    repl_config.prompt = "tv> ";

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "Help registration failed");
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]); ++index) {
        ESP_RETURN_ON_ERROR(
            esp_console_cmd_register(&commands[index]),
            TAG,
            "Command registration failed");
    }
    ESP_RETURN_ON_ERROR(
        esp_console_new_repl_uart(&uart_config, &repl_config, &repl),
        TAG,
        "UART console creation failed");
    return esp_console_start_repl(repl);
}

static esp_err_t nvs_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs recovery (%s); refusing to erase saved Wi-Fi and ADB identity",
                 esp_err_to_name(error));
    }
    return error;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init());
    ESP_ERROR_CHECK(wifi_init());

    esp_adb_config_t config = ESP_ADB_CONFIG_DEFAULT();
    config.load_private_key = load_adb_private_key;
    config.store_private_key = store_adb_private_key;
    ESP_ERROR_CHECK(esp_adb_client_create(&config, &s_adb));
    ESP_ERROR_CHECK(console_init());
    ESP_LOGI(TAG, "Ready. Type 'help' to list commands.");
}
