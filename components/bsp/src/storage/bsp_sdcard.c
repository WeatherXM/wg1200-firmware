/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include "bsp_board.h"
#include "bsp_storage.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "ff.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "sdmmc_cmd.h"
#include "esp_task_wdt.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_defs.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#if (SOC_SDMMC_HOST_SUPPORTED)
#include "driver/sdmmc_host.h"
#endif

#define DEFAULT_FD_NUM      2
#define DEFAULT_MOUNT_POINT "/sdcard"

/* SenseCAP Indicator TCA9535 IO expander pin definitions for shared SPI3_HOST */
#define EXPANDER_IO_RADIO_NSS   0
#define EXPANDER_IO_LCD_CS      4
#define EXPANDER_IO_SD_CS       11
#define EXPANDER_IO_SD_DET      12

typedef struct {
    bool valid;
    int sd_det_level;        /* 0 = card inserted (active low), 1 = not inserted, -1 = read error */
    uint16_t io_in_raw;      /* Raw 16-bit TCA9535 input port register */
    uint32_t cmd0_count;
    esp_err_t cmd0_err;
    uint32_t cmd0_r1;
    uint32_t cmd8_count;
    esp_err_t cmd8_err;
    uint32_t cmd8_r7;
    uint32_t cmd58_count;
    esp_err_t cmd58_err;
    uint32_t cmd58_ocr;
    uint32_t cmd55_count;
    esp_err_t cmd55_err;
    uint32_t cmd55_r1;
    uint32_t acmd41_count;
    esp_err_t acmd41_err;
    uint32_t acmd41_r1;
    uint32_t acmd41_last_arg;
    uint32_t last_cmd_opcode;
    esp_err_t last_cmd_err;
    uint32_t raw_stage;
    uint8_t raw_cmd0_r1;
    uint32_t raw_cmd8_r7;
    uint32_t raw_cmd58_ocr;
    uint8_t raw_acmd41_r1;
    uint8_t raw_cmd1_r1;
    uint16_t raw_cmd13_r2;
} bsp_sdcard_spi_diag_t;

static bsp_sdcard_spi_diag_t s_spi_diag = {
    .valid = false,
    .sd_det_level = -1,
};
static uint32_t s_prev_opcode = 0xFFFF;
static spi_device_handle_t s_clk_spi_handle = NULL;
static int s_sd_cs_level = -1;

static sdmmc_card_t *card = NULL;
static bool s_spi_bus_initialized = false;
static bsp_sdcard_status_info_t s_card_status = {
    .mounted = false,
    .present = false,
    .unusable = false,
    .formatting = false,
    .format_progress = 0,
    .capacity_gb = 0.0,
    .fs_type = "",
    .reason = ""
};
static const char *TAG = "bsp_sdcard";

/**
 * @brief Read the physical SD card detect switch (EXPANDER_IO_SD_DET, active-low)
 *        on the TCA9535 IO expander.
 * @param[out] out_raw_pins Optional pointer to receive the 16-bit TCA9535 input register.
 * @return 0 if card is physically inserted, 1 if slot is empty, -1 if IO expander unavailable.
 */
static int bsp_sdcard_read_sd_det_level(uint16_t *out_raw_pins)
{
    const board_res_desc_t *brd = bsp_board_get_description();
    if (!brd || !brd->io_expander_ops || !brd->io_expander_ops->read_input_pins) {
        return -1;
    }
    uint16_t pin_val = 0;
    esp_err_t err = brd->io_expander_ops->read_input_pins((uint8_t *)&pin_val);
    if (err != ESP_OK) {
        return -1;
    }
    if (out_raw_pins) {
        *out_raw_pins = pin_val;
    }
    return ((pin_val & (1U << EXPANDER_IO_SD_DET)) != 0) ? 1 : 0;
}

static bool bsp_sdcard_is_physically_detected(void)
{
    uint16_t raw_pins = 0;
    int level = bsp_sdcard_read_sd_det_level(&raw_pins);
    s_spi_diag.sd_det_level = level;
    if (level >= 0) {
        s_spi_diag.io_in_raw = raw_pins;
    }
    return (level == 0);
}

bool bsp_sdcard_is_inserted(void)
{
    bool present = bsp_sdcard_is_physically_detected();
    s_card_status.present = present;
    return present;
}

static void bsp_sdcard_deselect_other_cs(const board_res_desc_t *brd)
{
    if (!brd || !brd->io_expander_ops || !brd->io_expander_ops->set_level) {
        return;
    }
    /* Ensure shared SPI3 devices (SX1262 radio and LCD) have their CS deasserted (HIGH) */
    brd->io_expander_ops->set_level(EXPANDER_IO_RADIO_NSS, 1);
    brd->io_expander_ops->set_level(EXPANDER_IO_LCD_CS, 1);
}

static void bsp_sdcard_set_cs_expander(const board_res_desc_t *brd, bool cs_high)
{
    if (!brd || !brd->io_expander_ops || !brd->io_expander_ops->set_level) {
        return;
    }
    int target = cs_high ? 1 : 0;
    if (s_sd_cs_level != target) {
        brd->io_expander_ops->set_level(EXPANDER_IO_SD_CS, target);
        s_sd_cs_level = target;
    }
}

/**
 * @brief Clock out pure 0xFF bytes (MOSI held HIGH) on SPI3_HOST without sending
 *        any SD command header. For transfers >= 64 bytes, uses DMA + interrupt
 *        completion (spi_device_transmit) so SCLK runs continuously without gaps
 *        while yielding the CPU to FreeRTOS (satisfying SD Spec Section 4.4).
 */
static DRAM_ATTR uint8_t s_ff_dma_buf[512];
static bool s_ff_dma_buf_init = false;

static void bsp_sdcard_clock_ff(size_t num_bytes)
{
    if (s_clk_spi_handle == NULL || num_bytes == 0) {
        return;
    }
    if (!s_ff_dma_buf_init) {
        memset(s_ff_dma_buf, 0xFF, sizeof(s_ff_dma_buf));
        s_ff_dma_buf_init = true;
    }
    while (num_bytes > 0) {
        size_t chunk = (num_bytes > sizeof(s_ff_dma_buf)) ? sizeof(s_ff_dma_buf) : num_bytes;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = s_ff_dma_buf,
        };
        if (chunk >= 64) {
            spi_device_transmit(s_clk_spi_handle, &t);
        } else {
            spi_device_polling_transmit(s_clk_spi_handle, &t);
        }
        num_bytes -= chunk;
    }
}

static uint8_t bsp_sdcard_crc7(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80) {
                crc ^= 0x09;
            }
            d <<= 1;
        }
    }
    return crc & 0x7F;
}

static void bsp_sdcard_raw_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (s_clk_spi_handle == NULL || len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_polling_transmit(s_clk_spi_handle, &t);
}

static uint8_t bsp_sdcard_raw_cmd_ex(const board_res_desc_t *brd, bool frame_cs,
                                     uint8_t cmd, uint32_t arg, uint8_t crc_override,
                                     uint8_t *extra_rx, size_t extra_len,
                                     size_t post_clocks_cs_low)
{
    if (frame_cs) {
        bsp_sdcard_set_cs_expander(brd, false);
    }

    /*
     * Send pre-command idle byte (0xFF), 6-byte command frame, 9-byte N_CR/R1 window,
     * extra response bytes (R2/R3/R7 up to 4 bytes), and 2 trailing 0xFF bytes in a
     * SINGLE continuous SPI polling transaction so SCLK never pauses mid-command.
     */
    uint8_t tx_buf[24];
    uint8_t rx_buf[24];
    memset(tx_buf, 0xFF, sizeof(tx_buf));
    memset(rx_buf, 0xFF, sizeof(rx_buf));

    tx_buf[0] = 0xFF;
    tx_buf[1] = (uint8_t)(0x40U | (cmd & 0x3FU));
    tx_buf[2] = (uint8_t)(arg >> 24);
    tx_buf[3] = (uint8_t)(arg >> 16);
    tx_buf[4] = (uint8_t)(arg >> 8);
    tx_buf[5] = (uint8_t)(arg);
    tx_buf[6] = (crc_override != 0) ? crc_override
                                    : (uint8_t)((bsp_sdcard_crc7(&tx_buf[1], 5) << 1) | 0x01U);

    size_t capped_extra = (extra_len <= 4) ? extra_len : 4;
    size_t xfer_len = 7 + 9 + capped_extra + 2;
    bsp_sdcard_raw_xfer(tx_buf, rx_buf, xfer_len);

    uint8_t r1 = 0xFF;
    int r1_idx = -1;
    for (int i = 7; i <= 15; i++) {
        if ((rx_buf[i] & 0x80U) == 0) {
            r1 = rx_buf[i];
            r1_idx = i;
            break;
        }
    }

    if (r1_idx >= 0 && extra_rx != NULL && capped_extra > 0) {
        memcpy(extra_rx, &rx_buf[r1_idx + 1], capped_extra);
    }

    if (post_clocks_cs_low > 2) {
        bsp_sdcard_clock_ff(post_clocks_cs_low - 2);
    }

    if (frame_cs) {
        bsp_sdcard_set_cs_expander(brd, true);
        bsp_sdcard_clock_ff(2);
    }
    return r1;
}

