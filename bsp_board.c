#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "bsp_board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define MIC_SAMPLE_RATE 16000
#define MIC_BCLK_GPIO 4
#define MIC_WS_GPIO 5
#define MIC_DIN_GPIO 6
#define MIC_GAIN 4
#define MIC_SLOT 0 /* 0: L/R connected to GND; 1: L/R connected to 3.3 V */
#define I2S_FRAME_SLOTS 2
#define READ_CHUNK_FRAMES 128
#define READ_TIMEOUT_MS 1000

static const char *TAG = "inmp441";
static i2s_chan_handle_t rx_channel;
/* This BSP has one reader: main.c's feed_Task. Keep scratch data off its stack. */
static int32_t raw_samples[READ_CHUNK_FRAMES * I2S_FRAME_SLOTS];

esp_err_t bsp_board_init(void)
{
    if (rx_channel != NULL) {
        return ESP_OK;
    }

    const int bclk = MIC_BCLK_GPIO;
    const int ws = MIC_WS_GPIO;
    const int din = MIC_DIN_GPIO;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(bclk) || !GPIO_IS_VALID_OUTPUT_GPIO(ws) ||
        !GPIO_IS_VALID_GPIO(din) || bclk == ws || bclk == din || ws == din) {
        ESP_LOGE(TAG, "Configure three distinct, valid I2S GPIOs in bsp_board.c");
        return ESP_ERR_INVALID_ARG;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&channel_config, NULL, &rx_channel);
    if (err != ESP_OK) {
        return err;
    }

    /* The INMP441 needs 64 BCLKs per frame, even for a single microphone.
     * Receive both 32-bit slots and discard the unused slot in software. */
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
        },
    };

    err = i2s_channel_init_std_mode(rx_channel, &config);
    if (err != ESP_OK) {
        goto fail;
    }
    /* SD is high impedance outside the microphone's 24 valid data bits. */
    err = gpio_set_pull_mode(din, GPIO_PULLDOWN_ONLY);
    if (err != ESP_OK) {
        goto fail;
    }
    err = i2s_channel_enable(rx_channel);
    if (err != ESP_OK) {
        goto fail;
    }

    ESP_LOGI(TAG, "16 kHz microphone: BCLK=%d, WS=%d, DIN=%d, channel=%s, gain=%d",
             bclk, ws, din, MIC_SLOT == 0 ? "left" : "right", MIC_GAIN);
    return ESP_OK;

fail:
    i2s_del_channel(rx_channel);
    rx_channel = NULL;
    return err;
}

esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    /* Raw and processed feeds both contain the same single microphone. */
    (void)is_get_raw_channel;
    if (buffer == NULL || buffer_len <= 0 || buffer_len % sizeof(*buffer) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t samples_remaining = buffer_len / sizeof(*buffer);
    while (samples_remaining > 0) {
        const size_t frames = samples_remaining < READ_CHUNK_FRAMES ? samples_remaining : READ_CHUNK_FRAMES;
        const size_t read_size = frames * I2S_FRAME_SLOTS * sizeof(raw_samples[0]);
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_channel, raw_samples, read_size, &bytes_read, READ_TIMEOUT_MS);
        if (err != ESP_OK || bytes_read != read_size) {
            ESP_LOGE(TAG, "I2S read failed: %s, %u/%u bytes", esp_err_to_name(err),
                     (unsigned)bytes_read, (unsigned)read_size);
            return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
        }

        for (size_t i = 0; i < frames; ++i) {
            /* Samples are signed, MSB-aligned 24-bit values in 32-bit slots.
             * Discard the low padding byte, apply gain, then saturate to PCM16. */
            int32_t sample = ((raw_samples[I2S_FRAME_SLOTS * i + MIC_SLOT] >> 8) * MIC_GAIN) >> 8;
            if (sample > INT16_MAX) {
                sample = INT16_MAX;
            } else if (sample < INT16_MIN) {
                sample = INT16_MIN;
            }
            *buffer++ = (int16_t)sample;
        }
        samples_remaining -= frames;
    }
    return ESP_OK;
}

int bsp_get_feed_channel(void)
{
    return 1;
}

char *bsp_get_input_format(void)
{
    return "M";
}

/* This board has a microphone only. Report unsupported output/storage APIs
 * explicitly so existing Skainet callers can still link against the BSP. */
esp_err_t bsp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait)
{
    (void)data;
    (void)length;
    (void)ticks_to_wait;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_set_play_vol(int volume)
{
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_audio_get_play_vol(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *volume = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

void bsp_get_player_format(int *channels, int *sample_rate, int *bits_per_sample)
{
    if (channels != NULL) {
        *channels = 1;
    }
    if (sample_rate != NULL) {
        *sample_rate = MIC_SAMPLE_RATE;
    }
    if (bits_per_sample != NULL) {
        *bits_per_sample = 16;
    }
}

esp_err_t bsp_sdcard_init(char *mount_point, size_t max_files)
{
    (void)mount_point;
    (void)max_files;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_deinit(char *mount_point)
{
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
}
