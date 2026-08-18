#!/usr/bin/env bash

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$SCRIPT_DIR"
readonly BUILD_DIR="$PROJECT_DIR/.build"
readonly MINICHLINK_DIR="$BUILD_DIR/ch32fun"
readonly INSTALL_DIR="$HOME/.local/bin"
readonly UDEV_RULE="/etc/udev/rules.d/99-ch572d.rules"

readonly RESET='\033[0m'
readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly BLUE='\033[0;34m'

log()
{
    printf "${BLUE}[INFO]${RESET} %s\n" "$1"
}

success()
{
    printf "${GREEN}[ OK ]${RESET} %s\n" "$1"
}

warning()
{
    printf "${YELLOW}[WARN]${RESET} %s\n" "$1"
}

error()
{
    printf "${RED}[ERROR]${RESET} %s\n" "$1" >&2
}

die()
{
    error "$1"
    exit 1
}

cleanup()
{
    if [[ "${KEEP_BUILD_DIR:-0}" != "1" ]]; then
        rm -rf "$BUILD_DIR"
    fi
}

trap cleanup EXIT

require_command()
{
    command -v "$1" >/dev/null 2>&1
}

install_packages()
{
    local packages=("$@")

    if require_command pacman; then
        log "Detected Arch Linux."

        sudo pacman -S --needed --noconfirm "${packages[@]}"

    elif require_command apt-get; then
        log "Detected Debian/Ubuntu."

        sudo apt-get update
        sudo apt-get install -y "${packages[@]}"

    elif require_command dnf; then
        log "Detected Fedora."

        sudo dnf install -y "${packages[@]}"

    else
        die "Unsupported package manager. Supported systems: Arch Linux, Debian/Ubuntu and Fedora."
    fi
}

install_system_dependencies()
{
    log "Installing system dependencies..."

    if require_command pacman; then
        install_packages \
            base-devel \
            git \
            python \
            python-pip \
            libusb

    elif require_command apt-get; then
        install_packages \
            build-essential \
            git \
            python3 \
            python3-pip \
            libusb-1.0-0-dev

    elif require_command dnf; then
        install_packages \
            gcc \
            gcc-c++ \
            make \
            git \
            python3 \
            python3-pip \
            libusb1-devel
    fi

    success "System dependencies installed."
}

install_python_dependencies()
{
    log "Checking Python dependencies..."

    if python3 -c "import intelhex" >/dev/null 2>&1; then
        success "Python package 'intelhex' is already installed."
        return
    fi

    log "Installing Python package 'intelhex'..."

    if python3 -m pip install --user --break-system-packages intelhex >/dev/null 2>&1; then
        success "Python package 'intelhex' installed."
        return
    fi

    warning "User installation failed. Trying the system package manager."

    if require_command pacman; then
        sudo pacman -S --needed --noconfirm python-intelhex
    elif require_command apt-get; then
        sudo apt-get install -y python3-intelhex
    elif require_command dnf; then
        sudo dnf install -y python3-intelhex
    else
        die "Could not install the Python 'intelhex' package."
    fi

    python3 -c "import intelhex" >/dev/null 2>&1 \
        || die "Python package 'intelhex' could not be installed."

    success "Python package 'intelhex' installed."
}

find_hex2bin()
{
    if command -v hex2bin.py >/dev/null 2>&1; then
        command -v hex2bin.py
        return
    fi

    local user_bin="$HOME/.local/bin/hex2bin.py"

    if [[ -x "$user_bin" ]]; then
        echo "$user_bin"
        return
    fi

    local python_bin
    python_bin="$(python3 -c 'import sysconfig; print(sysconfig.get_path("scripts"))')"

    if [[ -x "$python_bin/hex2bin.py" ]]; then
        echo "$python_bin/hex2bin.py"
        return
    fi

    return 1
}

install_minichlink()
{
    if command -v minichlink >/dev/null 2>&1; then
        success "minichlink is already installed."
        return
    fi

    log "minichlink was not found. Downloading ch32fun..."

    mkdir -p "$BUILD_DIR"

    if [[ -d "$MINICHLINK_DIR/.git" ]]; then
        log "Updating existing ch32fun checkout..."
        git -C "$MINICHLINK_DIR" pull --ff-only
    else
        git clone \
            --depth 1 \
            https://github.com/cnlohr/ch32fun.git \
            "$MINICHLINK_DIR"
    fi

    log "Building minichlink..."

    local minichlink_path

    minichlink_path="$MINICHLINK_DIR/minichlink"

    [[ -d "$MINICHLINK_DIR/minichlink" ]] \
        || die "The minichlink directory was not found in ch32fun."

    make -C "$MINICHLINK_DIR/minichlink"

    [[ -x "$minichlink_path" ]] \
        || die "minichlink compilation failed."

    mkdir -p "$INSTALL_DIR"

    install -m 755 \
        "$minichlink_path" \
        "$INSTALL_DIR/minichlink"

    success "minichlink installed to $INSTALL_DIR/minichlink."
}

