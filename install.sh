#!/bin/bash
# install.sh — Install dependencies and compile the MeshPi TUI
# Usage: bash install.sh

set -e

echo "=== MeshPi — Installation ==="

echo ""
echo ">>> Installing packages..."
sudo apt update
sudo apt install -y batctl iw libncurses-dev gcc

echo ""
echo ">>> Loading batman-adv module..."
if ! lsmod | grep -q batman_adv; then
    sudo modprobe batman-adv
fi

# Ensure it loads at boot
if [ ! -f /etc/modules-load.d/batman-adv.conf ]; then
    echo batman-adv | sudo tee /etc/modules-load.d/batman-adv.conf > /dev/null
    echo "    Module configured to load at boot."
fi

echo ""
echo ">>> Compiling TUI..."
if [ -f meshpi-tui.c ]; then
    gcc -o meshpi-tui meshpi-tui.c -lncurses
    echo "    meshpi-tui compiled."
else
    echo "    meshpi-tui.c not found — skipping compilation."
fi

echo ""
echo "=== Installation complete ==="
echo ""
echo "Usage:"
echo "  ./script-mesh.sh <IP>           # direct mode"
echo "  sudo ./meshpi-tui               # TUI mode"
echo ""
echo "Examples:"
echo "  ./script-mesh.sh 192.168.1.10"
echo "  sudo ./meshpi-tui"