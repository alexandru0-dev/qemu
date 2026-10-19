Classic ESP32 optional peripherals
==================================

The ``esp32`` machine can attach an optional I2C GPIO expander. Existing
invocations retain no GPIO expander and the existing TMP105.

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

Tests
-----

``tests/qtest/esp32-peripherals-test`` exercises default configuration,
expander registers and reset. It requires no guest firmware, device
provisioning, or external network.
