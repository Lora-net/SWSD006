# SWSD006 - LR11xx Multi-stack Software Development Kit for nRF52840 & nRF54L15

SWSD006 is a collection of driver, protocol stack and utility software that facilitates development of Sidewalk
applications. The software includes numerous examples of how to leverage the unique capabilities of Semtech's LR11xx silicon. 
While the software targets the Nordic SoC, it is designed and distributed as a "full-source" offering, it enables users to modify
the contents across multiple layers of software stack. Potential modifications include re-targeting of host MCU and LR11xx silicon,
changes to the platform abstraction layer and enhancement of the packet fragmentation scheme. 
Please note that while software enhancement is enabled and encouraged, validation of described functionality was exclusively performed on the specified silicon variants and software component versions.

This repository implements the following functionality:
- LR11xx transceiver silicon support for Sidewalk MAC v1.16 CSS and FSK modulation 
- Drivers and examples demonstrating WIFI/GNSS NAV3 geolocation features of LR11xx silicon
- LoRaWAN Class A multi-stack operation using SWL2001 - LoRa Basics Modem 4.5.0; programmatic control over both LoRaWAN and Sidewalk stacks in one firmware image
- LR11xx transceiver firmware upgrade via SWTL001 port to nRF52840 (or nRF54L15)
- LoRaWAN Class A + WIFI/GNSS NAV3 operation example
- An example of packet fragmentation and re-assembly (overcomes CSS packet size limitations in Sidewalk)
- AWS lambda example code demonstrating End-to-End handling of LoRa EDGE application data: from transceiver to cloud service 
- A Zephyr compliant T2 topology supporting 3rd party extension of Nordic's VS Code IDE
- 'sid_dut' operation with LR11xx silicon, enabling Sidewalk Device Qualification testing
- A full-source platform abstraction layer supporting user customization of RF front-end designs and control paradigms 

## Getting started

Before getting started, make sure you have a proper nRF Connect SDK development environment.
Follow the official
[Installation guide](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/installation/install_ncs.html).

### Initialization

The first step is to initialize the workspace folder (``my-workspace``) where
the ``example-application`` and all nRF Connect SDK modules will be cloned. Run the following
command:

```shell
# when doing this from bash terminal started from nordic's toolchain manager, unset ZEPHYR_BASE
# initialize my-workspace, where 'my-workspace" can be a directory name of your choice
west init -m https://github.com/Lora-net/SWSD006 --mr v2.6.1 my-workspace

# update nRF Connect SDK modules
cd my-workspace
west update
west config build.sysbuild True
```
If you intend to build a LoRa Basics Modem project on the windows platform,  decide on a workspace location as near the root directory as possible in order to prevent path lengths that exceed the capability of the zephyr build system on windows.  This is not a concern on other platforms such as linux.
### configuring project for LR11xx
edit the project's ``.conf`` (default is ``prj.conf``)
```
CONFIG_SIDEWALK_LINK_MASK_LORA=y # run sidewalk in CSS, or..
CONFIG_SIDEWALK_LINK_MASK_FSK=y # run sidewalk in FSK
CONFIG_RADIO_LR11XX=y  # use LR11xx instead of sx126x
CONFIG_RADIO_TCXO=y  # set to n if using crystal on LR11xx instead of TCXO
```
Devices LR1110 and LR1120 and LR1121 have compatible SPI instructions.  This software will function with all these devices without modification, except for the ``CONFIG_RADIO_TCXO`` must be configured correctly for the 32MHz reference populated on-board with the radio transceiver.  
LR1121 device would typically be used with an XTAL, since it doesnt have GNSS functionality: ``CONFIG_RADIO_TCXO=n`` for reference crystal.  

All radio other configuration is declared in ``app_subGHz_config_lr11xx.c``, For example if you wanted to use the radio's LDO instead of it's DC-DC, you can modify the ``.regulator_mode = `` to ``LR11XX_SYSTEM_REG_MODE_LDO``
### Available example applications:
all example apps are built using ``west build -b <board> -- -DOVERLAY_CONFIG=foobar.conf``.  The app to build is selected by adding ``-- -DOVERLAY_CONFIG=foobar.conf``
 * the full build command, for example:
   | Hardware platforms | Board name | Build command |
   | -------------- | ---------- | -------------- |
   | nRF52840 DK    | nrf52840dk  | ``west build -p -b nrf52840dk/nrf52840 -- -DOVERLAY_CONFIG=overlay-nav3sid.conf`` |
   | nRF54L15 PDK   | nrf54l15pdk | ``west build -p -b nrf54l15pdk/nrf54l15/cpuapp -- -DOVERLAY_CONFIG=overlay-nav3sid.conf`` |

#### nRF54L15 PDK pinout:
For nRF54L15 PDK revision v0.3.0/0.7.0 use the following GPIO configuration:

