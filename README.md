# ESP32-C3 Mesh Network# ESP32-C3 Mesh Network# ESP32-C3 Mesh Network



A simple WiFi mesh network for ESP32-C3 devices using ESP-IDF.



## What it doesA simple WiFi mesh network for ESP32-C3 devices using ESP-IDF.A simple WiFi mesh network for ESP32-C3 devices using ESP-IDF.



- Creates a self-organizing mesh network between ESP32-C3 devices

- Automatically elects a root node to connect to your WiFi router

- Sends messages between devices in the mesh## What it does## What it does

- Works with 2 or more ESP32-C3 boards



## Quick Setup

- Creates a self-organizing mesh network between ESP32-C3 devices- Creates a self-organizing mesh network between ESP32-C3 devices

1. **Install ESP-IDF v5.5.1**

2. **Edit the WiFi settings** in `main/hello_world_main.c`:- Automatically elects a root node to connect to your WiFi router- Automatically elects a root node to connect to your WiFi router

   ```c

   static const char *ROUTER_SSID = "YourWiFiName";- Sends messages between devices in the mesh- Sends messages between devices in the mesh

   static const char *ROUTER_PASS = "YourWiFiPassword";

   ```- Works with 2 or more ESP32-C3 boards- Works with 2 or more ESP32-C3 boards

3. **Build and flash**:

   ```bash

   idf.py build

   idf.py flash monitor## Quick Setup## Quick Setup

   ```

4. **Repeat on multiple ESP32-C3 devices**



## How it works1. **Install ESP-IDF v5.5.1**1. **Install ESP-IDF v5.5.1**



- First device powered on becomes the root node2. **Edit the WiFi settings** in `main/hello_world_main.c`:2. **Edit the WiFi settings** in `main/hello_world_main.c`:

- Other devices automatically join as child nodes

- All devices can send messages to each other   ```c   ```c

- If the root node fails, another device takes over

   static const char *ROUTER_SSID = "YourWiFiName";   static const char *ROUTER_SSID = "YourWiFiName";

## What you'll see

   static const char *ROUTER_PASS = "YourWiFiPassword";   static const char *ROUTER_PASS = "YourWiFiPassword";

**Root device logs:**

```   ```   ```

I MESH_UNIFIED: MESH_STARTED, layer=1

I MESH_UNIFIED: PARENT_CONNECTED, layer=13. **Build and flash**:3. **Build and flash**:

I MESH_UNIFIED: STATUS: connected=YES, layer=1, routing_table_size=1

```   ```bash   ```bash



**Child device logs:**   idf.py build   idf.py build

```

I MESH_UNIFIED: MESH_STARTED, layer=-1   idf.py flash monitor   idf.py flash monitor

I MESH_UNIFIED: PARENT_CONNECTED, layer=2

I MESH_UNIFIED: STATUS: connected=YES, layer=2, routing_table_size=1   ```   ```

```

4. **Repeat on multiple ESP32-C3 devices**4. **Repeat on multiple ESP32-C3 devices**

## Files



- `main/hello_world_main.c` - Main mesh code

- `main/CMakeLists.txt` - Build configuration## How it works## How it works

- `README.md` - This file



## Troubleshooting

- First device powered on becomes the root node- First device powered on becomes the root node

**"No parent found" errors:** Make sure all devices have the same WiFi settings and are powered on.

- Other devices automatically join as child nodes  - Other devices automatically join as child nodes  

**Build errors:** Run `idf.py clean` then `idf.py build`

- All devices can send messages to each other- All devices can send messages to each other

## License

- If the root node fails, another device takes over- If the root node fails, another device takes over

MIT License - feel free to use this code in your projects.


## What you'll see## What you'll see



**Root device logs:****Root device logs:**

``````

I MESH_UNIFIED: MESH_STARTED, layer=1I MESH_UNIFIED: MESH_STARTED, layer=1

I MESH_UNIFIED: PARENT_CONNECTED, layer=1I MESH_UNIFIED: PARENT_CONNECTED, layer=1

I MESH_UNIFIED: STATUS: connected=YES, layer=1, routing_table_size=1I MESH_UNIFIED: STATUS: connected=YES, layer=1, routing_table_size=1

``````



**Child device logs:****Child device logs:**

``````

I MESH_UNIFIED: MESH_STARTED, layer=-1  I MESH_UNIFIED: MESH_STARTED, layer=-1  

I MESH_UNIFIED: PARENT_CONNECTED, layer=2I MESH_UNIFIED: PARENT_CONNECTED, layer=2

