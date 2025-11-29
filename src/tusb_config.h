/*
 * TinyUSB Configuration for MVS Capture
 * CDC (serial) device for streaming pixel data
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C"
{
#endif

  //--------------------------------------------------------------------
  // COMMON CONFIGURATION
  //--------------------------------------------------------------------

#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_OS OPT_OS_NONE

// Debug: 0=none, 1=errors, 2=warnings, 3=info
#define CFG_TUSB_DEBUG 0

// Memory alignment
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

  //--------------------------------------------------------------------
  // DEVICE CONFIGURATION
  //--------------------------------------------------------------------

#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

//--------------------------------------------------------------------
// CLASS CONFIGURATION
//--------------------------------------------------------------------

// CDC FIFO sizes - larger for better throughput
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 8192 // 8KB TX buffer for streaming

// Disable unused classes
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */