# Command Console Library

## What's Included?

See the [README.md](./README.md) for a complete description of the command console library.

## Known Issues
| Problem | Workaround |
| ------- | ---------- |
| Intermitently LE RX data may not be recieved when the LE COC connection is initated by the PSoC6 kit | Disconnect and reconnect from the peer device to resume RX data. Will be fixed in a future release.| 

## Changelog

### v1.0.1

* Documentation updates.
* Added fix to echo the commands as they are typed, for IAR toolchain.

### v1.0.0

* This is the first version of the Command Console Library for Mbed OS and AnyCloud. 

## Supported Software and Tools
The current version of the library was validated for compatibility with the following Software and Tools:

| Software and Tools                                      | Version |
| :---                                                    | :----:  |
| ModusToolbox Software Environment                       | 2.2     |
| - ModusToolbox Device Configurator                      | 2.20    |
| - ModusToolbox CapSense Configurator / Tuner tools      | 3.10    |
| PSoC 6 Peripheral Driver Library (PDL)                  | 2.0.0   |
| GCC Compiler                                            | 9.3.1   |
| IAR Compiler (only for AnyCloud)                        | 8.32    |
| Arm Compiler 6                                          | 6.14    |
| Mbed OS                                                 | 6.2.0   |