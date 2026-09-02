// Spike S3 sound path: I2S -> ES8311 -> NS4150B -> speaker.
// Init follows Waveshare's own 12_i2s_codec example; the tone synthesis is
// §9.1's — a sine with a 5ms attack and exponential decay — so what gets
// judged here is the actual cue vocabulary, not a generic beep.
#include "audio.h"

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "audio";
#define SAMPLE_RATE 16000

static esp_codec_dev_handle_t g_speaker;
static i2s_chan_handle_t      g_tx, g_rx;

esp_err_t audio_init(int volume) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &g_tx, &g_rx) != ESP_OK) return ESP_FAIL;

    const i2s_std_config_t i2s_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK,
                      .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN },
    };
    if (i2s_channel_init_std_mode(g_tx, &i2s_cfg) != ESP_OK) return ESP_FAIL;
    if (i2s_channel_init_std_mode(g_rx, &i2s_cfg) != ESP_OK) return ESP_FAIL;

    audio_codec_i2s_cfg_t i2s_data = { .port = CONFIG_BSP_I2S_NUM, .tx_handle = g_tx, .rx_handle = g_rx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data);
    if (!data_if) return ESP_FAIL;

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!ctrl_if || !gpio_if) return ESP_FAIL;

    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,        // the amp gate — silence means silent
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dev = es8311_codec_new(&es);
    if (!dev) return ESP_FAIL;

    esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dev, .data_if = data_if };
    g_speaker = esp_codec_dev_new(&cfg);
    if (!g_speaker) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16, .channel = 1, .channel_mask = 0,
        .sample_rate = SAMPLE_RATE, .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(g_speaker, &fs) != ESP_CODEC_DEV_OK) return ESP_FAIL;
    esp_codec_dev_set_out_vol(g_speaker, volume);

    ESP_LOGI(TAG, "ES8311 ready at %d Hz, volume %d", SAMPLE_RATE, volume);
    return ESP_OK;
}

/** One tone, rendered and written synchronously. §9.1's envelope: without
 *  the attack ramp a square-edged start pops audibly on a small speaker. */
void audio_tone(int freq_hz, int ms) {
    if (!g_speaker) return;

    const int n = (SAMPLE_RATE * ms) / 1000;
    static int16_t buf[SAMPLE_RATE / 4];               // 250ms ceiling per tone
    const int count = n > (int)(sizeof buf / sizeof buf[0]) ? (int)(sizeof buf / sizeof buf[0]) : n;
    const float step = 2.0f * (float)M_PI * freq_hz / SAMPLE_RATE;

    for (int i = 0; i < count; i++) {
        const float attack = fminf(1.0f, i / (0.005f * SAMPLE_RATE));
        const float decay  = expf(-3.0f * i / count);
        buf[i] = (int16_t)(sinf(step * i) * attack * decay * 12000.0f);
    }
    esp_codec_dev_write(g_speaker, buf, count * (int)sizeof(int16_t));
}

void audio_cue(audio_cue_t cue) {
    switch (cue) {                                     // §9's table
    case CUE_BLOOM:   audio_tone(523, 90);  audio_tone(659, 140); break;   // C5 -> E5
    case CUE_SUCCESS: audio_tone(659, 70);  audio_tone(880, 110); break;   // E5 -> A5
    case CUE_READY:   audio_tone(440, 120); break;                          // A4
    }
}
