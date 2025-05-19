#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include <time.h>
#include <ArduinoJson.h>
#include <Keypad.h>
#include <ESP32Servo.h>

#define WIFI_SSID    "Menes"
#define WIFI_PASS    "deneme123"
#define SERVER_IP    "172.20.10.3"
#define SERVER_PORT  "5000"
#define SERVER_URL   "http://" SERVER_IP ":" SERVER_PORT
#define UPLOAD_URL   SERVER_URL "/upload"
#define LOG_URL      SERVER_URL "/log_access"

// I2S Configuration for Voice Assistant
#define SAMPLE_RATE     16000
#define SAMPLE_BITS     I2S_BITS_PER_SAMPLE_16BIT
#define CHANNEL_FORMAT  I2S_CHANNEL_FMT_ONLY_LEFT
#define RECORD_TIME_SEC 10     // 10 saniye kayıt
#define CHUNK_SIZE      4096   // 4KB chunk boyutu
#define WAV_HEADER_SIZE 44     // WAV başlık boyutu

// I2S Pins (INMP441)
#define I2S0_BCK 14
#define I2S0_WS  13
#define I2S0_SD  15

// DAC Pins (PCM5102A)
#define DAC_BCK 38
#define DAC_WS  39
#define DAC_DIN 37

// Kayıt kontrol pini
#define RECORD_BUTTON 4  // Kayıt butonu için GPIO pin

// Voice Assistant Variables
uint8_t chunk_buffer[CHUNK_SIZE + WAV_HEADER_SIZE];  // Chunk + WAV header için buffer
AudioFileSourceHTTPStream *file;
AudioOutputI2S *out;
AudioGeneratorWAV *wav;

// Time Configuration
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10800;  // UTC+3 for Turkey
const int   daylightOffset_sec = 0;

// Wake word configuration
#define WAKEWORD_TIME_SEC 3     // Wake word için kayıt süresi
#define WAKEWORD_PHRASE "uyan"  // Wake word (küçük harflerle)

// Keypad tanımı
const byte ROWS = 4; // 4 satır
const byte COLS = 4; // 4 sütun
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {4, 5, 6, 7}; // Satır pinleri (değiştirin)
byte colPins[COLS] = {8, 9, 10, 11}; // Sütun pinleri (değiştirin)
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Servo tanımı
Servo doorServo;
#define SERVO_PIN 18 // Servo sinyal pini (değiştirin)
#define SERVO_OPEN 90
#define SERVO_CLOSED 0

// Şifre
const String correctPassword = "1234"; // Değiştirilebilir

// Function declarations
void initTime();
void handleVoiceAssistant();
String getCurrentTime();
bool check_server_connection();
bool checkWakeWord();
void handleKeypadAccess();
void handleRegisterUser();
void handleUserLogin();
String getNameByVoice();
String getCommandByVoice();

// WAV header üretici
void create_wav_header(uint8_t* h, size_t pcm_size, int sr) {
  int byte_rate   = sr * 2;
  int block_align = 2;
  memcpy(h, "RIFF", 4);
  uint32_t cs = pcm_size + 36;
  memcpy(h + 4, &cs, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t sub1 = 16;
  memcpy(h + 16, &sub1, 4);
  h[20] = 1; h[21] = 0;
  h[22] = 1; h[23] = 0;
  memcpy(h + 24, &sr, 4);
  memcpy(h + 28, &byte_rate, 4);
  h[32] = block_align; h[33] = 0;
  h[34] = 16; h[35] = 0;
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &pcm_size, 4);
}

// Wi-Fi bağlantısı
void wifi_connect() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi bağlandı: " + WiFi.localIP().toString());
}

