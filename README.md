(The file `/home/mathew/src/esp32s3-serialusb-network/README.md` exists, but is empty)
# ESP32-S3 Serial↔Network Bridge

This project runs on an ESP32-S3 and exposes a USB-hosted serial device over a network-accessible web terminal. It prefers a W5500 SPI Ethernet interface when available and falls back to WiFi if Ethernet is not present.

**Key features**
- Web terminal (HTTP + WebSocket) for live serial I/O
- USB Host CDC / vendor-specific VCP support (CH34x + generic CDC-ACM)
- W5500 SPI Ethernet support with automatic DHCP and mDNS
- LittleFS for serving the web UI and uploading files/firmware

**Where to look in the code**
- HTTP UI and WS terminal: [main/http-server.cpp](main/http-server.cpp)
- USB host + serial handling: [main/usb-handler.cpp](main/usb-handler.cpp)
- W5500 Ethernet glue: [main/w5500.cpp](main/w5500.cpp)
- Configuration constants: [main/config.h](main/config.h)

**Default network hostname (mDNS)**: train-serial

---

**Connecting Ethernet (W5500 module)**

- Wiring: connect your W5500 module to the ESP32-S3 SPI pins shown in `main/config.h`:

```
W5500_CS_PIN  = GPIO10  (CS)
W5500_SCK_PIN = GPIO14  (SCLK)
W5500_MISO_PIN= GPIO13  (MISO)
W5500_MOSI_PIN= GPIO11  (MOSI)
W5500_RST_PIN = GPIO2   (RESET, optional but recommended)
W5500_INT_PIN = -1      (interrupt pin; optional)
```

- Connect an Ethernet cable to the RJ45 on the W5500 module. The firmware initializes the SPI bus and the W5500 driver (requires `CONFIG_ETH_SPI_ETHERNET_W5500` in sdkconfig).
- If the board gets an IP by DHCP the device announces itself via mDNS as `train-serial`. If Ethernet fails the firmware will fall back to WiFi.

**Notes**
- `ENABLE_W5500_ETH` in `main/config.h` can be set to `0` to disable attempted W5500 initialization.
- The W5500 driver waits briefly for an IP; a timeout will cause the app to fall back to WiFi.

---

**Connecting USB serial devices**

- Plug a supported USB serial device into the ESP32-S3 USB Host port (board-specific - usually the USB-A host connector or an on-board USB host header).
- Supported device types:
	- CH34x VCP adapters (e.g., CH340, CH341). The code first attempts a CH34x vendor-specific open.
	- Generic CDC-ACM devices (USB CDC class) as a fallback (common for many native-USB boards and adapters that present a CDC interface).

- The code attempts multiple interface indices (0 and 1) and a set of candidate PIDs; see `main/usb-handler.cpp` for specifics. CP210x/FTDI helpers are present but commented out, so those vendor drivers are not currently active.

- Default serial settings (from `main/config.h`): 115200 baud, 8 data bits, no parity, 1 stop bit (115200 8N1). The firmware will attempt to set this line coding on the connected device.

---

**Web interface**

- Open http://train-serial/ (or the device IP) in a browser. The root page serves a terminal UI and communicates with the device over a WebSocket at `/ws`.
- Terminal output is broadcast to connected web clients; input from the web UI is forwarded to the USB device when connected.
- There are management pages for uploading firmware (`/upload`) and filesystem images (`/uploadfs`); these require authentication (password set by `HTTP_PASSWORD` in `main/config.h`).

---

**Build & flash (ESP-IDF)**

1. Install ESP-IDF and set up your environment as usual. Currently using ESP-IDF v5.4.7
2. Copy config.template.h to config.h and configure to suit.
3. From the repository root run:

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

4. If you use the W5500 module ensure the W5500 Ethernet driver is enabled in `idf.py menuconfig` (Component config → Ethernet → SPI W5500) and that websocket support is enabled in the HTTP Server (Component config → HTTP Server → Enable Websocket support).

---

Troubleshooting
- If the web UI does not appear via mDNS, check the serial console for the assigned IP address printed on boot.
- If a USB device is not recognised, check that it exposes the CDC class or is a CH34x device; check logs for VID/PID messages in `usb_handler`.
- Review `main/config.h` to change hostnames, passwords, and serial settings.

If you want, I can also add quick-start wiring diagrams or expand the supported-device list in the README. 

