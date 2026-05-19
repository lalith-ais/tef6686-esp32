/*
 * TEF6686 Minimal Hardware Test
 * ESP-IDF v5.x
 *
 * Wiring:
 *   TEF6686 SDA  -> GPIO 21
 *   TEF6686 SCL  -> GPIO 22
 *   TEF6686 VCC  -> 5V
 *   TEF6686 GND  -> GND
 *   (RDS pin)    -> leave unconnected
 *
 * Open serial monitor at 115200 baud.
 * Type a frequency like "9790" + Enter to retune (= 97.90 MHz).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "TEF6686";

// ---------- Pin / bus config ----------
#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22
#define I2C_SPEED_HZ    100000

#define TEF_I2C_ADDR    0x64        // 7-bit address

// ---------- I2C handles ----------
static i2c_master_bus_handle_t  bus_handle;
static i2c_master_dev_handle_t  tef_handle;

// ---------- Low-level I2C helpers ----------

/*
 * Write a TEF6686 command.
 * Protocol: [module][index][p1_hi][p1_lo][p2_hi][p2_lo][p3_hi][p3_lo]
 * Unused parameters should be 0.
 */
static esp_err_t tef_write(uint8_t module, uint8_t index,
                            int16_t p1, int16_t p2, int16_t p3)
{
    uint8_t buf[8] = {
        module,
        index,
        (uint8_t)(p1 >> 8), (uint8_t)(p1 & 0xFF),
        (uint8_t)(p2 >> 8), (uint8_t)(p2 & 0xFF),
        (uint8_t)(p3 >> 8), (uint8_t)(p3 & 0xFF),
    };
    return i2c_master_transmit(tef_handle, buf, sizeof(buf), 100);
}

/*
 * Read N bytes from the TEF6686.
 * The chip needs a write (module + index) then a repeated-start read.
 */
static esp_err_t tef_read(uint8_t module, uint8_t index,
                           uint8_t *out, size_t len)
{
    uint8_t reg[2] = { module, index };
    return i2c_master_transmit_receive(tef_handle,
                                       reg, sizeof(reg),
                                       out, len,
                                       100);
}

// ---------- TEF6686 initialisation ----------

static esp_err_t tef_init(void)
{
    esp_err_t ret;

    // Give the chip a moment after power-on
    vTaskDelay(pdMS_TO_TICKS(100));

    // APPL_SET_OP_MODE: normal operation (module 0x80, index 0x01, p1=1)
    ret = tef_write(0x80, 0x01, 1, 0, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(200));

    // FM_Set_Mode: enable FM (module 0x20, index 0x01, p1=1)
    ret = tef_write(0x20, 0x01, 1, 0, 0);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(50));

    // FM_Set_Deemphasis: 50 µs for Europe (p2=100 means 50 µs)
    ret = tef_write(0x20, 0x02, 0, 100, 0);
    return ret;
}

// ---------- Tuning ----------

/*
 * Tune to an FM frequency.
 * freq_10khz: frequency in units of 10 kHz
 *   e.g. 9790 = 97.90 MHz
 */
static esp_err_t tef_tune_fm(uint16_t freq_10khz)
{
    // FM_Tune_To: module 0x20, index 0x30, p1=0 (auto), p2=frequency
    return tef_write(0x20, 0x30, 0, (int16_t)freq_10khz, 0);
}

// ---------- Status readback ----------

/*
 * Read FM quality data (8 bytes):
 *   bytes 0-1: RSSI in 0.1 dBuV  (signed)
 *   bytes 2-3: USN  (Ultrasonic Noise detector, 0=clean)
 *   bytes 4-5: WAM  (Wideband AM detector, 0=no multipath)
 *   bytes 6-7: Frequency offset in 0.1 kHz (signed)
 */