// 1️⃣ Kayıt için I2S_NUM_0: RX-only
void i2s_record_init() {
  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t cfg = {
    .mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate       = SAMPLE_RATE,
    .bits_per_sample   = SAMPLE_BITS,
    .channel_format    = CHANNEL_FORMAT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count     = 4,
    .dma_buf_len       = 1024
  };
  i2s_pin_config_t pins = {
    .bck_io_num    = I2S0_BCK,
    .ws_io_num     = I2S0_WS,
    .data_out_num  = -1,
    .data_in_num   = I2S0_SD
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// 2️⃣ Çalma için I2S_NUM_0: TX-only
void i2s_play_init() {
  i2s_driver_uninstall(I2S_NUM_0);
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = SAMPLE_BITS,
    .channel_format = CHANNEL_FORMAT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = true
  };
  i2s_pin_config_t pins = {
    .bck_io_num = DAC_BCK,
    .ws_io_num = DAC_WS,
    .data_out_num = DAC_DIN,
    .data_in_num = -1
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, SAMPLE_BITS, I2S_CHANNEL_MONO);
}


String send_audio_to_server(uint8_t* data, size_t len) {
  // ➊ WiFiClient'ın timeout'unu 120s yap
  WiFiClient client;
  client.setTimeout(120000);

  HTTPClient http;
  http.begin(client, UPLOAD_URL);
  // ➋ HTTPClient'in de kendi timeout'unu 120s yap (sürümünüz destekliyorsa)
  http.setTimeout(120000);

  http.addHeader("Content-Type", "audio/wav");
  int code = http.sendRequest("POST", data, len);

  String resp = "";
  if (code == HTTP_CODE_OK) {
    resp = http.getString();
    Serial.printf("📨 Sunucudan gelen URL (%d bytes): %s\n", resp.length(), resp.c_str());
  } else {
    Serial.printf("🚫 HTTP hatası: %d %s\n",
                  code,
                  http.errorToString(code).c_str());
  }

  http.end();
  return resp;
}


// ▶️ Çalma: önce TX-only kur, sonra başlat
void play_wav_from_url(const String &url) {
  Serial.println("▶️ Playback başlıyor…");

  // ➊ no manual i2s_play_init();
  file = new AudioFileSourceHTTPStream(url.c_str());
  out  = new AudioOutputI2S();  
  out->SetPinout(DAC_BCK, DAC_WS, DAC_DIN);  
  out->SetGain(2.0);                // try a higher gain  
  wav = new AudioGeneratorWAV();

  if (!wav->begin(file, out)) {
    Serial.println("❌ wav.begin() başarısız!");
    return;
  }
  // optional: block until done
  while (wav->isRunning()) {
    wav->loop();
  }
}

void setup() {
  Serial.begin(115200);
  
  // Kayıt butonu için pin ayarı
  pinMode(RECORD_BUTTON, INPUT_PULLUP);
  
  wifi_connect();
  
  // Initialize components
  initTime();
  
  // Servo başlat
  doorServo.attach(SERVO_PIN);
  doorServo.write(SERVO_CLOSED);
  
  Serial.println("\n=== Sistem Hazır ===");
}

void loop() {
  Serial.println("\n=== Ana Menü ===");
  Serial.println("[A] Sesli Asistan");
  Serial.println("[B] Giriş İşlemleri");
  Serial.println("Seçiminizi yapın (A/B):");
  char choice = 0;
  while (true) {
    char key = keypad.getKey();
    if (key == 'A' || key == 'B') {
      choice = key;
      break;
    }
    delay(50);
  }
  switch (choice) {
    case 'A':
      handleVoiceAssistant();
      break;
    case 'B':
      while (true) {
        Serial.println("\n=== Giriş İşlemleri (sesli komut ile) ===");
        Serial.println("Lütfen yapmak istediğiniz işlemi sesli olarak söyleyin: 'yeni kullanıcı kaydı' veya 'ana menüye dön'");
        String command = getCommandByVoice();
        command.toLowerCase();
        command.trim();
        Serial.print("Algılanan komut: "); Serial.println(command);
        if (command.indexOf("yeni kullanıcı") != -1) {
          // Yeni kullanıcı kaydı akışı
          Serial.println("\nLütfen isminizi sesli olarak söyleyin ve kaydı başlatmak için butona basın...");
          String name = getNameByVoice();
          Serial.print("Algılanan isim: "); Serial.println(name);
          Serial.println("Şimdi 4 haneli şifrenizi girin (bitirmek için #):");
          String password = "";
          while (true) {
            char key = keypad.getKey();
            if (key) {
              if (key == '#') break;
              if (key >= '0' && key <= '9' && password.length() < 4) {
                password += key;
                Serial.print("*");
              }
            }
            delay(50);
          }
          Serial.println();
          if (name.length() == 0 || password.length() != 4) {
            Serial.println("Hatalı giriş! İsim ve 4 haneli şifre zorunlu.");
            break; // Hatalı girişte de ana menüye dön
          }
          // Sunucuya gönder
          WiFiClient client;
          HTTPClient http;
          http.begin(client, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/register_user");
          http.addHeader("Content-Type", "application/json");
          String json = String("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
          int httpCode = http.POST(json);
          if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            Serial.println("\nKayıt başarılı: " + response);
          } else {
            Serial.println("\nKayıt başarısız! HTTP kodu: " + String(httpCode));
          }
          http.end();
          break; // Kayıt sonrası ana menüye dön
        } else if (command.indexOf("ana menü") != -1) {
          break; // Ana menüye dön
        } else if (command.indexOf("giriş yap") != -1) {
          Serial.println("Lütfen isminizi sesli olarak söyleyin ve kaydı başlatmak için butona basın...");
          String name = getNameByVoice();
          name.trim();
          if (name.length() == 0) {
            Serial.println("İsim algılanamadı. Ana menüye dönülüyor.");
            break;
          }
          // Sunucuda bu isim var mı kontrol et
          WiFiClient client;
          HTTPClient http;
          http.begin(client, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/check_user");
          http.addHeader("Content-Type", "application/json");
          String checkJson = String("{\"name\":\"") + name + "\"}";
          int checkCode = http.POST(checkJson);
          if (checkCode == HTTP_CODE_OK) {
            String checkResp = http.getString();
            if (checkResp == "OK") {
              Serial.println("4 haneli şifrenizi girin (bitirmek için #):");
              String password = "";
              while (true) {
                char key = keypad.getKey();
                if (key) {
                  if (key == '#') break;
                  if (key >= '0' && key <= '9' && password.length() < 4) {
                    password += key;
                    Serial.print("*");
                  }
                }
                delay(50);
              }
              Serial.println();
              if (password.length() != 4) {
                Serial.println("Hatalı giriş! 4 haneli şifre zorunlu.");
                break;
              }
              // Şifreyi ve ismi sunucuya gönder
              WiFiClient loginClient;
              HTTPClient loginHttp;
              loginHttp.begin(loginClient, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/verify_user");
              loginHttp.addHeader("Content-Type", "application/json");
              String loginJson = String("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
              int loginCode = loginHttp.POST(loginJson);
              if (loginCode == HTTP_CODE_OK) {
                String response = loginHttp.getString();
                DynamicJsonDocument doc(256);
                DeserializationError error = deserializeJson(doc, response);
                if (!error && doc["status"] == "success") {
                  Serial.println("\nGiriş başarılı! Kapı açılıyor...");
                  doorServo.write(180); // 180 derece döndür
                  delay(2000);
                  doorServo.write(SERVO_CLOSED);
                  Serial.println("Kapı kapandı.");
                  // Welcome mesajı
                  String welcomeText = "Welcome " + name;
                  WiFiClient ttsClient;
                  HTTPClient ttsHttp;
                  ttsHttp.begin(ttsClient, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/upload");
                  ttsHttp.addHeader("Content-Type", "application/json");
                  String ttsJson = String("{\"text\":\"") + welcomeText + "\"}";
                  int ttsCode = ttsHttp.POST(ttsJson);
                  if (ttsCode == HTTP_CODE_OK) {
                    String ttsUrl = ttsHttp.getString();
                    if (ttsUrl.startsWith("http")) {
                      play_wav_from_url(ttsUrl);
                    }
                  }
                  ttsHttp.end();
                } else {
                  Serial.println("\nGiriş başarısız! Şifre yanlış.");
                }
              } else {
                Serial.println("\nGiriş başarısız! HTTP kodu: " + String(loginCode));
              }
              loginHttp.end();
            } else {
              Serial.println("Böyle bir kullanıcı bulunamadı. Ana menüye dönülüyor.");
            }
          } else {
            Serial.println("Sunucuya erişilemedi. Ana menüye dönülüyor.");
          }
          http.end();
          break;
        } else if (command.indexOf("en son kim girmiş") != -1) {
          // Sunucudan en son giriş yapanı al
          WiFiClient client;
          HTTPClient http;
          http.begin(client, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/last_login");
          int httpCode = http.GET();
          if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            Serial.println("Son giriş yapan: " + response);
            // Welcome gibi hoparlörden de oku
            String ttsText = "Son giriş yapan: " + response;
            WiFiClient ttsClient;
            HTTPClient ttsHttp;
            ttsHttp.begin(ttsClient, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/upload");
            ttsHttp.addHeader("Content-Type", "application/json");
            String ttsJson = String("{\"text\":\"") + ttsText + "\"}";
            int ttsCode = ttsHttp.POST(ttsJson);
            if (ttsCode == HTTP_CODE_OK) {
              String ttsUrl = ttsHttp.getString();
              if (ttsUrl.startsWith("http")) {
                play_wav_from_url(ttsUrl);
              }
            }
            ttsHttp.end();
          } else {
            Serial.println("Sunucudan bilgi alınamadı.");
          }
          http.end();
          break;
        } else {
          Serial.println("Komut anlaşılamadı. Lütfen tekrar deneyin.");
        }
      }
      break;
    default:
      Serial.println("Geçersiz seçim!");
      break;
  }
}

void initTime() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("✅ Zaman sunucusu ayarlandı");
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Time Error";
  }
  char timeString[20];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

// Server bağlantı kontrolü
bool check_server_connection() {
  WiFiClient client;
  HTTPClient http;
  
  Serial.println("🔍 Sunucu bağlantısı kontrol ediliyor...");
  Serial.println("URL: " SERVER_URL);
  
  if (!http.begin(client, SERVER_URL)) {
    Serial.println("❌ HTTP bağlantısı başlatılamadı!");
    return false;
  }
  
  int httpCode = http.GET();
  http.end();
  
  if (httpCode > 0) {
    Serial.printf("✅ Sunucu yanıt verdi (HTTP %d)\n", httpCode);
    return true;
  } else {
    Serial.printf("❌ Sunucuya erişilemiyor: %s\n", http.errorToString(httpCode).c_str());
    return false;
  }
}

void handleVoiceAssistant() {
  // Önce wake word kontrolü yap
  if (!checkWakeWord()) {
    Serial.println("❌ Wake word algılanmadı, asistan başlatılmıyor.");
    return;
  }
  
  Serial.println("✨ Wake word doğrulandı, asistan başlatılıyor...");
  delay(500); // Kısa bir bekleme
  
  // Sunucu bağlantısını kontrol et
  if (!check_server_connection()) {
    Serial.println("❌ Sunucu bağlantısı kurulamadı! Kayıt iptal ediliyor.");
    return;
  }
  
  // Önce mevcut I2S sürücüsünü temizle
  i2s_driver_uninstall(I2S_NUM_0);
  delay(100);
  
  i2s_record_init();
  Serial.println("🎙️ Ses kaydı başlıyor...");
  
  // Oturum ID'si oluştur
  String session_id = String(random(0xFFFFFFFF), HEX);
  
  // WiFi bağlantısını kontrol et
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi bağlantısı kopmuş! Yeniden bağlanılıyor...");
    wifi_connect();
  }
  
  // WiFi client'ı hazırla
  WiFiClient client;
  client.setTimeout(120000);  // 120 saniye timeout
  
  HTTPClient http;
  http.begin(client, UPLOAD_URL);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Session-ID", session_id);
  http.setTimeout(120000);
  
  // İlk chunk için WAV header oluştur
  size_t total_data_size = SAMPLE_RATE * RECORD_TIME_SEC * 2;  // PCM veri boyutu
  create_wav_header(chunk_buffer, total_data_size, SAMPLE_RATE);
  
  // Kayıt ve gönderme döngüsü
  size_t total_bytes = 0;
  unsigned long start_time = millis();
  bool first_chunk = true;
  String url = "";
  int chunk_count = 0;
  int http_errors = 0;  // HTTP hata sayacı
  
  while ((millis() - start_time) < RECORD_TIME_SEC * 1000) {
    size_t bytes_read = 0;
    
    // I2S'den veri oku
    esp_err_t result = i2s_read(I2S_NUM_0, 
                               first_chunk ? chunk_buffer + WAV_HEADER_SIZE : chunk_buffer, 
                               CHUNK_SIZE, 
                               &bytes_read, 
                               portMAX_DELAY);
    
    if (result == ESP_OK && bytes_read > 0) {
      // HTTP başlıklarını ayarla
      http.addHeader("X-First-Chunk", first_chunk ? "true" : "false");
      http.addHeader("X-Last-Chunk", (millis() - start_time) >= (RECORD_TIME_SEC * 1000 - 100) ? "true" : "false");
      
      // Chunk'ı gönder
      int httpCode;
      if (first_chunk) {
        // İlk chunk WAV header ile birlikte gönderilir
        httpCode = http.POST(chunk_buffer, bytes_read + WAV_HEADER_SIZE);
        first_chunk = false;
      } else {
        httpCode = http.POST(chunk_buffer, bytes_read);
      }
      
      if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        if (response.length() > 10) {  // URL gelmiş olabilir
          url = response;  // Son URL'i sakla
          Serial.println("✅ Sunucudan URL alındı: " + url);
        } else if (response.length() > 0) {
          Serial.println("ℹ️ Sunucu yanıtı: " + response);
        }
        chunk_count++;
        total_bytes += bytes_read;
        
        // Her saniye durum mesajı
        if ((millis() - start_time) % 1000 < 100) {
          Serial.printf("⏺️ Kayıt sürüyor: %d saniye, %u bytes gönderildi (%d chunk)\n", 
                       (millis() - start_time) / 1000,
                       total_bytes,
                       chunk_count);
        }
      } else {
        http_errors++;
        Serial.printf("❌ HTTP hatası (chunk %d): %d - %s\n", 
                     chunk_count, 
                     httpCode,
                     http.errorToString(httpCode).c_str());
        
        // Çok fazla hata varsa işlemi sonlandır
        if (http_errors > 5) {
          Serial.println("❌ Çok fazla HTTP hatası, işlem iptal ediliyor!");
          break;
        }
        
        // WiFi bağlantısını kontrol et ve gerekirse yeniden bağlan
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("❌ WiFi bağlantısı kopmuş! Yeniden bağlanılıyor...");
          wifi_connect();
          // HTTP client'ı yeniden başlat
          http.end();
          http.begin(client, UPLOAD_URL);
          http.addHeader("Content-Type", "audio/wav");
          http.addHeader("X-Session-ID", session_id);
          http.setTimeout(120000);
        }
      }
    }
  }
  
  // I2S'i durdur ve temizle
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  
  Serial.printf("✅ Kayıt tamamlandı. Toplam %u bytes gönderildi (%d chunk)\n", 
                total_bytes + WAV_HEADER_SIZE,
                chunk_count);
  
  if (http_errors > 0) {
    Serial.printf("⚠️ Toplam %d HTTP hatası oluştu\n", http_errors);
  }
  
  http.end();
  
  // Yanıt URL'i gelirse sesi çal
  if (url.length() > 0) {
    Serial.println("🔊 Yanıt çalınıyor: " + url);
    play_wav_from_url(url);
  } else {
    Serial.println("❌ URL alınamadı - Sunucu yanıt vermemiş olabilir");
  }
}

// Wake word kontrolü için ses kaydı ve sunucuya gönderme
bool checkWakeWord() {
  Serial.println("\n🎤 Wake word bekleniyor...");
  
  // Sunucu bağlantısını kontrol et
  if (!check_server_connection()) {
    Serial.println("❌ Sunucu bağlantısı kurulamadı!");
    return false;
  }
  
  // I2S'i kayıt için hazırla
  i2s_driver_uninstall(I2S_NUM_0);
  delay(100);
  i2s_record_init();
  
  bool wake_word_detected = false;
  int attempt = 1;
  
  while (!wake_word_detected) {
    Serial.printf("\n🔄 Dinleme denemesi #%d\n", attempt++);
    
    // Oturum ID'si oluştur
    String session_id = String(random(0xFFFFFFFF), HEX);
    
    // WiFi client'ı hazırla
    WiFiClient client;
    client.setTimeout(120000);
    
    HTTPClient http;
    http.begin(client, UPLOAD_URL);
    http.addHeader("Content-Type", "audio/wav");
    http.addHeader("X-Session-ID", session_id);
    http.addHeader("X-Wake-Check", "true");  // Wake word kontrolü için özel header
    http.setTimeout(120000);
    
    // WAV header oluştur
    size_t total_data_size = SAMPLE_RATE * WAKEWORD_TIME_SEC * 2;
    create_wav_header(chunk_buffer, total_data_size, SAMPLE_RATE);
    
    // Kayıt ve gönderme döngüsü
    size_t total_bytes = 0;
    unsigned long start_time = millis();
    bool first_chunk = true;
    String transcription = "";
    int chunk_count = 0;
    
    Serial.println("🎙️ Dinleniyor...");
    
    while ((millis() - start_time) < WAKEWORD_TIME_SEC * 1000) {
      size_t bytes_read = 0;
      
      esp_err_t result = i2s_read(I2S_NUM_0, 
                                 first_chunk ? chunk_buffer + WAV_HEADER_SIZE : chunk_buffer, 
                                 CHUNK_SIZE, 
                                 &bytes_read, 
                                 portMAX_DELAY);
      
      if (result == ESP_OK && bytes_read > 0) {
        http.addHeader("X-First-Chunk", first_chunk ? "true" : "false");
        http.addHeader("X-Last-Chunk", (millis() - start_time) >= (WAKEWORD_TIME_SEC * 1000 - 100) ? "true" : "false");
        
        int httpCode;
        if (first_chunk) {
          httpCode = http.POST(chunk_buffer, bytes_read + WAV_HEADER_SIZE);
          first_chunk = false;
        } else {
          httpCode = http.POST(chunk_buffer, bytes_read);
        }
        
        if (httpCode == HTTP_CODE_OK) {
          String response = http.getString();
          if (response.length() > 0 && !response.startsWith("http")) {
            transcription = response;  // Sunucudan gelen metni sakla
          }
          chunk_count++;
          total_bytes += bytes_read;
        } else {
          Serial.printf("❌ HTTP hatası: %d\n", httpCode);
        }
      }
    }
    
    // I2S'i durdur ve yeniden başlat
    i2s_stop(I2S_NUM_0);
    i2s_start(I2S_NUM_0);
    
    http.end();
    
    Serial.printf("✅ Kayıt tamamlandı. Toplam %u bytes gönderildi (%d chunk)\n", 
                  total_bytes + WAV_HEADER_SIZE,
                  chunk_count);
    
    // Transcription'ı küçük harfe çevir ve boşlukları temizle
    transcription.toLowerCase();
    transcription.trim();
    
    Serial.printf("🗣️ Algılanan metin: '%s'\n", transcription.c_str());
    Serial.printf("🎯 Beklenen wake word: '%s'\n", WAKEWORD_PHRASE);
    
    // Wake word kontrolü
    if (transcription.indexOf(WAKEWORD_PHRASE) != -1) {
      Serial.println("✅ Wake word tespit edildi!");
      wake_word_detected = true;
    } else {
      Serial.println("❌ Wake word tespit edilemedi, tekrar dinleniyor...");
      delay(100); // Kısa bir bekleme
    }
  }
  
  // I2S'i temizle
  i2s_driver_uninstall(I2S_NUM_0);
  return true;
}

void handleKeypadAccess() {
  Serial.println("\n=== Şifreli Giriş Sistemi ===");
  Serial.println("4 haneli şifreyi girin:");
  String input = "";
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') {
        if (input.length() == 4) {
          if (input == correctPassword) {
            Serial.println("\n✅ Şifre doğru! Kapı açılıyor...");
            doorServo.write(SERVO_OPEN);
            delay(2000); // Kapı açık kalma süresi
            doorServo.write(SERVO_CLOSED);
            Serial.println("Kapı kapandı.");
          } else {
            Serial.println("\n❌ Şifre yanlış!");
          }
          input = "";
          Serial.println("Yeniden şifre girin:");
        } else {
          Serial.println("\nLütfen 4 haneli şifre girin!");
          input = "";
        }
      } else if (key == '*') {
        input = "";
        Serial.println("Giriş sıfırlandı. Tekrar girin:");
      } else if (input.length() < 4 && key >= '0' && key <= '9') {
        input += key;
        Serial.print("*");
      }
    }
    delay(50);
  }
}

