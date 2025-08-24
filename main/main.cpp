
#include "esp-mdns.h"
#include "wifi.h"
#include "littlefs.h"
#include "http-server.h"
#include "usb-handler.h"
#include "led_indicator.h"

extern "C" void app_main(void)
{
    auto ledIndicator = std::make_shared<LedIndicator>();
    ledIndicator->init();

    mount_littlefs();
    wifi_init_sta(ledIndicator);
    initialise_mdns();
    auto usbHandler = std::make_shared<UsbHandler>(ledIndicator);
    auto httpServer = std::make_shared<HttpServer>(usbHandler, ledIndicator);
    httpServer->start();
    usbHandler->usb_loop();
    
}
