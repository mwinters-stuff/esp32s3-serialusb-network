#include "local-ch34x-device.h"

#include <assert.h>
#include <cinttypes>

#include <esp_bit_defs.h>
#include <esp_log.h>

#include <esp_private/cdc_host_common.h>
#include <usb/vcp_ch34x.hpp>

static const char *TAG = "VCP";

namespace esp_usb {

namespace
{
constexpr uint8_t CH34X_WRITE_REQ = 0x40;
constexpr uint8_t CH34X_CMD_WRITE = 0x9A;
constexpr uint8_t CH34X_CMD_MODEM_OUT = 0xA4;
constexpr uint16_t CH34X_CONTROL_DTR = 0x20;
constexpr uint16_t CH34X_CONTROL_RTS = 0x40;
constexpr uint8_t CH34X_LCR_ENABLE_RX = 0x80;
constexpr uint8_t CH34X_LCR_ENABLE_TX = 0x40;
constexpr uint8_t CH34X_LCR_MARK_SPACE = 0x20;
constexpr uint8_t CH34X_LCR_PAR_EVEN = 0x10;
constexpr uint8_t CH34X_LCR_ENABLE_PAR = 0x08;
constexpr uint8_t CH34X_LCR_STOP_BITS_2 = 0x04;
constexpr uint8_t CH34X_LCR_CS8 = 0x03;
constexpr uint8_t CH34X_LCR_CS7 = 0x02;
constexpr uint8_t CH34X_LCR_CS6 = 0x01;
constexpr uint8_t CH34X_LCR_CS5 = 0x00;

int calculate_ch34x_baud_divisor(uint32_t baud_rate, uint8_t *factor, uint8_t *divisor)
{
  assert(factor);
  assert(divisor);

  unsigned char a = 0;
  unsigned char b = 0;
  unsigned long c = 0;

  switch (baud_rate)
  {
  case 921600:
    a = 0xF3;
    b = 7;
    break;
  case 307200:
    a = 0xD9;
    b = 7;
    break;
  default:
    if (baud_rate == 0)
    {
      return -1;
    }

    if (baud_rate > 6000000 / 255)
    {
      b = 3;
      c = 6000000;
    }
    else if (baud_rate > 750000 / 255)
    {
      b = 2;
      c = 750000;
    }
    else if (baud_rate > 93750 / 255)
    {
      b = 1;
      c = 93750;
    }
    else
    {
      b = 0;
      c = 11719;
    }

    a = static_cast<unsigned char>(c / baud_rate);
    if (a == 0 || a == 0xFF)
    {
      return -1;
    }

    const int delta_0 = static_cast<int>(c / a) - static_cast<int>(baud_rate);
    const int delta_1 = static_cast<int>(baud_rate) - static_cast<int>(c / (a + 1));
    if (delta_0 > delta_1)
    {
      a++;
    }

    a = static_cast<unsigned char>(256 - a);
    break;
  }

  *factor = a;
  *divisor = b;
  return 0;
}
}

LocalCh34xDevice::LocalCh34xDevice(uint16_t pid, const cdc_acm_host_device_config_t *dev_config, uint8_t interface_idx)
{
  const esp_err_t err = ch34x_vcp_open(pid, interface_idx, dev_config, &this->cdc_hdl);
  if (err != ESP_OK)
  {
    throw(err);
  }
}

esp_err_t LocalCh34xDevice::line_coding_set(cdc_acm_line_coding_t *line_coding)
{
  assert(line_coding);

  ESP_LOGI(TAG, "LocalCh34xDevice::line_coding_set baud=%" PRIu32 " data=%u parity=%u stop=%u",
           line_coding->dwDTERate,
           line_coding->bDataBits,
           line_coding->bParityType,
           line_coding->bCharFormat);

  if (line_coding->dwDTERate != 0)
  {
    uint8_t factor = 0;
    uint8_t divisor = 0;
    if (calculate_ch34x_baud_divisor(line_coding->dwDTERate, &factor, &divisor) != 0)
    {
      return ESP_ERR_INVALID_ARG;
    }

    uint16_t baud_reg_val = static_cast<uint16_t>((factor << 8) | divisor);
    baud_reg_val |= BIT7;
    ESP_LOGI(TAG, "CH34x baud setup: req=%" PRIu32 " factor=0x%02X divisor=0x%02X reg=0x%04X",
             line_coding->dwDTERate, factor, divisor, baud_reg_val);
    esp_err_t err = this->send_custom_request(CH34X_WRITE_REQ, CH34X_CMD_WRITE, 0x1312, baud_reg_val, 0, NULL);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Set baudrate failed: %s", esp_err_to_name(err));
      return err;
    }
  }

  if (line_coding->bDataBits != 0)
  {
    uint8_t lcr = CH34X_LCR_ENABLE_RX | CH34X_LCR_ENABLE_TX;

    switch (line_coding->bDataBits)
    {
    case 5:
      lcr |= CH34X_LCR_CS5;
      break;
    case 6:
      lcr |= CH34X_LCR_CS6;
      break;
    case 7:
      lcr |= CH34X_LCR_CS7;
      break;
    case 8:
      lcr |= CH34X_LCR_CS8;
      break;
    default:
      return ESP_ERR_INVALID_ARG;
    }

    switch (line_coding->bParityType)
    {
    case 0:
      break;
    case 1:
      lcr |= CH34X_LCR_ENABLE_PAR;
      break;
    case 2:
      lcr |= CH34X_LCR_ENABLE_PAR | CH34X_LCR_PAR_EVEN;
      break;
    case 3:
    case 4:
      lcr |= CH34X_LCR_ENABLE_PAR | CH34X_LCR_MARK_SPACE;
      break;
    default:
      return ESP_ERR_INVALID_ARG;
    }

    switch (line_coding->bCharFormat)
    {
    case 0:
      break;
    case 2:
      lcr |= CH34X_LCR_STOP_BITS_2;
      break;
    default:
      return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = this->send_custom_request(CH34X_WRITE_REQ, CH34X_CMD_WRITE, 0x2518, lcr, 0, NULL);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Set line coding failed: %s", esp_err_to_name(err));
      return err;
    }
  }

  return ESP_OK;
}

esp_err_t LocalCh34xDevice::set_control_line_state(bool dtr, bool rts)
{
  CDC_ACM_CHECK(this->cdc_hdl, ESP_ERR_INVALID_STATE);
  CDC_ACM_CHECK(this->cdc_hdl->data.intf_desc, ESP_ERR_INVALID_STATE);

  uint16_t wValue = CH34X_CONTROL_DTR | CH34X_CONTROL_RTS;
  const uint16_t interface_number = this->cdc_hdl->notif.intf_desc ? this->cdc_hdl->notif.intf_desc->bInterfaceNumber : this->cdc_hdl->data.intf_desc->bInterfaceNumber;

  ESP_LOGI(TAG, "LocalCh34xDevice::set_control_line_state dtr=%d rts=%d iface=%u notif=%d",
           dtr,
           rts,
           interface_number,
           this->cdc_hdl->notif.intf_desc ? 1 : 0);

  if (dtr)
  {
    wValue &= ~CH34X_CONTROL_DTR;
  }
  if (rts)
  {
    wValue &= ~CH34X_CONTROL_RTS;
  }

  return this->send_custom_request(CH34X_WRITE_REQ, CH34X_CMD_MODEM_OUT, wValue, interface_number, 0, NULL);
}

} // namespace esp_usb
