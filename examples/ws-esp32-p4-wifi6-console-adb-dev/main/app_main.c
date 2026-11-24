#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_adb.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT pdMS_TO_TICKS(30000)
#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_PASSWORD_MAX_LENGTH 64
#define ADB_RESPONSE_SIZE 512
#define ADB_COMMAND_SIZE 512
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

static const char *TAG = "console_adb";
static EventGroupHandle_t s_wifi_events;
static unsigned int s_wifi_retries;
static bool s_wifi_configured;
static bool s_wifi_reconfiguring;
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
        if (!s_wifi_configured) {
            ESP_LOGI(TAG, "Wi-Fi ready; configure credentials with 'wifi-set <ssid>'");
            return;
        }
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi connection start failed: %s", esp_err_to_name(error));
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_wifi_reconfiguring) {
            return;
        }
        if (!s_wifi_configured) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
            return;
        }
        if (s_wifi_retries < CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY) {
            ++s_wifi_retries;
            esp_err_t error = esp_wifi_connect();
            if (error == ESP_OK) {
                ESP_LOGI(TAG, "Reconnecting to Wi-Fi (attempt %u)", s_wifi_retries);
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
        s_wifi_retries = 0;
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
            s_wifi_configured = false;
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
        s_wifi_configured = true;
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
            s_wifi_configured = false;
            return error;
        }
        ESP_LOGI(TAG, "Loaded saved Wi-Fi credentials from NVS");
    } else if (error != ESP_ERR_NVS_NOT_FOUND) {
        return error;
    } else {
        s_wifi_configured = false;
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

static int read_hidden_password(char *password, size_t password_size, size_t *length)
{
    size_t used = 0;
    bool overflow = false;
    printf("Wi-Fi password (input hidden; Enter for open network): ");
    fflush(stdout);

    for (;;) {
        int character = getchar();
        if (character == EOF || character == 3) {
            secure_zero(password, password_size);
            printf("\nPassword input cancelled.\n");
            return -1;
        }
        if (character == '\r' || character == '\n') {
            break;
        }
        if (character == '\b' || character == 127) {
            if (used != 0) {
                password[--used] = '\0';
            }
            continue;
        }
        if (used + 1 >= password_size) {
            overflow = true;
            continue;
        }
        password[used++] = (char)character;
    }
    password[used] = '\0';
    *length = used;
    printf("\n");
    if (overflow) {
        secure_zero(password, password_size);
        printf("Password is too long (maximum %u bytes).\n", (unsigned)password_size - 1);
        return -1;
    }
    return 0;
}

static int console_wifi_set(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: wifi-set <ssid>\n");
        printf("The password is entered at a hidden prompt and is not added to console history.\n");
        return 1;
    }

    size_t ssid_length = strlen(argv[1]);
    if (ssid_length == 0 || ssid_length > WIFI_SSID_MAX_LENGTH) {
        printf("SSID must be 1-%u bytes.\n", WIFI_SSID_MAX_LENGTH);
        return 1;
    }

    wifi_credentials_t credentials = {0};
    credentials.version = WIFI_CREDENTIALS_VERSION;
    credentials.ssid_length = (uint8_t)ssid_length;
    memcpy(credentials.ssid, argv[1], ssid_length);
    size_t password_length = 0;
    if (read_hidden_password(
            credentials.password,
            sizeof(credentials.password),
            &password_length) != 0) {
        secure_zero(&credentials, sizeof(credentials));
        return 1;
    }
    credentials.password_length = (uint8_t)password_length;

    bool previous_configured = s_wifi_configured;
    wifi_credentials_t previous_credentials = s_wifi_credentials;
    bool storage_touched = false;
    s_wifi_configured = false;
    s_wifi_reconfiguring = true;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_wifi_retries = CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY;
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
    s_wifi_configured = true;
    s_wifi_reconfiguring = false;
    s_wifi_retries = 0;
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
        s_wifi_configured = previous_configured;
        s_wifi_reconfiguring = false;
        s_wifi_retries = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        if (previous_configured) {
            restore_error = esp_wifi_connect();
        }
    }
    if (restore_error != ESP_OK) {
        s_wifi_reconfiguring = false;
        ESP_LOGE(TAG, "Could not restore previous Wi-Fi configuration: %s",
                 esp_err_to_name(restore_error));
    }
    printf("Could not save/apply Wi-Fi credentials; see the error above.\n");
    secure_zero(&previous_credentials, sizeof(previous_credentials));
    secure_zero(&credentials, sizeof(credentials));
    return 1;
}