I MESH_UNIFIED: STATUS: connected=YES, layer=2, routing_table_size=1I MESH_UNIFIED: STATUS: connected=YES, layer=2, routing_table_size=1

``````



## Files## Files



- `main/hello_world_main.c` - Main mesh code- `main/hello_world_main.c` - Main mesh code

- `main/CMakeLists.txt` - Build configuration  - `main/CMakeLists.txt` - Build configuration  

- `README.md` - This file- `README.md` - This file



## Troubleshooting## Troubleshooting



**"No parent found" errors:** Make sure all devices have the same WiFi settings and are powered on.**"No parent found" errors:** Make sure all devices have the same WiFi settings and are powered on.



**Build errors:** Run `idf.py clean` then `idf.py build`**Build errors:** Run `idf.py clean` then `idf.py build`



## License## License



MIT License - feel free to use this code in your projects.MIT License - feel free to use this code in your projects.

```c
// 🆔 Unique mesh network identifier (must match on all nodes)
static const uint8_t MESH_ID[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };

// 🌐 WiFi router credentials for internet backhaul
static const char *ROUTER_SSID = "IsolationSwitchWiFi";
static const char *ROUTER_PASS = "Cutoutswitch1"; 

// 🎯 Deployment mode: false = auto-election, true = force root
static const bool FORCE_ROOT = false;
```

### 🔒 Security & Limits

| Setting | Value | Purpose |
|---------|-------|---------|
| **Mesh Password** | `"meshpassword"` | Inter-node authentication |
| **Max Children** | `6` | Connections per parent node |
| **Message Buffer** | `256 bytes` | RX/TX buffer size |
| **Status Interval** | `10 seconds` | Health check frequency |

## 🚀 Quick Start

### 📋 Prerequisites

```bash
# Install ESP-IDF v5.5.1
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && git checkout v5.5.1
./install.sh esp32c3

# Activate environment
source export.sh  # Linux/macOS
# OR
export.bat        # Windows
```

### 🛠️ Build & Deploy

```bash
# Clone this repository
git clone https://github.com/heathatgallagher/esp32-mesh-project.git
cd esp32-mesh-project

# Configure for ESP32-C3
idf.py set-target esp32c3

# Build firmware
idf.py build

# Flash to device and monitor
idf.py -p /dev/ttyUSB0 flash monitor  # Linux
# OR
idf.py -p COM3 flash monitor          # Windows
```

### 📦 Required Components

Automatically included via `main/CMakeLists.txt`:

- `nvs_flash` → Non-volatile storage management
- `esp_wifi` → WiFi stack and mesh networking  
- `esp_netif` → Network interface abstraction
- `esp_event` → Event-driven architecture

## 📱 Deployment Modes

### 🤖 Auto-Election Mode (Recommended)

Perfect for production deployment - zero configuration required!

```bash
# 1️⃣ Set auto-election on ALL devices
FORCE_ROOT = false

# 2️⃣ Flash identical firmware to all ESP32-C3 boards  
idf.py flash

# 3️⃣ Power on devices in any order
# ✨ First device becomes root automatically
# ✨ Additional devices join as child nodes
```

### 🎯 Fixed Root Mode

For testing or specific topology requirements:

```bash
# 1️⃣ Designate one device as permanent root
FORCE_ROOT = true    # Root device only

# 2️⃣ Set all other devices as nodes
FORCE_ROOT = false   # Child devices

# 3️⃣ Power on root device FIRST, then children
```

## 📊 Real-Time Monitoring

### 🟢 Healthy Root Node Output
```bash
I MESH_UNIFIED: MESH_STARTED, mesh_id: 11:22:33:44:55:66, layer=1
I MESH_UNIFIED: ROOT_VOTE_STARTED
I MESH_UNIFIED: ROOT_VOTE_STOPPED  
I MESH_UNIFIED: PARENT_CONNECTED, layer=1, parent=aa:bb:cc:dd:ee:ff
I MESH_UNIFIED: CHILD_CONNECTED: 12:34:56:78:90:ab
I MESH_UNIFIED: STATUS: connected=YES, layer=1, routing_table_size=2
```

### 🔵 Healthy Child Node Output  
```bash
I MESH_UNIFIED: MESH_STARTED, mesh_id: 11:22:33:44:55:66, layer=-1
I MESH_UNIFIED: PARENT_CONNECTED, layer=2, parent=aa:bb:cc:dd:ee:ff
I MESH_UNIFIED: ROOT_ADDRESS: aa:bb:cc:dd:ee:ff
I MESH_UNIFIED: RX from aa:bb:cc:dd:ee:ff (10 bytes): hello-mesh
I MESH_UNIFIED: STATUS: connected=YES, layer=2, routing_table_size=1
```