static void tef_print_quality(void)
{
    uint8_t buf[8] = {0};
    esp_err_t ret = tef_read(0x20, 0x81, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Quality read failed: %s", esp_err_to_name(ret));
        return;
    }

    int16_t rssi   = (int16_t)((buf[0] << 8) | buf[1]);
    int16_t usn    = (int16_t)((buf[2] << 8) | buf[3]);
    int16_t wam    = (int16_t)((buf[4] << 8) | buf[5]);
    int16_t offset = (int16_t)((buf[6] << 8) | buf[7]);

    // Also read stereo pilot status (4 bytes, index 0x80)
    uint8_t stat[4] = {0};
    bool stereo = false;
    if (tef_read(0x20, 0x80, stat, sizeof(stat)) == ESP_OK) {
        // bit 0 of byte 2 indicates stereo pilot detected
        stereo = (stat[2] & 0x01) != 0;
    }

    printf("RSSI: %+.1f dBuV | USN: %d | WAM: %d | Offset: %.1f kHz | %s\n",
           rssi / 10.0f,
           usn,
           wam,
           offset / 10.0f,
           stereo ? "STEREO" : "mono");
}

// ---------- Simple UART line reader ----------
// Reads a newline-terminated string from UART0 without blocking the scheduler.

#define UART_BUF_SIZE 64

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(UART_NUM_0, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &cfg);
}

/*
 * Non-blocking check for a complete line on UART0.
 * Returns true and fills buf if a '\n' was received, false otherwise.
 */
static char uart_line_buf[UART_BUF_SIZE];
static int  uart_line_pos = 0;

static bool uart_poll_line(char *out, size_t out_len)
{
    uint8_t ch;
    while (uart_read_bytes(UART_NUM_0, &ch, 1, 0) == 1) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            uart_line_buf[uart_line_pos] = '\0';
            snprintf(out, out_len, "%s", uart_line_buf);
            uart_line_pos = 0;
            return true;
        }
        if (uart_line_pos < (int)(sizeof(uart_line_buf) - 1)) {
            uart_line_buf[uart_line_pos++] = (char)ch;
        }
    }
    return false;
}

// ---------- Main task ----------

static uint16_t current_freq = 9790;   // start at 97.90 MHz

static void tef_task(void *arg)
{
    char line[UART_BUF_SIZE];

    printf("\n=== TEF6686 Minimal Test ===\n");
    printf("Type a frequency in 10 kHz units (e.g. '9790' = 97.90 MHz) + Enter to retune.\n\n");

    while (1) {
        // Print signal quality every second
        printf("[%.2f MHz] ", current_freq / 100.0f);
        tef_print_quality();

        // Check for retune command from serial
        if (uart_poll_line(line, sizeof(line))) {
            uint16_t f = (uint16_t)atoi(line);
            if (f >= 6400 && f <= 10800) {
                current_freq = f;
                esp_err_t ret = tef_tune_fm(current_freq);
                if (ret == ESP_OK) {
                    printf(">> Tuned to %.2f MHz\n", current_freq / 100.0f);
                } else {
                    printf(">> Tune failed: %s\n", esp_err_to_name(ret));
                }
                vTaskDelay(pdMS_TO_TICKS(400));  // let it settle
            } else {
                printf(">> Bad frequency. Range: 6400–10800 (64.00–108.00 MHz)\n");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------- app_main ----------

void app_main(void)
{
    // --- Init UART ---
    uart_init();

    // --- Init I2C bus ---
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_PORT,
        .sda_io_num    = I2C_SDA_PIN,
        .scl_io_num    = I2C_SCL_PIN,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // use ESP32 internal pull-ups
                                               // add 4k7 externals if signal is noisy
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    // --- Add TEF6686 as a device on the bus ---
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TEF_I2C_ADDR,
        .scl_speed_hz    = I2C_SPEED_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &tef_handle));

    // --- Probe: check the chip responds ---
    esp_err_t probe = i2c_master_probe(bus_handle, TEF_I2C_ADDR, 50);
    if (probe == ESP_OK) {
        ESP_LOGI(TAG, "TEF6686 found at 0x%02X", TEF_I2C_ADDR);
    } else {
        ESP_LOGE(TAG, "TEF6686 NOT found! Check wiring. Error: %s",
                 esp_err_to_name(probe));
        // Halt here so the error is obvious
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // --- Initialise chip ---
    esp_err_t init_ret = tef_init();
    if (init_ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed: %s", esp_err_to_name(init_ret));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "TEF6686 initialised");

    // --- Tune to starting frequency ---
    tef_tune_fm(current_freq);
    ESP_LOGI(TAG, "Tuned to %.2f MHz", current_freq / 100.0f);
    vTaskDelay(pdMS_TO_TICKS(400));

    // --- Start main loop task ---
    xTaskCreate(tef_task, "tef_task", 4096, NULL, 5, NULL);
}
