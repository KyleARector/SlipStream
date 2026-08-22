#include "ota_update.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "ota_trust_key.h"
#include "usb_printer_host.h"

static const char *TAG = "ota_update";

#define OTA_HTTP_TIMEOUT_MS      30000
#define OTA_DOWNLOAD_CHUNK_LEN   4096
/* Comfortably covers any P256 DER signature (~70-72 bytes) plus the 2-byte
 * length field, with margin for a different (still small) signature size
 * later without needing to change this. */
#define OTA_TAIL_BUFFER_LEN      160
/* Sanity bound on the parsed signature length -- rejects a clearly-bogus
 * trailer (e.g. an unsigned test image whose last 2 bytes aren't really a
 * length field) before it can be misread as an absurd size. */
#define OTA_MAX_SIGNATURE_LEN    96

typedef struct {
    esp_ota_handle_t ota_handle;
    mbedtls_sha256_context sha_ctx;
    uint8_t tail_buf[OTA_TAIL_BUFFER_LEN];
    size_t tail_len;
} ota_download_state_t;

/* Feeds newly-received bytes through a sliding tail buffer: everything
 * that falls off the front (once more than OTA_TAIL_BUFFER_LEN bytes are
 * buffered) is flashed via esp_ota_write() and hashed via
 * mbedtls_sha256_update(), on the assumption that it's real image content.
 * The true image/signature split is only known once the stream ends (the
 * trailing 2-byte length field says how much of the final tail is really
 * the signature) -- until then, everything is provisionally treated as
 * image, which is correct for all but the last OTA_TAIL_BUFFER_LEN bytes
 * of the whole transfer. */
static esp_err_t ota_consume_bytes(ota_download_state_t *dl, const uint8_t *data, size_t len)
{
    size_t total = dl->tail_len + len;
    if (total <= OTA_TAIL_BUFFER_LEN) {
        memcpy(dl->tail_buf + dl->tail_len, data, len);
        dl->tail_len = total;
        return ESP_OK;
    }

    size_t flush_len = total - OTA_TAIL_BUFFER_LEN;
    size_t from_tail = (flush_len <= dl->tail_len) ? flush_len : dl->tail_len;
    size_t from_data = flush_len - from_tail;

    if (from_tail > 0) {
        esp_err_t err = esp_ota_write(dl->ota_handle, dl->tail_buf, from_tail);
        if (err != ESP_OK) {
            return err;
        }
        mbedtls_sha256_update(&dl->sha_ctx, dl->tail_buf, from_tail);
    }
    if (from_data > 0) {
        esp_err_t err = esp_ota_write(dl->ota_handle, data, from_data);
        if (err != ESP_OK) {
            return err;
        }
        mbedtls_sha256_update(&dl->sha_ctx, data, from_data);
    }

    /* New tail = whatever wasn't just flushed, from both the old tail and
     * the new data, concatenated in order. Always sums to exactly
     * OTA_TAIL_BUFFER_LEN -- see the arithmetic this relies on: total -
     * flush_len == OTA_TAIL_BUFFER_LEN by construction above. */
    uint8_t new_tail[OTA_TAIL_BUFFER_LEN];
    size_t pos = 0;
    size_t leftover_old = dl->tail_len - from_tail;
    size_t leftover_new = len - from_data;
    if (leftover_old > 0) {
        memcpy(new_tail + pos, dl->tail_buf + from_tail, leftover_old);
        pos += leftover_old;
    }
    if (leftover_new > 0) {
        memcpy(new_tail + pos, data + from_data, leftover_new);
        pos += leftover_new;
    }
    memcpy(dl->tail_buf, new_tail, pos);
    dl->tail_len = pos;
    return ESP_OK;
}

/* Verifies the DER-encoded ECDSA-P256 signature (already extracted from
 * the tail buffer) against a SHA-256 digest of the image, using the
 * embedded trust key. Returns true only if the signature is
 * cryptographically valid for exactly this digest. */
static bool verify_signature(const uint8_t *digest, const uint8_t *signature, size_t signature_len)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    int rc = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)k_ota_trust_key_pem,
                                          sizeof(k_ota_trust_key_pem));
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to parse embedded OTA trust key: -0x%04x", -rc);
        mbedtls_pk_free(&pk);
        return false;
    }

    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, signature, signature_len);
    mbedtls_pk_free(&pk);

    return rc == 0;
}

/* Downloads {server_url}/firmware/{version} into the inactive OTA
 * partition and verifies it. Returns ESP_OK only if a valid signed image
 * was fully written and is ready to boot into (caller still has to call
 * esp_ota_set_boot_partition() + esp_restart()). Any failure leaves the
 * currently-running partition completely untouched. */
