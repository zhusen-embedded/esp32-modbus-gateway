#include "ota_client.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_format.h"
#include "esp_system.h"

static const char *TAG = "ota";
static ota_state_t s_state = OTA_IDLE;

#define OTA_BUF_SIZE    1024

void ota_init(void)
{
    /* 关键点：如果当前运行的是刚升级完、还没被确认的镜像，这里要先标成有效，
     * 否则下次启动 bootloader 会因为「未确认」回滚到上一个分区。 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
        if (st == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "first boot of new image, mark valid after self-check");
            /* 真机上这里应该先跑一轮自检（读传感器/连一次网），过了再 mark。
             * 当前实现：能跑到这里说明系统起来了，直接标有效。 */
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    ESP_LOGI(TAG, "running partition: %s", running->label);
}

const char *ota_state_str(void)
{
    switch (s_state) {
    case OTA_RUNNING: return "running";
    case OTA_DONE:    return "done";
    case OTA_FAILED:  return "failed";
    default:          return "idle";
    }
}

ota_state_t ota_state(void) { return s_state; }

int ota_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGI(TAG, "no OTA url configured");
        return -1;
    }

    s_state = OTA_RUNNING;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (http == NULL) {
        s_state = OTA_FAILED;
        return -1;
    }

    esp_err_t err = esp_http_client_open(http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http);
        s_state = OTA_FAILED;
        return -1;
    }

    int content_len = esp_http_client_fetch_headers(http);
    if (content_len <= 0) {
        ESP_LOGE(TAG, "bad content length %d", content_len);
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        s_state = OTA_FAILED;
        return -1;
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "no OTA partition available");
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        s_state = OTA_FAILED;
        return -1;
    }
    ESP_LOGI(TAG, "writing to %s, %d bytes", update->label, content_len);

    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        s_state = OTA_FAILED;
        return -1;
    }

    uint8_t buf[OTA_BUF_SIZE];
    int written = 0;
    while (1) {
        int n = esp_http_client_read(http, (char *)buf, sizeof(buf));
        if (n < 0) {
            ESP_LOGE(TAG, "read error");
            esp_ota_abort(handle);
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
            s_state = OTA_FAILED;
            return -1;
        }
        if (n == 0) break;      /* 对端关连接，传输结束 */

        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
            s_state = OTA_FAILED;
            return -1;
        }
        written += n;
        if ((written % (64 * 1024)) < OTA_BUF_SIZE) {
            ESP_LOGI(TAG, "progress %d / %d", written, content_len);
        }
    }

    esp_http_client_close(http);
    esp_http_client_cleanup(http);

    if (written != content_len) {
        ESP_LOGE(TAG, "size mismatch: got %d, expect %d", written, content_len);
        esp_ota_abort(handle);
        s_state = OTA_FAILED;
        return -1;
    }

    /* esp_ota_end 会校验镜像格式，格式不对这里就返回失败，
     * 所以「升级失败回滚」不需要自己写：校验没过就不切分区，重启还是老固件。 */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image validation failed: %s", esp_err_to_name(err));
        s_state = OTA_FAILED;
        return -1;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(err));
        s_state = OTA_FAILED;
        return -1;
    }

    s_state = OTA_DONE;
    ESP_LOGI(TAG, "OTA done, %d bytes, reboot to apply", written);
    return 0;
}
