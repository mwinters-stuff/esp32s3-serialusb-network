#ifndef _LOCAL_CH34X_DEVICE_H
#define _LOCAL_CH34X_DEVICE_H

#include <usb/cdc_acm_host.h>
#include <usb/vcp.hpp>

namespace esp_usb {

class LocalCh34xDevice : public CdcAcmDevice
{
public:
  LocalCh34xDevice(uint16_t pid, const cdc_acm_host_device_config_t *dev_config, uint8_t interface_idx = 0);
  esp_err_t line_coding_set(cdc_acm_line_coding_t *line_coding) override;
  esp_err_t set_control_line_state(bool dtr, bool rts) override;
};

} // namespace esp_usb

#endif
