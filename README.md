# nxp-pn5xx

Kernel driver for the NXP NPC300 NFC chip found in many ThinkPad models (T470, T480, T480s, X280, X1 Carbon Gen 6+, X1 Yoga Gen 3, X1 Yoga Gen 8). Any model containing Lenovo part number **01AX745** should be supported. This is a fork of NXP's upstream pn5xx driver with added ACPI support for the `NXP1001` hardware ID used by Lenovo.

Requires Linux **≥ 3.17**.

## What it does

Exposes the chip as `/dev/pn544`. GPIO configuration is sourced automatically from either ACPI (x86 ThinkPads) or a Device Tree node (embedded) - see `sample_devicetree.txt` for a DT example. The DT binding supports optional `firmware-gpios` and `clkreq-gpios` in addition to the required `interrupt-gpios` and `enable-gpios`.

## Userland

Use <https://github.com/StarGate01/linux_libnfc-nci> with the kernel driver (I2C mode). Build with `--enable-i2c` - that flag makes the HAL use `/dev/pn544` via this driver.

## Known issues

On some older models (T480s with Elan touchpad) the touchpad becomes laggy while the NFC driver is active. This is a conflict at the I2C bus level between the touchpad and NFC drivers, not a bug in this driver. Suspending and resuming the system restores normal touchpad behaviour.
