#include "adb_internal.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

#define ADB_RSA_BITS 2048
#define ADB_RSA_BYTES ADB_RSA_KEY_BYTES
#define ADB_RSA_PUBLIC_DER_MAX 512

typedef struct {
    const uint8_t *cursor;
    const uint8_t *end;
} der_cursor_t;

static esp_err_t psa_status_to_esp_err(psa_status_t status)
{
    if (status == PSA_ERROR_INSUFFICIENT_MEMORY) {
        return ESP_ERR_NO_MEM;
    }
    if (status == PSA_ERROR_NOT_SUPPORTED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (status == PSA_ERROR_INVALID_ARGUMENT) {
        return ESP_ERR_INVALID_ARG;
    }
    return status == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

static esp_err_t import_key(
    const uint8_t *private_key,
    size_t private_key_length,
    psa_key_id_t *key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, ADB_RSA_BITS);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(
        &attributes,
        PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1));

    psa_status_t status = psa_import_key(
        &attributes,
        private_key,
        private_key_length,
        key_id);
    psa_reset_key_attributes(&attributes);
    return psa_status_to_esp_err(status);
}

static esp_err_t generate_key(psa_key_id_t *key_id, bool exportable)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_RSA_KEY_PAIR);
    psa_set_key_bits(&attributes, ADB_RSA_BITS);
    psa_set_key_usage_flags(
        &attributes,
        PSA_KEY_USAGE_SIGN_HASH
            | (exportable ? PSA_KEY_USAGE_EXPORT : 0));
    psa_set_key_algorithm(
        &attributes,
        PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1));

    psa_status_t status = psa_generate_key(&attributes, key_id);
    psa_reset_key_attributes(&attributes);
    return psa_status_to_esp_err(status);
}

