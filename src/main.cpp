#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "FS.h"
#include "SPIFFS.h"

// Wi-Fi Bilgileri (kendine göre değiştir)
const char* ssid = "sinem";
const char* password = "sinem1234";

// Flask sunucusunun IP adresi
const char* serverURL = "http://192.168.137.126:5000/upload_audio";

// I2S mikrofon pinleri (INMP441 için)
#define I2S_WS  13
#define I2S_SD  15
#define I2S_SCK 14

#define I2S_MIC_PORT I2S_NUM_0
#define I2S_DAC_PORT I2S_NUM_1

// I2S DAC pinleri
#define I2S_DAC_BCK  38   // BCK (Bit Clock)
#define I2S_DAC_WS   39   // LCK (Word Select / LRCK)
#define I2S_DAC_DOUT 37   // DIN (Data In)

// Ses örnekleme ayarları
#define SAMPLE_RATE     16000
#define SAMPLE_TIME_SEC 3
#define SAMPLE_SIZE     (SAMPLE_RATE * SAMPLE_TIME_SEC * 2)

void playPCMFromSPIFFS(const char* path);

void setupI2SMic() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_MIC_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

void setupI2SDAC() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_DAC_BCK,
    .ws_io_num = I2S_DAC_WS,
    .data_out_num = I2S_DAC_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_DAC_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_DAC_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_DAC_PORT);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS başlatılamadı!");
    return;
  }
  Serial.println("✅ SPIFFS başlatıldı.");

  WiFi.begin(ssid, password);
  Serial.print("📡 WiFi bağlanıyor");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi bağlı: " + WiFi.localIP().toString());

  setupI2SMic();
  setupI2SDAC();

  Serial.println("🔘 Lütfen sorgu başlatmak için '1' tuşuna basın.");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1') {
      Serial.println("🎬 Sorgu başlatılıyor...");

      uint8_t* buffer = (uint8_t*)malloc(SAMPLE_SIZE);
      if (!buffer) {
        Serial.println("💥 Bellek ayrımı başarısız");
        return;
      }

      size_t bytesRead = 0, totalRead = 0;
      Serial.println("🎙️ 3 saniye kayıt başlıyor...");
      while (totalRead < SAMPLE_SIZE) {
        i2s_read(I2S_MIC_PORT, buffer + totalRead, SAMPLE_SIZE - totalRead, &bytesRead, portMAX_DELAY);
        totalRead += bytesRead;
      }

      HTTPClient http;
      http.begin(serverURL);
      http.setTimeout(20000);  // 🔧 20 saniye timeout eklendi
      http.addHeader("Content-Type", "application/octet-stream");

      Serial.println("📤 Sunucuya gönderiliyor...");
      int httpResponseCode = http.POST(buffer, SAMPLE_SIZE);

      if (httpResponseCode > 0) {
        Serial.printf("✅ Sunucu yanıtı: %d\n", httpResponseCode);

        WiFiClient* stream = http.getStreamPtr();
        File audioFile = SPIFFS.open("/response.pcm", FILE_WRITE);
        if (audioFile && stream) {
          uint8_t temp[128];
          while (http.connected() && stream->available()) {
            size_t len = stream->readBytes(temp, sizeof(temp));
            audioFile.write(temp, len);
          }
          audioFile.close();
          Serial.println("📁 Yanıt SPIFFS'e kaydedildi: /response.pcm");

          i2s_driver_uninstall(I2S_MIC_PORT);
          i2s_driver_uninstall(I2S_DAC_PORT); // 🔧 DAC driver kaldırılıyor
          setupI2SDAC();                      // 🔄 Yeniden başlatılıyor
          playPCMFromSPIFFS("/response.pcm");
        } else {
          Serial.println("❌ Yanıt dosyası oluşturulamadı.");
        }
      } else {
        Serial.printf("❌ HTTP Hatası: %s\n", http.errorToString(httpResponseCode).c_str());
      }

      http.end();
      free(buffer);
    }
  }
}

void playPCMFromSPIFFS(const char* path) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    Serial.println("❌ PCM dosyası açılamadı!");
    return;
  }
  Serial.println("🔊 Ses çalınıyor...");
  uint8_t buffer[512];
  size_t bytesRead;
  while ((bytesRead = file.read(buffer, sizeof(buffer))) > 0) {
    size_t bytes_written = 0;
    i2s_write(I2S_DAC_PORT, buffer, bytesRead, &bytes_written, portMAX_DELAY);
    Serial.printf("bytes_written: %d\n", bytes_written);
  }
  file.close();
  Serial.println("✅ Ses çalma tamamlandı.");
}