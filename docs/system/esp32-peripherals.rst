Classic ESP32 optional peripherals
==================================

The ``esp32`` machine can select native Ethernet and an I2C GPIO expander
without a separate product-specific machine type. Existing invocations retain
the OpenETH backend, no GPIO expander, and zero default PSRAM capacity.

Native Ethernet
---------------

``-machine esp32,emac=on`` replaces OpenETH with the native ESP32 EMAC at
``0x3ff69000``. The models share a register range and are mutually exclusive.
Attach a network backend using ``-nic user,model=esp32-emac`` or another
supported backend. Without a backend, the device remains present with link
down. The native model includes DMA descriptors and a limited RTL8201F MDIO
register model; unsupported registers are not a complete PHY implementation.

The PHY address defaults to zero. Override it explicitly when needed::

    -machine esp32,emac=on -global esp32-emac.phy-address=1

GPIO expander
-------------

``-machine esp32,pca9575=on`` adds a PCA9575 to I2C0.
The default address is ``0x2f``; ``pca9575-address`` accepts a 7-bit address
other than ``0x48``, which is reserved for the existing TMP105::

    -machine esp32,pca9575=on,pca9575-address=0x2e

The existing I2C temperature sensor is retained. The model exposes GPIO input,
output, output-enable and interrupt signals, register auto-increment, and
reset/migration state. Pull and bus-hold registers do not model analog signals.
Attaching I2C slaves through ``-device`` is still restricted by this machine's
existing detached peripheral-bus topology; these options do not change reset
ownership or expose a new user-pluggable bus.

PSRAM capacity
--------------

Explicit RAM sizes up to ``-m 8M`` select the corresponding PSRAM die capacity.
The CPU cache window remains 4 MiB: this does not implement bank switching,
himem, or full SPI/cache coherency for the unmapped half of an 8 MiB die.

Tests
-----

``tests/qtest/esp32-peripherals-test`` exercises default configuration,
expander registers and reset, and native Ethernet register/MDIO behavior.
It requires no guest firmware, device provisioning, or external network.
