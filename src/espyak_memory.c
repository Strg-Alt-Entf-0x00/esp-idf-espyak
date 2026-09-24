/**
 * @file espyak_memory.c
 * @brief ESP32-P4 optimized memory management
 * 
 * Smart memory allocation with PSRAM/DRAM placement strategy:
 * - Hot path data → DRAM (low latency)
 * - Large static data → PSRAM (dictionary, phoneme tables)
 * - Cache-aligned allocation for DMA
 */

#include "espyak_internal.h"
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#ifdef CONFIG_ESPYAK_USE_PSRAM
#include <esp_psram.h>
#endif

static const char* TAG = "espyak_mem";

/* Memory allocation strategy:
 * - If PSRAM enabled and available: prefer PSRAM for large allocations
 * - Fallback to DRAM if PSRAM unavailable or allocation fails
 * - Always use DRAM for small (<1KB) hot-path structures
 */

#define PSRAM_THRESHOLD 1024  /* Allocations >= 1KB go to PSRAM if enabled */

void* espyak_malloc(size_t size, const espyak_config_t* config) {
    if (size == 0) {
        return NULL;
    }
    
    uint32_t caps = MALLOC_CAP_8BIT;
    
#ifdef CONFIG_ESPYAK_USE_PSRAM
    if (config && config->use_psram && size >= PSRAM_THRESHOLD) {
        /* Try PSRAM first for large allocations */
        caps |= MALLOC_CAP_SPIRAM;
        void* ptr = heap_caps_malloc(size, caps);
        if (ptr) {
            return ptr;
        }
        
        /* Fallback to DRAM if PSRAM failed */
        ESP_LOGW(TAG, "PSRAM allocation failed (%zu bytes), falling back to DRAM", size);
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    } else {
        /* Small allocations always go to DRAM for low latency */
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
#else
    (void)config;
    caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    
    void* ptr = heap_caps_malloc(size, caps);
    if (!ptr) {
        ESP_LOGE(TAG, "Memory allocation failed: %zu bytes", size);
    }
    
    return ptr;
}

void* espyak_calloc(size_t nmemb, size_t size, const espyak_config_t* config) {
    if (nmemb == 0 || size == 0) {
        return NULL;
    }
    
    size_t total = nmemb * size;
    
    /* Check for overflow */
    if (total / nmemb != size) {
        ESP_LOGE(TAG, "Integer overflow in calloc: %zu * %zu", nmemb, size);
        return NULL;
    }
    
    uint32_t caps = MALLOC_CAP_8BIT;
    
#ifdef CONFIG_ESPYAK_USE_PSRAM
    if (config && config->use_psram && total >= PSRAM_THRESHOLD) {
        caps |= MALLOC_CAP_SPIRAM;
        void* ptr = heap_caps_calloc(nmemb, size, caps);
        if (ptr) {
            return ptr;
        }
        
        ESP_LOGW(TAG, "PSRAM calloc failed (%zu bytes), falling back to DRAM", total);
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    } else {
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
#else
    (void)config;
    caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    
    void* ptr = heap_caps_calloc(nmemb, size, caps);
    if (!ptr) {
        ESP_LOGE(TAG, "Memory calloc failed: %zu * %zu = %zu bytes", nmemb, size, total);
    }
    
    return ptr;
}

void* espyak_realloc(void* ptr, size_t size, const espyak_config_t* config) {
    if (size == 0) {
        if (ptr) {
            heap_caps_free(ptr);
        }
        return NULL;
    }
    
    if (!ptr) {
        return espyak_malloc(size, config);
    }
    
    /* heap_caps_realloc maintains the original memory type (PSRAM/DRAM) */
    void* new_ptr = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    if (!new_ptr) {
        ESP_LOGE(TAG, "Memory realloc failed: %zu bytes", size);
    }
    
    return new_ptr;
}

void espyak_free(void* ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

char* espyak_strdup(const char* s, const espyak_config_t* config) {
    if (!s) {
        return NULL;
    }
    
    size_t len = strlen(s) + 1;
    char* copy = espyak_malloc(len, config);
    if (copy) {
        memcpy(copy, s, len);
    }
    
    return copy;
}

/* ========================================================================
 * Cache-Aligned Allocation (for DMA buffers)
 * ======================================================================== */

void* espyak_malloc_aligned(size_t size, size_t alignment, 
                            const espyak_config_t* config) {
    if (size == 0 || alignment == 0) {
        return NULL;
    }
    
    /* Alignment must be power of 2 */
    if ((alignment & (alignment - 1)) != 0) {
        ESP_LOGE(TAG, "Alignment must be power of 2: %zu", alignment);
        return NULL;
    }
    
    uint32_t caps = MALLOC_CAP_8BIT;
    
#ifdef CONFIG_ESPYAK_USE_PSRAM
    if (config && config->use_psram && size >= PSRAM_THRESHOLD) {
        caps |= MALLOC_CAP_SPIRAM;
    } else {
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
#else
    (void)config;
    caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    
    void* ptr = heap_caps_aligned_alloc(alignment, size, caps);
    if (!ptr) {
        ESP_LOGE(TAG, "Aligned allocation failed: %zu bytes (align %zu)", size, alignment);
    }
    
    return ptr;
}

/* ========================================================================
 * Memory Profiling (DEBUG)
 * ======================================================================== */

#ifdef CONFIG_ESPYAK_DEBUG_LOGGING

void espyak_print_heap_info(void) {
    ESP_LOGI(TAG, "=== Heap Information ===");
    
    /* Internal DRAM */
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "DRAM: %zu / %zu bytes free (%.1f%% used)",
             internal_free, internal_total,
             100.0 * (1.0 - (double)internal_free / internal_total));
    
#ifdef CONFIG_ESPYAK_USE_PSRAM
    /* External PSRAM */
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (spiram_total > 0) {
        ESP_LOGI(TAG, "PSRAM: %zu / %zu bytes free (%.1f%% used)",
                 spiram_free, spiram_total,
                 100.0 * (1.0 - (double)spiram_free / spiram_total));
    } else {
        ESP_LOGI(TAG, "PSRAM: Not available");
    }
#endif
    
    /* Largest free block */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Largest free block: %zu bytes", largest);
    
    /* Minimum ever free */
    size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Minimum free (low water mark): %zu bytes", min_free);
}

#else

void espyak_print_heap_info(void) {
    /* No-op in release builds */
}

#endif /* CONFIG_ESPYAK_DEBUG_LOGGING */