static uint8_t bsp_sdcard_raw_cmd(const board_res_desc_t *brd, bool frame_cs,
                                  uint8_t cmd, uint32_t arg, uint8_t *extra_rx, size_t extra_len)
{
    return bsp_sdcard_raw_cmd_ex(brd, frame_cs, cmd, arg, 0, extra_rx, extra_len, 2);
}

static bool bsp_sdcard_raw_spi_handshake(const board_res_desc_t *brd)
{
    if (s_clk_spi_handle == NULL) {
        return false;
    }

    /*
     * Stage 0: Without issuing CMD0, send CMD58(0) (which sdmmc_init_ocr skipped) and
     * continue polling CMD55(0) + ACMD41(0x40000000) with continuous-SCLK framing.
     */
    bsp_sdcard_set_cs_expander(brd, true);
    bsp_sdcard_clock_ff(4);
    {
        uint8_t ocr0[4] = {0};
        uint8_t r1_58 = bsp_sdcard_raw_cmd_ex(brd, true, 58, 0, 0, ocr0, 4, 2);
        if (r1_58 != 0xFF) {
            s_spi_diag.raw_cmd58_ocr = ((uint32_t)ocr0[0] << 24) |
                                       ((uint32_t)ocr0[1] << 16) |
                                       ((uint32_t)ocr0[2] << 8) |
                                       (uint32_t)ocr0[3];
        }
    }
    for (int p = 0; p < 50; p++) {
        uint8_t r1_55 = bsp_sdcard_raw_cmd_ex(brd, true, 55, 0, 0, NULL, 0, 2);
        uint8_t r1_41 = bsp_sdcard_raw_cmd_ex(brd, true, 41, 0x40000000U, 0, NULL, 0, 256);
        s_spi_diag.raw_acmd41_r1 = r1_41;
        if (r1_41 == 0x00) {
            bsp_sdcard_set_cs_expander(brd, false);
            uint8_t ocr_b[4] = {0};
            bsp_sdcard_raw_cmd(brd, false, 58, 0, ocr_b, 4);
            s_spi_diag.raw_cmd58_ocr = ((uint32_t)ocr_b[0] << 24) |
                                       ((uint32_t)ocr_b[1] << 16) |
                                       ((uint32_t)ocr_b[2] << 8) |
                                       (uint32_t)ocr_b[3];
            bsp_sdcard_raw_cmd(brd, false, 59, 1, NULL, 0);
            s_spi_diag.raw_stage = 10;
            ESP_LOGI(TAG, "Raw SPI handshake succeeded in stage 0 at poll %d (cmd55=0x%02x OCR=0x%08lx)",
                     p, r1_55, (unsigned long)s_spi_diag.raw_cmd58_ocr);
            return true;
        }
        /* 256 bytes CS LOW + 256 bytes CS HIGH = 4096 continuous SCLK cycles (10.24 ms via DMA) */
        bsp_sdcard_clock_ff(256);
    }

    /*
     * Multi-mode clean SPI initialization sequences with continuous SCLK (SD Spec Section 4.4 & Fig 7-2):
     *   mode 1: CS-framed, CMD0(0) -> CMD8(0x1AA) -> CMD58(0) -> 120 polls of CMD55(0)+ACMD41 with continuous DMA SCLK
     *   mode 2: CS held LOW, CMD0(0) -> CMD8(0x1AA) -> CMD58(0) -> 100 polls of CMD55(0)+ACMD41 with continuous DMA SCLK
     *   mode 3: CS-framed, CMD0(0) -> CMD8(0x1AA) -> CMD59(1) -> CMD58(0) -> 60 polls where polls 1..3 re-send CMD8(0x1AA)
     *   mode 4: CS-framed, CMD0(0) -> 30 polls SDSC ACMD41(0) + 30 polls MMC CMD1(0x40FF8000)
     */
    for (uint32_t mode = 1; mode <= 4; mode++) {
        bool frame_cs = (mode != 2);

        bsp_sdcard_deselect_other_cs(brd);
        s_sd_cs_level = -1;
        bsp_sdcard_set_cs_expander(brd, true);
        esp_rom_delay_us(200);
        bsp_sdcard_clock_ff(128);
        esp_rom_delay_us(100);

        if (!frame_cs) {
            bsp_sdcard_set_cs_expander(brd, false);
            bsp_sdcard_clock_ff(2);
        }

        /* Single clean CMD0(0) to enter SPI Idle state */
        uint8_t r1_0 = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 0, 0, 0, NULL, 0, 2);
        if (r1_0 != 0x01) {
            esp_rom_delay_us(1000);
            r1_0 = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 0, 0, 0, NULL, 0, 2);
        }
        s_spi_diag.raw_cmd0_r1 = r1_0;

        if (mode <= 3) {
            uint8_t r7_b[4] = {0};
            uint8_t r1_8 = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 8, 0x1AA, 0, r7_b, 4, 2);
            if (r1_8 != 0xFF) {
                s_spi_diag.raw_cmd8_r7 = ((uint32_t)r7_b[0] << 24) |
                                         ((uint32_t)r7_b[1] << 16) |
                                         ((uint32_t)r7_b[2] << 8) |
                                         (uint32_t)r7_b[3];
            }
            if (mode == 3) {
                bsp_sdcard_raw_cmd_ex(brd, frame_cs, 59, 1, 0, NULL, 0, 2);
            }
            uint8_t ocr_b[4] = {0};
            uint8_t r1_58 = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 58, 0, 0, ocr_b, 4, 2);
            if (r1_58 != 0xFF) {
                s_spi_diag.raw_cmd58_ocr = ((uint32_t)ocr_b[0] << 24) |
                                           ((uint32_t)ocr_b[1] << 16) |
                                           ((uint32_t)ocr_b[2] << 8) |
                                           (uint32_t)ocr_b[3];
            }
        }

        int max_polls = (mode == 1) ? 120 : (mode == 2) ? 100 : 60;
        for (int p = 0; p < max_polls; p++) {
            uint8_t r1_init = 0xFF;
            if (mode == 3 && p >= 1 && p <= 3) {
                /* Re-latch CMD8(0x1AA) + CMD58(0) without CMD0 in case poll 0 caused a one-time brownout reset */
                uint8_t r7_b[4] = {0};
                bsp_sdcard_raw_cmd_ex(brd, frame_cs, 8, 0x1AA, 0, r7_b, 4, 2);
                uint8_t ocr_b[4] = {0};
                bsp_sdcard_raw_cmd_ex(brd, frame_cs, 58, 0, 0, ocr_b, 4, 2);
            }

            if (mode == 4 && p >= 30) {
                /* MMC CMD1 */
                r1_init = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 1, 0x40FF8000U, 0, NULL, 0, 256);
                s_spi_diag.raw_cmd1_r1 = r1_init;
            } else {
                uint32_t acmd_arg = 0x40000000U;
                if (mode == 1) {
                    acmd_arg = (p < 60) ? 0x40000000U : (p < 90) ? 0x50000000U : 0x40FF8000U;
                } else if (mode == 4) {
                    acmd_arg = 0x00000000U;
                }
                bsp_sdcard_raw_cmd_ex(brd, frame_cs, 55, 0, 0, NULL, 0, 2);
                r1_init = bsp_sdcard_raw_cmd_ex(brd, frame_cs, 41, acmd_arg, 0, NULL, 0, frame_cs ? 256 : 512);
                s_spi_diag.raw_acmd41_r1 = r1_init;
            }

            if (r1_init == 0x00) {
                bsp_sdcard_set_cs_expander(brd, false);
                uint8_t ocr_b[4] = {0};
                bsp_sdcard_raw_cmd(brd, false, 58, 0, ocr_b, 4);
                s_spi_diag.raw_cmd58_ocr = ((uint32_t)ocr_b[0] << 24) |
                                           ((uint32_t)ocr_b[1] << 16) |
                                           ((uint32_t)ocr_b[2] << 8) |
                                           (uint32_t)ocr_b[3];
                /* Enable CRC (CMD59 arg=1) to match sdspi_host expectation */
                bsp_sdcard_raw_cmd(brd, false, 59, 1, NULL, 0);
                s_spi_diag.raw_stage = mode;
                ESP_LOGI(TAG, "Raw SPI handshake succeeded in mode %lu at poll %d (OCR=0x%08lx)",
                         (unsigned long)mode, p, (unsigned long)s_spi_diag.raw_cmd58_ocr);
                return true;
            }

            if (frame_cs) {
                /* 256 bytes CS LOW (inside raw_cmd_ex) + 256 bytes CS HIGH = 512 bytes (10.24 ms continuous DMA SCLK) */
                bsp_sdcard_clock_ff(256);
            }
        }
    }

    /*
     * Diagnostic probe if all stages stayed in 0x01:
     *   1. CMD0(0) -> CMD8(0x1AA) -> CMD59(1) -> CMD55(0) + ACMD41(0x40000000) -> 512 continuous clock bytes
     *   2. Send CMD55(0) with deliberate bad CRC (0x01): high byte 0x09 proves CRC-on persisted across ACMD41
     *   3. Send CMD9(0) (SEND_CSD) with valid CRC: low byte records whether card accepts data commands or returns 0x05/0x01
     */
    {
        bsp_sdcard_raw_cmd_ex(brd, true, 0, 0, 0, NULL, 0, 2);
        uint8_t r7_b[4] = {0};
        bsp_sdcard_raw_cmd_ex(brd, true, 8, 0x1AA, 0, r7_b, 4, 2);
        bsp_sdcard_raw_cmd_ex(brd, true, 59, 1, 0, NULL, 0, 2);
        bsp_sdcard_raw_cmd_ex(brd, true, 55, 0, 0, NULL, 0, 2);
        bsp_sdcard_raw_cmd_ex(brd, true, 41, 0x40000000U, 0, NULL, 0, 256);
        bsp_sdcard_clock_ff(256);
        uint8_t r1_post_acmd41_bad_crc = bsp_sdcard_raw_cmd_ex(brd, true, 55, 0, 0x01U, NULL, 0, 2);
        uint8_t r1_cmd9 = bsp_sdcard_raw_cmd_ex(brd, true, 9, 0, 0, NULL, 0, 2);
        s_spi_diag.raw_cmd13_r2 = ((uint16_t)r1_post_acmd41_bad_crc << 8) | r1_cmd9;
    }

    bsp_sdcard_set_cs_expander(brd, false);
    ESP_LOGW(TAG, "Raw SPI handshake exhausted: cmd0=0x%02x cmd8=0x%08lx ocr=0x%08lx acmd41=0x%02x cmd1=0x%02x post_acmd41_diag=0x%04x",
             s_spi_diag.raw_cmd0_r1,
             (unsigned long)s_spi_diag.raw_cmd8_r7,
             (unsigned long)s_spi_diag.raw_cmd58_ocr,
             s_spi_diag.raw_acmd41_r1,
             s_spi_diag.raw_cmd1_r1,
             s_spi_diag.raw_cmd13_r2);
    return false;
}

