#ifdef SERIAL_PORT_TEST
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  // On ESP32-C3 Arduino core, only Serial (USB-CDC) and Serial1 exist.
  // Serial1 maps to the UART pins (GPIO20 RX, GPIO21 TX) by default on C3.
  Serial1.begin(115200, SERIAL_8N1, 20, 21);
  delay(500);

  Serial.println("Hello from Serial");
  Serial1.println("Hello from Serial1 (UART0 pins 20/21)");

  // Keep emitting so you can see which port carries which stream
  Serial.println("Serial test running - watch for tick counters");
  Serial1.println("Serial1 test running - watch for tick counters");
}

void loop() {
  static uint32_t counter = 0;
  counter++;
  Serial.printf("Serial tick %lu\n", (unsigned long)counter);
  Serial1.printf("Serial1 tick %lu\n", (unsigned long)counter);
  delay(1000);
}
#endif
