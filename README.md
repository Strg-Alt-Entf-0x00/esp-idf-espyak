# esp-idf-espyak
**A lightweight, PSRAM-optimized English Phonemizer for ESP32 and ESP-IDF.**

`espyak` is a pure C/C++ port of a text-to-phoneme engine designed specifically for embedded systems. It replaces the incredibly heavy `eSpeak-NG` dependency for text-to-speech (TTS) projects, making it possible to run neural TTS (like VITS or Piper) on memory-constrained microcontrollers like the **ESP32-S3** and **ESP32-P4**.

## 🌟 Features
* **Zero OS Dependencies:** Pure C/C++ implementation. No Linux/POSIX dependencies.
* **PSRAM Optimized:** The heavy English dictionaries (~140 KB compressed) are loaded directly into PSRAM via `MALLOC_CAP_SPIRAM`, keeping the critical internal SRAM free for your Neural Networks.
* **Extremely Fast:** Optimized binary search for dictionary lookups. Tokenization happens in milliseconds.
* **Component Registry Ready:** Built natively as an ESP-IDF component (`idf_component.yml` included).

## 📦 Installation
You can easily add `espyak` to your ESP-IDF project via the IDF Component Manager (or via Git URL).

In your project's `main/idf_component.yml` (or wherever your components live), add:
```yaml
dependencies:
  espyak:
    git: https://github.com/Strg-Alt-Entf-0x00/esp-idf-espyak.git
```
*(Once published to the official Espressif registry, this will just be `Strg-Alt-Entf-0x00/espyak: "*"`)*

## 🚀 Usage

```cpp
#include "espyak.h"
#include "esp_log.h"

void app_main() {
    // 1. Initialize espyak for English
    espyak_init("en");

    // 2. Tokenize a sentence
    const char *text = "Hello, I am an ESP32!";
    espyak_result_t result;
    
    if (espyak_tokenize(text, &result)) {
        ESP_LOGI("TTS", "Phonemes: %s", result.phonemes);
        // Do something with result.tokens (e.g. pass to VITS Vocoder)
    }

    // 3. Clean up
    espyak_deinit();
}
```

## 🛠 Configuration (menuconfig)
Under `Component config` -> `espyak Phonemizer`, you can configure:
* **Use binary dictionary format:** Enabled by default. Uses a compressed binary structure.
* **Enable dictionary compression (LZ4):** Reduces the flash footprint of the dictionaries by ~50%.

## 📝 License
This project is open-source. Please see the `LICENSE` file for details.
