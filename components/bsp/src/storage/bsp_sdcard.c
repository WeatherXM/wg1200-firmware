/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include "bsp_board.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_task_wdt.h"

#if (SOC_SDMMC_HOST_SUPPORTED)
#include "driver/sdmmc_host.h"
#endif

#define DEFAULT_FD_NUM      2
#define DEFAULT_MOUNT_POINT "/sdcard"

static sdmmc_card_t *card;
static const char *TAG = "bsp_sdcard";

esp_err_t bsp_sdcard_init(char *mount_point, size_t max_files, bool format_if_mount_failed)
{
    if (NULL != card) {
        return ESP_ERR_INVALID_STATE;
    }
    const board_res_desc_t *brd = bsp_board_get_description();

    /* Check if SD crad is supported */
    if (!brd->FUNC_SDMMC_EN && !brd->FUNC_SDSPI_EN) {
        ESP_LOGE(TAG, "SDMMC and SDSPI not supported on this board!");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret_val = ESP_OK;

    /**
     * @brief Options for mounting the filesystem.
     *   If format_if_mount_failed is set to true, SD card will be partitioned and
     *   formatted in case when mounting fails.
     *
     */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = max_files,
        .allocation_unit_size = 16 * 1024
    };

    /**
     * @brief Use settings defined above to initialize SD card and mount FAT filesystem.
     *   Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
     *   Please check its source code and implement error recovery when developing
     *   production applications.
     *
     */
    sdmmc_host_t host = {0};
    if (brd->FUNC_SDMMC_EN) {
        sdmmc_host_t h = SDMMC_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
    } else {
        sdmmc_host_t h = SDSPI_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
        host.slot = brd->SDSPI_HOST;
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = brd->GPIO_SDSPI_MOSI,
            .miso_io_num = brd->GPIO_SDSPI_MISO,
            .sclk_io_num = brd->GPIO_SDSPI_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4000,
        };
        ret_val = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret_val != ESP_OK && ret_val != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize bus.");
            return ret_val;
        }
    }

    /**
     * @brief This initializes the slot without card detect (CD) and write protect (WP) signals.
     *   Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
     *
     */
    if (brd->FUNC_SDMMC_EN) {
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        /* Config SD data width. 0, 4 or 8. Currently for SD card, 8 bit is not supported. */
        slot_config.width = brd->SDMMC_BUS_WIDTH;

        /**
         * @brief On chips where the GPIOs used for SD card can be configured, set them in
         *   the slot_config structure.
         *
         */
#if SOC_SDMMC_USE_GPIO_MATRIX
        slot_config.clk = brd->GPIO_SDMMC_CLK;
        slot_config.cmd = brd->GPIO_SDMMC_CMD;
        slot_config.d0 = brd->GPIO_SDMMC_D0;
        slot_config.d1 = brd->GPIO_SDMMC_D1;
        slot_config.d2 = brd->GPIO_SDMMC_D2;
        slot_config.d3 = brd->GPIO_SDMMC_D3;
#endif
        slot_config.cd = brd->GPIO_SDMMC_DET;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        ret_val = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    } else {
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = brd->GPIO_SDSPI_CS;
        slot_config.host_id = host.slot;
        ret_val = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    }

    /* Check for SDMMC mount result. */
    if (ret_val != ESP_OK) {
        if (ret_val == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret_val));
        }
        return ret_val;
    }

    /* Card has been initialized, print its properties. */
    sdmmc_card_print_info(stdout, card);

    return ret_val;
}

esp_err_t bsp_sdcard_init_default(void)
{
    return bsp_sdcard_init(DEFAULT_MOUNT_POINT, DEFAULT_FD_NUM, false);
}

esp_err_t bsp_sdcard_deinit(char *mount_point)
{
    if (NULL == mount_point) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Unmount an SD card from the FAT filesystem and release resources acquired */
    esp_err_t ret_val = esp_vfs_fat_sdcard_unmount(mount_point, card);

    /* Make SD/MMC card information structure pointer NULL */
    card = NULL;

    return ret_val;
}

esp_err_t bsp_sdcard_deinit_default(void)
{
    return bsp_sdcard_deinit(DEFAULT_MOUNT_POINT);
}

#include "driver/sdspi_host.h"

