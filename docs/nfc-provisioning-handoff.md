# NFC Provisioning Handoff

## Summary

This note captures the NT3H2111 NFC bring-up and boot-time NDEF provisioning work on the ESP32-C3 production tag, plus where the effort was paused.

Current status:
- The firmware path for boot-time NTAG provisioning exists.
- The tag was successfully formatted and written at least once during bring-up.
- The current board revision has now repurposed `GPIO4` for the battery ADC input.
- Because `GPIO4` was the NTAG SDA pin, production firmware now disables NTAG I2C provisioning on this rev.

## Intended NFC behavior

On a true cold boot after battery insertion:
- power the NT3H2111
- format the EEPROM as NDEF
- write the tag MAC as plain text into an NDEF Text record
- skip this work on deep-sleep wakeups
- after the first successful write, skip later power-on attempts unless flash/NVS is erased

Relevant code paths:
- `main/tag_main.c`: calls `tag_hw_write_mac_ndef()` on `ESP_RST_POWERON` and `ESP_RST_UNKNOWN`
- `components/tag_hw/tag_hw.c`: NFC power control, I2C access, NDEF record generation, and NVS one-time guard
- `scripts/flash_demo_tag.sh`: uses `erase_flash`, so every reflash clears the NVS `already provisioned` flag

## Hardware assumptions used during bring-up

Original production pin map used for NFC work:
- `GPIO0`: tamper
- `GPIO3`: battery ADC
- `GPIO4`: NTAG SDA
- `GPIO5`: NTAG FD / wake
- `GPIO6`: NTAG SCL
- `GPIO10`: NTAG power transistor control
- `GPIO18`: battery sense enable

Important discovery from logs:
- NTAG power on this board behaved as active-low on `GPIO10`

## What worked

The provisioning flow did work once during bring-up.
Observed success pattern:
- primary NTAG power polarity probe failed
- fallback polarity succeeded
- NTAG was formatted and written
- phone readback showed the stored text payload

At that point the payload still contained the Wi-Fi MAC, and phone readback showed:
- `10:B4:1D:06:7E:E4`

That confirmed:
- NFC RF side was alive
- I2C write path could work on the hardware
- NDEF formatting and text record generation were valid enough for a phone reader

## What changed after that

Follow-up firmware changes were made to:
- switch the stored text from Wi-Fi MAC to BLE MAC
- add an NVS guard so provisioning only happens once after a successful write
- clean up the active-low NFC power handling
- add more diagnostics around NTAG probe and block access
- try a more explicit I2C transaction path for NTAG block reads/writes

## Failure modes seen later

Later boots stopped rewriting the NTAG, so phone readback kept showing the older stored value.

Detailed failure patterns seen in logs included:
- `i2c_master_probe()` timeout on the wrong power polarity
- fallback power polarity appearing more plausible
- first real NTAG block read failing after probe with `ESP_ERR_INVALID_STATE`
- provisioning aborting before any new NDEF payload was written

One representative failure sequence was:
- primary power level probe timed out
- fallback power level got farther
- `NTAG block read failed with power level=0: ESP_ERR_INVALID_STATE`
- `NTAG MAC provisioning failed: ESP_ERR_INVALID_STATE`

This left the old EEPROM contents untouched, which is why phone reads kept returning the stale earlier text.

## Last attempted software direction

The last software change before this work was paused was to make NTAG access fully explicit at the I2C transaction level:
- use `I2C_DEVICE_ADDRESS_NOT_USED`
- emit raw `addr+W` and `addr+R` bytes explicitly
- issue a combined NTAG read as:
  - `START -> addr+W -> block -> RESTART -> addr+R -> READ -> STOP`

That change lives in `components/tag_hw/tag_hw.c`.

Even after that change, the user reported that NFC provisioning still did not work, and the effort was paused before collecting a new detailed success log.

## Things worth remembering if this is resumed

1. `flash_demo_tag.sh` erases flash.
That clears the NVS one-time provisioning flag, so a fresh flash always behaves like a first boot.

2. A stale phone read does not mean the newest firmware wrote successfully.
It can simply be older EEPROM content surviving because the latest provisioning attempt failed before any write.

3. The board showed strong signs that NTAG power polarity differs from the original firmware assumption.
Earlier success came from the fallback polarity path.

4. The useful files to revisit are:
- `components/tag_hw/tag_hw.c`
- `main/tag_main.c`
- `scripts/flash_demo_tag.sh`

5. If NFC write support is revisited on a future board, start by validating:
- NFC power polarity
- SDA/SCL pull-ups and routing
- whether the driver-level explicit I2C operation path is still needed
- whether provisioning should remain one-time via NVS or instead check NTAG contents directly

## Where the work is paused

This effort is paused because the current board revision needed `GPIO4` for the battery ADC bodge.
That steals the NTAG SDA line on this rev, so production firmware now treats NTAG I2C provisioning as unavailable.

Current implication for this hardware rev:
- NFC field-detect wake on `GPIO5` may still exist if the NTAG remains powered and connected there
- NTAG I2C formatting/writing is disabled in production firmware because SDA is no longer available on `GPIO4`
