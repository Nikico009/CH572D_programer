# CH572D Flasher

A lightweight command-line utility for flashing firmware to the **WCH CH572D** microcontroller on Linux.

The tool automates the firmware flashing process and optionally starts a serial monitor after the device has been programmed and re-enumerated as a USB CDC serial device.

## Features

* Flash Intel HEX firmware images to the CH572D.
* Automatically attempt to remove flash protection.
* Convert Intel HEX firmware to a binary image.
* Flash the generated binary using `minichlink`.
* Wait automatically for the CH572D to re-enumerate after flashing.
* Monitor the CH572D USB CDC serial output at 115200 baud.
* Assert the DTR control line when opening the serial port.
* Cleanly terminate the serial monitor with `Ctrl+C`.
* No shell-based command execution for external tools.

## Requirements

The following software is required:

* Linux
* GCC or another C compiler compatible with C11
* `minichlink`
* `hex2bin.py`
* Python with the required Intel HEX support for `hex2bin.py`

The external tools must be available through the system `PATH`.

## Compilation

Compile the program with:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 flasher.c -o ch572d-flasher
```

For a development build with debugging information, you can use:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O0 -g flasher.c -o ch572d-flasher
```

## Usage

To flash a firmware image without starting the serial monitor:

```bash
./ch572d-flasher firmware.hex
```

To flash the firmware and automatically start the serial monitor on `/dev/ttyACM0`:

```bash
./ch572d-flasher firmware.hex -m /dev/ttyACM0
```

The monitor uses:

* Baud rate: **115200**
* Data bits: **8**
* Parity: **None**
* Stop bits: **1**
* Hardware flow control: **Disabled**
* Software flow control: **Disabled**

Press `Ctrl+C` to stop the serial monitor.

## Flashing Process

When a firmware image is flashed, the program performs the following steps:

1. Attempts to remove flash protection using `minichlink -u`.
2. Converts the Intel HEX firmware image into a temporary binary file using `hex2bin.py`.
3. Writes the binary firmware to the CH572D using `minichlink`.
4. Removes the temporary binary file.
5. If a serial port was specified, waits for the CH572D to re-enumerate as a USB CDC device.
6. Opens the serial port and starts the monitor.

The flash protection step is treated as a warning because `minichlink -u` may return an error when the device is already in an unlocked state.

## Serial Monitoring

After programming, the CH572D normally resets and disappears from the USB bus temporarily before enumerating again.

Instead of relying on a fixed delay, the program periodically attempts to open the specified serial device until it becomes available or a timeout is reached.

Once the device is available, the program configures the port and asserts DTR before starting the monitor.

## Permissions

Depending on the Linux distribution, access to `/dev/ttyACM*` may require membership in a specific system group such as `uucp` or `dialout`.

For example, on systems using the `uucp` group:

```bash
sudo usermod -aG uucp "$USER"
```

Log out and back in after changing group membership.

The exact group depends on the Linux distribution and its udev configuration.

## Project Status

This project is currently focused on providing a simple Linux-based flashing and serial-monitoring workflow for the CH572D.

Future versions may include additional functionality and improvements.

### Hardware Documentation

The hardware schematics for the CH572D board are planned to be provided in a future update.

## Contributing

Contributions, bug reports and improvements are welcome.

If you find a problem or have an idea for improving the tool, feel free to open an issue or submit a pull request.

## License

This project is released under the **MIT License**.

See [`LICENSE`](LICENSE) for the complete license text.
