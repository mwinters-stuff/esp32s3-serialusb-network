#ifndef _W5500_H
#define _W5500_H

#include <memory>
#include "led_indicator.h"

/**
 * Initializes W5500 Ethernet connection
 * Returns true if connection successful, false otherwise
 */
bool w5500_init(std::shared_ptr<LedIndicator> ledIndicator);

/**
 * Check if W5500 is currently connected
 */
bool w5500_is_connected();

#endif
