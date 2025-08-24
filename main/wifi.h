#ifndef _WIFI_H
#define _WIFI_H

#include <memory>
#include "led_indicator.h"

void wifi_init_sta(std::shared_ptr<LedIndicator> ledIndicator);

#endif