#!/usr/bin/env bash
# setup.sh — prepare the build environment on Ubuntu
# Run once after cloning: bash setup.sh

set -e

echo "==> Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y gcc make perl

echo "==> Cloning theft (property-based testing library)..."
if [ ! -d "tests/theft" ]; then
    git clone --depth 1 https://github.com/silentbicycle/theft.git tests/theft
    echo "    theft cloned."
else
    echo "    tests/theft already exists, skipping."
fi

echo "==> Building tsh..."
make tsh

echo ""
echo "Done! Run './tsh' to start the shell."
echo "Run 'make test01' through 'make test16' to test against trace files."
