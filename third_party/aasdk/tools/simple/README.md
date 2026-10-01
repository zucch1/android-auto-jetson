# aasdk Simple Headunit Test App


This is a minimal test application for verifying Android Auto headunit functionality using aasdk. It demonstrates:

- USB AOAP handshake and accessory mode switching
- Robust AOAP re-enumeration and retry logic
- Minimal logging

## Usage


1. Build the app:
   ```sh
   ./build.sh
   ```
2. Run the app with your Android device connected via USB:
   ```sh
   build-release/aasdk_simple_headunit
   ```
3. The app will perform the AOAP handshake, switch the device to accessory mode if needed, and robustly retry detection until the device is ready.
4. Android Auto should launch automatically if your AOAP strings match and the device is supported.

## File Overview
- `main.cpp`: Entry point, runs the minimal headunit logic.
- `usb_init.cpp`/`.hpp`: Handles USB AOAP handshake, accessory mode switching, and robust retry logic.
- `build.sh`: Simple build script for CMake.

## Notes
- No media channel, audio, video sink, or TLS logic is present in this minimal version.
- Only the files above are required for the minimal test app.