| nRF54L15 PDK | LR1110MB1LCKS |
| -------------- | ---------- |
| P2.06   | SCK  |
| P1.11   | MISO |
| P2.08   | MOSI |
| P2.10   | CS |
| P0.02   | LR_NRESET |
| P0.00   | BUSY |
| P0.01   | ACC_INT1 |
| P0.03   | DIO9 |
| P1.12   | LNA_CTRL_MCU |

> **NOTE**
>
> To use the suggested pins, disable ``VCOM0`` (not used by samples) through the [Board Configurator](https://docs.nordicsemi.com/bundle/nrf-connect-board-configurator/page/index.html) tool in the nRF Connect for Desktop.
> This step is required for the shield to work as some pins are connected to ``VCOM0`` by default.
>
> See the picture below for ``VCOM0`` suggested configuration:
> ![VCOM0 configuration](./pics/board_configurator_nrf54l15_0_3_0.png)

#### Apps provided by Nordic:
from the directory ``samples/sid_end_device`` enables LR11xx in ``prj.conf`` -->
* hello
   * ``-DOVERLAY_CONFIG=overlay-hello.conf``
* sensor_monitoring
   * ``-DOVERLAY_CONFIG=overlay-demo.conf``
* dut, aka ``sid_dut``
   * ``-DOVERLAY_CONFIG=overlay-dut.conf``
#### Apps added by Semtech:
from the directory ``samples/lbm_sid_end_device`` -->
* lora basics modem (lorawan end device), running in sidewalk environment
   * ``-DOVERLAY_CONFIG=overlay-lbm.conf``
* NAV3 running simultanously with sidewalk (aka NAV3 in bypass mode):
  * ``-DOVERLAY_CONFIG=overlay-nav3sid.conf``
* NAV3 running in lora basics modem:
  * ``-DOVERLAY_CONFIG=overlay-nav3lbm.conf``
 * LR11xx firmware update, and almanac erase:
   * build in ``samples/SWTL001``directory
 

## GNSS Performance Evaluation Notice

The included GNSS example source code is provided solely to demonstrate the GNSS scan functionality under ideal conditions.  The source code and GNSS scan results are not representative of the optimal configuration or performance characteristics  of the silicon. The LR11xx product family is flexible and can be embodied and configured in a multitude of ways to realize  various trade-offs regarding performance, battery life, PCB size, cost, etc. The GNSS example included in this release and the corresponding evaluation  kits are designed & configured by the included source code in a default capacity which is sufficient to demonstrate functional GNSS scan capability only. Care must be taken if/when attempting to assess performance characteristics of GNSS scan functionality and we strongly encourage those conducting such analysis to contact Semtech via the provided support channels so that we can ensure appropriate configuration settings are employed for a given performance evaluation use-case.
### LoRa Basics Modem project organization
LoRa Basics Modem (LBM) library exists as a zephyr module in this project.  LBM resides as branches in this repository.  For example, branches ``lbm_v431`` and ``lbm_v450``are different versions of LBM provided as a zephyr module.  The purpose of presenting LBM as zephyr module is to minimize the path lengths given to the zephyr build system, which is required to accommodate path length restrictions on the windows platform.
### Supported Semtech DVKs

The following table summarizes the compatibility of the reference design firmware with Semtech LR11xx Development Kit PCBs.

| Products | Shield PCB# | Shield Note | Operation | LoRaWAN | GNSS/WiFi scan |
| -------------- | ---------- | -------------- | -----------------  | -----------------  | -----------------  |
| LR1110MB1DIS   | E516V03A   | LR1110 Passive | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
|                | E516V02A/B | LR1110 Passive | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
| LR1110MB1DJS   | E592V02A   | LR1110 Active  | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
|                | E592V01A   | LR1110 Active  | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
| LR1120MB1DIS   | E655V01A   | LR1120 Passive | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
| LR1120MB1DJS   | E656V01A   | LR1120 Active  | :heavy_check_mark: | :heavy_check_mark: | :heavy_check_mark: |
| LR1121MB1DIS   | E656V01A   | LR1121         | [^1]                | :heavy_check_mark: | :x:                |
| SX1262MB2CAS   | E499V01B   | SX1262 shield  | [^1], [^2]            | :heavy_check_mark: | :x:                |
| (Modification) | (Any)      | XTAL mounted   | [^1]                | :heavy_check_mark: | :x:                |
| (Modification) | (Any)      | LDO mode       | [^3]                | :heavy_check_mark: | :heavy_check_mark: |

[^1]: XTAL: Specify ``CONFIG_RADIO_TCXO=n`` in the build configuration. The LR1121 and SX126x transceivers do not include GNSS.
[^2]: SX126x: Specify ``-DCONFIG_RADIO_LR11XX=n``or  ``-DCONFIG_RADIO_SX126X=y`` in the build configuration.
[^3]: LDO: Specify ``LR11XX_SYSTEM_REG_MODE_LDO`` as the radio regulator mode.
