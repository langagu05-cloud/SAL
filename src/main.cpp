#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"
#include <time.h>
#include <esp_wifi.h> 

// ============================================================================
// CONFIGURAÇÃO DE REDE E API
// ============================================================================
const char* WIFI_SSID = "LANGA";
const char* WIFI_PASS = "12345678";

// Chave da API do Gemini inserida diretamente
const char* GEMINI_API_KEY   = "AQ.Ab8RN6IHnIPnvblf0lPSnEJBgpH8C9wOsELarpF_niyO060a6g"; 
const char* GEMINI_HOST      = "generativelanguage.googleapis.com";
const char* GEMINI_MODEL     = "gemini-2.5-flash";

// --- Pinos (ESP32 WROOM + INMP441) ---
#define MIC_PORT  I2S_NUM_0
#define MIC_SCK   14
#define MIC_WS    15
#define MIC_SD    32

#define PIN_LED       2

// --- Áudio ---
#define MIC_SAMPLE_RATE   8000
#define ACTUAL_REC_SEC    3     
#define WAV_HEADER_SIZE   44
#define PCM_BYTES         (MIC_SAMPLE_RATE * ACTUAL_REC_SEC * 2)
#define AUDIO_BUF_SIZE    (WAV_HEADER_SIZE + PCM_BYTES)
#define MIC_GAIN_SHIFT    12
#define VAD_THRESHOLD     1200  

uint8_t* audioBuf = NULL;

static inline size_t minz(size_t a, size_t b) { return a < b ? a : b; }

// ============================================================================
// REDE: CONEXÃO LIMPA PARA O CELULAR (HOTSPOT)
// ============================================================================
void syncTime() {
  configTime(-10800, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[INFO] Sincronizando relogio NTP...");
  struct tm timeinfo;
  int tentativas = 0;
  while (!getLocalTime(&timeinfo) && tentativas < 10) {
    Serial.print(".");
    delay(500);
    tentativas++;
  }
  if (tentativas < 10) {
    Serial.println("\n[INFO] Relogio sincronizado com sucesso!");
  } else {
    Serial.println("\n[AVISO] Falha ao sincronizar hora, mas continuando...");
  }
}

void wifiEnsure() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("\n==================================================");
  Serial.print("[WiFi] TESTANDO NO CELULAR (HOTSPOT): ");
  Serial.println(WIFI_SSID);
  
  WiFi.disconnect(true);
  delay(1000);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t tStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - tStart > 20000) {
      Serial.println("\n[ERRO CRITICO] Timeout. Reiniciando...");
      ESP.restart();
    }
  }

  Serial.print("\n[WiFi] CONECTADO AO CELULAR COM SUCESSO! IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("==================================================");
  
  syncTime();
}

// ============================================================================
// MICROFONE (INMP441)
// ============================================================================
void setupMic() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  i2s_driver_install(MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(MIC_PORT, &pins);
  i2s_zero_dma_buffer(MIC_PORT);
}

size_t micRead(int16_t* out, size_t maxSamples) {
  static int32_t raw[256];
  size_t total = 0;
  while (total < maxSamples) {
    size_t want = minz(maxSamples - total, 256);
    size_t br = 0;
    i2s_read(MIC_PORT, raw, want * sizeof(int32_t), &br, pdMS_TO_TICKS(200));
    size_t n = br / sizeof(int32_t);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      int32_t s = raw[i] >> MIC_GAIN_SHIFT;
      if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
      out[total + i] = (int16_t)s;
    }
    total += n;
  }
  return total;
}

void micFlush(uint32_t ms) {
  static int16_t tmp[256];
  uint32_t t0 = millis();
  while (millis() - t0 < ms) micRead(tmp, 256);
}

static uint32_t meanAbs(const int16_t* s, size_t n) {
  if (!n) return 0;
  uint32_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (s[i] < 0) ? -s[i] : s[i];
  return acc / n;
}

bool listenAndRecord() {
  int16_t* pcm = (int16_t*)(audioBuf + 44);
  const size_t totalSamples = PCM_BYTES / 2;
  const size_t BLOCK = 512;

  micFlush(200);
  size_t pos = 0; int loud = 0;
  uint32_t lastWifiCheck = millis();

  while (loud < 3) {
    if (millis() - lastWifiCheck > 5000) { lastWifiCheck = millis(); wifiEnsure(); }
    size_t n = micRead(pcm + pos, BLOCK);
    if (n == 0) continue;
    uint32_t level = meanAbs(pcm + pos, n);
    if (level > VAD_THRESHOLD) { loud++; } else { loud = 0; }
  }

  digitalWrite(PIN_LED, HIGH);
  Serial.println("\n[MIC] Voz detectada! Ouvindo...");
  
  pos = 0;
  while (pos < totalSamples) {
    size_t n = micRead(pcm + pos, minz(BLOCK, totalSamples - pos));
    if (n == 0) break;
    pos += n;
  }
  if (pos < totalSamples) memset(pcm + pos, 0, (totalSamples - pos) * 2);

  digitalWrite(PIN_LED, LOW);
  return true;
}

