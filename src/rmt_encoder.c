#include "rmt_encoder.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "rmt_encoder";

// ============================================================================
// Uniform (constant-speed) encoder
// ============================================================================

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t copy_encoder;
    rmt_symbol_word_t symbol;
    uint32_t steps_encoded;
    uint32_t total_steps;
} stepper_uniform_encoder_t;

static size_t stepper_uniform_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                     const void *primary_data, size_t data_size,
                                     rmt_encode_state_t *ret_state)
{
    stepper_uniform_encoder_t *enc = __containerof(encoder, stepper_uniform_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t total_written = 0;

    while (enc->steps_encoded < enc->total_steps) {
        rmt_encode_state_t copy_state = RMT_ENCODING_RESET;
        size_t written = enc->copy_encoder->encode(enc->copy_encoder, channel,
                                                   &enc->symbol, sizeof(rmt_symbol_word_t),
                                                   &copy_state);
        total_written += written;

        if (copy_state & RMT_ENCODING_COMPLETE) {
            enc->steps_encoded++;
        }
        if (copy_state & RMT_ENCODING_MEM_FULL) {
            session_state |= RMT_ENCODING_MEM_FULL;
            break;
        }
    }

    if (enc->steps_encoded >= enc->total_steps) {
        session_state |= RMT_ENCODING_COMPLETE;
    }

    *ret_state = session_state;
    return total_written;
}

static esp_err_t stepper_uniform_reset(rmt_encoder_t *encoder)
{
    stepper_uniform_encoder_t *enc = __containerof(encoder, stepper_uniform_encoder_t, base);
    rmt_encoder_reset(enc->copy_encoder);
    enc->steps_encoded = 0;
    enc->total_steps = 0;
    return ESP_OK;
}

static esp_err_t stepper_uniform_del(rmt_encoder_t *encoder)
{
    stepper_uniform_encoder_t *enc = __containerof(encoder, stepper_uniform_encoder_t, base);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

esp_err_t rmt_new_stepper_uniform_encoder(const stepper_uniform_encoder_config_t *config,
                                          rmt_encoder_handle_t *ret_encoder)
{
    ESP_RETURN_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    ESP_RETURN_ON_FALSE(config->freq_hz > 0, ESP_ERR_INVALID_ARG, TAG, "frequency must be > 0");
    ESP_RETURN_ON_FALSE(config->resolution_hz > 0, ESP_ERR_INVALID_ARG, TAG, "resolution must be > 0");

    stepper_uniform_encoder_t *enc = calloc(1, sizeof(stepper_uniform_encoder_t));
    ESP_RETURN_ON_FALSE(enc, ESP_ERR_NO_MEM, TAG, "failed to allocate encoder");

    rmt_copy_encoder_config_t copy_config = {};
    esp_err_t ret = rmt_new_copy_encoder(&copy_config, &enc->copy_encoder);
    if (ret != ESP_OK) {
        free(enc);
        ESP_LOGE(TAG, "failed to create copy encoder: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t period_ticks = config->resolution_hz / config->freq_hz;
    uint32_t high_ticks = config->pulse_ticks;
    uint32_t low_ticks = (period_ticks > high_ticks) ? (period_ticks - high_ticks) : 1;

    enc->symbol.duration0 = high_ticks;
    enc->symbol.level0 = 1;
    enc->symbol.duration1 = low_ticks;
    enc->symbol.level1 = 0;

    enc->base.encode = stepper_uniform_encode;
    enc->base.reset = stepper_uniform_reset;
    enc->base.del = stepper_uniform_del;

    *ret_encoder = &enc->base;

    ESP_LOGI(TAG, "uniform encoder: freq=%luHz period=%lu pulse=%lu ticks",
             (unsigned long)config->freq_hz, (unsigned long)period_ticks,
             (unsigned long)high_ticks);
    return ESP_OK;
}

void stepper_uniform_encoder_set_steps(rmt_encoder_handle_t encoder, uint32_t steps)
{
    stepper_uniform_encoder_t *enc = __containerof(encoder, stepper_uniform_encoder_t, base);
    enc->steps_encoded = 0;
    enc->total_steps = steps;
}

// ============================================================================
// S-curve acceleration/deceleration encoder
// ============================================================================

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_handle_t copy_encoder;
    rmt_symbol_word_t *symbols;  // Pre-computed symbol table for the ramp
    uint32_t total_steps;
    uint32_t steps_encoded;
} stepper_scurve_encoder_t;

static size_t stepper_scurve_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                    const void *primary_data, size_t data_size,
                                    rmt_encode_state_t *ret_state)
{
    stepper_scurve_encoder_t *enc = __containerof(encoder, stepper_scurve_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    size_t total_written = 0;

    while (enc->steps_encoded < enc->total_steps) {
        rmt_encode_state_t copy_state = RMT_ENCODING_RESET;
        size_t written = enc->copy_encoder->encode(
            enc->copy_encoder, channel,
            &enc->symbols[enc->steps_encoded], sizeof(rmt_symbol_word_t),
            &copy_state);
        total_written += written;

        if (copy_state & RMT_ENCODING_COMPLETE) {
            enc->steps_encoded++;
        }
        if (copy_state & RMT_ENCODING_MEM_FULL) {
            session_state |= RMT_ENCODING_MEM_FULL;
            break;
        }
    }

    if (enc->steps_encoded >= enc->total_steps) {
        session_state |= RMT_ENCODING_COMPLETE;
    }

    *ret_state = session_state;
    return total_written;
}

static esp_err_t stepper_scurve_reset(rmt_encoder_t *encoder)
{
    stepper_scurve_encoder_t *enc = __containerof(encoder, stepper_scurve_encoder_t, base);
    rmt_encoder_reset(enc->copy_encoder);
    enc->steps_encoded = 0;
    return ESP_OK;
}

static esp_err_t stepper_scurve_del(rmt_encoder_t *encoder)
{
    stepper_scurve_encoder_t *enc = __containerof(encoder, stepper_scurve_encoder_t, base);
    rmt_del_encoder(enc->copy_encoder);
    free(enc->symbols);
    free(enc);
    return ESP_OK;
}

// Smoothstep: 3t^2 - 2t^3, maps [0,1] -> [0,1]
static inline float smoothstep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

esp_err_t rmt_new_stepper_scurve_encoder(const stepper_scurve_encoder_config_t *config,
                                         rmt_encoder_handle_t *ret_encoder)
{
    ESP_RETURN_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    ESP_RETURN_ON_FALSE(config->accel_steps > 0, ESP_ERR_INVALID_ARG, TAG, "accel_steps must be > 0");
    ESP_RETURN_ON_FALSE(config->start_freq_hz > 0 && config->target_freq_hz > 0,
                        ESP_ERR_INVALID_ARG, TAG, "frequencies must be > 0");
    ESP_RETURN_ON_FALSE(config->target_freq_hz > config->start_freq_hz,
                        ESP_ERR_INVALID_ARG, TAG, "target_freq must be > start_freq");

    stepper_scurve_encoder_t *enc = calloc(1, sizeof(stepper_scurve_encoder_t));
    ESP_RETURN_ON_FALSE(enc, ESP_ERR_NO_MEM, TAG, "failed to allocate s-curve encoder");

    enc->symbols = calloc(config->accel_steps, sizeof(rmt_symbol_word_t));
    if (!enc->symbols) {
        free(enc);
        ESP_LOGE(TAG, "failed to allocate %lu symbols for s-curve",
                 (unsigned long)config->accel_steps);
        return ESP_ERR_NO_MEM;
    }

    rmt_copy_encoder_config_t copy_config = {};
    esp_err_t ret = rmt_new_copy_encoder(&copy_config, &enc->copy_encoder);
    if (ret != ESP_OK) {
        free(enc->symbols);
        free(enc);
        ESP_LOGE(TAG, "failed to create copy encoder: %s", esp_err_to_name(ret));
        return ret;
    }

    // Pre-compute symbols for each step in the ramp
    for (uint32_t i = 0; i < config->accel_steps; i++) {
        float t = (float)i / (float)(config->accel_steps - 1);
        if (config->accel_steps == 1) {
            t = 1.0f;
        }

        float s = smoothstep(t);
        float freq;
        if (config->reverse) {
            // Deceleration: target -> start
            freq = (float)config->target_freq_hz -
                   s * (float)(config->target_freq_hz - config->start_freq_hz);
        } else {
            // Acceleration: start -> target
            freq = (float)config->start_freq_hz +
                   s * (float)(config->target_freq_hz - config->start_freq_hz);
        }

        uint32_t period_ticks = (uint32_t)(config->resolution_hz / freq);
        uint32_t high_ticks = config->pulse_ticks;
        uint32_t low_ticks = (period_ticks > high_ticks) ? (period_ticks - high_ticks) : 1;

        enc->symbols[i].duration0 = high_ticks;
        enc->symbols[i].level0 = 1;
        enc->symbols[i].duration1 = low_ticks;
        enc->symbols[i].level1 = 0;
    }

    enc->total_steps = config->accel_steps;
    enc->steps_encoded = 0;

    enc->base.encode = stepper_scurve_encode;
    enc->base.reset = stepper_scurve_reset;
    enc->base.del = stepper_scurve_del;

    *ret_encoder = &enc->base;

    ESP_LOGI(TAG, "s-curve encoder: %s %lu->%luHz in %lu steps",
             config->reverse ? "decel" : "accel",
             (unsigned long)(config->reverse ? config->target_freq_hz : config->start_freq_hz),
             (unsigned long)(config->reverse ? config->start_freq_hz : config->target_freq_hz),
             (unsigned long)config->accel_steps);
    return ESP_OK;
}
