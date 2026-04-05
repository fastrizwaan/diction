#!/bin/bash

# Diction Installation Script
# Builds and installs Diction with desktop integration

set -e  # Exit on any error

echo "Building and installing Diction..."

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    echo "Setting up build directory..."
    meson setup build
fi

# Recompile the project
echo "Compiling Diction..."
ninja -C build

# Install the application
echo "Installing Diction..."
sudo ninja -C build install

# Update icon cache and desktop database
echo "Updating icon cache and desktop database..."
sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor || true
sudo update-desktop-database /usr/local/share/applications || true

echo "Installation complete!"
echo ""
echo "You can now:"
echo "- Run 'diction' from the command line"
echo "- Find 'Diction' in your applications menu"
echo ""
echo "To uninstall, you can run: sudo ninja -C build uninstall"