static esp_err_t bsp_sdcard_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    const board_res_desc_t *brd = bsp_board_get_description();
    uint32_t opcode = cmdinfo->opcode;
    bool is_acmd = (s_prev_opcode == MMC_APP_CMD);

    /*
     * Block SDIO-only probe commands (CMD52 SD_IO_RW_DIRECT and CMD5 SD_IO_SEND_OP_COND)
     * from being transmitted to the microSD card slot.
     */
    if (!is_acmd && (opcode == SD_IO_RW_DIRECT || opcode == SD_IO_SEND_OP_COND)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /*
     * Phase 1 of sdmmc_send_cmd_go_idle_state():
     *   sdspi_host_do_transaction() only runs go_idle_clockout() (80 clocks) when !(flags & SCF_RSP_R1).
     *   Keep SD_CS HIGH during those clocks and during the 20 ms pre-CMD0 delay (SD Spec 6.4.1.1).
     */
    if (opcode == MMC_GO_IDLE_STATE && (cmdinfo->flags & SCF_RSP_R1) == 0) {
        uint16_t raw_pins = 0;
        int det = bsp_sdcard_read_sd_det_level(&raw_pins);
        memset(&s_spi_diag, 0, sizeof(s_spi_diag));
        s_spi_diag.valid = true;
        s_spi_diag.sd_det_level = det;
        s_spi_diag.io_in_raw = raw_pins;
        s_prev_opcode = 0xFFFF;

        bsp_sdcard_deselect_other_cs(brd);
        s_sd_cs_level = -1;
        bsp_sdcard_set_cs_expander(brd, true);
        esp_rom_delay_us(200);
        bsp_sdcard_clock_ff(20);
        esp_err_t ret = sdspi_host_do_transaction(slot, cmdinfo);
        s_prev_opcode = opcode;
        return ret;
    }

    /* Phase 2 of CMD0: assert CS LOW, clock 2 setup bytes of 0xFF, send GO_IDLE_STATE, and keep CS LOW (matching v0.8.28) */
    if (opcode == MMC_GO_IDLE_STATE && (cmdinfo->flags & SCF_RSP_R1) != 0) {
        bsp_sdcard_set_cs_expander(brd, false);
        bsp_sdcard_clock_ff(2);
        esp_err_t ret = sdspi_host_do_transaction(slot, cmdinfo);
        if (ret != ESP_OK) {
            bsp_sdcard_set_cs_expander(brd, true);
            esp_rom_delay_us(200);
            bsp_sdcard_clock_ff(20);
            bsp_sdcard_set_cs_expander(brd, false);
            bsp_sdcard_clock_ff(2);
            ret = sdspi_host_do_transaction(slot, cmdinfo);
        }
        s_spi_diag.cmd0_count++;
        s_spi_diag.cmd0_err = ret;
        s_spi_diag.cmd0_r1 = cmdinfo->response[0];
        s_spi_diag.last_cmd_opcode = opcode;
        s_spi_diag.last_cmd_err = ret;
        s_prev_opcode = opcode;
        ESP_LOGI(TAG, "SD SPI CMD0: err=%s r1=0x%08lx (SD_DET=%d io=0x%04x)",
                 esp_err_to_name(ret), (unsigned long)cmdinfo->response[0],
                 s_spi_diag.sd_det_level, (unsigned)s_spi_diag.io_in_raw);
        return ret;
    }

    /* Ensure SD_CS is asserted LOW before the command */
    bsp_sdcard_set_cs_expander(brd, false);
    esp_err_t ret = sdspi_host_do_transaction(slot, cmdinfo);

    if (!is_acmd && opcode == SD_SEND_IF_COND) {
        s_spi_diag.cmd8_count++;
        s_spi_diag.cmd8_err = ret;
        s_spi_diag.cmd8_r7 = cmdinfo->response[0];
        ESP_LOGI(TAG, "SD SPI CMD8: err=%s r7=0x%08lx",
                 esp_err_to_name(ret), (unsigned long)cmdinfo->response[0]);
    } else if (!is_acmd && opcode == SD_READ_OCR) {
        s_spi_diag.cmd58_count++;
        s_spi_diag.cmd58_err = ret;
        s_spi_diag.cmd58_ocr = SD_SPI_R3(cmdinfo->response);
        ESP_LOGI(TAG, "SD SPI CMD58 (READ_OCR): err=%s ocr=0x%08lx",
                 esp_err_to_name(ret), (unsigned long)s_spi_diag.cmd58_ocr);
    } else if (!is_acmd && opcode == MMC_APP_CMD) {
        s_spi_diag.cmd55_count++;
        s_spi_diag.cmd55_err = ret;
        s_spi_diag.cmd55_r1 = cmdinfo->response[0];
    } else if (is_acmd && opcode == SD_APP_OP_COND) {
        s_spi_diag.acmd41_count++;
        s_spi_diag.acmd41_err = ret;
        s_spi_diag.acmd41_r1 = cmdinfo->response[0];
        s_spi_diag.acmd41_last_arg = cmdinfo->arg;
        if (s_spi_diag.acmd41_count <= 3 ||
            (s_spi_diag.acmd41_count % 25) == 0 ||
            (ret == ESP_OK && (cmdinfo->response[0] & SD_SPI_R1_IDLE_STATE) == 0)) {
            ESP_LOGI(TAG, "SD SPI ACMD41[%lu]: arg=0x%08lx err=%s r1=0x%08lx",
                     (unsigned long)s_spi_diag.acmd41_count,
                     (unsigned long)cmdinfo->arg,
                     esp_err_to_name(ret),
                     (unsigned long)cmdinfo->response[0]);
        }

        if (ret == ESP_OK && (cmdinfo->response[0] & SD_SPI_R1_IDLE_STATE) != 0) {
            /* Supply 256 continuous SCLK cycles (32 x 0xFF) while CS is LOW */
            bsp_sdcard_clock_ff(32);

            /*
             * If 30 standard v0.8.28 ACMD41 polls (~150 ms) have not cleared IDLE_STATE,
             * run the multi-stage raw SPI handshake AFTER ACMD41 completes (never between CMD55 and ACMD41).
             */
            if (s_spi_diag.acmd41_count == 30) {
                if (bsp_sdcard_raw_spi_handshake(brd)) {
                    cmdinfo->response[0] = 0x00;
                    s_spi_diag.acmd41_r1 = 0x00;
                    ret = ESP_OK;
                } else {
                    ret = ESP_ERR_TIMEOUT;
                }
            } else if (s_spi_diag.acmd41_count > 30) {
                /* Return 3 consecutive timeouts so sdmmc_send_cmd_send_op_cond exits immediately */
                ret = ESP_ERR_TIMEOUT;
            }
        }
    }

    s_spi_diag.last_cmd_opcode = is_acmd ? (0x80U | opcode) : opcode;
    s_spi_diag.last_cmd_err = ret;
    s_prev_opcode = opcode;
    return ret;
}

