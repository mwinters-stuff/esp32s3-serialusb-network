#ifndef _CONFIG_H
#define _CONFIG_H

#define MDNS_HOSTNAME "esp32-vcp"
#define MDNS_INSTANCE "ESP32 with mDNS"

#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "password"

// Password for web interface upload pages
#define HTTP_PASSWORD "admin"


// Change these values to match your needs
#define BAUDRATE (115200)
#define STOP_BITS (0) // 0: 1 stopbit, 1: 1.5 stopbits, 2: 2 stopbits
#define PARITY (0)    // 0: None, 1: Odd, 2: Even, 3: Mark, 4: Space
#define DATA_BITS (8)



#endif