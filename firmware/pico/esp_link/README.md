# Pico-to-ESP32 Link

Primary Wi-Fi / ESP32 implementation: Justin Nicholas Yang

The final Pico application reuses Justin's existing ESP32-C3 protocol:

- Pico sends `PING`
- ESP32 replies `PONG`
- Pico sends `SCAN`
- ESP32 replies with `AP,<ssid>,<rssi>,<channel>`
- the scan ends with `END,<count>`
- errors use `ERR,...`

Hardware:
- Pico GP8 TX -> ESP32 RX
- Pico GP9 RX <- ESP32 TX
- common GND
- UART1 at 115200 baud

uT-Kernel integration:
- one ESP Link Task owns UART1 on processor 2
- idle heartbeat sends PING every 500 ms
- PONG marks the ESP32 link as OK
- no PONG for 1500 ms marks the ESP32 link as LOST
- Wi-Fi scans temporarily use the same task, so heartbeat and scan parsing never
  compete to read UART1
- the whole scan result is received before any LCD result drawing
- completed results and link-state changes are sent to the Display Task

This keeps the Display Task as the only LCD owner and avoids two tasks racing to
consume the same UART bytes.