### 📈 Status Dashboard

| Metric | Healthy Range | Warning Signs |
|--------|---------------|---------------|
| **Layer** | 1 (root), 2-6 (nodes) | Layer 0 = disconnected |
| **Routing Table** | 1+ entries | 0 = isolated node |
| **Connection** | `YES` | `NO` = mesh issues |
| **Events/Min** | < 100 | > 500 = instability |

## 💬 Messaging System

### 📡 P2P Communication Protocol

Send JSON commands between any mesh nodes:

```c
// 🎯 Command examples
{"cmd":"toggle"}           // Toggle GPIO output
{"cmd":"status"}           // Request node status
{"cmd":"sensor_read"}      // Read sensor data

// 📤 Broadcasting to all descendants  
mesh_data_t data = {
    .data = (uint8_t*)"{\"cmd\":\"toggle\"}",
    .size = 17,
    .proto = MESH_PROTO_BIN,
    .tos = MESH_TOS_P2P
};

mesh_addr_t broadcast = {0};
memset(broadcast.addr, 0xFF, 6);  // Broadcast address
esp_mesh_send(&broadcast, &data, MESH_DATA_P2P, NULL, 0);
```

### 📥 Message Reception

Automatic message handling in dedicated FreeRTOS task:

```c
// Messages processed in rx_task()
if (strstr((char*)data.data, "toggle")) {
    gpio_set_level(LED_GPIO, !gpio_get_level(LED_GPIO));
    ESP_LOGI(TAG, "LED toggled via mesh command");
}
```

## 🔧 Troubleshooting Guide

### ❌ Common Issues & Solutions

| Problem | Symptoms | Solution |
|---------|----------|----------|
| **🔍 No Parent Found** | Continuous scanning logs | ✅ Verify `MESH_ID` matches<br/>✅ Check WiFi router accessibility<br/>✅ Confirm mesh password |
| **🚫 Build Errors** | Component not found | ✅ Check `main/CMakeLists.txt` dependencies<br/>✅ Run `idf.py reconfigure` |
| **📶 Poor Connection** | Frequent disconnections | ✅ Check signal strength (RSSI)<br/>✅ Reduce distance between nodes<br/>✅ Change WiFi channel |
| **🔒 Auth Failures** | Router connection fails | ✅ Verify WiFi credentials<br/>✅ Check router security settings |

### 🛠️ Diagnostic Commands

```bash
# 📊 Monitor real-time logs with filtering
idf.py monitor | grep -E "(MESH_|STATUS:|ERROR)"

# ⚙️ Advanced configuration
idf.py menuconfig
  └── Component Config → ESP Wi-Fi Mesh

# 📏 Memory usage analysis  
idf.py size-components

# 🧹 Clean rebuild
idf.py fullclean && idf.py build
```

### 🩺 Health Check Indicators

```bash
# 🟢 Healthy mesh formation
✅ MESH_STARTED within 30 seconds
✅ ROOT_VOTE_STOPPED (election complete)  
✅ PARENT_CONNECTED (joined hierarchy)
✅ STATUS: connected=YES, layer>0

# 🔴 Problematic patterns
❌ Repeated NO_PARENT_FOUND events
❌ Layer stays at 0 after 60+ seconds
❌ Routing table size remains 0
❌ Frequent PARENT_DISCONNECTED events
```

## File Structure

```
├── .github/
│   └── copilot-instructions.md    # AI coding assistant instructions
├── .vscode/
│   └── settings.json              # VS Code ESP-IDF configuration
├── main/
│   ├── CMakeLists.txt             # Component dependencies
│   └── hello_world_main.c         # Core mesh implementation
├── CMakeLists.txt                 # Project configuration
├── sdkconfig                      # ESP-IDF build configuration
└── README.md                      # This file
```

## Development

### VS Code Setup
- ESP-IDF extension configured for ESP32-C3
- CLANGD integration for IntelliSense
- Serial monitor integration

### Testing
- Hardware testing requires multiple ESP32-C3 devices
- Python pytest framework available
- Monitor mesh formation and message routing

## License

This project is based on ESP-IDF examples and follows the same licensing terms.

## Support

For technical issues:
- Check the comprehensive logging output
- Review mesh event sequences
- Verify network topology in status messages