static esp_err_t bsp_sdcard_raw_init(sdmmc_card_t **out_card)
{
    const board_res_desc_t *brd = bsp_board_get_description();
    if (!brd->FUNC_SDMMC_EN && !brd->FUNC_SDSPI_EN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    sdmmc_card_t *temp_card = malloc(sizeof(sdmmc_card_t));
    if (!temp_card) {
        return ESP_ERR_NO_MEM;
    }

    sdmmc_host_t host = {0};
    esp_err_t ret = ESP_FAIL;

    if (brd->FUNC_SDMMC_EN) {
#if (SOC_SDMMC_HOST_SUPPORTED)
        sdmmc_host_t h = SDMMC_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
        
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.width = brd->SDMMC_BUS_WIDTH;
#if SOC_SDMMC_USE_GPIO_MATRIX
        slot_config.clk = brd->GPIO_SDMMC_CLK;
        slot_config.cmd = brd->GPIO_SDMMC_CMD;
        slot_config.d0 = brd->GPIO_SDMMC_D0;
        slot_config.d1 = brd->GPIO_SDMMC_D1;
        slot_config.d2 = brd->GPIO_SDMMC_D2;
        slot_config.d3 = brd->GPIO_SDMMC_D3;
#endif
        slot_config.cd = brd->GPIO_SDMMC_DET;
        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        ret = sdmmc_host_init();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            ret = sdmmc_host_init_slot(host.slot, &slot_config);
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
                ret = sdmmc_card_init(&host, temp_card);
            }
        }
#endif
    } else {
        sdmmc_host_t h = SDSPI_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
        host.slot = brd->SDSPI_HOST;

        spi_bus_config_t bus_cfg = {
            .mosi_io_num = brd->GPIO_SDSPI_MOSI,
            .miso_io_num = brd->GPIO_SDSPI_MISO,
            .sclk_io_num = brd->GPIO_SDSPI_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4000,
        };
        
        ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            free(temp_card);
            return ret;
        }

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = brd->GPIO_SDSPI_CS;
        slot_config.host_id = host.slot;

        ret = sdspi_host_init();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            sdspi_dev_handle_t dev_handle;
            ret = sdspi_host_init_device(&slot_config, &dev_handle);
            if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
                ret = sdmmc_card_init(&host, temp_card);
            }
        }
    }

    if (ret == ESP_OK) {
        *out_card = temp_card;
    } else {
        free(temp_card);
    }
    return ret;
}