static esp_err_t bsp_sdcard_ensure_spi_bus(const board_res_desc_t *brd)
{
    /* Always enable internal pull-ups on SPI3 MISO/MOSI/SCLK to prevent floating MISO
     * or false start bits during SD card initialization. */
    if (brd->GPIO_SDSPI_MISO != GPIO_NUM_NC) {
        gpio_pullup_en(brd->GPIO_SDSPI_MISO);
    }
    if (brd->GPIO_SDSPI_MOSI != GPIO_NUM_NC) {
        gpio_pullup_en(brd->GPIO_SDSPI_MOSI);
    }
    if (brd->GPIO_SDSPI_SCLK != GPIO_NUM_NC) {
        gpio_pullup_en(brd->GPIO_SDSPI_SCLK);
    }

    if (!s_spi_bus_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = brd->GPIO_SDSPI_MOSI,
            .miso_io_num = brd->GPIO_SDSPI_MISO,
            .sclk_io_num = brd->GPIO_SDSPI_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4000,
        };
        esp_err_t ret = spi_bus_initialize(brd->SDSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus (%s).", esp_err_to_name(ret));
            return ret;
        }
        s_spi_bus_initialized = true;
    }

    if (s_clk_spi_handle == NULL) {
        spi_device_interface_config_t clk_devcfg = {
            .clock_speed_hz = SDMMC_FREQ_PROBING * 1000,
            .mode = 0,
            .spics_io_num = GPIO_NUM_NC,
            .queue_size = 1,
        };
        esp_err_t clk_ret = spi_bus_add_device(brd->SDSPI_HOST, &clk_devcfg, &s_clk_spi_handle);
        if (clk_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add SPI clock helper device (%s)", esp_err_to_name(clk_ret));
            s_clk_spi_handle = NULL;
        }
    }

    return ESP_OK;
}

static esp_err_t bsp_sdcard_raw_init(sdmmc_card_t **out_card)
{
    const board_res_desc_t *brd = bsp_board_get_description();
    if (!brd->FUNC_SDMMC_EN && !brd->FUNC_SDSPI_EN) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Update physical SD_DET state before attempting SPI initialization */
    bsp_sdcard_is_physically_detected();

    sdmmc_card_t *temp_card = calloc(1, sizeof(sdmmc_card_t));
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
        host.do_transaction = bsp_sdcard_do_transaction;

        ret = bsp_sdcard_ensure_spi_bus(brd);
        if (ret != ESP_OK) {
            free(temp_card);
            return ret;
        }

        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = brd->GPIO_SDSPI_CS;
        slot_config.host_id = host.slot;

        ret = sdspi_host_init();
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            sdspi_dev_handle_t dev_handle = -1;
            ret = sdspi_host_init_device(&slot_config, &dev_handle);
            if (ret == ESP_OK) {
                host.slot = dev_handle;
                ret = sdmmc_card_init(&host, temp_card);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "sdmmc_card_init failed (%s): SD_DET=%d CMD0=%s(0x%02lx) CMD8=%s(0x%08lx) CMD58=%s(0x%08lx) ACMD41=%lu/%s(0x%02lx)",
                             esp_err_to_name(ret),
                             s_spi_diag.sd_det_level,
                             esp_err_to_name(s_spi_diag.cmd0_err), (unsigned long)s_spi_diag.cmd0_r1,
                             esp_err_to_name(s_spi_diag.cmd8_err), (unsigned long)s_spi_diag.cmd8_r7,
                             esp_err_to_name(s_spi_diag.cmd58_err), (unsigned long)s_spi_diag.cmd58_ocr,
                             (unsigned long)s_spi_diag.acmd41_count,
                             esp_err_to_name(s_spi_diag.acmd41_err), (unsigned long)s_spi_diag.acmd41_r1);
                    bsp_sdcard_set_cs_expander(brd, true);
                    bsp_sdcard_clock_ff(2);
                    sdspi_host_remove_device(dev_handle);
                }
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

static void bsp_sdcard_raw_deinit(sdmmc_card_t *temp_card)
{
    if (!temp_card) {
        return;
    }
    const board_res_desc_t *brd = bsp_board_get_description();
    bsp_sdcard_set_cs_expander(brd, true);
    bsp_sdcard_clock_ff(2);
    if (temp_card->host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
        if (temp_card->host.deinit_p) {
            temp_card->host.deinit_p(temp_card->host.slot);
        }
    } else {
        if (temp_card->host.deinit) {
            temp_card->host.deinit();
        }
    }
    free(temp_card);
}

/**
 * @brief Validate whether a 512-byte sector is a valid FAT12/FAT16/FAT32 VBR
 *        using the same checks as FatFs check_fs() / find_volume().
 */
static bool bsp_sdcard_check_fat_vbr(const uint8_t *buf, char *out_fs_type, size_t fs_type_len)
{
    if (!buf) {
        return false;
    }
    uint16_t sig = (uint16_t)buf[510] | ((uint16_t)buf[511] << 8);
    if (sig != 0xAA55) {
        return false;
    }

    bool has_jmp = (buf[0] == 0xE9 || (buf[0] == 0xEB && buf[2] == 0x90));
    bool has_fat_str16 = (memcmp(buf + 54, "FAT", 3) == 0);
    bool has_fat_str32 = (memcmp(buf + 82, "FAT", 3) == 0);
    if (!has_jmp && !has_fat_str16 && !has_fat_str32) {
        return false;
    }

    uint16_t bps = (uint16_t)buf[11] | ((uint16_t)buf[12] << 8);
    if (bps != 512) {
        return false;
    }

    uint8_t spc = buf[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        return false;
    }

    uint16_t rsv = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
    if (rsv == 0) {
        return false;
    }

    uint8_t n_fats = buf[16];
    if (n_fats != 1 && n_fats != 2) {
        return false;
    }

    uint16_t nrdir = (uint16_t)buf[17] | ((uint16_t)buf[18] << 8);
    if (nrdir != 0 && (nrdir % 16) != 0) {
        return false;
    }

    uint16_t tot16 = (uint16_t)buf[19] | ((uint16_t)buf[19 + 1] << 8);
    uint32_t tot32 = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
                     ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
    uint32_t totsect = (tot16 != 0) ? tot16 : tot32;
    if (totsect == 0) {
        return false;
    }

    uint16_t fsize16 = (uint16_t)buf[22] | ((uint16_t)buf[23] << 8);
    uint32_t fsize32 = (uint32_t)buf[36] | ((uint32_t)buf[37] << 8) |
                       ((uint32_t)buf[38] << 16) | ((uint32_t)buf[39] << 24);
    uint32_t fasize = (fsize16 != 0) ? fsize16 : fsize32;
    if (fasize == 0) {
        return false;
    }

    uint32_t sysect = (uint32_t)rsv + (uint32_t)n_fats * fasize + ((uint32_t)nrdir / 16U);
    if (totsect < sysect + spc) {
        return false;
    }

    uint32_t nclst = (totsect - sysect) / spc;
    if (nclst == 0) {
        return false;
    }

    if (out_fs_type && fs_type_len > 0) {
        if (fsize16 == 0 && nrdir == 0) {
            snprintf(out_fs_type, fs_type_len, "FAT32");
        } else if (nclst >= 4085) {
            snprintf(out_fs_type, fs_type_len, "FAT16");
        } else {
            snprintf(out_fs_type, fs_type_len, "FAT12");
        }
    }
    return true;
}

/**
 * @brief Inspect Sector 0 (and MBR partition VBRs if present) before touching VFS.
 *        Returns true if a mountable FAT12/16/32 volume exists, false otherwise.
 */
