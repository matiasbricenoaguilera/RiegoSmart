 // SKETCH CANARY — diagnóstico de hardware vs software
// Flashear ANTES que central.ino para determinar causa del loop rst:0x3.
//
// SIN WiFi, SIN U8g2, SIN Preferences, SIN WDT.
// Si este sketch también entra en loop → problema de fuente de alimentación (hardware).
// Si funciona → problema de NVS corrupta o constructor global (software).
//
// Resultado esperado en monitor serie (115200 baud):
//   [CANARY] Boot #1  motivo: ENCENDIDO_NORMAL
//   [CANARY] Heap: 296xxx bytes
//   [CANARY] OK — sin crash, sin loop
//   (blink cada 500ms)

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

static int bootCount = 0;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // deshabilitar brownout detector
  pinMode(2, OUTPUT);                           // LED interno ESP32 (GPIO2)
  Serial.begin(115200);
  delay(300);

  bootCount++;

  const char *motivo = "DESCONOCIDO";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  motivo = "ENCENDIDO_NORMAL"; break;
    case ESP_RST_SW:       motivo = "SW_RESET";         break;
    case ESP_RST_PANIC:    motivo = "PANIC";            break;
    case ESP_RST_INT_WDT:  motivo = "INT_WDT";          break;
    case ESP_RST_TASK_WDT: motivo = "TASK_WDT";         break;
    case ESP_RST_BROWNOUT: motivo = "BROWNOUT";         break;
    default: break;
  }

  Serial.printf("[CANARY] Boot #%d  motivo: %s\n", bootCount, motivo);
  Serial.printf("[CANARY] Heap: %u bytes\n", esp_get_free_heap_size());
  Serial.println("[CANARY] OK — sin crash, sin loop");
}

void loop() {
  digitalWrite(2, HIGH); delay(500);
  digitalWrite(2, LOW);  delay(500);
}