static esp_err_t download_and_verify(const char *server_url, const char *api_key, const char *version,
                                      const esp_partition_t *target_partition)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/firmware/%s", server_url, version);
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA download request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "OTA download returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    ota_download_state_t dl = {0};
    mbedtls_sha256_init(&dl.sha_ctx);
    mbedtls_sha256_starts(&dl.sha_ctx, 0);

    err = esp_ota_begin(target_partition, OTA_SIZE_UNKNOWN, &dl.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        mbedtls_sha256_free(&dl.sha_ctx);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    uint8_t chunk[OTA_DOWNLOAD_CHUNK_LEN];
    int read_len;
    bool io_error = false;
    while ((read_len = esp_http_client_read(client, (char *)chunk, sizeof(chunk))) > 0) {
        if (ota_consume_bytes(&dl, chunk, (size_t)read_len) != ESP_OK) {
            io_error = true;
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0 || io_error) {
        ESP_LOGW(TAG, "OTA download interrupted before completion");
        mbedtls_sha256_free(&dl.sha_ctx);
        esp_ota_abort(dl.ota_handle);
        return ESP_FAIL;
    }

    /* The final tail_buf holds [remaining image bytes][signature][2-byte
     * length] -- extract the signature per the confirmed footer format,
     * flash+hash whatever image bytes precede it, then finalize. */
    if (dl.tail_len < 2) {
        ESP_LOGW(TAG, "OTA image too short to contain a signature footer");
        mbedtls_sha256_free(&dl.sha_ctx);
        esp_ota_abort(dl.ota_handle);
        return ESP_FAIL;
    }
    size_t sig_len = ((size_t)dl.tail_buf[dl.tail_len - 2] << 8) | dl.tail_buf[dl.tail_len - 1];
    if (sig_len == 0 || sig_len > OTA_MAX_SIGNATURE_LEN || sig_len + 2 > dl.tail_len) {
        ESP_LOGW(TAG, "OTA image signature footer is malformed (sig_len=%u)", (unsigned)sig_len);
        mbedtls_sha256_free(&dl.sha_ctx);
        esp_ota_abort(dl.ota_handle);
        return ESP_FAIL;
    }

    size_t final_image_len = dl.tail_len - 2 - sig_len;
    uint8_t signature[OTA_MAX_SIGNATURE_LEN];
    memcpy(signature, dl.tail_buf + final_image_len, sig_len);

    if (final_image_len > 0) {
        err = esp_ota_write(dl.ota_handle, dl.tail_buf, final_image_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to flash final image bytes: %s", esp_err_to_name(err));
            mbedtls_sha256_free(&dl.sha_ctx);
            esp_ota_abort(dl.ota_handle);
            return err;
        }
        mbedtls_sha256_update(&dl.sha_ctx, dl.tail_buf, final_image_len);
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&dl.sha_ctx, digest);
    mbedtls_sha256_free(&dl.sha_ctx);

    /* Validates the image's own structure (ESP32 app header, its
     * internally-appended checksum) -- catches gross corruption
     * independent of our own signature check below. */
    err = esp_ota_end(dl.ota_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Downloaded image failed structural validation: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    if (!verify_signature(digest, signature, sig_len)) {
        ESP_LOGW(TAG, "Downloaded image failed signature verification -- rejecting");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA image downloaded and verified (%u signature bytes)", (unsigned)sig_len);
    return ESP_OK;
}

void ota_update_check_and_apply(const char *server_url, const char *api_key, const char *latest_version)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (strcmp(latest_version, app_desc->version) == 0) {
        return;
    }

    if (!usb_printer_host_queue_is_idle()) {
        ESP_LOGI(TAG, "New firmware version %s available, deferring OTA until the print queue drains",
                 latest_version);
        return;
    }

    ESP_LOGI(TAG, "New firmware version %s available (running %s) -- starting OTA", latest_version,
             app_desc->version);

    const esp_partition_t *target_partition = esp_ota_get_next_update_partition(NULL);
    if (target_partition == NULL) {
        ESP_LOGE(TAG, "No inactive OTA partition available");
        return;
    }

    esp_err_t err = download_and_verify(server_url, api_key, latest_version, target_partition);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA update aborted, continuing on current firmware");
        return;
    }

    err = esp_ota_set_boot_partition(target_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set new boot partition: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "OTA update applied, rebooting into %s", latest_version);
    esp_restart();
}

void ota_update_confirm_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "First post-OTA check-in succeeded -- marking this image permanently bootable");
        } else {
            ESP_LOGW(TAG, "Failed to confirm OTA image valid: %s", esp_err_to_name(err));
        }
    }
}
