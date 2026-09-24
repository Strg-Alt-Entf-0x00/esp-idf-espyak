/**
 * @file espyak_api.c
 * @brief Main API implementation for espyak ESP32-P4
 * 
 * Core G2P translator API optimized for ESP32-P4 Rev 1.3
 */

#include "espyak.h"
#include "espyak_internal.h"

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#ifdef CONFIG_ESPYAK_USE_PSRAM
#include <esp_psram.h>
#endif

static const char* TAG = "espyak";

/* Version string */
const char* espyak_version(void) {
    return ESPYAK_VERSION;
}

/* Error strings */
static const char* error_strings[] = {
    [ESPYAK_OK] = "Success",
    [ESPYAK_ERR_INVALID_ARG] = "Invalid argument",
    [ESPYAK_ERR_NO_MEM] = "Out of memory",
    [ESPYAK_ERR_NOT_FOUND] = "Resource not found",
    [ESPYAK_ERR_BUFFER_TOO_SMALL] = "Buffer too small",
    [ESPYAK_ERR_INVALID_LANG] = "Invalid language code",
    [ESPYAK_ERR_INIT_FAILED] = "Initialization failed",
    [ESPYAK_ERR_NOT_INITIALIZED] = "Handle not initialized",
};

const char* espyak_err_str(espyak_err_t err) {
    if (err < 0 || err >= sizeof(error_strings) / sizeof(error_strings[0])) {
        return "Unknown error";
    }
    return error_strings[err];
}

/* Language availability table (compile-time configured) */
static const char* available_languages[] = {
#ifdef CONFIG_ESPYAK_LANG_EN
    "en",
#endif
#ifdef CONFIG_ESPYAK_LANG_ES
    "es",
#endif
#ifdef CONFIG_ESPYAK_LANG_FR
    "fr",
#endif
#ifdef CONFIG_ESPYAK_LANG_DE
    "de",
#endif
#ifdef CONFIG_ESPYAK_LANG_RU
    "ru",
#endif
#ifdef CONFIG_ESPYAK_LANG_PT
    "pt",
#endif
#ifdef CONFIG_ESPYAK_LANG_CMN
    "cmn",
#endif
    NULL
};

const char** espyak_get_languages(size_t* count) {
    if (count) {
        size_t n = 0;
        while (available_languages[n] != NULL) n++;
        *count = n;
    }
    return available_languages;
}

bool espyak_lang_available(const char* lang) {
    if (!lang) return false;
    
    for (size_t i = 0; available_languages[i] != NULL; i++) {
        if (strcmp(lang, available_languages[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Internal handle structure */
struct espyak_handle_s {
    uint32_t magic;                  /* Magic number for validation */
    char lang[8];                    /* Language code */
    espyak_config_t config;          /* Configuration */
    
    /* Core components */
    espyak_phoneme_table_t* phoneme_table;
    espyak_dictionary_t* dictionary;
    espyak_translator_t* translator;
    
    /* Memory tracking */
    size_t heap_used;
    
#ifdef CONFIG_ESPYAK_STATS_COLLECTION
    espyak_stats_t stats;
#endif
};

#define ESPYAK_MAGIC 0xE5F7A12C

/* Validate handle */
static inline bool is_valid_handle(espyak_handle_t handle) {
    return handle != NULL && handle->magic == ESPYAK_MAGIC;
}

/* ========================================================================
 * Initialization & Cleanup
 * ======================================================================== */

espyak_err_t espyak_init(const char* lang, const espyak_config_t* config,
                         espyak_handle_t* out_handle) {
    if (!lang || !out_handle) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    if (!espyak_lang_available(lang)) {
        ESP_LOGE(TAG, "Language '%s' not available (not compiled in)", lang);
        return ESPYAK_ERR_INVALID_LANG;
    }
    
    /* Use default config if none provided */
    espyak_config_t cfg = config ? *config : (espyak_config_t)ESPYAK_CONFIG_DEFAULT();
    
    ESP_LOGI(TAG, "Initializing espyak for language '%s'", lang);
    ESP_LOGI(TAG, "  PSRAM: %s", cfg.use_psram ? "enabled" : "disabled");
    ESP_LOGI(TAG, "  HW Accel: %s", cfg.enable_hwaccel ? "enabled" : "disabled");
    
    /* Allocate handle (always in DRAM for fast access) */
    espyak_handle_t handle = heap_caps_calloc(1, sizeof(struct espyak_handle_s),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        return ESPYAK_ERR_NO_MEM;
    }
    
    handle->magic = ESPYAK_MAGIC;
    strncpy(handle->lang, lang, sizeof(handle->lang) - 1);
    handle->config = cfg;
    handle->heap_used = sizeof(struct espyak_handle_s);
    
    espyak_err_t err;
    
    /* Initialize phoneme table */
    err = espyak_phoneme_table_init(lang, &cfg, &handle->phoneme_table);
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to initialize phoneme table: %s", espyak_err_str(err));
        goto cleanup;
    }
    handle->heap_used += espyak_phoneme_table_size(handle->phoneme_table);
    
    /* Initialize dictionary */
    err = espyak_dictionary_init(lang, &cfg, &handle->dictionary);
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to initialize dictionary: %s", espyak_err_str(err));
        goto cleanup;
    }
    handle->heap_used += espyak_dictionary_size(handle->dictionary);
    
    /* Load embedded dictionary data */
    err = espyak_dictionary_load_embedded(handle->dictionary, lang);
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to load dictionary data: %s", espyak_err_str(err));
        goto cleanup;
    }
    handle->heap_used += espyak_dictionary_size(handle->dictionary);
    
    /* Initialize translator */
    err = espyak_translator_init(lang, &cfg, handle->phoneme_table,
                                 handle->dictionary, &handle->translator);
    if (err != ESPYAK_OK) {
        ESP_LOGE(TAG, "Failed to initialize translator: %s", espyak_err_str(err));
        goto cleanup;
    }
    handle->heap_used += espyak_translator_size(handle->translator);
    
    ESP_LOGI(TAG, "Initialization complete. Heap used: %zu bytes", handle->heap_used);
    
    *out_handle = handle;
    return ESPYAK_OK;
    
cleanup:
    if (handle->translator) espyak_translator_deinit(handle->translator);
    if (handle->dictionary) espyak_dictionary_deinit(handle->dictionary);
    if (handle->phoneme_table) espyak_phoneme_table_deinit(handle->phoneme_table);
    free(handle);
    return err;
}

void espyak_deinit(espyak_handle_t handle) {
    if (!is_valid_handle(handle)) {
        return;
    }
    
    ESP_LOGI(TAG, "Deinitializing espyak handle for '%s'", handle->lang);
    
    if (handle->translator) espyak_translator_deinit(handle->translator);
    if (handle->dictionary) espyak_dictionary_deinit(handle->dictionary);
    if (handle->phoneme_table) espyak_phoneme_table_deinit(handle->phoneme_table);
    
    handle->magic = 0;  /* Invalidate */
    free(handle);
}

/* ========================================================================
 * Phonemization
 * ======================================================================== */

espyak_err_t espyak_phonemize(espyak_handle_t handle, const char* text,
                              char* output, size_t out_size, bool ipa) {
    espyak_options_t opts = ESPYAK_OPTIONS_DEFAULT();
    opts.format = ipa ? ESPYAK_FORMAT_IPA : ESPYAK_FORMAT_KIRSHENBAUM;
    return espyak_phonemize_ex(handle, text, output, out_size, &opts);
}

espyak_err_t espyak_phonemize_ex(espyak_handle_t handle, const char* text,
                                 char* output, size_t out_size,
                                 const espyak_options_t* options) {
    if (!is_valid_handle(handle)) {
        return ESPYAK_ERR_NOT_INITIALIZED;
    }
    
    if (!text || !output || out_size == 0) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    if (strlen(text) > handle->config.max_word_length) {
        ESP_LOGW(TAG, "Input text exceeds max length (%u bytes)", 
                 handle->config.max_word_length);
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    espyak_options_t opts = options ? *options : (espyak_options_t)ESPYAK_OPTIONS_DEFAULT();
    
#ifdef CONFIG_ESPYAK_STATS_COLLECTION
    uint64_t start = esp_timer_get_time();
    handle->stats.phonemize_calls++;
    handle->stats.total_chars += strlen(text);
#endif
    
    /* Main translation pipeline:
     * 1. Word split & normalization
     * 2. Dictionary lookup / rule matching
     * 3. Stress assignment
     * 4. Phoneme programs
     * 5. Render to IPA/Kirshenbaum
     */
    espyak_err_t err = espyak_translator_phonemize(
        handle->translator,
        text,
        output,
        out_size,
        &opts
    );
    
#ifdef CONFIG_ESPYAK_STATS_COLLECTION
    uint64_t elapsed = esp_timer_get_time() - start;
    handle->stats.total_time_us += elapsed;
    if (handle->heap_used > handle->stats.peak_heap_used) {
        handle->stats.peak_heap_used = handle->heap_used;
    }
#endif
    
    if (err == ESPYAK_OK) {
        ESP_LOGD(TAG, "Phonemized: '%s' -> '%s'", text, output);
    } else {
        ESP_LOGE(TAG, "Phonemization failed: %s", espyak_err_str(err));
    }
    
    return err;
}

espyak_err_t espyak_render(espyak_handle_t handle, const char* phonemes,
                           char* output, size_t out_size, bool ipa) {
    if (!is_valid_handle(handle)) {
        return ESPYAK_ERR_NOT_INITIALIZED;
    }
    
    if (!phonemes || !output || out_size == 0) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    espyak_format_t format = ipa ? ESPYAK_FORMAT_IPA : ESPYAK_FORMAT_KIRSHENBAUM;
    
    return espyak_render_phoneme_string(
        handle->phoneme_table,
        phonemes,
        output,
        out_size,
        format,
        NULL,  /* separator */
        NULL   /* tie */
    );
}

/* ========================================================================
 * Performance & Debugging
 * ======================================================================== */

#ifdef CONFIG_ESPYAK_STATS_COLLECTION
espyak_err_t espyak_get_stats(espyak_handle_t handle, espyak_stats_t* stats) {
    if (!is_valid_handle(handle)) {
        return ESPYAK_ERR_NOT_INITIALIZED;
    }
    
    if (!stats) {
        return ESPYAK_ERR_INVALID_ARG;
    }
    
    *stats = handle->stats;
    return ESPYAK_OK;
}

void espyak_reset_stats(espyak_handle_t handle) {
    if (!is_valid_handle(handle)) {
        return;
    }
    
    memset(&handle->stats, 0, sizeof(espyak_stats_t));
}
#else
espyak_err_t espyak_get_stats(espyak_handle_t handle, espyak_stats_t* stats) {
    (void)handle;
    (void)stats;
    return ESPYAK_ERR_NOT_FOUND;
}

void espyak_reset_stats(espyak_handle_t handle) {
    (void)handle;
}
#endif

size_t espyak_get_heap_usage(espyak_handle_t handle) {
    if (!is_valid_handle(handle)) {
        return 0;
    }
    
    return handle->heap_used;
}