esp_err_t bsp_sdcard_diagnose(char *out_buf, size_t max_len)
{
    if (out_buf == NULL || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_card_t *temp_card = card;
    bool temp_initialized = false;

    if (temp_card != NULL) {
        // Quick verification if the card is still present by reading sector 0
        uint8_t dummy_buf[512];
        esp_err_t test_err = sdmmc_read_sectors(temp_card, dummy_buf, 0, 1);
        if (test_err != ESP_OK) {
            ESP_LOGW(TAG, "Stale mount pointer detected. Sector read failed (%s). Card likely removed.", esp_err_to_name(test_err));
            bsp_sdcard_deinit_default();
            temp_card = NULL;
        }
    }

    if (temp_card == NULL) {
        esp_err_t err = bsp_sdcard_raw_init(&temp_card);
        if (err != ESP_OK) {
            snprintf(out_buf, max_len, 
                "{\"mounted\":false,\"present\":false,\"error\":\"%s\"}", 
                esp_err_to_name(err));
            return ESP_OK;
        }
        temp_initialized = true;
    }

    uint64_t capacity = (uint64_t)temp_card->csd.capacity * temp_card->csd.sector_size;
    if (capacity == 0) {
        capacity = (uint64_t)temp_card->csd.capacity * 512;
    }
    double cap_gb = (double)capacity / (1024.0 * 1024.0 * 1024.0);

    // Sanitize the card name to avoid unescaped JSON characters or binary junk
    char clean_name[16] = "";
    int clean_idx = 0;
    for (int i = 0; i < 6 && temp_card->cid.name[i] != '\0'; i++) {
        char c = temp_card->cid.name[i];
        if (c >= 32 && c <= 126 && c != '"' && c != '\\') {
            clean_name[clean_idx++] = c;
        }
    }
    clean_name[clean_idx] = '\0';

    uint8_t sector_buf[512];
    esp_err_t read_err = sdmmc_read_sectors(temp_card, sector_buf, 0, 1);
    
    char part_info[768] = "";
    if (read_err == ESP_OK) {
        uint16_t signature = (sector_buf[511] << 8) | sector_buf[510];
        
        int valid_partitions = 0;
        char parts_json[512] = "";
        
        // Only parse partition details if a valid MBR signature is found
        if (signature == 0xAA55) {
            for (int i = 0; i < 4; i++) {
                int offset = 446 + (i * 16);
                uint8_t type = sector_buf[offset + 4];
                uint32_t lba_start = sector_buf[offset + 8] | 
                                     (sector_buf[offset + 9] << 8) | 
                                     (sector_buf[offset + 10] << 16) | 
                                     (sector_buf[offset + 11] << 24);
                uint32_t sector_count = sector_buf[offset + 12] | 
                                        (sector_buf[offset + 13] << 8) | 
                                        (sector_buf[offset + 14] << 16) | 
                                        (sector_buf[offset + 15] << 24);
                
                if (type != 0) {
                    valid_partitions++;
                    const char *type_desc = "Unknown";
                    if (type == 0x83) type_desc = "Linux ext2/ext3/ext4";
                    else if (type == 0x0C || type == 0x0B) type_desc = "FAT32";
                    else if (type == 0x07) type_desc = "NTFS/exFAT";
                    else if (type == 0x01) type_desc = "FAT12";
                    else if (type == 0x04 || type == 0x06 || type == 0x0E) type_desc = "FAT16";
                    else if (type == 0xEE) type_desc = "GPT Partition Table";
                    
                    double part_size_gb = ((double)sector_count * 512.0) / (1024.0 * 1024.0 * 1024.0);
                    
                    char one_part[128];
                    snprintf(one_part, sizeof(one_part),
                             "{\"id\":%d,\"type\":\"0x%02X\",\"desc\":\"%s\",\"start\":%lu,\"sectors\":%lu,\"size_gb\":%.2f},",
                             i + 1, type, type_desc, (unsigned long)lba_start, (unsigned long)sector_count, part_size_gb);
                    strlcat(parts_json, one_part, sizeof(parts_json));
                }
            }
            
            size_t len = strlen(parts_json);
            if (len > 0 && parts_json[len - 1] == ',') {
                parts_json[len - 1] = '\0';
            }
        }
        
        snprintf(part_info, sizeof(part_info),
                 "\"mbr_signature\":\"0x%04X\",\"partition_count\":%d,\"partitions\":[%s]",
                 signature, valid_partitions, parts_json);
    } else {
        snprintf(part_info, sizeof(part_info),
                 "\"mbr_error\":\"Failed to read sector 0: %s\"",
                 esp_err_to_name(read_err));
    }

    double vfs_total_gb = 0.0;
    double vfs_free_gb = 0.0;
    double vfs_used_gb = 0.0;
    bool vfs_stats_ok = false;
    if (card != NULL) {
        struct statvfs s;
        if (statvfs(DEFAULT_MOUNT_POINT, &s) == 0) {
            uint64_t total_bytes = (uint64_t)s.f_frsize * s.f_blocks;
            uint64_t free_bytes = (uint64_t)s.f_frsize * s.f_bfree;
            uint64_t used_bytes = total_bytes - free_bytes;
            vfs_total_gb = (double)total_bytes / (1024.0 * 1024.0 * 1024.0);
            vfs_free_gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
            vfs_used_gb = (double)used_bytes / (1024.0 * 1024.0 * 1024.0);
            vfs_stats_ok = true;
        }
    }

    char vfs_info[128] = "";
    if (vfs_stats_ok) {
        snprintf(vfs_info, sizeof(vfs_info),
                 "\"vfs_total_gb\":%.2f,\"vfs_free_gb\":%.2f,\"vfs_used_gb\":%.2f,",
                 vfs_total_gb, vfs_free_gb, vfs_used_gb);
    }

    snprintf(out_buf, max_len,
             "{\"mounted\":%s,\"present\":true,\"name\":\"%s\",\"capacity_gb\":%.2f,\"speed_mhz\":%d,%s%s}",
             (card != NULL) ? "true" : "false",
             clean_name,
             cap_gb,
             temp_card->max_freq_khz / 1000,
             vfs_info,
             part_info);

    if (temp_initialized) {
        free(temp_card);
    }
    return ESP_OK;
}

static void bsp_sdcard_set_idle_wdt(bool enable)
{
#if CONFIG_FREERTOS_UNICORE
    TaskHandle_t handle = xTaskGetIdleTaskHandle();
    if (handle != NULL) {
        if (enable) {
            esp_task_wdt_add(handle);
        } else {
            esp_task_wdt_delete(handle);
        }
    }
#else
    for (int i = 0; i < SOC_CPU_CORES_NUM; i++) {
        TaskHandle_t handle = xTaskGetIdleTaskHandleForCore(i);
        if (handle != NULL) {
            if (enable) {
                esp_task_wdt_add(handle);
            } else {
                esp_task_wdt_delete(handle);
            }
        }
    }
#endif
}

esp_err_t bsp_sdcard_format(void)
{
    // Show LCD alert popup for formatting
    bsp_show_sd_format_alert("Formatting SD", "Takes ~30 sec. Do not\nturn off or unplug device.", false);

    // Disable idle task TWDT during slow FAT format to prevent watchdog reset
    bsp_sdcard_set_idle_wdt(false);

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = DEFAULT_FD_NUM,
        .allocation_unit_size = 16 * 1024
    };

    esp_err_t err = ESP_OK;

    if (card != NULL) {
        err = esp_vfs_fat_sdcard_format_cfg(DEFAULT_MOUNT_POINT, card, &mount_config);
        bsp_sdcard_set_idle_wdt(true);
        
        if (err == ESP_OK) {
            bsp_show_sd_format_alert("Format Success", "SD Card formatted\nsuccessfully!", false);
            vTaskDelay(pdMS_TO_TICKS(2000));
        } else {
            bsp_show_sd_format_alert("Format Failed", "An error occurred\nduring formatting.", true);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        bsp_hide_sd_format_alert();
        return err;
    }

    ESP_LOGI(TAG, "Card not mounted. Attempting raw initialization for force format...");
    sdmmc_card_t *temp_card = NULL;
    err = bsp_sdcard_raw_init(&temp_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to raw initialize SD card for formatting: %s", esp_err_to_name(err));
        bsp_sdcard_set_idle_wdt(true);
        bsp_show_sd_format_alert("Format Failed", "Failed to raw initialize\nSD Card.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        bsp_hide_sd_format_alert();
        return err;
    }

    ESP_LOGI(TAG, "Zeroing out sector 0 (MBR) to wipe partition table...");
    uint8_t *zero_buf = malloc(512);
    if (!zero_buf) {
        free(temp_card);
        bsp_sdcard_set_idle_wdt(true);
        bsp_show_sd_format_alert("Format Failed", "Out of memory.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        bsp_hide_sd_format_alert();
        return ESP_ERR_NO_MEM;
    }
    memset(zero_buf, 0, 512);

    err = sdmmc_write_sectors(temp_card, zero_buf, 0, 1);
    free(zero_buf);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to zero out Sector 0: %s", esp_err_to_name(err));
        free(temp_card);
        bsp_sdcard_set_idle_wdt(true);
        bsp_show_sd_format_alert("Format Failed", "Failed to wipe Sector 0.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        bsp_hide_sd_format_alert();
        return err;
    }

    ESP_LOGI(TAG, "Sector 0 wiped successfully. Card is raw now. Cleaning up handles...");
    free(temp_card);

    ESP_LOGI(TAG, "Attempting mount with auto-format...");
    err = bsp_sdcard_init(DEFAULT_MOUNT_POINT, DEFAULT_FD_NUM, true);
    bsp_sdcard_set_idle_wdt(true);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD Card formatted and mounted successfully!");
        bsp_show_sd_format_alert("Format Success", "SD Card formatted\nsuccessfully!", false);
        vTaskDelay(pdMS_TO_TICKS(2000));
    } else {
        ESP_LOGE(TAG, "Mount with auto-format failed: %s", esp_err_to_name(err));
        bsp_show_sd_format_alert("Format Failed", "Mount and auto-format\nfailed.", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    bsp_hide_sd_format_alert();
    return err;
}


