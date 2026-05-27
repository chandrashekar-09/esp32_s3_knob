/* On-board microSD card — 4-bit SDMMC on the JC3636K718.
 *
 * Pin mapping from the v1.0 schematic (Schematic5_3, ESP32-S3 page):
 *   CMD  → GPIO38
 *   CLK  → GPIO39
 *   D0   → GPIO40
 *   D1   → GPIO41
 *   D2   → GPIO48
 *   D3   → GPIO47
 *
 * 10 kΩ external pull-ups already on the board (R10, R46-R49). We
 * still enable the internal pull-ups as a belt-and-braces fallback
 * for boards that may have unpopulated resistors. SDMMC slot 1 is
 * used because slot 0 on the S3 has fixed pins that overlap with
 * other peripherals on this board.
 *
 * Mount point: /sdcard (FAT). Auto-format is OFF — callers should
 * insert a pre-formatted card to avoid surprises.
 */

#include "sd_card.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/gpio_num.h"

static const char *kTag = "sd_card";
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;   /* 20 MHz — safe baseline */

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = GPIO_NUM_39;
    slot_cfg.cmd = GPIO_NUM_38;
    slot_cfg.d0  = GPIO_NUM_40;
    slot_cfg.d1  = GPIO_NUM_41;
    slot_cfg.d2  = GPIO_NUM_48;
    slot_cfg.d3  = GPIO_NUM_47;
    slot_cfg.width = 4;
    slot_cfg.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(kTag, "mounting SDMMC slot 1 (4-bit, custom GPIOs)");
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg,
                                            &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mount FAILED (%s) — no card? wrong FS?",
                 esp_err_to_name(err));
        s_card = NULL;
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(kTag, "mounted at /sdcard");
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_card != NULL;
}
