#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_BAUDRATE B115200
#define SERIAL_TIMEOUT_MS 5000
#define POLL_INTERVAL_MS 100
#define TEMP_BIN_FILE "/tmp/ch572d_fw.bin"

static volatile sig_atomic_t monitor_running = 1;

/**
 * Print the program usage information.
 */
static void print_usage(const char *program_name)
{
    printf(
        "Usage:\n"
        "  %s <firmware.hex>\n"
        "  %s <firmware.hex> -m <serial_port>\n\n"
        "Options:\n"
        "  -m <serial_port>   Monitor the CH572D serial output after flashing\n"
        "  -h                 Show this help message\n",
        program_name,
        program_name
    );
}

/**
 * Handle SIGINT so the serial monitor can terminate cleanly.
 */
static void handle_sigint(int signal_number)
{
    (void)signal_number;
    monitor_running = 0;
}

/**
 * Sleep for the specified number of milliseconds.
 *
 * nanosleep() is used instead of usleep(), as it is part of POSIX.1-2008
 * and provides a straightforward way to handle interrupted sleeps.
 */
static void sleep_ms(unsigned int milliseconds)
{
    struct timespec request = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L
    };

    while (nanosleep(&request, &request) == -1)
    {
        if (errno != EINTR)
            break;
    }
}

/**
 * Wait until a serial device becomes available.
 *
 * This is useful after flashing because the CH572D resets and needs
 * some time to re-enumerate as a USB CDC serial device.
 *
 * @param portname   Serial device path, e.g. /dev/ttyACM0
 * @param timeout_ms Maximum amount of time to wait in milliseconds
 *
 * @return 0 on success, -1 on timeout
 */
static int wait_for_serial(const char *portname, unsigned int timeout_ms)
{
    unsigned int elapsed = 0;

    while (elapsed < timeout_ms)
    {
        int fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);

        if (fd >= 0)
        {
            close(fd);

            printf("Serial port found: %s\n", portname);
            return 0;
        }

        sleep_ms(POLL_INTERVAL_MS);
        elapsed += POLL_INTERVAL_MS;
    }

    fprintf(
        stderr,
        "Timeout waiting for serial port: %s\n",
        portname
    );

    return -1;
}

/**
 * Configure and open a serial port using 115200 8N1.
 *
 * The configuration disables both software and hardware flow control.
 * DTR is then asserted through TIOCMSET. On USB CDC devices this
 * corresponds to the host requesting the DTR control-line state.
 *
 * @param portname Serial device path
 *
 * @return File descriptor on success, -1 on failure
 */
static int setup_serial(const char *portname)
{
    int fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);

    if (fd < 0)
    {
        fprintf(
            stderr,
            "Failed to open serial port '%s': %s\n",
            portname,
            strerror(errno)
        );
        return -1;
    }

    struct termios tty;

    if (tcgetattr(fd, &tty) != 0)
    {
        fprintf(
            stderr,
            "tcgetattr() failed: %s\n",
            strerror(errno)
        );

        close(fd);
        return -1;
    }

    /*
     * Configure 115200 baud, 8 data bits, no parity, one stop bit.
     */
    if (cfsetospeed(&tty, DEFAULT_BAUDRATE) != 0 ||
        cfsetispeed(&tty, DEFAULT_BAUDRATE) != 0)
    {
        fprintf(
            stderr,
            "Failed to configure serial baud rate: %s\n",
            strerror(errno)
        );

        close(fd);
        return -1;
    }

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;

    /*
     * Disable break processing, canonical mode, echo and output
     * processing. This provides raw serial input.
     */
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;

    /*
     * Read at least one byte and use a short read timeout.
     */
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    /*
     * Disable software flow control.
     */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /*
     * Enable the receiver and ignore modem-control lines.
     */
    tty.c_cflag |= CLOCAL | CREAD;

    /*
     * Disable parity and configure one stop bit.
     */
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;

    /*
     * Disable hardware flow control.
     */
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        fprintf(
            stderr,
            "tcsetattr() failed: %s\n",
            strerror(errno)
        );

        close(fd);
        return -1;
    }

    /*
     * Assert DTR.
     */
    int modem_status;

    if (ioctl(fd, TIOCMGET, &modem_status) == -1)
    {
        fprintf(
            stderr,
            "TIOCMGET failed: %s\n",
            strerror(errno)
        );

        close(fd);
        return -1;
    }

    modem_status |= TIOCM_DTR;

    if (ioctl(fd, TIOCMSET, &modem_status) == -1)
    {
        fprintf(
            stderr,
            "Failed to assert DTR: %s\n",
            strerror(errno)
        );

        close(fd);
        return -1;
    }

    printf("DTR asserted.\n");

    return fd;
}

/**
 * Execute an external command and wait for it to finish.
 *
 * fork()/execvp() is used instead of system(), avoiding the shell and
 * therefore avoiding shell interpretation of command arguments.
 *
 * @param program Program to execute
 * @param argv    NULL-terminated argument list
 *
 * @return 0 if the command exits successfully, -1 otherwise
 */
