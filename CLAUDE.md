# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains `@pokusew/pcsclite`, a Node.js binding for the PC/SC (Personal Computer/Smart Card) API, which allows interaction with smart cards and NFC devices. It works on Linux, macOS, and Windows. The package provides access to smart card readers and cards through the system's PC/SC API by utilizing Node.js native C++ addons.

## Build and Development Commands

### Installation

The package requires both Node.js native module build tools and the PC/SC API:

1. For native module building (all platforms):
   ```bash
   # Install node-gyp dependencies according to your OS
   # See: https://github.com/nodejs/node-gyp#installation
   ```

2. For PC/SC API (Linux only):
   ```bash
   # Debian/Ubuntu
   apt-get install libpcsclite1 libpcsclite-dev pcscd
   ```

3. Install dependencies:
   ```bash
   yarn install
   # or
   npm install
   ```

### Building

The module is built automatically during installation:
```bash
node-gyp rebuild
```

### Testing

Run the test suite:
```bash
npm test
# or
yarn test
```

The tests use Mocha framework with Should and Sinon for assertions and mocking.

## Architecture

The library consists of two main layers:

1. **Native C++ Addon Layer**:
   - `src/addon.cpp`: Entry point for the Node.js addon
   - `src/pcsclite.cpp` and `src/pcsclite.h`: Provides the PCSCLite class that interfaces with the PC/SC API
   - `src/cardreader.cpp` and `src/cardreader.h`: Implements the CardReader class for reader operations
   - These components are compiled via node-gyp into `build/Release/pcsclite.node`

2. **JavaScript Wrapper Layer**:
   - `lib/pcsclite.js`: Main JavaScript interface that wraps the native addon
   - `index.d.ts`: TypeScript definitions for the library

### Key Components

- **PCSCLite**: Main class to monitor for card readers. It's an EventEmitter that emits:
  - 'reader' events when card readers are detected
  - 'error' events for errors

- **CardReader**: Represents a physical card reader and handles communication with cards. It's an EventEmitter that emits:
  - 'status' events when card status changes (insertion/removal)
  - 'error' events for errors
  - 'end' events when the reader is removed

## Cross-Platform Considerations

The binding.gyp file configures the build process differently depending on the platform:
- Linux: Links with libpcsclite
- macOS: Uses the PCSC framework
- Windows: Links with WinSCard

## Notable Implementation Details

1. The module exposes an event-driven API that allows monitoring for card insertion/removal
2. It handles multiple readers simultaneously
3. TypeScript definitions are provided for full type safety
4. The library is compatible with Node.js versions 8.x through 20.x

## Examples

See the examples directory for sample code showing how to:
- Detect card readers
- Handle card insertion and removal
- Communicate with cards via APDU commands