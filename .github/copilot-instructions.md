# ESP-IDF Mesh Networking Project Instructions

## Project Overview
This is an ESP32-C3 mesh networking demo using ESP-IDF v5.5.1. The project implements a self-organizing WiFi mesh network where devices can communicate peer-to-peer and connect to a WiFi router through a designated root node.

## Architecture
- **Target Hardware**: ESP32-C3 (RISC-V, single-core, WiFi + BLE)
- **Mesh Topology**: Self-organizing tree with automatic root election or forced root assignment
- **Communication**: P2P messaging with JSON commands (e.g., `{"cmd":"toggle"}`)
- **Backhaul**: Root node connects to `IsolationSwitchWiFi` router for internet access

## Key Configuration Patterns

### Component Dependencies
Always include these components in `main/CMakeLists.txt`:
```cmake
PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event
```

### Mesh Configuration Constants
Located in `hello_world_main.c`:
- `MESH_ID`: Common identifier for all mesh nodes (currently `{0x11,0x22,0x33,0x44,0x55,0x66}`)
- `FORCE_ROOT`: Toggle between root (`true`) and node (`false`) behavior at compile time
- `ROUTER_SSID/ROUTER_PASS`: Backhaul WiFi credentials for root node internet access
- Mesh password: `"meshpassword"` for inter-node authentication

### Event-Driven Architecture
The mesh uses ESP-IDF's event system with `mesh_event_handler()` handling:
- `MESH_EVENT_STARTED`: Mesh initialization complete
- `MESH_EVENT_PARENT_CONNECTED`: Node joined mesh hierarchy
- `MESH_EVENT_ROUTING_TABLE_*`: Network topology changes

### Message Processing
- **RX Task**: Dedicated FreeRTOS task (`rx_task`) for receiving mesh messages
- **Command Format**: JSON strings like `{"cmd":"toggle"}` for device control
- **P2P Broadcast**: Uses `MESH_DATA_P2P` protocol with broadcast MAC (`0xFF` x 6)

## Development Workflow

### Build Commands
```bash
idf.py build          # Compile project
idf.py flash monitor  # Flash and monitor serial output
idf.py menuconfig     # Configure project settings
```

### VS Code Integration
- Uses ESP-IDF extension with pre-configured paths in `.vscode/settings.json`
- Target set to `esp32c3` with RISC-V toolchain
- CLANGD integration for IntelliSense with cross-compilation support

### Testing Strategy
- Hardware testing requires multiple ESP32-C3 devices
- Set different `FORCE_ROOT` values to test mesh formation
- Monitor serial output for mesh events and message routing
- Python pytest framework available for automated testing

## Critical Implementation Details

### WiFi/Mesh Initialization Sequence
1. NVS flash initialization
2. Network interface setup (`esp_netif_init`)
3. Event loop creation
4. WiFi stack initialization  
5. Mesh configuration and start
6. RX task creation for message handling

### Memory Management
- Static RX buffer: `rx_buf[256]` for incoming messages
- Minimal build configuration to reduce binary size
- FreeRTOS task stack: 4096 bytes for RX task

### Common Pitfalls
- Missing component dependencies cause header file not found errors
- Incorrect mesh ID prevents inter-device communication  
- Wrong WiFi credentials prevent root node internet access
- Message size limits in `RX_BUF_SZ` (256 bytes)

## File Structure
- `main/hello_world_main.c`: Core mesh implementation
- `main/CMakeLists.txt`: Component dependencies
- `sdkconfig`: ESP-IDF configuration (mesh support enabled)
- `.vscode/settings.json`: IDE configuration for ESP32-C3 target