// Kullanıcı kayıt fonksiyonu
void handleRegisterUser() {
  Serial.println("\n=== Yeni Kullanıcı Oluştur ===");
  String name = "";
  String password = "";
  Serial.println("Kullanıcı adı girin (en az 1 karakter, bitirmek için #):");
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') break;
      if (key != '*' && name.length() < 16) {
        name += key;
        Serial.print(key);
      }
    }
    delay(50);
  }
  Serial.println();
  Serial.println("4 haneli şifre girin (bitirmek için #):");
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') break;
      if (key >= '0' && key <= '9' && password.length() < 4) {
        password += key;
        Serial.print("*");
      }
    }
    delay(50);
  }
  Serial.println();
  if (name.length() == 0 || password.length() != 4) {
    Serial.println("Hatalı giriş! Kullanıcı adı ve 4 haneli şifre zorunlu.");
    return;
  }
  // Sunucuya gönder
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/register_user");
  http.addHeader("Content-Type", "application/json");
  String json = String("{\"name\":\"") + name + "\",\"password\":\"" + password + "\"}";
  int httpCode = http.POST(json);
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    Serial.println("\nKayıt başarılı: " + response);
  } else {
    Serial.println("\nKayıt başarısız! HTTP kodu: " + String(httpCode));
  }
  http.end();
}

