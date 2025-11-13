#!/bin/bash

# ========================================
# LittleFS Upload Script for M5StickC Plus
# ========================================
# This script creates and uploads a LittleFS filesystem image
# from the data/ folder to your M5StickC Plus
#
# Requirements (install via Homebrew):
#   brew install mklittlefs esptool

set -e  # Exit on any error

# ============ CONFIGURATION ============
PORT=""                                # Auto-detected port
BAUD="115200"                          # Upload speed (reliable for M5StickC Plus)
DATA_DIR="./data"                      # Your web files folder
IMAGE_FILE="./littlefs.bin"            # Temporary image file
PARTITION_SIZE="917504"                # Size for huge_app partition
BLOCK_SIZE="4096"
PAGE_SIZE="256"
PARTITION_OFFSET="0x310000"            # huge_app LittleFS offset

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# ============ FUNCTIONS ============

print_header() {
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}  LittleFS Upload for M5StickC Plus${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_step() {
    echo -e "\n${GREEN}▶${NC} $1"
}

print_error() {
    echo -e "${RED}✗ ERROR:${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}⚠${NC} $1"
}

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Verify required tools are installed
check_tools() {
    local missing=()
    
    if ! command_exists mklittlefs; then
        missing+=("mklittlefs")
    fi
    
    if ! command_exists esptool; then
        missing+=("esptool")
    fi
    
    if [[ ${#missing[@]} -gt 0 ]]; then
        print_error "Missing required tools: ${missing[*]}"
        echo -e "\n${YELLOW}Install with Homebrew:${NC}"
        echo -e "  ${GREEN}brew install ${missing[*]}${NC}\n"
        exit 1
    fi
    
    print_success "Tools found: mklittlefs, esptool"
}

# Auto-detect serial port
detect_port() {
    if [[ -e "$PORT" ]]; then
        print_success "Using port: $PORT"
        return 0
    fi
    
    print_warning "Port $PORT not found. Scanning..."
    
    # Find USB serial devices
    PORTS=($(ls /dev/tty.* 2>/dev/null | grep -iE "usb|serial" || true))
    
    if [[ ${#PORTS[@]} -eq 0 ]]; then
        print_error "No USB serial ports found!"
        print_error "Please connect your M5StickC Plus and try again."
        exit 1
    elif [[ ${#PORTS[@]} -eq 1 ]]; then
        PORT="${PORTS[0]}"
        print_success "Auto-detected port: $PORT"
    else
        echo -e "\n${YELLOW}Multiple USB ports found:${NC}"
        for i in "${!PORTS[@]}"; do
            echo "  $((i+1)). ${PORTS[$i]}"
        done
        read -p "Select port [1-${#PORTS[@]}]: " selection
        PORT="${PORTS[$((selection-1))]}"
        print_success "Selected port: $PORT"
    fi
}

# ============ MAIN SCRIPT ============

print_header

# Step 1: Verify data directory exists
print_step "Checking data directory..."
if [[ ! -d "$DATA_DIR" ]]; then
    print_error "Data directory not found: $DATA_DIR"
    exit 1
fi

FILE_COUNT=$(find "$DATA_DIR" -type f | wc -l | tr -d ' ')
print_success "Found $FILE_COUNT files in $DATA_DIR"
ls -lh "$DATA_DIR"

# Step 2: Check tools are installed
print_step "Checking required tools..."
check_tools

# Step 3: Detect/verify serial port
print_step "Detecting M5StickC Plus..."
detect_port

# Step 4: Create LittleFS image
print_step "Creating LittleFS image..."
rm -f "$IMAGE_FILE"

mklittlefs -c "$DATA_DIR" -b "$BLOCK_SIZE" -p "$PAGE_SIZE" -s "$PARTITION_SIZE" "$IMAGE_FILE"

if [[ ! -f "$IMAGE_FILE" ]]; then
    print_error "Failed to create LittleFS image"
    exit 1
fi

IMAGE_SIZE=$(ls -lh "$IMAGE_FILE" | awk '{print $5}')
print_success "Created $IMAGE_FILE ($IMAGE_SIZE)"

# Step 5: Upload to M5StickC Plus
print_step "Uploading to M5StickC Plus..."
print_warning "Please wait, do not disconnect the device..."

esptool --chip esp32 \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default-reset \
    --after hard-reset \
    write-flash \
    -z \
    --flash-mode dio \
    --flash-freq 80m \
    --flash-size detect \
    "$PARTITION_OFFSET" \
    "$IMAGE_FILE"

# Step 6: Cleanup
print_step "Cleaning up..."
rm -f "$IMAGE_FILE"
print_success "Temporary files removed"

# Done!
echo -e "\n${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}✓ LittleFS upload complete!${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "\n${BLUE}Next steps:${NC}"
echo -e "  1. Upload your Arduino sketch (.ino file)"
echo -e "  2. Open Serial Monitor to get the device IP"
echo -e "  3. Browse to http://iss.local/ or the IP address"
echo ""