static bool bsp_sdcard_inspect_card(sdmmc_card_t *temp_card,
                                    char *out_fs_type, size_t fs_type_len,
                                    char *out_reason, size_t reason_len)
{
    if (out_fs_type && fs_type_len > 0) out_fs_type[0] = '\0';
    if (out_reason && reason_len > 0) out_reason[0] = '\0';

    uint8_t *sector0 = malloc(512);
    if (!sector0) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Memory Error");
        if (out_reason) snprintf(out_reason, reason_len, "Out of memory while inspecting SD card.");
        return false;
    }

    esp_err_t read_err = sdmmc_read_sectors(temp_card, sector0, 0, 1);
    if (read_err != ESP_OK) {
        free(sector0);
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Read Error");
        if (out_reason) {
            snprintf(out_reason, reason_len, "Failed to read sector 0 (%s). Card may be damaged or incompatible.",
                     esp_err_to_name(read_err));
        }
        return false;
    }

    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 512; i++) {
        if (sector0[i] != 0x00) all_zero = false;
        if (sector0[i] != 0xFF) all_ff = false;
    }
    if (all_zero || all_ff) {
        free(sector0);
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Unformatted (Blank)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is blank/unformatted. Format the card as FAT32 in the Web UI to use it.");
        }
        return false;
    }

    if (memcmp(sector0 + 3, "EXFAT   ", 8) == 0) {
        free(sector0);
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "exFAT (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as exFAT, which is not supported. Format the card as FAT32 in the Web UI.");
        }
        return false;
    }
    if (memcmp(sector0 + 3, "NTFS    ", 8) == 0) {
        free(sector0);
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "NTFS (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as NTFS, which is not supported. Format the card as FAT32 in the Web UI.");
        }
        return false;
    }

    uint16_t sig = (uint16_t)sector0[510] | ((uint16_t)sector0[511] << 8);
    if (sig != 0xAA55) {
        free(sector0);
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Unformatted / Raw");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card has no valid boot signature (0x%04X != 0xAA55). Format the card as FAT32 in the Web UI.",
                     sig);
        }
        return false;
    }

    // Check if Sector 0 itself is a superfloppy FAT VBR
    if (bsp_sdcard_check_fat_vbr(sector0, out_fs_type, fs_type_len)) {
        free(sector0);
        if (out_reason) snprintf(out_reason, reason_len, "Mounted and ready (%s superfloppy)", out_fs_type);
        return true;
    }

    // Inspect MBR partition table entries
    int valid_partitions = 0;
    uint8_t first_part_type = 0;
    bool saw_exfat_vbr = false;
    bool saw_ntfs_vbr = false;

    uint8_t *vbr = malloc(512);
    for (int i = 0; i < 4; i++) {
        int offset = 446 + (i * 16);
        uint8_t ptype = sector0[offset + 4];
        uint32_t lba_start = (uint32_t)sector0[offset + 8] |
                             ((uint32_t)sector0[offset + 9] << 8) |
                             ((uint32_t)sector0[offset + 10] << 16) |
                             ((uint32_t)sector0[offset + 11] << 24);
        uint32_t sector_count = (uint32_t)sector0[offset + 12] |
                                ((uint32_t)sector0[offset + 13] << 8) |
                                ((uint32_t)sector0[offset + 14] << 16) |
                                ((uint32_t)sector0[offset + 15] << 24);

        if (ptype != 0 && lba_start != 0 && sector_count != 0) {
            valid_partitions++;
            if (first_part_type == 0) {
                first_part_type = ptype;
            }
            if (vbr && sdmmc_read_sectors(temp_card, vbr, lba_start, 1) == ESP_OK) {
                if (bsp_sdcard_check_fat_vbr(vbr, out_fs_type, fs_type_len)) {
                    free(vbr);
                    free(sector0);
                    if (out_reason) {
                        snprintf(out_reason, reason_len, "Valid %s partition at LBA %lu",
                                 out_fs_type, (unsigned long)lba_start);
                    }
                    return true;
                }
                if (memcmp(vbr + 3, "EXFAT   ", 8) == 0) {
                    saw_exfat_vbr = true;
                } else if (memcmp(vbr + 3, "NTFS    ", 8) == 0) {
                    saw_ntfs_vbr = true;
                }
            }
        }
    }

    if (vbr) {
        free(vbr);
    }
    free(sector0);

    if (valid_partitions == 0) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Unformatted (No Partitions)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card has no partitions or filesystem. Format the card as FAT32 in the Web UI to use it.");
        }
    } else if (saw_exfat_vbr) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "exFAT (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as exFAT (partition 0x%02X). Only FAT32/FAT16 is supported; format as FAT32 in the Web UI.",
                     first_part_type);
        }
    } else if (saw_ntfs_vbr) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "NTFS (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as NTFS (partition 0x%02X). Only FAT32/FAT16 is supported; format as FAT32 in the Web UI.",
                     first_part_type);
        }
    } else if (first_part_type == 0x07) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "exFAT / NTFS (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as exFAT/NTFS (partition 0x07). Only FAT32/FAT16 is supported; format as FAT32 in the Web UI.");
        }
    } else if (first_part_type == 0x83) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Linux ext2/3/4 (Unsupported)");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card is formatted as Linux ext2/3/4 (partition 0x83). Only FAT32/FAT16 is supported; format as FAT32 in the Web UI.");
        }
    } else if (first_part_type == 0xEE) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "GPT Partition Table");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card uses a GPT partition table (0xEE) without a mountable FAT volume. Format as FAT32 in the Web UI.");
        }
    } else if (first_part_type == 0x01 || first_part_type == 0x04 || first_part_type == 0x06 ||
               first_part_type == 0x0B || first_part_type == 0x0C || first_part_type == 0x0E) {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Unformatted / Corrupted FAT");
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card has a FAT partition entry (0x%02X) with an unformatted or corrupted boot sector. Format as FAT32 in the Web UI.",
                     first_part_type);
        }
    } else {
        if (out_fs_type) snprintf(out_fs_type, fs_type_len, "Unsupported (0x%02X)", first_part_type);
        if (out_reason) {
            snprintf(out_reason, reason_len,
                     "SD card has unsupported partition type 0x%02X. Format as FAT32 in the Web UI to use it.",
                     first_part_type);
        }
    }

    return false;
}

void bsp_sdcard_get_status_info(bsp_sdcard_status_info_t *out_info)
{
    if (!out_info) {
        return;
    }
    *out_info = s_card_status;
    out_info->mounted = (card != NULL);
    if (card != NULL) {
        out_info->present = true;
        out_info->unusable = false;
        uint64_t cap = (uint64_t)card->csd.capacity * (card->csd.sector_size ? card->csd.sector_size : 512);
        out_info->capacity_gb = (double)cap / (1024.0 * 1024.0 * 1024.0);
        if (out_info->fs_type[0] == '\0') {
            snprintf(out_info->fs_type, sizeof(out_info->fs_type), "FAT32");
        }
        if (out_info->reason[0] == '\0') {
            snprintf(out_info->reason, sizeof(out_info->reason), "Mounted and ready");
        }
    }
}

esp_err_t bsp_sdcard_init(char *mount_point, size_t max_files, bool format_if_mount_failed)
{
    if (NULL != card) {
        return ESP_ERR_INVALID_STATE;
    }
    const board_res_desc_t *brd = bsp_board_get_description();

    /* Check if SD card is supported */
    if (!brd->FUNC_SDMMC_EN && !brd->FUNC_SDSPI_EN) {
        ESP_LOGE(TAG, "SDMMC and SDSPI not supported on this board!");
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret_val = ESP_OK;

    /*
     * Pre-validate card presence and FAT volume structure via raw SD/SPI read
     * BEFORE calling esp_vfs_fat_sdspi_mount() when auto-format is disabled.
     * This prevents esp_vfs_fat_mount_internal() from registering and immediately
     * unregistering a transient slot in ESP-IDF's global s_vfs[] table when no card
     * is inserted or when an unformatted/exFAT/non-FAT card is inserted.
     */
    char detected_fs[32] = "";
    char detected_reason[160] = "";
    double detected_cap_gb = 0.0;

    if (!format_if_mount_failed) {
        sdmmc_card_t *probe_card = NULL;
        esp_err_t raw_err = bsp_sdcard_raw_init(&probe_card);
        if (raw_err != ESP_OK) {
            bool phys_present = bsp_sdcard_is_physically_detected();
            s_card_status.mounted = false;
            s_card_status.present = phys_present;
            s_card_status.unusable = phys_present;
            s_card_status.capacity_gb = 0.0;
            if (phys_present) {
                if (s_spi_diag.cmd0_r1 == 0x01 && s_spi_diag.cmd8_r7 == 0x000001AAU && s_spi_diag.acmd41_r1 == 0x01) {
                    snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "Card Busy (ACMD41)");
                    snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                             "Card responds on SPI (CMD0=0x01, CMD8=0x1AA, OCR=0x%08lx) but controller stays busy in ACMD41 (0x01); eject/reinsert or replace card",
                             (unsigned long)s_spi_diag.raw_cmd58_ocr);
                } else {
                    snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "SPI Init Failed");
                    snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                             "Card detected (SD_DET=0), SPI init failed (%s): CMD0=%s(0x%02lx) CMD8=%s(0x%08lx) ACMD41=%lu(0x%02lx)",
                             esp_err_to_name(raw_err),
                             esp_err_to_name(s_spi_diag.cmd0_err), (unsigned long)s_spi_diag.cmd0_r1,
                             esp_err_to_name(s_spi_diag.cmd8_err), (unsigned long)s_spi_diag.cmd8_r7,
                             (unsigned long)s_spi_diag.acmd41_count, (unsigned long)s_spi_diag.acmd41_r1);
                }
            } else {
                snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "No Card");
                snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                         "No SD card detected (%s)", esp_err_to_name(raw_err));
            }
            ESP_LOGW(TAG, "SD card pre-check failed (%s, SD_DET=%d); skipping VFS mount.",
                     esp_err_to_name(raw_err), s_spi_diag.sd_det_level);
            return raw_err;
        }

        uint64_t cap_bytes = (uint64_t)probe_card->csd.capacity *
                             (probe_card->csd.sector_size ? probe_card->csd.sector_size : 512);
        detected_cap_gb = (double)cap_bytes / (1024.0 * 1024.0 * 1024.0);

        bool mountable = bsp_sdcard_inspect_card(probe_card,
                                                 detected_fs, sizeof(detected_fs),
                                                 detected_reason, sizeof(detected_reason));
        bsp_sdcard_raw_deinit(probe_card);

        if (!mountable) {
            s_card_status.mounted = false;
            s_card_status.present = true;
            s_card_status.unusable = true;
            s_card_status.capacity_gb = detected_cap_gb;
            snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "%s", detected_fs);
            snprintf(s_card_status.reason, sizeof(s_card_status.reason), "%s", detected_reason);
            ESP_LOGW(TAG, "SD card present (%.2f GB) but unusable [%s]: %s",
                     detected_cap_gb, detected_fs, detected_reason);
            return ESP_FAIL;
        }
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = max_files,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = {0};
    if (brd->FUNC_SDMMC_EN) {
        sdmmc_host_t h = SDMMC_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
    } else {
        sdmmc_host_t h = SDSPI_HOST_DEFAULT();
        memcpy(&host, &h, sizeof(sdmmc_host_t));
        host.slot = brd->SDSPI_HOST;
        host.do_transaction = bsp_sdcard_do_transaction;
        ret_val = bsp_sdcard_ensure_spi_bus(brd);
        if (ret_val != ESP_OK) {
            return ret_val;
        }
    }

    if (brd->FUNC_SDMMC_EN) {
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
        ret_val = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
    } else {
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = brd->GPIO_SDSPI_CS;
        slot_config.host_id = host.slot;
        ret_val = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    }

    /* Check for SDMMC mount result. */
    if (ret_val != ESP_OK) {
        s_card_status.mounted = false;
        if (ret_val == ESP_FAIL) {
            s_card_status.present = true;
            s_card_status.unusable = true;
            s_card_status.capacity_gb = detected_cap_gb;
            snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type),
                     detected_fs[0] ? detected_fs : "Unmountable FAT");
            snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                     "Failed to mount FAT filesystem. Card may be corrupted or unformatted; format as FAT32 in the Web UI.");
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            bool phys_present = bsp_sdcard_is_physically_detected();
            s_card_status.present = phys_present;
            s_card_status.unusable = phys_present;
            s_card_status.capacity_gb = 0.0;
            if (phys_present) {
                snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "SPI Init Failed");
                snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                         "Card detected (SD_DET=0), SPI init failed (%s): CMD0=%s(0x%02lx) CMD8=%s(0x%08lx) ACMD41=%lu(0x%02lx)",
                         esp_err_to_name(ret_val),
                         esp_err_to_name(s_spi_diag.cmd0_err), (unsigned long)s_spi_diag.cmd0_r1,
                         esp_err_to_name(s_spi_diag.cmd8_err), (unsigned long)s_spi_diag.cmd8_r7,
                         (unsigned long)s_spi_diag.acmd41_count, (unsigned long)s_spi_diag.acmd41_r1);
            } else {
                snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "No Card");
                snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                         "Failed to initialize card (%s)", esp_err_to_name(ret_val));
            }
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret_val));
        }
        return ret_val;
    }

    /* Card has been initialized, update status and print its properties. */
    uint64_t cap_bytes = (uint64_t)card->csd.capacity * (card->csd.sector_size ? card->csd.sector_size : 512);
    s_card_status.mounted = true;
    s_card_status.present = true;
    s_card_status.unusable = false;
    s_card_status.capacity_gb = (double)cap_bytes / (1024.0 * 1024.0 * 1024.0);
    snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "%s",
             detected_fs[0] ? detected_fs : "FAT32");
    snprintf(s_card_status.reason, sizeof(s_card_status.reason), "Mounted and ready");

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
    if (NULL == card) {
        s_card_status.mounted = false;
        return ESP_OK;
    }

    /* Unmount an SD card from the FAT filesystem and release resources acquired */
    esp_err_t ret_val = esp_vfs_fat_sdcard_unmount(mount_point, card);

    /* Make SD/MMC card information structure pointer NULL */
    card = NULL;
    const board_res_desc_t *brd = bsp_board_get_description();
    bsp_sdcard_set_cs_expander(brd, true);
    s_card_status.mounted = false;
    s_card_status.unusable = false;
    s_card_status.present = bsp_sdcard_is_physically_detected();
    snprintf(s_card_status.reason, sizeof(s_card_status.reason),
             s_card_status.present ? "Ejected (safe to remove)" : "SD card removed (safe)");
    card = NULL;
    s_card_status.mounted = false;

    return ret_val;
}

