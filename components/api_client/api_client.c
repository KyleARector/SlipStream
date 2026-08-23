#include "api_client.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_update.h"
#include "sntp_sync.h"
#include "usb_printer_host.h"
#include "wifi_station.h"

static const char *TAG = "api_client";

#define API_CLIENT_POLL_INTERVAL_MS  (30 * 1000)
#define API_CLIENT_TASK_STACK_SIZE   6144
#define API_CLIENT_RESPONSE_BUF_SIZE 2048
#define API_CLIENT_HTTP_TIMEOUT_MS   10000

/* Set once in api_client_start() from caller-owned string-literal
 * constants (secrets.h); read only from api_client_task() afterward. */
static const char *s_server_url;
static const char *s_api_key;

static char s_response_buf[API_CLIENT_RESPONSE_BUF_SIZE];

static void handle_checkin_response(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse check-in response JSON");
        return;
    }

    cJSON *latest_version = cJSON_GetObjectItemCaseSensitive(root, "latest_firmware_version");
    if (cJSON_IsString(latest_version)) {
        ESP_LOGI(TAG, "Server reports latest firmware version: %s", latest_version->valuestring);
        /* May reboot the device (esp_restart()) if a newer, validly-signed
         * image is available and the print queue is idle -- see M22's
         * ota_update component. Never returns in that case. */
        ota_update_check_and_apply(s_server_url, s_api_key, latest_version->valuestring);
    }

    cJSON *job_array = cJSON_GetObjectItemCaseSensitive(root, "jobs");
    if (cJSON_IsArray(job_array)) {
        cJSON *job;
        cJSON_ArrayForEach(job, job_array) {
            cJSON *type = cJSON_GetObjectItemCaseSensitive(job, "type");
            if (!cJSON_IsString(type)) {
                ESP_LOGW(TAG, "Skipping malformed job entry in check-in response");
                continue;
            }

            /* Whether the printer feeds+cuts after this job before starting
             * the next one -- defaults to true (preserves the printer's
             * previous unconditional per-job cut) when absent, so older
             * server responses without this field still behave exactly as
             * before. See usb_printer_host_enqueue_print()'s doc comment. */
            cJSON *cut_after_json = cJSON_GetObjectItemCaseSensitive(job, "cut_after");
            bool cut_after = !cJSON_IsBool(cut_after_json) || cJSON_IsTrue(cut_after_json);

            if (strcmp(type->valuestring, "text") == 0) {
                cJSON *text = cJSON_GetObjectItemCaseSensitive(job, "text");
                if (!cJSON_IsString(text)) {
                    ESP_LOGW(TAG, "Skipping malformed text job entry");
                    continue;
                }

                size_t len = strlen(text->valuestring);
                ESP_LOGI(TAG, "Enqueuing print job from server (%u bytes)", (unsigned)len);

                /* Safe to call from any task -- hands off via its own
                 * FreeRTOS queue rather than touching the print job
                 * FSM/queue directly (see M8's Concurrency Model note;
                 * same entry point BLE uses). */
                esp_err_t print_err = usb_printer_host_enqueue_print(text->valuestring, len, cut_after);
                if (print_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to enqueue server print job: %s", esp_err_to_name(print_err));
                }
                continue;
            }

            if (strcmp(type->valuestring, "image") == 0) {
                cJSON *image_ref = cJSON_GetObjectItemCaseSensitive(job, "image_ref");
                cJSON *width_dots = cJSON_GetObjectItemCaseSensitive(job, "width_dots");
                cJSON *height_dots = cJSON_GetObjectItemCaseSensitive(job, "height_dots");
                if (!cJSON_IsString(image_ref) || !cJSON_IsNumber(width_dots) || !cJSON_IsNumber(height_dots)) {
                    ESP_LOGW(TAG, "Skipping malformed image job entry");
                    continue;
                }

                /* usb_printer_host resolves this exact encoding at dequeue
                 * time by fetching GET {server}/images/{image_ref} -- see
                 * usb_printer_host_enqueue_image()'s doc comment. */
                char encoded_ref[96];
                int written = snprintf(encoded_ref, sizeof(encoded_ref), "%s:%dx%d", image_ref->valuestring,
                                        width_dots->valueint, height_dots->valueint);
                if (written < 0 || (size_t)written >= sizeof(encoded_ref)) {
                    ESP_LOGW(TAG, "Image job reference too long to encode, skipping");
                    continue;
                }

                ESP_LOGI(TAG, "Enqueuing image job from server (%dx%d dots)", width_dots->valueint,
                         height_dots->valueint);
                esp_err_t print_err = usb_printer_host_enqueue_image(encoded_ref, (size_t)written, cut_after);
                if (print_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to enqueue server image job: %s", esp_err_to_name(print_err));
                }
                continue;
            }

            ESP_LOGW(TAG, "Skipping unsupported job type \"%s\"", type->valuestring);
        }
    }

    cJSON_Delete(root);
}

static void do_checkin(void)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/checkin", s_server_url);

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_api_key);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = API_CLIENT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGW(TAG, "Failed to init HTTP client for check-in");
        return;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Check-in request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Check-in returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int read_len = esp_http_client_read_response(client, s_response_buf, sizeof(s_response_buf) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0) {
        ESP_LOGW(TAG, "Failed to read check-in response body");
        return;
    }
    s_response_buf[read_len] = '\0';

    ESP_LOGI(TAG, "Check-in OK (%d byte response)", read_len);

    /* A successful check-in is exactly the "first successful post-update
     * poll" signal M22's rollback-safety requirement depends on -- a no-op
     * once the running image is already confirmed valid. */
    ota_update_confirm_valid_if_pending();

    handle_checkin_response(s_response_buf);
}

static void api_client_task(void *arg)
{
    (void)arg;
    bool time_synced = false;

    for (;;) {
        if (wifi_sta_wait_connected(pdMS_TO_TICKS(5000)) != ESP_OK) {
            continue;
        }

        if (!time_synced) {
            if (!sntp_sync_has_synced()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            time_synced = true;
        }

        do_checkin();
        vTaskDelay(pdMS_TO_TICKS(API_CLIENT_POLL_INTERVAL_MS));
    }
}

esp_err_t api_client_start(const char *server_url, const char *api_key)
{
    s_server_url = server_url;
    s_api_key = api_key;

    BaseType_t created = xTaskCreate(api_client_task, "api_client", API_CLIENT_TASK_STACK_SIZE, NULL, 3, NULL);
    return created == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}
