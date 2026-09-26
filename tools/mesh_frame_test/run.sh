#!/usr/bin/env bash
# Build and run the Meshtastic frame wire-format test.
# Requires g++ and python3 with the "cryptography" package.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -O2 -o mesh_frame_test mesh_frame_test.cpp
python3 verify.py