// Kullanıcı giriş fonksiyonu
void handleUserLogin() {
  Serial.println("\n=== Giriş Yap ===");
  String password = "";
  Serial.println("4 haneli şifrenizi girin (bitirmek için #):");
  while (true) {
    char key = keypad.getKey();
    if (key) {
      if (key == '#') break;
      if (key >= '0' && key <= '9' && password.length() < 4) {
        password += key;
        Serial.print("*");
      }
    }
    delay(50);
  }
  Serial.println();
  if (password.length() != 4) {
    Serial.println("Hatalı giriş! 4 haneli şifre zorunlu.");
    return;
  }
  // Sunucuya gönder
  WiFiClient client;
  HTTPClient http;
  http.begin(client, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/verify_user");
  http.addHeader("Content-Type", "application/json");
  String json = String("{\"password\":\"") + password + "\"}";
  int httpCode = http.POST(json);
  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, response);
    if (!error && doc["status"] == "success") {
      Serial.println("\nGiriş başarılı! Kapı açılıyor...");
      doorServo.write(180); // 180 derece döndür
      delay(2000);
      doorServo.write(SERVO_CLOSED);
      Serial.println("Kapı kapandı.");
      // Welcome mesajı
      String name = doc["name"].as<String>();
      String welcomeText = "Welcome " + name;
      // Sunucuya metni gönderip ses dosyası al
      WiFiClient ttsClient;
      HTTPClient ttsHttp;
      ttsHttp.begin(ttsClient, String("http://") + SERVER_IP + ":" + SERVER_PORT + "/upload");
      ttsHttp.addHeader("Content-Type", "application/json");
      String ttsJson = String("{\"text\":\"") + welcomeText + "\"}";
      int ttsCode = ttsHttp.POST(ttsJson);
      if (ttsCode == HTTP_CODE_OK) {
        String ttsUrl = ttsHttp.getString();
        if (ttsUrl.startsWith("http")) {
          play_wav_from_url(ttsUrl);
        }
      }
      ttsHttp.end();
    } else {
      Serial.println("\nGiriş başarısız! Şifre yanlış veya kullanıcı yok.");
    }
  } else {
    Serial.println("\nGiriş başarısız! HTTP kodu: " + String(httpCode));
  }
  http.end();
}