static esp_err_t validate_key(psa_key_id_t key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status = psa_get_key_attributes(key_id, &attributes);
    if (status != PSA_SUCCESS) {
        psa_reset_key_attributes(&attributes);
        return psa_status_to_esp_err(status);
    }

    bool valid = psa_get_key_type(&attributes) == PSA_KEY_TYPE_RSA_KEY_PAIR
        && psa_get_key_bits(&attributes) == ADB_RSA_BITS
        && (psa_get_key_usage_flags(&attributes) & PSA_KEY_USAGE_SIGN_HASH) != 0
        && psa_get_key_algorithm(&attributes)
            == PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1);
    psa_reset_key_attributes(&attributes);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t adb_auth_init(
    adb_auth_t *auth,
    const esp_adb_config_t *config,
    const char *identity)
{
    if (auth == NULL || config == NULL || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(auth, 0, sizeof(*auth));
    size_t identity_length = strlen(identity);
    if (identity_length == 0 || identity_length >= sizeof(auth->identity)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(auth->identity, identity, identity_length + 1);

    psa_status_t crypto_status = psa_crypto_init();
    if (crypto_status != PSA_SUCCESS) {
        return psa_status_to_esp_err(crypto_status);
    }

    bool loaded = false;
    if (config->load_private_key != NULL) {
        uint8_t private_key[2048] = {0};
        size_t private_key_length = 0;
        esp_err_t err = config->load_private_key(
            config->key_store_context,
            private_key,
            sizeof(private_key),
            &private_key_length);

        if (err == ESP_OK) {
            if (private_key_length == 0 || private_key_length > sizeof(private_key)) {
                mbedtls_platform_zeroize(private_key, sizeof(private_key));
                return ESP_ERR_INVALID_SIZE;
            }
            err = import_key(private_key, private_key_length, &auth->key_id);
            mbedtls_platform_zeroize(private_key, sizeof(private_key));
            if (err != ESP_OK) {
                return err;
            }
            loaded = true;
        } else {
            mbedtls_platform_zeroize(private_key, sizeof(private_key));
            if (err != ESP_ERR_NOT_FOUND) {
                return err;
            }
        }
    }

    if (!loaded) {
        esp_err_t err = generate_key(
            &auth->key_id,
            config->store_private_key != NULL);
        if (err != ESP_OK) {
            return err;
        }

        if (config->store_private_key != NULL) {
            uint8_t private_key[2048] = {0};
            size_t private_key_length = 0;
            psa_status_t status = psa_export_key(
                auth->key_id,
                private_key,
                sizeof(private_key),
                &private_key_length);
            if (status != PSA_SUCCESS) {
                err = psa_status_to_esp_err(status);
                mbedtls_platform_zeroize(private_key, sizeof(private_key));
                adb_auth_deinit(auth);
                return err;
            }

            psa_key_id_t signing_key_id = 0;
            err = import_key(
                private_key,
                private_key_length,
                &signing_key_id);
            if (err == ESP_OK) {
                err = config->store_private_key(
                    config->key_store_context,
                    private_key,
                    private_key_length);
                if (err == ESP_OK) {
                    psa_destroy_key(auth->key_id);
                    auth->key_id = signing_key_id;
                    signing_key_id = 0;
                }
            }
            if (signing_key_id != 0) {
                psa_destroy_key(signing_key_id);
            }
            mbedtls_platform_zeroize(private_key, sizeof(private_key));
            if (err != ESP_OK) {
                adb_auth_deinit(auth);
                return err;
            }
        }
    }

    esp_err_t err = validate_key(auth->key_id);
    if (err != ESP_OK) {
        adb_auth_deinit(auth);
        return err;
    }

    auth->initialized = true;
    return ESP_OK;
}

void adb_auth_deinit(adb_auth_t *auth)
{
    if (auth == NULL) {
        return;
    }
    if (auth->key_id != 0) {
        psa_destroy_key(auth->key_id);
        auth->key_id = 0;
    }
    auth->initialized = false;
}

esp_err_t adb_auth_sign(
    adb_auth_t *auth,
    const uint8_t *token,
    size_t token_length,
    uint8_t signature[ADB_RSA_KEY_BYTES],
    size_t *signature_length)
{
    if (auth == NULL || !auth->initialized || token == NULL
        || token_length != ADB_AUTH_TOKEN_BYTES || signature == NULL
        || signature_length == NULL || *signature_length < ADB_RSA_KEY_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    psa_status_t status = psa_sign_hash(
        auth->key_id,
        PSA_ALG_RSA_PKCS1V15_SIGN(PSA_ALG_SHA_1),
        token,
        token_length,
        signature,
        *signature_length,
        signature_length);
    return status == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

static bool der_read_length(der_cursor_t *cursor, size_t *length)
{
    if (cursor->cursor >= cursor->end) {
        return false;
    }

    uint8_t first = *cursor->cursor++;
    if ((first & 0x80U) == 0) {
        *length = first;
    } else {
        size_t length_bytes = first & 0x7FU;
        if (length_bytes == 0 || length_bytes > sizeof(size_t)
            || (size_t)(cursor->end - cursor->cursor) < length_bytes
            || cursor->cursor[0] == 0) {
            return false;
        }

        size_t decoded_length = 0;
        for (size_t i = 0; i < length_bytes; ++i) {
            if (decoded_length > (SIZE_MAX >> 8)) {
                return false;
            }
            decoded_length = (decoded_length << 8) | cursor->cursor[i];
        }
        if (decoded_length < 128) {
            return false;
        }
        cursor->cursor += length_bytes;
        *length = decoded_length;
    }

    return *length <= (size_t)(cursor->end - cursor->cursor);
}

static bool der_read_element(
    der_cursor_t *cursor,
    uint8_t expected_tag,
    const uint8_t **value,
    size_t *value_length)
{
    if (cursor->cursor >= cursor->end || *cursor->cursor++ != expected_tag
        || !der_read_length(cursor, value_length)) {
        return false;
    }
    *value = cursor->cursor;
    cursor->cursor += *value_length;
    return true;
}

static bool parse_rsa_public_key(
    const uint8_t *der,
    size_t der_length,
    uint8_t modulus[ADB_RSA_BYTES],
    uint32_t *exponent)
{
    der_cursor_t document = { der, der + der_length };
    const uint8_t *sequence_bytes;
    size_t sequence_length;
    if (!der_read_element(&document, 0x30, &sequence_bytes, &sequence_length)
        || document.cursor != document.end) {
        return false;
    }

    der_cursor_t sequence = {
        sequence_bytes,
        sequence_bytes + sequence_length,
    };
    const uint8_t *modulus_bytes;
    const uint8_t *exponent_bytes;
    size_t modulus_length;
    size_t exponent_length;
    if (!der_read_element(&sequence, 0x02, &modulus_bytes, &modulus_length)
        || !der_read_element(&sequence, 0x02, &exponent_bytes, &exponent_length)
        || sequence.cursor != sequence.end) {
        return false;
    }

    while (modulus_length > 0 && *modulus_bytes == 0) {
        ++modulus_bytes;
        --modulus_length;
    }
    while (exponent_length > 0 && *exponent_bytes == 0) {
        ++exponent_bytes;
        --exponent_length;
    }
    if (modulus_length != ADB_RSA_BYTES || exponent_length == 0
        || exponent_length > sizeof(*exponent)) {
        return false;
    }

    memcpy(modulus, modulus_bytes, modulus_length);
    *exponent = 0;
    for (size_t i = 0; i < exponent_length; ++i) {
        *exponent = (*exponent << 8) | exponent_bytes[i];
    }
    return *exponent != 0;
}

static uint32_t calculate_n0inv(uint32_t least_significant_word)
{
    uint32_t inverse = 1;
    for (unsigned int i = 0; i < 5; ++i) {
        inverse *= 2U - least_significant_word * inverse;
    }
    return 0U - inverse;
}

static void write_le_words(
    uint8_t *output,
    const uint8_t input[ADB_RSA_BYTES])
{
    for (size_t offset = 0; offset < ADB_RSA_BYTES; offset += 4) {
        output[offset] = input[ADB_RSA_BYTES - offset - 1];
        output[offset + 1] = input[ADB_RSA_BYTES - offset - 2];
        output[offset + 2] = input[ADB_RSA_BYTES - offset - 3];
        output[offset + 3] = input[ADB_RSA_BYTES - offset - 4];
    }
}

static int compare_bytes(
    const uint8_t *left,
    const uint8_t *right,
    size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    return 0;
}

static void calculate_rr(
    const uint8_t modulus[ADB_RSA_BYTES],
    uint8_t rr_output[ADB_RSA_BYTES])
{
    uint8_t value[ADB_RSA_BYTES + 1] = {0};
    uint8_t extended_modulus[ADB_RSA_BYTES + 1] = {0};
    value[ADB_RSA_BYTES] = 1;
    memcpy(&extended_modulus[1], modulus, ADB_RSA_BYTES);

    for (size_t bit = 0; bit < 4096; ++bit) {
        uint16_t carry = 0;
        for (size_t index = sizeof(value); index > 0; --index) {
            uint16_t doubled = (uint16_t)value[index - 1] * 2U + carry;
            value[index - 1] = (uint8_t)doubled;
            carry = doubled >> 8;
        }

        if (compare_bytes(value, extended_modulus, sizeof(value)) >= 0) {
            int borrow = 0;
            for (size_t index = sizeof(value); index > 0; --index) {
                int difference = (int)value[index - 1]
                    - (int)extended_modulus[index - 1] - borrow;
                if (difference < 0) {
                    difference += 256;
                    borrow = 1;
                } else {
                    borrow = 0;
                }
                value[index - 1] = (uint8_t)difference;
            }
        }
    }

    memcpy(rr_output, &value[1], ADB_RSA_BYTES);
    mbedtls_platform_zeroize(value, sizeof(value));
}

esp_err_t adb_auth_get_public_key(
    adb_auth_t *auth,
    uint8_t *output,
    size_t output_size,
    size_t *output_length)
{
    if (auth == NULL || !auth->initialized || output == NULL
        || output_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t public_der[ADB_RSA_PUBLIC_DER_MAX] = {0};
    uint8_t modulus[ADB_RSA_BYTES] = {0};
    uint8_t rr[ADB_RSA_BYTES] = {0};
    uint8_t adb_key[ADB_PUBLIC_KEY_BYTES] = {0};
    size_t public_der_length = 0;
    uint32_t exponent = 0;

    psa_status_t status = psa_export_public_key(
        auth->key_id,
        public_der,
        sizeof(public_der),
        &public_der_length);
    if (status != PSA_SUCCESS) {
        mbedtls_platform_zeroize(public_der, sizeof(public_der));
        return psa_status_to_esp_err(status);
    }
    if (public_der_length > sizeof(public_der)
        || !parse_rsa_public_key(
            public_der,
            public_der_length,
            modulus,
            &exponent)) {
        mbedtls_platform_zeroize(public_der, sizeof(public_der));
        return ESP_ERR_INVALID_RESPONSE;
    }

    adb_key[0] = ADB_RSA_WORDS;
    uint32_t n0 = ((uint32_t)modulus[252] << 24)
        | ((uint32_t)modulus[253] << 16)
        | ((uint32_t)modulus[254] << 8)
        | (uint32_t)modulus[255];
    uint32_t n0inv = calculate_n0inv(n0);
    adb_key[4] = (uint8_t)n0inv;
    adb_key[5] = (uint8_t)(n0inv >> 8);
    adb_key[6] = (uint8_t)(n0inv >> 16);
    adb_key[7] = (uint8_t)(n0inv >> 24);
    write_le_words(&adb_key[8], modulus);
    calculate_rr(modulus, rr);
    write_le_words(&adb_key[264], rr);
    adb_key[520] = (uint8_t)exponent;
    adb_key[521] = (uint8_t)(exponent >> 8);
    adb_key[522] = (uint8_t)(exponent >> 16);
    adb_key[523] = (uint8_t)(exponent >> 24);

    char suffix[ESP_ADB_IDENTITY_MAX_LENGTH + 2];
    int suffix_length = snprintf(
        suffix,
        sizeof(suffix),
        " %s",
        auth->identity);
    if (suffix_length < 0 || (size_t)suffix_length + 1 > sizeof(suffix)) {
        mbedtls_platform_zeroize(public_der, sizeof(public_der));
        mbedtls_platform_zeroize(modulus, sizeof(modulus));
        mbedtls_platform_zeroize(rr, sizeof(rr));
        return ESP_FAIL;
    }

    size_t suffix_size = (size_t)suffix_length + 1;
    size_t encoded_length = 0;
    int result = output_size < suffix_size
        ? MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
        : mbedtls_base64_encode(
            output,
            output_size - suffix_size,
            &encoded_length,
            adb_key,
            sizeof(adb_key));
    esp_err_t err = ESP_OK;
    if (result == 0 && encoded_length + suffix_size <= output_size) {
        memcpy(output + encoded_length, suffix, suffix_size);
        *output_length = encoded_length + suffix_size;
    } else if (result == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = ESP_FAIL;
    }

    mbedtls_platform_zeroize(public_der, sizeof(public_der));
    mbedtls_platform_zeroize(modulus, sizeof(modulus));
    mbedtls_platform_zeroize(rr, sizeof(rr));
    mbedtls_platform_zeroize(adb_key, sizeof(adb_key));
    return err;
}