void buildWavHeader(uint8_t* h, uint32_t pcmBytes, uint32_t rate) {
  uint32_t fSize = 36 + pcmBytes;
  uint32_t byteRate = rate * 2;
  const uint8_t hdr[44] = {
    'R','I','F','F', (uint8_t)fSize, (uint8_t)(fSize>>8), (uint8_t)(fSize>>16), (uint8_t)(fSize>>24),
    'W','A','V','E','f','m','t',' ', 16, 0, 0, 0, 1, 0, 1, 0,
    (uint8_t)rate, (uint8_t)(rate>>8), (uint8_t)(rate>>16), (uint8_t)(rate>>24),
    (uint8_t)byteRate, (uint8_t)(byteRate>>8), (uint8_t)(byteRate>>16), (uint8_t)(byteRate>>24),
    2, 0, 16, 0, 'd','a','t','a',
    (uint8_t)pcmBytes, (uint8_t)(pcmBytes>>8), (uint8_t)(pcmBytes>>16), (uint8_t)(pcmBytes>>24)
  };
  memcpy(h, hdr, 44);
}

// ============================================================================
// GEMINI
// ============================================================================
String getSystemPrompt() {
  struct tm timeinfo;
  String dataHora = "Desconhecida";
  if (getLocalTime(&timeinfo)) {
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%d/%m/%Y %H:%M", &timeinfo);
    dataHora = String(timeStringBuff);
  }

  String prompt = "És o Sálvio, um assistente de voz inteligente, carinhoso, paciente e prestativo, feito para conversar com um senhor de idade. "
                  "Moras em São Bento do Sul, SC, Brasil. A data e hora atual é: " + dataHora + ". "
                  "Podes responder perguntas gerais, dar receitas culinárias simples, contar piadas limpas, falar sobre o clima ou conversar amigavelmente. "
                  "Responde obrigatoriamente em formato JSON estruturado contendo apenas o campo 'fala'. "
                  "O texto do campo 'fala' deve ser claro, com frases curtas e afetuosas (como um neto atencioso). "
                  "Se o áudio for apenas ruído ou chiado, devolve o campo 'fala' vazio.";
  return prompt;
}

String jsonEscape(const String& in) {
  String out; out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += "\\\""; else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

bool askGemini(String& fala) {
  fala = "";
  buildWavHeader(audioBuf, PCM_BYTES, MIC_SAMPLE_RATE);

  size_t b64Len = ((AUDIO_BUF_SIZE + 2) / 3) * 4;
  char* b64Buffer = (char*)malloc(b64Len + 1);
  if (!b64Buffer) return false;
  
  size_t outLen = 0;
  mbedtls_base64_encode((unsigned char*)b64Buffer, b64Len + 1, &outLen, audioBuf, AUDIO_BUF_SIZE);
  b64Buffer[outLen] = '\0';

  String promptDinamico = getSystemPrompt();
  
  String payload = "{\"systemInstruction\":{\"parts\":[{\"text\":\"" + jsonEscape(promptDinamico) + "\"}]},";
  payload += "\"tools\": [{\"googleSearch\": {}}], "; 
  payload += "\"generationConfig\": {\"responseMimeType\": \"application/json\", \"responseSchema\": { \"type\": \"OBJECT\", \"properties\": { \"fala\": {\"type\": \"STRING\"} } }},";
  payload += "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"Ouve o que o vovo disse:\"},{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"" + String(b64Buffer) + "\"}}]}]}";
  free(b64Buffer);

  Serial.println("[PROCESSANDO] Consultando o Sálvio na nuvem...");
  WiFiClientSecure client;
  client.setInsecure();
  
  if (!client.connect(GEMINI_HOST, 443)) {
    Serial.println("[ERRO] Falha na conexao HTTPS.");
    return false;
  }

  String path = String("/v1beta/models/") + GEMINI_MODEL + ":generateContent?key=" + String(GEMINI_API_KEY);
  
  client.printf("POST %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", GEMINI_HOST);
  client.println("Content-Type: application/json");
  client.printf("Content-Length: %u\r\n", payload.length());
  client.println("Connection: close\r\n");
  
  const int chunkSize = 1024;
  for (size_t i = 0; i < payload.length(); i += chunkSize) {
    client.print(payload.substring(i, i + chunkSize));
  }

  while (client.connected() && !client.available()) delay(10);
  
  String response = "";
  bool headersPassed = false;
  while (client.available() && client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") headersPassed = true;
    else if (headersPassed) response += line;
  }
  client.stop();

  if (response.indexOf("candidates") == -1) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, response);
  if (err) return false;

  const char* text = doc["candidates"][0]["content"]["parts"][0]["text"] | "";
  JsonDocument inner;
  if (!deserializeJson(inner, text)) {
    fala = String((const char*)(inner["fala"] | ""));
    return true;
  }
  return false;
}

// ============================================================================
// SETUP E LOOP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== SÁLVIO - ASSISTENTE DE VOZ (ESP32 + INMP441) ===");

  audioBuf = (uint8_t*)malloc(AUDIO_BUF_SIZE);
  if (!audioBuf) { Serial.println("[ERRO] Memória RAM insuficiente!"); return; }

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  setupMic();
  wifiEnsure();
  Serial.println("--- Sálvio pronto e escutando! ---");
}

void loop() {
  wifiEnsure();
  if (listenAndRecord()) {
    String fala;
    if (askGemini(fala)) {
      if (fala.length() > 0) {
        Serial.println("==================================================");
        Serial.printf("👴💬 SALVIO DIZ: \"%s\"\n", fala.c_str());
        Serial.println("==================================================");
      }
    } else {
      Serial.println("[AVISO] Nao foi possivel processar a fala.");
    }
  }
}