static int console_wifi_forget(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: wifi-forget\n");
        return 1;
    }

    bool previous_configured = s_wifi_configured;
    wifi_credentials_t previous_credentials = s_wifi_credentials;
    s_wifi_configured = false;
    s_wifi_reconfiguring = true;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
    s_wifi_retries = CONFIG_EXAMPLE_WIFI_MAXIMUM_RETRY;
    esp_err_t error = esp_wifi_disconnect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
        print_error(error);
        s_wifi_configured = previous_configured;
        s_wifi_reconfiguring = false;
        s_wifi_retries = 0;
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
        s_wifi_configured = previous_configured;
        s_wifi_reconfiguring = false;
        s_wifi_retries = 0;
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
        s_wifi_reconfiguring = false;
        if (restore_error == ESP_OK && previous_configured) {
            restore_error = esp_wifi_connect();
        }
        if (restore_error != ESP_OK) {
            ESP_LOGE(TAG, "Could not restore Wi-Fi after credential erase failed: %s",
                     esp_err_to_name(restore_error));
        }
        s_wifi_configured = previous_configured;
        secure_zero(&previous_credentials, sizeof(previous_credentials));
        return 1;
    }
    secure_zero(&s_wifi_credentials, sizeof(s_wifi_credentials));
    s_wifi_reconfiguring = false;
    secure_zero(&previous_credentials, sizeof(previous_credentials));
    printf("Saved Wi-Fi credentials erased from NVS.\n");
    return 0;
}

static int console_wifi(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_events);
        printf("Wi-Fi: %s\n", !s_wifi_configured ? "not configured"
            : (bits & WIFI_CONNECTED_BIT) ? "connected"
            : (bits & WIFI_FAILED_BIT) ? "reconnect limit reached"
            : "connecting");
        return 0;
    }
    if (argc != 2 || strcmp(argv[1], "connect") != 0) {
        printf("Usage: wifi [status|connect]\n");
        return 1;
    }
    if (!s_wifi_configured) {
        printf("Wi-Fi is not configured. Use 'wifi-set <ssid>' first.\n");
        return 1;
    }

    EventBits_t current_bits = xEventGroupGetBits(s_wifi_events);
    if ((current_bits & WIFI_CONNECTED_BIT) != 0) {
        printf("Wi-Fi connected.\n");
        return 0;
    }
    if ((current_bits & WIFI_FAILED_BIT) != 0) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
        s_wifi_retries = 0;
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

static int console_adb_connect(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("Usage: adb-connect <host> [port]\n");
        return 1;
    }
    unsigned long port = ESP_ADB_DEFAULT_PORT;
    if (argc == 3 && parse_uint(argv[2], UINT16_MAX, &port) != 0) {
        printf("Invalid port (expected 1-%u).\n", UINT16_MAX);
        return 1;
    }

    esp_err_t error = esp_adb_connect(s_adb, argv[1], (uint16_t)port);
    if (error == ESP_OK) {
        printf("ADB connected to %s:%lu.\n", argv[1], port);
        return 0;
    }
    print_error(error);
    return 1;
}

static int console_adb_disconnect(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: adb-disconnect\n");
        return 1;
    }
    esp_err_t error = esp_adb_disconnect(s_adb);
    print_error(error);
    return error == ESP_OK ? 0 : 1;
}

static int console_adb_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("Usage: adb-status\n");
        return 1;
    }
    bool connected = false;
    esp_err_t error = esp_adb_is_connected(s_adb, &connected);
    if (error != ESP_OK) {
        print_error(error);
        return 1;
    }
    printf("ADB: %s\n", connected ? "connected" : "disconnected");
    return 0;
}

static int console_adb_key(int argc, char **argv)
{
    unsigned long keycode;
    if (argc != 2 || parse_uint(argv[1], UINT16_MAX, &keycode) != 0) {
        printf("Usage: adb-key <android-keycode>\n");
        return 1;
    }
    esp_err_t error = esp_adb_send_key(s_adb, (uint16_t)keycode);
    print_error(error);
    return error == ESP_OK ? 0 : 1;
}

static int console_adb_shell(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: adb-shell <command>\n");
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

    char response[ADB_RESPONSE_SIZE];
    size_t response_length = 0;
    esp_err_t error = esp_adb_shell(
        s_adb,
        command,
        response,
        sizeof(response),
        &response_length);
    if (error != ESP_OK) {
        print_error(error);
        if (error == ESP_ERR_INVALID_SIZE) {
            printf("Response exceeded the %u-byte example buffer.\n", (unsigned)sizeof(response) - 1);
        }
        return 1;
    }
    printf("%s", response);
    if (response_length != 0 && response[response_length < sizeof(response)
            ? response_length : sizeof(response) - 1] != '\n') {
        printf("\n");
    }
    return 0;
}

static esp_err_t console_init(void)
{
    static const esp_console_cmd_t commands[] = {
        { .command = "wifi", .help = "Show Wi-Fi status or connect: wifi [status|connect]", .func = console_wifi },
        { .command = "wifi-set", .help = "Save Wi-Fi credentials: wifi-set <ssid> (password prompted privately)", .func = console_wifi_set },
        { .command = "wifi-forget", .help = "Erase saved Wi-Fi credentials from NVS", .func = console_wifi_forget },
        { .command = "adb-connect", .help = "Connect: adb-connect <host> [port]", .func = console_adb_connect },
        { .command = "adb-disconnect", .help = "Disconnect from the ADB server", .func = console_adb_disconnect },
        { .command = "adb-status", .help = "Show ADB connection status", .func = console_adb_status },
        { .command = "adb-key", .help = "Send Android key event: adb-key <keycode>", .func = console_adb_key },
        { .command = "adb-shell", .help = "Run a raw shell command: adb-shell <command>", .func = console_adb_shell },
    };
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    repl_config.task_stack_size = 12 * 1024;
    repl_config.prompt = "adb> ";

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