// getNameByVoice fonksiyonu ekle
String getNameByVoice() {
  Serial.println("Ses kaydı başlatılıyor...");
  // handleVoiceAssistant fonksiyonundaki gibi ses kaydını başlatıp sunucuya gönder
  // ve dönen metni isim olarak al
  // (Kısa süreli kayıt ve wake word kontrolü olmadan, direkt metin alınacak şekilde)
  // Burada örnek olarak handleVoiceAssistant fonksiyonunun bir kısmı tekrar kullanılabilir
  // Kısa kayıt süresi (ör: 3 saniye)
  i2s_driver_uninstall(I2S_NUM_0);
  delay(100);
  i2s_record_init();
  Serial.println("🎙️ İsim için ses kaydı başlıyor...");
  String session_id = String(random(0xFFFFFFFF), HEX);
  WiFiClient client;
  client.setTimeout(120000);
  HTTPClient http;
  http.begin(client, UPLOAD_URL);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Session-ID", session_id);
  http.addHeader("X-Wake-Check", "true"); // Sadece metin dönecek
  http.setTimeout(120000);
  size_t total_data_size = SAMPLE_RATE * 3 * 2; // 3 saniye kayıt
  create_wav_header(chunk_buffer, total_data_size, SAMPLE_RATE);
  size_t total_bytes = 0;
  unsigned long start_time = millis();
  bool first_chunk = true;
  String transcription = "";
  int chunk_count = 0;
  while ((millis() - start_time) < 3000) {
    size_t bytes_read = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, first_chunk ? chunk_buffer + WAV_HEADER_SIZE : chunk_buffer, CHUNK_SIZE, &bytes_read, portMAX_DELAY);
    if (result == ESP_OK && bytes_read > 0) {
      http.addHeader("X-First-Chunk", first_chunk ? "true" : "false");
      http.addHeader("X-Last-Chunk", (millis() - start_time) >= (3000 - 100) ? "true" : "false");
      int httpCode;
      if (first_chunk) {
        httpCode = http.POST(chunk_buffer, bytes_read + WAV_HEADER_SIZE);
        first_chunk = false;
      } else {
        httpCode = http.POST(chunk_buffer, bytes_read);
      }
      if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        if (response.length() > 0 && !response.startsWith("http")) {
          transcription = response;
        }
        chunk_count++;
        total_bytes += bytes_read;
      }
    }
  }
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  transcription.trim();
  Serial.print("🗣️ Algılanan isim: "); Serial.println(transcription);
  return transcription;
}

