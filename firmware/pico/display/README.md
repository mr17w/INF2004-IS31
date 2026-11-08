# Display Module

Reusable ST7789 display support for the Ottie eBadge.

Current support:
- ST7789 initialisation
- full-screen fills and rectangles
- text rendering
- dedicated uT-Kernel Display Task
- main Ottie menu and feature screens
- Wi-Fi scanning status
- completed Wi-Fi result list with SSID, RSSI and channel
- ESP32 link badge: ESP OK / ESP LOST / ESP ...

Only the Display Task directly accesses the LCD. Other tasks send display
messages through the display message buffer.

The ESP Link Task sends status changes to the Display Task. This means unplugging
or reconnecting the ESP32 can update the small link badge without another task
writing directly to the ST7789.
