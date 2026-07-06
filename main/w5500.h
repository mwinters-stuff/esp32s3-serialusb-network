#ifndef _W5500_H
#define _W5500_H

#include <memory>

#include "led_indicator.h"

// Initializes W5500 Ethernet and waits briefly for DHCP.
bool w5500_init(std::shared_ptr<LedIndicator> ledIndicator);

// Returns true when ethernet has an active IP configuration.
bool w5500_is_connected();

#endif