esp_err_t bsp_sdcard_deinit_default(void)
{
    return bsp_sdcard_deinit(DEFAULT_MOUNT_POINT);
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
            bool phys_present = bsp_sdcard_is_physically_detected();
            s_card_status.mounted = false;
            s_card_status.present = phys_present;
            s_card_status.unusable = phys_present;
            s_card_status.capacity_gb = 0.0;
            if (phys_present) {
                if (s_spi_diag.cmd0_r1 == 0x01 && s_spi_diag.cmd8_r7 == 0x000001AAU && s_spi_diag.acmd41_r1 == 0x01) {
                    snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "Card Busy (ACMD41)");
                    snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                             "Card responds on SPI (CMD0=0x01, CMD8=0x1AA, OCR=0x%08lx) but controller stays busy in ACMD41 (0x01); eject/reinsert or replace card",
                             (unsigned long)s_spi_diag.raw_cmd58_ocr);
                } else {
                    snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "SPI Init Failed");
                    snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                             "Card detected (SD_DET=0), SPI init failed (%s): CMD0=%s(0x%02lx) CMD8=%s(0x%08lx) ACMD41=%lu(0x%02lx)",
                             esp_err_to_name(err),
                             esp_err_to_name(s_spi_diag.cmd0_err), (unsigned long)s_spi_diag.cmd0_r1,
                             esp_err_to_name(s_spi_diag.cmd8_err), (unsigned long)s_spi_diag.cmd8_r7,
                             (unsigned long)s_spi_diag.acmd41_count, (unsigned long)s_spi_diag.acmd41_r1);
                }
            } else {
                snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "No Card");
                snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                         "No SD card detected (%s)", esp_err_to_name(err));
            }
            snprintf(out_buf, max_len,
                "{\"mounted\":false,\"present\":%s,\"unusable\":%s,\"error\":\"%s\","
                "\"fs_type\":\"%s\",\"status_reason\":\"%s\","
                "\"sd_det\":%d,\"io_in_raw\":\"0x%04X\","
                "\"spi_diag\":{\"cmd0_err\":\"%s\",\"cmd0_r1\":\"0x%02lX\","
                "\"cmd8_err\":\"%s\",\"cmd8_r7\":\"0x%08lX\","
                "\"cmd58_err\":\"%s\",\"cmd58_ocr\":\"0x%08lX\","
                "\"cmd55_r1\":\"0x%02lX\","
                "\"acmd41_count\":%lu,\"acmd41_err\":\"%s\",\"acmd41_r1\":\"0x%02lX\",\"acmd41_arg\":\"0x%08lX\","
                "\"raw_stage\":%lu,\"raw_cmd0\":\"0x%02X\",\"raw_cmd8\":\"0x%08lX\",\"raw_ocr\":\"0x%08lX\","
                "\"raw_acmd41\":\"0x%02X\",\"raw_cmd1\":\"0x%02X\",\"raw_cmd13\":\"0x%04X\","
                "\"last_opcode\":%lu,\"last_err\":\"%s\"}}",
                phys_present ? "true" : "false",
                phys_present ? "true" : "false",
                esp_err_to_name(err),
                s_card_status.fs_type,
                s_card_status.reason,
                s_spi_diag.sd_det_level,
                (unsigned)s_spi_diag.io_in_raw,
                esp_err_to_name(s_spi_diag.cmd0_err), (unsigned long)s_spi_diag.cmd0_r1,
                esp_err_to_name(s_spi_diag.cmd8_err), (unsigned long)s_spi_diag.cmd8_r7,
                esp_err_to_name(s_spi_diag.cmd58_err), (unsigned long)s_spi_diag.cmd58_ocr,
                (unsigned long)s_spi_diag.cmd55_r1,
                (unsigned long)s_spi_diag.acmd41_count,
                esp_err_to_name(s_spi_diag.acmd41_err), (unsigned long)s_spi_diag.acmd41_r1,
                (unsigned long)s_spi_diag.acmd41_last_arg,
                (unsigned long)s_spi_diag.raw_stage,
                (unsigned)s_spi_diag.raw_cmd0_r1,
                (unsigned long)s_spi_diag.raw_cmd8_r7,
                (unsigned long)s_spi_diag.raw_cmd58_ocr,
                (unsigned)s_spi_diag.raw_acmd41_r1,
                (unsigned)s_spi_diag.raw_cmd1_r1,
                (unsigned)s_spi_diag.raw_cmd13_r2,
                (unsigned long)s_spi_diag.last_cmd_opcode,
                esp_err_to_name(s_spi_diag.last_cmd_err));
            return ESP_OK;
        }
        temp_initialized = true;
    }

    uint64_t capacity = (uint64_t)temp_card->csd.capacity * temp_card->csd.sector_size;
    if (capacity == 0) {
        capacity = (uint64_t)temp_card->csd.capacity * 512;
    }
    double cap_gb = (double)capacity / (1024.0 * 1024.0 * 1024.0);

    char inspect_fs[32] = "";
    char inspect_reason[160] = "";
    bool mountable = bsp_sdcard_inspect_card(temp_card,
                                             inspect_fs, sizeof(inspect_fs),
                                             inspect_reason, sizeof(inspect_reason));
    if (card == NULL) {
        s_card_status.mounted = false;
        s_card_status.present = true;
        s_card_status.unusable = !mountable;
        s_card_status.capacity_gb = cap_gb;
        snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "%s", inspect_fs);
        snprintf(s_card_status.reason, sizeof(s_card_status.reason), "%s", inspect_reason);
    }

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

    bsp_sdcard_is_physically_detected();
    snprintf(out_buf, max_len,
             "{\"mounted\":%s,\"present\":true,\"unusable\":%s,\"fs_type\":\"%s\",\"status_reason\":\"%s\","
             "\"sd_det\":%d,\"io_in_raw\":\"0x%04X\","
             "\"name\":\"%s\",\"capacity_gb\":%.2f,\"speed_mhz\":%d,%s%s}",
             (card != NULL) ? "true" : "false",
             (card == NULL && !mountable) ? "true" : "false",
             inspect_fs,
             inspect_reason,
             s_spi_diag.sd_det_level,
             (unsigned)s_spi_diag.io_in_raw,
             clean_name,
             cap_gb,
             temp_card->max_freq_khz / 1000,
             vfs_info,
             part_info);

    if (temp_initialized) {
        bsp_sdcard_raw_deinit(temp_card);
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

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

extern esp_err_t sdmmc_write_sectors_dma(sdmmc_card_t *card, const void *src, size_t start_block, size_t block_count, size_t buffer_len);

static esp_err_t bsp_sdcard_write_multi(sdmmc_card_t *target_card, const void *buf, size_t start_block, size_t block_count)
{
    if (target_card->host.flags & SDMMC_HOST_FLAG_SPI) {
        return sdmmc_write_sectors_dma(target_card, buf, start_block, block_count, block_count * 512U);
    }
    return sdmmc_write_sectors(target_card, buf, start_block, block_count);
}

/**
 * @brief Fast on-device FAT32 formatter using multi-block (CMD25) writes
 *        and large (up to 64 KB) clusters for SDHC/SDXC cards.
 */
static esp_err_t bsp_sdcard_fast_format_fat32(sdmmc_card_t *target_card)
{
    if (!target_card || target_card->csd.capacity <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t sector_size = target_card->csd.sector_size ? (uint32_t)target_card->csd.sector_size : 512U;
    if (sector_size != 512U) {
        ESP_LOGE(TAG, "Fast FAT32 format requires 512-byte sectors (got %lu)", (unsigned long)sector_size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t total_sectors = (uint32_t)target_card->csd.capacity;
    if (total_sectors < 65536U) {
        ESP_LOGE(TAG, "Card too small for fast FAT32 (%lu sectors)", (unsigned long)total_sectors);
        return ESP_ERR_INVALID_SIZE;
    }

    // Align partition start to 4 MB (8192 sectors) erase unit boundary on SDHC/SDXC
    const uint32_t part_start = (total_sectors >= 262144U) ? 8192U : 2048U;
    const uint32_t part_sectors = total_sectors - part_start;

    // Select optimal sectors-per-cluster (up to 128 sectors = 64 KB)
    uint32_t csize = 128U; // 64 KB for >= 8 GB
    if (part_sectors < 16777216U) {
        if (part_sectors >= 8388608U) {
            csize = 64U;   // 32 KB for 4..8 GB
        } else if (part_sectors >= 4194304U) {
            csize = 32U;   // 16 KB for 2..4 GB
        } else if (part_sectors >= 1048576U) {
            csize = 16U;   // 8 KB for 512 MB..2 GB
        } else {
            csize = 8U;    // 4 KB for < 512 MB
        }
    }
    while (csize > 1U && (part_sectors / csize) < 66000U) {
        csize >>= 1;
    }

    // Align reserved sectors and FAT size to 128 sectors (64 KB) so FAT and Data start on 64 KB boundaries.
    // Use 1 FAT copy (matching ESP-IDF f_mkfs() default for flash media) to halve format time and flash wear.
    const uint32_t rsv_sectors = 128U;
    const uint32_t n_fats = 1U;
    uint32_t max_clusters = (part_sectors - rsv_sectors) / csize;
    uint32_t fat_sectors = (max_clusters + 2U + 127U) / 128U;
    fat_sectors = (fat_sectors + 127U) & ~127U; // Round up to multiple of 128 sectors (64 KB)

    if (rsv_sectors + (n_fats * fat_sectors) + csize >= part_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t data_sectors = part_sectors - rsv_sectors - (n_fats * fat_sectors);
    const uint32_t n_clst = data_sectors / csize;
    if (n_clst < 65525U) {
        ESP_LOGE(TAG, "Cluster count %lu too small for FAT32", (unsigned long)n_clst);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate 64 KB (128-sector) multi-block buffer (in SPI mode sdspi_host copies 512B per sector to its internal DMA slot buffer)
    size_t buf_sectors = 128U;
    uint8_t *buf = NULL;
    while (buf_sectors >= 8U) {
        if (target_card->host.flags & SDMMC_HOST_FLAG_SPI) {
            buf = (uint8_t *)heap_caps_malloc(buf_sectors * 512U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                buf = (uint8_t *)malloc(buf_sectors * 512U);
            }
        } else {
            buf = (uint8_t *)heap_caps_malloc(buf_sectors * 512U, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        }
        if (buf != NULL) {
            break;
        }
        buf_sectors >>= 1;
    }
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer for fast FAT32 format");
        return ESP_ERR_NO_MEM;
    }

    const uint32_t fat1_start = part_start + rsv_sectors;
    const uint32_t fat2_start = fat1_start + fat_sectors;
    const uint32_t data_start = fat1_start + (n_fats * fat_sectors);
    const uint32_t total_init_sectors = (n_fats * fat_sectors) + csize;

    ESP_LOGI(TAG,
             "Fast FAT32 format: total=%lu sec (%.2f GB), part_start=%lu, csize=%lu (%lu KB), "
             "fat_sectors=%lu (%.2f MB x%lu), clusters=%lu, write_buf=%u KB",
             (unsigned long)total_sectors,
             ((double)total_sectors * 512.0) / (1024.0 * 1024.0 * 1024.0),
             (unsigned long)part_start,
             (unsigned long)csize,
             (unsigned long)(csize / 2U),
             (unsigned long)fat_sectors,
             ((double)fat_sectors * 512.0) / (1024.0 * 1024.0),
             (unsigned long)n_fats,
             (unsigned long)n_clst,
             (unsigned)(buf_sectors / 2U));

    s_card_status.format_progress = 2;

    // 1. Write MBR at LBA 0 and wipe LBA 1..buf_sectors-1 (clears any leftover GPT headers)
    memset(buf, 0, buf_sectors * 512U);
    uint8_t *pt = buf + 446;
    pt[0] = 0x00;
    pt[1] = 0x20;
    pt[2] = 0x21;
    pt[3] = 0x00;
    pt[4] = 0x0C; // FAT32 LBA
    pt[5] = 0xFE;
    pt[6] = 0xFF;
    pt[7] = 0xFF;
    put_le32(pt + 8, part_start);
    put_le32(pt + 12, part_sectors);
    buf[510] = 0x55;
    buf[511] = 0xAA;

    esp_err_t err = bsp_sdcard_write_multi(target_card, buf, 0, buf_sectors);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed writing MBR at sector 0: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }

    s_card_status.format_progress = 5;

    // 2. Write Reserved Sectors (VBR at +0, FSInfo at +1, Backup VBR at +6, Backup FSInfo at +7)
    const uint32_t vol_id = esp_random();
    for (uint32_t offset = 0; offset < rsv_sectors; offset += buf_sectors) {
        uint32_t chunk = (rsv_sectors - offset < buf_sectors) ? (rsv_sectors - offset) : (uint32_t)buf_sectors;
        memset(buf, 0, chunk * 512U);

        if (offset == 0) {
            // Construct VBR at sector 0 of buffer and copy to backup sector 6
            uint8_t *vbr = buf;
            vbr[0] = 0xEB;
            vbr[1] = 0x58;
            vbr[2] = 0x90;
            memcpy(vbr + 3, "MSDOS5.0", 8);
            put_le16(vbr + 11, 512U);               // BPB_BytsPerSec
            vbr[13] = (uint8_t)csize;               // BPB_SecPerClus
            put_le16(vbr + 14, (uint16_t)rsv_sectors); // BPB_RsvdSecCnt
            vbr[16] = (uint8_t)n_fats;              // BPB_NumFATs
            put_le16(vbr + 17, 0U);                 // BPB_RootEntCnt
            put_le16(vbr + 19, 0U);                 // BPB_TotSec16
            vbr[21] = 0xF8;                         // BPB_Media
            put_le16(vbr + 22, 0U);                 // BPB_FATSz16
            put_le16(vbr + 24, 63U);                // BPB_SecPerTrk
            put_le16(vbr + 26, 255U);               // BPB_NumHeads
            put_le32(vbr + 28, part_start);         // BPB_HiddSec
            put_le32(vbr + 32, part_sectors);       // BPB_TotSec32
            put_le32(vbr + 36, fat_sectors);        // BPB_FATSz32
            put_le16(vbr + 40, 0U);                 // BPB_ExtFlags
            put_le16(vbr + 42, 0U);                 // BPB_FSVer
            put_le32(vbr + 44, 2U);                 // BPB_RootClus
            put_le16(vbr + 48, 1U);                 // BPB_FSInfo
            put_le16(vbr + 50, 6U);                 // BPB_BkBootSec
            vbr[64] = 0x80;                         // BS_DrvNum
            vbr[66] = 0x29;                         // BS_BootSig
            put_le32(vbr + 67, vol_id);             // BS_VolID
            memcpy(vbr + 71, "WG1200 SD  ", 11);    // BS_VolLab
            memcpy(vbr + 82, "FAT32   ", 8);        // BS_FilSysType
            vbr[510] = 0x55;
            vbr[511] = 0xAA;

            // Construct FSInfo at sector 1 of buffer
            uint8_t *fsi = buf + 512U;
            put_le32(fsi + 0, 0x41615252U);         // FSI_LeadSig
            put_le32(fsi + 484, 0x61417272U);       // FSI_StrucSig
            put_le32(fsi + 488, n_clst - 1U);       // FSI_Free_Count
            put_le32(fsi + 492, 2U);                // FSI_Nxt_Free
            fsi[510] = 0x55;
            fsi[511] = 0xAA;

            if (chunk >= 8U) {
                memcpy(buf + (6U * 512U), vbr, 512U);
                memcpy(buf + (7U * 512U), fsi, 512U);
            }
        }

        err = bsp_sdcard_write_multi(target_card, buf, part_start + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed writing reserved sectors at +%lu: %s", (unsigned long)offset, esp_err_to_name(err));
            free(buf);
            return err;
        }
    }

    s_card_status.format_progress = 8;

    // 3. Initialize FAT table(s) using 64 KB multi-block CMD25 writes
    uint32_t written_init_sectors = 0;
    uint32_t chunk_counter = 0;

    for (uint32_t fat_idx = 0; fat_idx < n_fats; ++fat_idx) {
        const uint32_t fat_base = (fat_idx == 0) ? fat1_start : fat2_start;
        for (uint32_t offset = 0; offset < fat_sectors; offset += buf_sectors) {
            uint32_t chunk = (fat_sectors - offset < buf_sectors) ? (fat_sectors - offset) : (uint32_t)buf_sectors;
            memset(buf, 0, chunk * 512U);
            if (offset == 0) {
                put_le32(buf + 0, 0x0FFFFFF8U); // FAT entry 0: media descriptor
                put_le32(buf + 4, 0xFFFFFFFFU); // FAT entry 1: EOC
                put_le32(buf + 8, 0x0FFFFFFFU); // FAT entry 2: Root directory cluster EOC
            }
            err = bsp_sdcard_write_multi(target_card, buf, fat_base + offset, chunk);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed writing FAT%lu at offset %lu: %s",
                         (unsigned long)(fat_idx + 1), (unsigned long)offset, esp_err_to_name(err));
                free(buf);
                return err;
            }
            written_init_sectors += chunk;
            uint32_t pct = 8U + (written_init_sectors * 86U) / total_init_sectors;
            if (pct > 94U) pct = 94U;
            s_card_status.format_progress = (uint8_t)pct;

            if ((++chunk_counter & 3U) == 0U) {
                bsp_spi_unlock();
                vTaskDelay(1);
                bsp_spi_lock();
            }
        }
    }

    // 4. Zero out Root Directory Cluster (Cluster 2 at data_start)
    for (uint32_t offset = 0; offset < csize; offset += buf_sectors) {
        uint32_t chunk = (csize - offset < buf_sectors) ? (csize - offset) : (uint32_t)buf_sectors;
        memset(buf, 0, chunk * 512U);
        err = bsp_sdcard_write_multi(target_card, buf, data_start + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed zeroing root cluster at +%lu: %s", (unsigned long)offset, esp_err_to_name(err));
            free(buf);
            return err;
        }
    }

    free(buf);
    s_card_status.format_progress = 96;
    ESP_LOGI(TAG, "Fast FAT32 format layout write complete");
    return ESP_OK;
}

esp_err_t bsp_sdcard_format(void)
{
    TickType_t t_start_ticks = xTaskGetTickCount();
    s_card_status.formatting = true;
    s_card_status.format_progress = 1;
    snprintf(s_card_status.reason, sizeof(s_card_status.reason), "Formatting SD card (FAT32)...");

    bsp_show_sd_format_alert("Formatting SD", "Fast FAT32 format...\nDo not unplug!", false);
    bsp_sdcard_set_idle_wdt(false);

    esp_err_t err = ESP_OK;

    if (card != NULL) {
        vfs_fat_sd_ctx_t *sd_ctx = get_vfs_fat_get_sd_ctx(card);
        if (sd_ctx != NULL && sd_ctx->fs != NULL) {
            char drv[3] = {(char)('0' + sd_ctx->pdrv), ':', 0};
            // Unmount FatFs logical drive in-place without unregistering s_vfs[]
            f_mount(NULL, drv, 0);

            err = bsp_sdcard_fast_format_fat32(card);
            if (err == ESP_OK) {
                FRESULT fres = f_mount(sd_ctx->fs, drv, 1);
                if (fres != FR_OK) {
                    ESP_LOGE(TAG, "In-place f_mount after fast format failed (%d)", (int)fres);
                    err = ESP_FAIL;
                }
            } else {
                // Attempt to restore mount if fast format failed
                f_mount(sd_ctx->fs, drv, 1);
            }
        } else {
            esp_vfs_fat_mount_config_t mount_config = {
                .format_if_mount_failed = true,
                .max_files = DEFAULT_FD_NUM,
                .allocation_unit_size = 64 * 1024
            };
            err = esp_vfs_fat_sdcard_format_cfg(DEFAULT_MOUNT_POINT, card, &mount_config);
        }

        bsp_sdcard_set_idle_wdt(true);
        if (err == ESP_OK) {
            s_card_status.mounted = true;
            s_card_status.present = true;
            s_card_status.unusable = false;
            s_card_status.format_progress = 100;
            snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "FAT32");
            snprintf(s_card_status.reason, sizeof(s_card_status.reason), "Mounted and ready");
            ESP_LOGI(TAG, "Fast SD format completed in %.2f s",
                     (double)((xTaskGetTickCount() - t_start_ticks) * portTICK_PERIOD_MS) / 1000.0);
            bsp_show_sd_format_alert("Format Success", "SD Card formatted\nsuccessfully!", false);
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else {
            snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                     "Format failed (%s)", esp_err_to_name(err));
            bsp_show_sd_format_alert("Format Failed", "An error occurred\nduring formatting.", true);
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
        s_card_status.formatting = false;
        bsp_hide_sd_format_alert();
        return err;
    }

    ESP_LOGI(TAG, "Card not mounted. Attempting raw initialization for fast FAT32 format...");
    sdmmc_card_t *temp_card = NULL;
    err = bsp_sdcard_raw_init(&temp_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to raw initialize SD card for formatting: %s", esp_err_to_name(err));
        bsp_sdcard_set_idle_wdt(true);
        if (s_spi_diag.cmd0_r1 == 0x01 && s_spi_diag.cmd8_r7 == 0x000001AAU && s_spi_diag.acmd41_r1 == 0x01) {
            snprintf(s_card_status.fs_type, sizeof(s_card_status.fs_type), "Card Busy (ACMD41)");
            snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                     "Format failed: card responds on SPI (CMD8=0x1AA, OCR=0x%08lx) but stays busy in ACMD41 (0x01); eject/reinsert or replace card",
                     (unsigned long)s_spi_diag.raw_cmd58_ocr);
        } else {
            snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                     "Format failed: card init error (%s)", esp_err_to_name(err));
        }
        s_card_status.formatting = false;
        bsp_show_sd_format_alert("Format Failed", "Failed to initialize\nSD Card.", true);
        vTaskDelay(pdMS_TO_TICKS(2500));
        bsp_hide_sd_format_alert();
        return err;
    }

    err = bsp_sdcard_fast_format_fat32(temp_card);
    bsp_sdcard_raw_deinit(temp_card);

    if (err != ESP_OK) {
        bsp_sdcard_set_idle_wdt(true);
        snprintf(s_card_status.reason, sizeof(s_card_status.reason),
                 "Fast FAT32 format failed (%s)", esp_err_to_name(err));
        s_card_status.formatting = false;
        bsp_show_sd_format_alert("Format Failed", "Failed writing FAT32\nfilesystem.", true);
        vTaskDelay(pdMS_TO_TICKS(2500));
        bsp_hide_sd_format_alert();
        return err;
    }

    s_card_status.format_progress = 98;
    ESP_LOGI(TAG, "Fast FAT32 layout written. Mounting formatted volume...");
    err = bsp_sdcard_init(DEFAULT_MOUNT_POINT, DEFAULT_FD_NUM, false);
    bsp_sdcard_set_idle_wdt(true);

    if (err == ESP_OK) {
        s_card_status.format_progress = 100;
        ESP_LOGI(TAG, "SD Card fast-formatted and mounted in %.2f s!",
                 (double)((xTaskGetTickCount() - t_start_ticks) * portTICK_PERIOD_MS) / 1000.0);
        bsp_show_sd_format_alert("Format Success", "SD Card formatted\nsuccessfully!", false);
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGE(TAG, "Mount after fast format failed: %s", esp_err_to_name(err));
        bsp_show_sd_format_alert("Format Failed", "Mount after format\nfailed.", true);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    s_card_status.formatting = false;
    bsp_hide_sd_format_alert();
    return err;
}


