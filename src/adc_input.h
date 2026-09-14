// adc_input.h
//
// Support for the Ascent VRX Pro's physical buttons. The 5-way joystick,
// OK/REC/BACK buttons are a single resistor-ladder network read off one
// SARADC channel -- not GPIOs, and not the UART "FSMP" protocol that stock
// firmware's ar_ldy_gnd also contains (that's real code, but it's for
// something else, likely the wireless link -- the actual physical buttons
// on this hardware go through Fxn_key_detect's ADC polling instead). See
// adc_input.cpp for exactly how this was derived and validated.
#ifndef ADC_INPUT_H
#define ADC_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Validates config["gsmenu"]["vrx_adc"], if present and enabled. No-op (and
// safe to call) when that section is absent.
void setup_vrx_adc(void);

// Poll the ADC channel and dispatch any recognized key transition. Call
// once per input tick, same cadence as handle_gpio_input().
void handle_vrx_adc_input(void);

// No persistent resources are held (the sysfs node is opened fresh on each
// poll, matching stock firmware's own approach), but kept for symmetry with
// the other input backends' setup/cleanup pairing.
void cleanup_vrx_adc(void);

// Poll the dedicated "bind" button's ADC channel (in_voltage0_raw -- a
// separate SARADC channel from the joystick ladder above) and, on a press
// edge, launch /usr/bin/bind via bind_dialog_trigger(). Call once per input
// tick alongside handle_vrx_adc_input(); no-op unless vrx_adc is configured.
void handle_vrx_bind_input(void);

#ifdef __cplusplus
}
#endif

#endif // ADC_INPUT_H
