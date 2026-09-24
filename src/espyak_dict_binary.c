/**
 * @file espyak_dict_binary.c
 * @brief Binary dictionary loader for espyak
 * 
 * Loads compact binary dictionaries from FLASH/SPIFFS
 */

#include "espyak_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_err.h>
#include <esp_log.h>

static const char *TAG = "espyak_dictbin";

// Binary format constants
#define DICT_MAGIC     0x54434445  // "EDCT"
#define DICT_VERSION   0x00010000  // v1.0
#define FLAG_COMPRESSED 0x00000001

// Binary header structure (32 bytes)
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_entries;
    uint32_t flags;
    uint32_t pool_size;
    uint32_t reserved[3];
} espyak_dict_header_t;

// Binary entry structure (16 bytes)
typedef struct __attribute__((packed)) {
    uint32_t word_offset;
    uint32_t phoneme_offset;
    uint32_t flags;
    uint32_t reserved;
} espyak_dict_entry_bin_t;

/**
 * @brief Load binary dictionary from memory
 * 
 * @param data Binary dictionary data (must remain valid!)
 * @param size Size of binary data
 * @param out_dict Output dictionary structure
 * @return ESP_OK on success
 */
esp_err_t espyak_dict_load_binary(const uint8_t *data, size_t size, espyak_dictionary_t *out_dict) {
    if (!data || !out_dict) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (size < sizeof(espyak_dict_header_t)) {
        ESP_LOGE(TAG, "Dictionary too small: %zu bytes", size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Parse header
    const espyak_dict_header_t *header = (const espyak_dict_header_t *)data;
    
    if (header->magic != DICT_MAGIC) {
        ESP_LOGE(TAG, "Invalid magic: 0x%08lx (expected 0x%08x)", header->magic, DICT_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (header->version != DICT_VERSION) {
        ESP_LOGE(TAG, "Unsupported version: 0x%08lx", header->version);
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Calculate offsets
    const espyak_dict_entry_bin_t *entries = (const espyak_dict_entry_bin_t *)(data + sizeof(espyak_dict_header_t));
    const char *string_pool = (const char *)(data + sizeof(espyak_dict_header_t) + 
                                              (header->num_entries * sizeof(espyak_dict_entry_bin_t)));
    
    // Validate size
    size_t expected_size = sizeof(espyak_dict_header_t) + 
                           (header->num_entries * sizeof(espyak_dict_entry_bin_t)) +
                           header->pool_size;
    
    if (size < expected_size) {
        ESP_LOGE(TAG, "Dictionary truncated: %zu < %zu bytes", size, expected_size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Compression not yet supported in this version
    if (header->flags & FLAG_COMPRESSED) {
        ESP_LOGE(TAG, "Compressed dictionaries not yet supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    
    // Allocate dictionary entries (pointers to FLASH data)
    espyak_dict_entry_t *dict_entries = calloc(header->num_entries, sizeof(espyak_dict_entry_t));
    if (!dict_entries) {
        ESP_LOGE(TAG, "Failed to allocate %lu entries", header->num_entries);
        return ESP_ERR_NO_MEM;
    }
    
    // Map binary entries to runtime entries
    for (uint32_t i = 0; i < header->num_entries; i++) {
        dict_entries[i].word = string_pool + entries[i].word_offset;
        dict_entries[i].phonemes = string_pool + entries[i].phoneme_offset;
        dict_entries[i].flags = entries[i].flags;
    }
    
    // Fill output structure
    out_dict->entries = dict_entries;
    out_dict->count = header->num_entries;
    out_dict->binary_data = data;  // Keep reference to prevent free
    
    ESP_LOGI(TAG, "Loaded binary dictionary: %lu entries, %lu bytes", 
             header->num_entries, size);
    ESP_LOGI(TAG, "  Memory used: %zu KB (pointers only)", 
             (header->num_entries * sizeof(espyak_dict_entry_t)) / 1024);
    
    return ESP_OK;
}

/**
 * @brief Load binary dictionary from file
 * 
 * @param path File path (e.g., "/spiffs/en.dictbin")
 * @param out_dict Output dictionary structure
 * @return ESP_OK on success
 */
esp_err_t espyak_dict_load_file(const char *path, espyak_dictionary_t *out_dict) {
    if (!path || !out_dict) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Loading dictionary from: %s", path);
    
    // Open file
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(f);
        ESP_LOGE(TAG, "Invalid file size: %ld", size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Allocate buffer
    uint8_t *data = malloc(size);
    if (!data) {
        fclose(f);
        ESP_LOGE(TAG, "Failed to allocate %ld bytes", size);
        return ESP_ERR_NO_MEM;
    }
    
    // Read file
    size_t read = fread(data, 1, size, f);
    fclose(f);
    
    if (read != size) {
        free(data);
        ESP_LOGE(TAG, "Read failed: %zu/%ld bytes", read, size);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Load dictionary
    esp_err_t err = espyak_dict_load_binary(data, size, out_dict);
    if (err != ESP_OK) {
        free(data);
        return err;
    }
    
    // Dictionary now owns the data
    return ESP_OK;
}

/**
 * @brief Free binary dictionary
 * 
 * @param dict Dictionary to free
 */
void espyak_dict_free_binary(espyak_dictionary_t *dict) {
    if (!dict) {
        return;
    }
    
    if (dict->entries) {
        free(dict->entries);
        dict->entries = NULL;
    }
    
    if (dict->binary_data) {
        free((void *)dict->binary_data);
        dict->binary_data = NULL;
    }
    
    dict->count = 0;
}
