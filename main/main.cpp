
#include "esp-mdns.h"
#include "wifi.h"
#include "littlefs.h"
#include "http-server.h"
#include "usb-handler.h"

extern "C" void app_main(void)
{
    mount_littlefs();
    wifi_init_sta(); 
    initialise_mdns();
    auto usbHandler = std::make_shared<UsbHandler>();
    auto httpServer = std::make_shared<HttpServer>(usbHandler);
    httpServer->start();
    usbHandler->usb_loop();
    
}