static int run_command(const char *program, char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
    {
        fprintf(
            stderr,
            "Failed to fork: %s\n",
            strerror(errno)
        );
        return -1;
    }

    if (pid == 0)
    {
        execvp(program, argv);

        fprintf(
            stderr,
            "Failed to execute '%s': %s\n",
            program,
            strerror(errno)
        );

        _exit(127);
    }

    int status;

    while (waitpid(pid, &status, 0) == -1)
    {
        if (errno != EINTR)
        {
            fprintf(
                stderr,
                "waitpid() failed: %s\n",
                strerror(errno)
            );
            return -1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    return 0;
}

/**
 * Convert an Intel HEX firmware image to a binary image and flash it
 * to the CH572D using minichlink.
 *
 * The flashing process consists of:
 *
 * 1. Removing flash protection using minichlink.
 * 2. Converting the Intel HEX file to a binary image.
 * 3. Writing the binary image to the CH572D.
 *
 * @param hex_file Path to the Intel HEX firmware image
 *
 * @return 0 on success, -1 on failure
 */
static int flash_chip(const char *hex_file)
{
    printf("[1/3] Removing flash protection...\n");

    char *unlock_args[] = {
        "minichlink",
        "-u",
        NULL
    };

    /*
     * Flash protection removal may fail when the chip is already
     * unlocked, so this is treated as a warning rather than a
     * fatal error.
     */
    if (run_command("minichlink", unlock_args) != 0)
    {
        fprintf(
            stderr,
            "Warning: minichlink -u returned an error.\n"
        );
    }

    printf(
        "[2/3] Converting %s to binary...\n",
        hex_file
    );

    char *convert_args[] = {
        "hex2bin.py",
        (char *)hex_file,
        TEMP_BIN_FILE,
        NULL
    };

    if (run_command("hex2bin.py", convert_args) != 0)
    {
        fprintf(
            stderr,
            "Failed to convert the Intel HEX file.\n"
            "Make sure hex2bin.py is installed and available in PATH.\n"
        );

        return -1;
    }

    printf("[3/3] Flashing firmware with minichlink...\n");

    char *flash_args[] = {
        "minichlink",
        "-w",
        TEMP_BIN_FILE,
        "flash",
        "-b",
        NULL
    };

    if (run_command("minichlink", flash_args) != 0)
    {
        fprintf(
            stderr,
            "Failed to flash the CH572D firmware.\n"
        );

        unlink(TEMP_BIN_FILE);
        return -1;
    }

    /*
     * The binary image is no longer needed after flashing.
     */
    unlink(TEMP_BIN_FILE);

    printf("Firmware flashed successfully.\n");

    return 0;
}

/**
 * Monitor the CH572D USB CDC serial output.
 *
 * The function waits for the device to re-enumerate after flashing,
 * configures the serial port and continuously forwards received data
 * to stdout.
 *
 * @param portname Serial device path
 */
static void monitor_serial(const char *portname)
{
    printf("Waiting for USB re-enumeration...\n");

    if (wait_for_serial(portname, SERIAL_TIMEOUT_MS) != 0)
        return;

    int fd = setup_serial(portname);

    if (fd < 0)
        return;

    printf(
        "\n--- CH572D Serial Monitor (%s @ 115200) ---\n",
        portname
    );

    printf("Press Ctrl+C to exit.\n\n");

    char buffer[256];

    while (monitor_running)
    {
        ssize_t bytes_read = read(
            fd,
            buffer,
            sizeof(buffer)
        );

        if (bytes_read > 0)
        {
            fwrite(buffer, 1, (size_t)bytes_read, stdout);
            fflush(stdout);
        }
        else if (bytes_read < 0)
        {
            if (errno == EINTR)
                continue;

            fprintf(
                stderr,
                "\nSerial read failed: %s\n",
                strerror(errno)
            );

            break;
        }
    }

    close(fd);

    printf("\nSerial monitor stopped.\n");
}

/**
 * Program entry point.
 */
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *hex_file = argv[1];
    const char *monitor_port = NULL;

    /*
     * Parse command-line options.
     */
    for (int i = 2; i < argc; ++i)
    {
        if (strcmp(argv[i], "-m") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(
                    stderr,
                    "Error: -m requires a serial port.\n"
                );

                return EXIT_FAILURE;
            }

            monitor_port = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else
        {
            fprintf(
                stderr,
                "Error: unknown option '%s'.\n\n",
                argv[i]
            );

            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /*
     * Install the SIGINT handler before starting the monitor.
     */
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        fprintf(
            stderr,
            "Warning: failed to install SIGINT handler: %s\n",
            strerror(errno)
        );
    }

    /*
     * Flash the firmware.
     */
    if (flash_chip(hex_file) != 0)
    {
        fprintf(
            stderr,
            "\nFirmware flashing failed.\n"
        );

        return EXIT_FAILURE;
    }

    /*
     * Start the serial monitor if requested.
     */
    if (monitor_port != NULL)
    {
        monitor_serial(monitor_port);
    }
    else
    {
        printf(
            "Flashing completed without serial monitoring.\n"
        );
    }

    return EXIT_SUCCESS;
}