install_flasher()
{
    log "Compiling CH572D flasher..."

    [[ -f "$PROJECT_DIR/flasher.c" ]] \
        || die "flasher.c was not found in $PROJECT_DIR."

    mkdir -p "$INSTALL_DIR"

    gcc \
        -std=c11 \
        -Wall \
        -Wextra \
        -Wpedantic \
        -O2 \
        "$PROJECT_DIR/flasher.c" \
        -o "$INSTALL_DIR/ch572d-flasher"

    chmod 755 "$INSTALL_DIR/ch572d-flasher"

    success "CH572D flasher installed to $INSTALL_DIR/ch572d-flasher."
}

install_udev_rules()
{
    log "Installing udev rules..."

    sudo tee "$UDEV_RULE" >/dev/null <<'EOF'
# CH572D / WCH USB devices
# Allow users in the uucp/dialout groups to access WCH USB devices.

SUBSYSTEM=="usb", ATTR{idVendor}=="1a86", MODE="0660", TAG+="uaccess"
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0660", TAG+="uaccess"
EOF

    sudo udevadm control --reload-rules
    sudo udevadm trigger

    success "udev rules installed."
}

check_path()
{
    case ":$PATH:" in
        *":$INSTALL_DIR:"*)
            return
            ;;
    esac

    warning "$INSTALL_DIR is not currently in PATH."

    local shell_name
    shell_name="$(basename "${SHELL:-bash}")"

    case "$shell_name" in
        bash)
            local rc="$HOME/.bashrc"
            ;;
        zsh)
            local rc="$HOME/.zshrc"
            ;;
        fish)
            mkdir -p "$HOME/.config/fish"
            local rc="$HOME/.config/fish/config.fish"

            if ! grep -Fq "$INSTALL_DIR" "$rc" 2>/dev/null; then
                printf '\nset -gx PATH %s $PATH\n' "$INSTALL_DIR" >> "$rc"
            fi

            success "Added $INSTALL_DIR to fish PATH."
            return
            ;;
        *)
            warning "Unknown shell. Add $INSTALL_DIR to PATH manually."
            return
            ;;
    esac

    if ! grep -Fq "$INSTALL_DIR" "$rc" 2>/dev/null; then
        printf '\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "$rc"
        success "Added $INSTALL_DIR to $rc."
    fi
}

verify_installation()
{
    log "Verifying installation..."

    local failed=0

    if [[ -x "$INSTALL_DIR/ch572d-flasher" ]]; then
        success "ch572d-flasher"
    else
        error "ch572d-flasher was not installed."
        failed=1
    fi

    if [[ -x "$INSTALL_DIR/minichlink" ]]; then
        success "minichlink"
    else
        error "minichlink was not installed."
        failed=1
    fi

    if find_hex2bin >/dev/null 2>&1; then
        success "hex2bin.py"
    else
        error "hex2bin.py was not found."
        failed=1
    fi

    python3 -c "import intelhex" >/dev/null 2>&1 \
        && success "Python intelhex module" \
        || {
            error "Python intelhex module was not found."
            failed=1
        }

    [[ "$failed" -eq 0 ]] \
        || die "Installation verification failed."

    success "All dependencies and tools are installed."
}

main()
{
    printf "\n"
    printf "========================================\n"
    printf "       CH572D Flasher Installer\n"
    printf "========================================\n\n"

    if [[ "$EUID" -eq 0 ]]; then
        die "Do not run this installer as root. Run it as your normal user."
    fi

    install_system_dependencies
    install_python_dependencies
    install_minichlink
    install_flasher
    install_udev_rules
    check_path
    verify_installation

    printf "\n"
    success "Installation completed successfully."

    printf "\n"
    printf "You can now use:\n\n"
    printf "  ch572d-flasher firmware.hex\n"
    printf "  ch572d-flasher firmware.hex -m /dev/ttyACM0\n\n"

    warning "Restart your shell if ~/.local/bin was added to PATH."
}

main "$@"