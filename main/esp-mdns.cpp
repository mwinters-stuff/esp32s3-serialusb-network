#include <string.h>

#include "config.h"
#include <esp_log.h>
#include <mdns.h>

#define TAG "MDNS"

void initialise_mdns(void) {
  // initialize mDNS
  ESP_ERROR_CHECK(mdns_init());
  // set mDNS hostname (required if you want to advertise services)
  ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
  ESP_LOGI(TAG, "mdns hostname set to: [%s]", MDNS_HOSTNAME);
  // set default mDNS instance name
  ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE));

  // structure with TXT records
  mdns_txt_item_t serviceTxtData[3] = {
      {"board", "esp32"},
  };

  // initialize service
  ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80,
                                   serviceTxtData, 3));
  ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("ESP32-WebServer", "_http",
                                                    "_tcp", NULL, "_server"));
}

// Force mDNS to re-announce on all interfaces — call after getting an IP
void mdns_announce() {
  mdns_service_txt_item_set("_http", "_tcp", "board", "esp32");
  ESP_LOGI(TAG, "mDNS re-announced");
}