// getCommandByVoice fonksiyonu ekle
String getCommandByVoice() {
  Serial.println("Komut için ses kaydı başlatılıyor...");
  // 3 saniyelik kısa kayıt ile komut alınacak
  i2s_driver_uninstall(I2S_NUM_0);
  delay(100);
  i2s_record_init();
  Serial.println("🎙️ Komut için ses kaydı başlıyor...");
  String session_id = String(random(0xFFFFFFFF), HEX);
  WiFiClient client;
  client.setTimeout(120000);
  HTTPClient http;
  http.begin(client, UPLOAD_URL);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Session-ID", session_id);
  http.addHeader("X-Wake-Check", "true"); // Sadece metin dönecek
  http.setTimeout(120000);
  size_t total_data_size = SAMPLE_RATE * 3 * 2; // 3 saniye kayıt
  create_wav_header(chunk_buffer, total_data_size, SAMPLE_RATE);
  size_t total_bytes = 0;
  unsigned long start_time = millis();
  bool first_chunk = true;
  String transcription = "";
  int chunk_count = 0;
  while ((millis() - start_time) < 3000) {
    size_t bytes_read = 0;
    esp_err_t result = i2s_read(I2S_NUM_0, first_chunk ? chunk_buffer + WAV_HEADER_SIZE : chunk_buffer, CHUNK_SIZE, &bytes_read, portMAX_DELAY);
    if (result == ESP_OK && bytes_read > 0) {
      http.addHeader("X-First-Chunk", first_chunk ? "true" : "false");
      http.addHeader("X-Last-Chunk", (millis() - start_time) >= (3000 - 100) ? "true" : "false");
      int httpCode;
      if (first_chunk) {
        httpCode = http.POST(chunk_buffer, bytes_read + WAV_HEADER_SIZE);
        first_chunk = false;
      } else {
        httpCode = http.POST(chunk_buffer, bytes_read);
      }
      if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        if (response.length() > 0 && !response.startsWith("http")) {
          transcription = response;
        }
        chunk_count++;
        total_bytes += bytes_read;
      }
    }
  }
  i2s_stop(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  transcription.trim();
  Serial.print("🗣️ Algılanan komut: "); Serial.println(transcription);
  return transcription;
}