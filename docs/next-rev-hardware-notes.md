# Next Rev Hardware Notes

## Pin planning

Current production pin map in firmware:
- GPIO5: tamper comparator output / deep-sleep wake
- GPIO0: battery sense enable
- GPIO3: battery ADC input
- GPIO10: NFC power transistor control
- GPIO4: reserved for NFC SDA
- GPIO18: reserved for NFC SCL
- GPIO19: currently unused in firmware

ESP32-C3 deep-sleep wake-capable GPIOs:
- GPIO0
- GPIO1
- GPIO2
- GPIO3
- GPIO4
- GPIO5

Non-wake control signals should be moved off GPIO0-5 where possible.

## Recommended next change

Goal:
- free GPIO0 for future deep-sleep wake use
- keep battery ADC on GPIO3
- move battery sense enable to a non-wakeup GPIO

Recommended order:
1. Prefer GPIO6 for battery sense enable if it is actually routed and otherwise unused on the PCB.
   Reason: avoids consuming a wake-capable pin.
   Reason: avoids the USB-JTAG caveat on GPIO18/GPIO19.
   Reason: battery-enable is just a slow control output, so a normal GPIO is ideal.
2. If GPIO6 is not available, use GPIO19.
   Reason: still a reasonable non-wake control pin.
   Caveat: GPIO19 is tied to USB-JTAG on ESP32-C3, so this is less clean than GPIO6.
3. Avoid using GPIO4 or GPIO18 for battery-enable if the next rev still wants NFC I2C reserved.
4. Avoid using GPIO0-5 for battery-enable going forward, because those are your best deep-sleep wake pins.

## Tamper / NFC / battery lessons from this board

- Tamper on GPIO5 works well as an active-low deep-sleep wake source.
- GPIO10 should stay as NFC power transistor control, not a wake input.
- Battery ADC path is GPIO3 and appears correct.
- Battery sense enable should move off GPIO0 on the next rev if GPIO0 is needed as a wake source.

## Firmware follow-up when hardware changes

When the next board revision moves battery-enable:
- update TAG_BATTERY_ENABLE_GPIO in components/tag_hw/tag_hw.c
- keep TAG_BATTERY_ADC_CHANNEL on ADC_CHANNEL_3 unless the ADC pin also changes
- recheck battery enable polarity on first bring-up
- re-verify deep-sleep wake mask after moving GPIO0 into wake use
