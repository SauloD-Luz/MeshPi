#!/bin/bash
# script-mesh.sh — BATMAN-adv mesh over Wi-Fi IBSS
# Usage: ./script-mesh.sh [interface] <IP>
#   interface: Wi-Fi device name (optional — auto-detected)
#   IP: IPv4 address for the bat0 interface
#
# Examples:
#   ./script-mesh.sh 192.168.1.10            (auto-detect interface)
#   ./script-mesh.sh wlp5s1 192.168.1.10      (explicit interface)
#
# To customize the network name, SSID or channel, edit the
# CONFIGURATION section below before running.

# --- CONFIGURATION (edit as needed) ---
SSID="MeshPi-Network"     # Name broadcast on the mesh
CONN_NAME="MeshPi"        # NetworkManager connection label
CHANNEL=6                 # Wi-Fi channel (fallbacks: 1, 11)
NETMASK=24                # Subnet mask (CIDR) — overridden if IP contains /CIDR
# --------------------------------------

# --- Argument parsing ---
if [ "$#" -eq 1 ]; then
    # Only IP: auto-detect interface
    IP_ADDR="$1"
    DETECT_INTERFACE=1
elif [ "$#" -eq 2 ]; then
    # Interface + IP
    INTERFACE="$1"
    IP_ADDR="$2"
    DETECT_INTERFACE=0
else
    echo "Usage: $0 [interface] <IP>"
    echo "Ex:   $0 192.168.1.10"
    echo "      $0 wlp5s1 192.168.1.11"
    exit 1
fi

# --- Auto-detect Wi-Fi interface ---
if [ "$DETECT_INTERFACE" = "1" ]; then
    # Try iw dev first (most reliable)
    INTERFACE=$(iw dev 2>/dev/null | awk '/Interface/{print $2}' | head -1)

    # Fallback: ip link
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(ip link show 2>/dev/null | grep -oE 'wl[^:]*' | head -1)
    fi

    # Fallback: legacy iwconfig
    if [ -z "$INTERFACE" ]; then
        INTERFACE=$(iwconfig 2>/dev/null | grep -m1 'IEEE' | awk '{print $1}')
    fi

    if [ -z "$INTERFACE" ]; then
        echo "ERROR: could not detect Wi-Fi interface."
        echo "Use explicit mode: $0 <interface> <IP>"
        exit 1
    fi
fi

# --- Extract CIDR from IP (if present) — then strip it ---
if echo "$IP_ADDR" | grep -q '/'; then
    CIDR="${IP_ADDR#*/}"
    IP_ADDR="${IP_ADDR%/*}"
else
    CIDR="$NETMASK"
fi

echo ">>> Wi-Fi interface: $INTERFACE"
echo ">>> Target IP:       $IP_ADDR/$CIDR"
echo ">>> SSID:            $SSID"

# --- Dependency check ---
if ! command -v batctl &>/dev/null || ! command -v iw &>/dev/null; then
    echo "ERROR: missing dependencies (batctl, iw)."
    echo "Run the installer first: sudo ./install.sh"
    exit 1
fi

# --- Execution ---
echo ">>> Loading batman-adv module..."
sudo modprobe batman-adv

echo ">>> Cleaning previous state..."
sudo nmcli con down "$CONN_NAME" 2>/dev/null
sudo nmcli con delete "$CONN_NAME" 2>/dev/null
sudo batctl if del "$INTERFACE" 2>/dev/null
sudo ip link set bat0 down 2>/dev/null

echo ">>> Creating IBSS via nmcli..."
sudo nmcli device set "$INTERFACE" managed yes 2>/dev/null
sudo nmcli con add type wifi ifname "$INTERFACE" con-name "$CONN_NAME" \
    autoconnect no ssid "$SSID" mode adhoc \
    ipv4.method link-local 2>/dev/null
sudo nmcli con modify "$CONN_NAME" 802-11-wireless.band bg 2>/dev/null
sudo nmcli con modify "$CONN_NAME" 802-11-wireless.channel "$CHANNEL" 2>/dev/null
sudo nmcli con up "$CONN_NAME"
sleep 3

echo ""
echo "--- IBSS Status ---"
iwconfig "$INTERFACE" 2>/dev/null | grep -E "Mode|ESSID|Cell|Frequency"
echo ""

if iwconfig "$INTERFACE" 2>/dev/null | grep -q "Not-Associated"; then
    echo ">>> Channel $CHANNEL did not associate. Trying channel 1..."
    sudo nmcli con down "$CONN_NAME"
    sudo nmcli con modify "$CONN_NAME" 802-11-wireless.channel 1
    sudo nmcli con up "$CONN_NAME"
    sleep 3
    iwconfig "$INTERFACE" 2>/dev/null | grep -E "Mode|ESSID|Cell|Frequency"
fi

if iwconfig "$INTERFACE" 2>/dev/null | grep -q "Not-Associated"; then
    echo ">>> Channel 1 also failed. Trying channel 11..."
    sudo nmcli con down "$CONN_NAME"
    sudo nmcli con modify "$CONN_NAME" 802-11-wireless.channel 11
    sudo nmcli con up "$CONN_NAME"
    sleep 3
    iwconfig "$INTERFACE" 2>/dev/null | grep -E "Mode|ESSID|Cell|Frequency"
fi

echo ">>> Removing IP from Wi-Fi interface..."
for addr in $(ip -4 addr show dev "$INTERFACE" | grep inet | awk '{print $2}'); do
    sudo ip addr del "$addr" dev "$INTERFACE"
done

echo ">>> Adding interface to BATMAN-adv..."
sudo batctl if add "$INTERFACE"
sudo ip link set up bat0
sudo ip addr add "$IP_ADDR/$CIDR" dev bat0

sudo iw dev "$INTERFACE" set power_save off 2>/dev/null || true

echo ""
echo "=== DIAGNOSTICS ==="
iwconfig "$INTERFACE" 2>/dev/null | grep -E "Mode|ESSID|Cell|Frequency"
batctl if 2>/dev/null
ip -4 addr show bat0 2>/dev/null | grep inet
echo ""
echo "Mesh active at $IP_ADDR/$CIDR via $INTERFACE (SSID: $SSID)"
echo "Neighbors: batctl o"
echo "Route:     batctl tr <IP>"