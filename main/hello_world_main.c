#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "driver/gpio.h"


static const char *TAG = "MESH_UNIFIED";

// Forward declarations
static void ip_check_task(void *arg);
static void log_all_netifs(const char *reason);
static void try_start_dhcp_on_all(void);
static bool parse_mac_str(const char *s, uint8_t out[6]);

// Mesh netif handles (STA connects upstream to router; AP serves downstream children)
static esp_netif_t *mesh_netif_sta = NULL;
static esp_netif_t *mesh_netif_ap = NULL;

// --- Demo config (later: load these from NVS/captive portal) ---
static const uint8_t MESH_ID[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };  // same for all nodes
static const char *ROUTER_SSID   = "IsolationSwitchWiFi";
static const char *ROUTER_PASS   = "Cutoutswitch1";

// Simple RX buffer
#define RX_BUF_SZ 256
static uint8_t rx_buf[RX_BUF_SZ];

// LED control (ESP32-C3 built-in LED on GPIO8)
#define LED_GPIO 8
static bool led_state = false;

// Web server
static httpd_handle_t web_server = NULL;
static bool is_root_node = false;
static bool ip_check_active = false;

// Node registry for web interface
#define MAX_MESH_NODES 10
typedef struct {
    mesh_addr_t addr;
    uint32_t last_seen;
    bool led_state;
    int layer;
    bool is_active;
    // Route hint: last mesh source address we saw for this node (for unicast P2P)
    mesh_addr_t last_from;
    bool has_route;
    int rssi; // last reported RSSI (dBm) to parent/router on the node side
} node_info_t;

static node_info_t known_nodes[MAX_MESH_NODES];
static int node_count = 0;

// Parse MAC address string (aa:bb:cc:dd:ee:ff) into 6-byte array
static bool parse_mac_str(const char *s, uint8_t out[6]) {
    if (!s) return false;
    int vals[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)vals[i];
    }
    return true;
}

// LED Control Functions
static void led_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0); // Start with LED off
    ESP_LOGI(TAG, "LED initialized on GPIO%d", LED_GPIO);
}

static void led_set(bool state) {
    led_state = state;
    gpio_set_level(LED_GPIO, state ? 1 : 0);
    ESP_LOGI(TAG, "LED %s", state ? "ON" : "OFF");
}

static void led_toggle(void) {
    led_set(!led_state);
}

// Node Registry Functions
static void add_or_update_node(mesh_addr_t *addr, int layer) {
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Filter out invalid entries: ignore self and non-mesh layers
    if (layer < 1) {
        ESP_LOGD(TAG, "Ignoring node with invalid layer %d", layer);
        return;
    }
    // Check against WiFi MAC (not mesh ID) to catch heartbeat loopback
    uint8_t self_wifi_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_wifi_mac);
    if (memcmp(self_wifi_mac, addr->addr, 6) == 0) {
        // Don't add ourselves from loopback/broadcasts
        return;
    }
    
    // Check if node already exists
    for (int i = 0; i < node_count; i++) {
        if (memcmp(known_nodes[i].addr.addr, addr->addr, 6) == 0) {
            known_nodes[i].last_seen = now;
            known_nodes[i].layer = layer;
            known_nodes[i].is_active = true;
            return;
        }
    }
    
    // Add new node if there's space
    if (node_count < MAX_MESH_NODES) {
        memcpy(known_nodes[node_count].addr.addr, addr->addr, 6);
        known_nodes[node_count].last_seen = now;
        known_nodes[node_count].layer = layer;
        known_nodes[node_count].led_state = false;
        known_nodes[node_count].is_active = true;
        memset(&known_nodes[node_count].last_from, 0, sizeof(known_nodes[node_count].last_from));
        known_nodes[node_count].has_route = false;
        known_nodes[node_count].rssi = -127;
        node_count++;
        ESP_LOGI(TAG, "Added node %02x:%02x:%02x:%02x:%02x:%02x to registry (layer %d)",
                 addr->addr[0], addr->addr[1], addr->addr[2], addr->addr[3], addr->addr[4], addr->addr[5], layer);
    }
}

// Convert RSSI (dBm) to a rough signal percentage for UI (0 to 100)
static int rssi_to_percent(int rssi) {
    if (rssi <= -90) return 0;
    if (rssi >= -50) return 100;
    // Map [-90..-50] dBm to [0..100]
    int pct = (rssi + 90) * 25 / 10; // approx 2.5x
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// Web Server HTTP Handlers
static esp_err_t root_handler(httpd_req_t *req) {
    const char* html = "<!DOCTYPE html>\n"
    "<html><head><title>ESP32 Mesh Controller</title>\n"
    "<style>body{font-family:Arial;margin:20px}table{border-collapse:collapse;width:100%}th,td{border:1px solid #ddd;padding:8px;text-align:left}th{background-color:#f2f2f2}.btn{padding:5px 10px;margin:2px;cursor:pointer}.btn-on{background-color:#4CAF50;color:white}.btn-off{background-color:#f44336;color:white}.sigbar{height:8px;background:#ddd;border-radius:4px;overflow:hidden}.sigfill{height:8px;background:#4CAF50}</style>\n"
    "<script>\n"
    "async function toggleLED(mac) {\n"
    "  try {\n"
    "    const response = await fetch(`/api/led/${mac}`, {method: 'POST'});\n"
    "    if (response.ok) { loadNodes(); }\n"
    "  } catch (e) { console.error('Failed to toggle LED:', e); }\n"
    "}\n"
    "async function loadNodes() {\n"
    "  try {\n"
    "    const response = await fetch('/api/nodes');\n"
    "    const nodes = await response.json();\n"
    "    const tbody = document.getElementById('nodeTable');\n"
    "    tbody.innerHTML = '';\n"
    "    nodes.forEach(node => {\n"
    "      const label = node.signal >= 75 ? 'Strong' : (node.signal >= 50 ? 'Good' : (node.signal >= 25 ? 'Fair' : 'Weak'));\n"
    "      const bar = `<div class='sigbar'><div class='sigfill' style='width:${node.signal}%' /></div>`;\n"
    "      const row = `<tr>\n"
    "        <td>${node.mac}</td>\n"
    "        <td>${node.layer}</td>\n"
    "        <td>${node.active ? 'Active' : 'Inactive'}</td>\n"
    "        <td>${node.rssi ?? ''} dBm</td>\n"
    "        <td>${bar} <small>${node.signal ?? 0}% (${label})</small></td>\n"
    "        <td>${node.via ?? ''}</td>\n"
    "        <td><button class='btn ${node.led ? 'btn-on' : 'btn-off'}' onclick='toggleLED(\"${node.mac}\")'>${node.led ? 'ON' : 'OFF'}</button></td>\n"
    "      </tr>`;\n"
    "      tbody.innerHTML += row;\n"
    "    });\n"
    "  } catch (e) { console.error('Failed to load nodes:', e); }\n"
    "}\n"
    "setInterval(loadNodes, 2000); // Refresh every 2 seconds\n"
    "</script></head>\n"
    "<body onload='loadNodes()'>\n"
    "<h1>ESP32 Mesh Network Controller</h1>\n"
    "<h2>Connected Nodes</h2>\n"
    "<table><thead><tr><th>MAC Address</th><th>Layer</th><th>Status</th><th>RSSI</th><th>Signal</th><th>Via</th><th>LED Control</th></tr></thead><tbody id='nodeTable'></tbody></table>\n"
    "</body></html>";
    
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t api_nodes_handler(httpd_req_t *req) {
    // Stream JSON in chunks to keep HTTPD stack usage low
    uint8_t self_addr[6];
    esp_wifi_get_mac(WIFI_IF_STA, self_addr);
    httpd_resp_set_type(req, "application/json");

    // Mark stale nodes as inactive (no heartbeat/status for 60 seconds)
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    const uint32_t STALE_TIMEOUT_MS = 60000; // 60 seconds
    for (int i = 0; i < node_count; i++) {
        if (now - known_nodes[i].last_seen > STALE_TIMEOUT_MS) {
            known_nodes[i].is_active = false;
        }
    }

    // Start JSON array
    httpd_resp_send_chunk(req, "[", 1);

    // Add self to the list
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             self_addr[0], self_addr[1], self_addr[2],
             self_addr[3], self_addr[4], self_addr[5]);

    // Determine our own RSSI to current AP (router if root, parent if not root)
    int self_rssi = -127;
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        self_rssi = ap.rssi;
    }
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "{\"mac\":\"%s\",\"layer\":%d,\"active\":true,\"led\":%s,\"rssi\":%d,\"signal\":%d,\"via\":\"root\"}",
                     mac_str, esp_mesh_get_layer(), led_state ? "true" : "false", self_rssi, rssi_to_percent(self_rssi));
    if (n > 0) httpd_resp_send_chunk(req, buf, n);
    
    // Add known nodes (show both active and inactive)
    for (int i = 0; i < node_count; i++) {
        snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                 known_nodes[i].addr.addr[0], known_nodes[i].addr.addr[1], known_nodes[i].addr.addr[2],
                 known_nodes[i].addr.addr[3], known_nodes[i].addr.addr[4], known_nodes[i].addr.addr[5]);

        char via_str[18] = "";
        if (known_nodes[i].has_route) {
            snprintf(via_str, sizeof(via_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                     known_nodes[i].last_from.addr[0], known_nodes[i].last_from.addr[1], known_nodes[i].last_from.addr[2],
                     known_nodes[i].last_from.addr[3], known_nodes[i].last_from.addr[4], known_nodes[i].last_from.addr[5]);
            // If via equals node MAC, treat as direct
            if (strcmp(via_str, mac_str) == 0) {
                strcpy(via_str, "direct");
            }
        } else {
            strcpy(via_str, "?");
        }
        n = snprintf(buf, sizeof(buf),
                     ",{\"mac\":\"%s\",\"layer\":%d,\"active\":%s,\"led\":%s,\"rssi\":%d,\"signal\":%d,\"via\":\"%s\"}",
                     mac_str, known_nodes[i].layer,
                     known_nodes[i].is_active ? "true" : "false",
                     known_nodes[i].led_state ? "true" : "false",
                     known_nodes[i].rssi, rssi_to_percent(known_nodes[i].rssi), via_str);
        if (n > 0) httpd_resp_send_chunk(req, buf, n);
    }

    // End JSON array
    httpd_resp_send_chunk(req, "]", 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t api_led_handler(httpd_req_t *req) {
    // Extract MAC from URL (e.g., /api/led/aa:bb:cc:dd:ee:ff)
    const char *uri = req->uri;
    ESP_LOGI(TAG, "LED handler called for URI: %s", uri);
    
    const char *mac_start = strrchr(uri, '/');
    if (!mac_start || strlen(mac_start + 1) != 17) {
        ESP_LOGW(TAG, "Invalid URI format: %s", uri);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MAC address format");
        return ESP_FAIL;
    }
    
    char mac_param[18];
    if (mac_start && strlen(mac_start + 1) == 17) {
        strncpy(mac_param, mac_start + 1, 17);
        mac_param[17] = '\0';
        
        // Check if it's for this device
        uint8_t self_addr[6];
        esp_wifi_get_mac(WIFI_IF_STA, self_addr);
        char self_mac[18];
        snprintf(self_mac, sizeof(self_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 self_addr[0], self_addr[1], self_addr[2],
                 self_addr[3], self_addr[4], self_addr[5]);
        
        if (strcmp(mac_param, self_mac) == 0) {
            // Toggle local LED
            led_toggle();
            httpd_resp_send(req, "OK", 2);
            return ESP_OK;
        } else {
            // Send command to mesh network
            char cmd_str[128];
            snprintf(cmd_str, sizeof(cmd_str), "{\"cmd\":\"led_toggle\",\"target_mac\":\"%s\"}", mac_param);
            
            mesh_data_t data = {
                .data = (uint8_t*)cmd_str,
                .size = strlen(cmd_str),
                .proto = MESH_PROTO_BIN,
                .tos = MESH_TOS_P2P
            };
            // Try unicast first if we have a route for this MAC; fallback to broadcast
            bool sent = false;
            for (int i = 0; i < node_count; i++) {
                char node_mac[18];
                snprintf(node_mac, sizeof(node_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         known_nodes[i].addr.addr[0], known_nodes[i].addr.addr[1], known_nodes[i].addr.addr[2],
                         known_nodes[i].addr.addr[3], known_nodes[i].addr.addr[4], known_nodes[i].addr.addr[5]);
                if (strcmp(node_mac, mac_param) == 0 && known_nodes[i].has_route) {
                    esp_err_t uerr = esp_mesh_send(&known_nodes[i].last_from, &data, MESH_DATA_P2P, NULL, 0);
                    ESP_LOGI(TAG, "Sent LED toggle (unicast) to %s via %02x:%02x:%02x:%02x:%02x:%02x: %s",
                             mac_param,
                             known_nodes[i].last_from.addr[0], known_nodes[i].last_from.addr[1], known_nodes[i].last_from.addr[2],
                             known_nodes[i].last_from.addr[3], known_nodes[i].last_from.addr[4], known_nodes[i].last_from.addr[5],
                             esp_err_to_name(uerr));
                    sent = (uerr == ESP_OK);
                    break;
                }
            }
            if (!sent) {
                mesh_addr_t bcast = {0};
                memset(bcast.addr, 0xFF, 6);
                bcast.mip.port = MESH_DATA_P2P;
                esp_err_t berr = esp_mesh_send(&bcast, &data, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(TAG, "Sent LED toggle (broadcast) to %s: %s", mac_param, esp_err_to_name(berr));
            }
            
            httpd_resp_send(req, "OK", 2);
            return ESP_OK;
        }
    }
    
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MAC address");
    return ESP_FAIL;
}

// Web Server Management
static esp_err_t start_web_server(void) {
    if (web_server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return ESP_OK;
    }
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    // Increase HTTPD stack: default is ~4KB, bump to 8KB to handle JSON building and templating
    config.stack_size = 8192;
    // Enable wildcard URI matching so handlers like "/api/led/*" work
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);
    
    if (httpd_start(&web_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &root_uri);
        
        httpd_uri_t api_nodes_uri = {
            .uri = "/api/nodes",
            .method = HTTP_GET,
            .handler = api_nodes_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &api_nodes_uri);
        
        httpd_uri_t api_led_uri = {
            .uri = "/api/led/*",
            .method = HTTP_POST,
            .handler = api_led_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(web_server, &api_led_uri);
        
        ESP_LOGI(TAG, "Web server started successfully");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start web server");
    return ESP_FAIL;
}

static void stop_web_server(void) {
    if (web_server != NULL) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(web_server);
        web_server = NULL;
    }
}

static esp_err_t start_mdns_service(void) {
    ESP_LOGI(TAG, "Starting mDNS service");
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }
    
    mdns_hostname_set("mesh-controller");
    mdns_instance_name_set("ESP32 Mesh Controller");
    
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS service started - accessible at http://mesh-controller.local");
    
    // Also log the IP address as fallback
    esp_netif_t *netif = mesh_netif_sta ? mesh_netif_sta : esp_netif_get_handle_from_ifkey("MESH_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Fallback IP access: http://" IPSTR ":80", IP2STR(&ip_info.ip));
        }
    }
    return ESP_OK;
}

static void handle_root_transition(bool becoming_root) {
    ESP_LOGI(TAG, "Root transition called: becoming_root=%s, current_is_root=%s", 
             becoming_root ? "true" : "false", is_root_node ? "true" : "false");
    
    if (becoming_root && !is_root_node) {
        ESP_LOGI(TAG, "Becoming root node - starting web services");
        is_root_node = true;
        
        // Start background task to wait for IP assignment
        if (!ip_check_active) {
            ip_check_active = true;
            xTaskCreate(ip_check_task, "ip_check", 4096, NULL, 5, NULL);
            ESP_LOGI(TAG, "Started IP monitoring task");
        }
        
        // Start web server
        start_web_server();
        
        // Start mDNS service
        start_mdns_service();
        
        // Request status from all nodes
        const char *cmd_str = "{\"cmd\":\"status_request\"}";
        
        mesh_data_t data = {
            .data = (uint8_t*)cmd_str,
            .size = strlen(cmd_str),
            .proto = MESH_PROTO_BIN,
            .tos = MESH_TOS_P2P
        };
        mesh_addr_t bcast = {0};
        memset(bcast.addr, 0xFF, 6);
        bcast.mip.port = MESH_DATA_P2P;
        
        esp_mesh_send(&bcast, &data, MESH_DATA_P2P, NULL, 0);
        
    } else if (!becoming_root && is_root_node) {
        ESP_LOGI(TAG, "No longer root node - stopping web services");
        is_root_node = false;
        
        // Stop web server
        stop_web_server();
        
        // Stop mDNS
        mdns_free();
    }
}

static void mesh_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    static uint32_t event_count = 0;
    event_count++;
    
    // Rate limit logging to prevent spam
    if (event_count % 10 == 1) {
        ESP_LOGI(TAG, "Mesh event rate: %lu events processed", event_count);
    }
    
    switch (id) {
    case MESH_EVENT_STARTED: {
        mesh_addr_t addr;
        esp_mesh_get_id(&addr);
        ESP_LOGI(TAG, "MESH_STARTED, mesh_id: %02x:%02x:%02x:%02x:%02x:%02x, layer=%d", 
                 addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3], addr.addr[4], addr.addr[5],
                 esp_mesh_get_layer());
        
        // Check if we became root
        handle_root_transition(esp_mesh_is_root());
        break;
    }
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *conn = (mesh_event_connected_t *)data;
        ESP_LOGI(TAG, "PARENT_CONNECTED, layer=%d, parent=%02x:%02x:%02x:%02x:%02x:%02x",
                 esp_mesh_get_layer(), 
                 conn->connected.bssid[0], conn->connected.bssid[1], conn->connected.bssid[2],
                 conn->connected.bssid[3], conn->connected.bssid[4], conn->connected.bssid[5]);
        
        // Add parent to node registry (but skip if it's the router - only track mesh nodes)
        // Root connects to router, not another mesh node, so don't add it to registry
        if (esp_mesh_get_layer() > 1) {
            mesh_addr_t parent_addr;
            memcpy(parent_addr.addr, conn->connected.bssid, 6);
            add_or_update_node(&parent_addr, esp_mesh_get_layer() - 1);
        }
        
        // Check if we became root (connected directly to router)
        if (esp_mesh_get_layer() == 1) {
            ESP_LOGI(TAG, "Connected to router - checking root status");
            
            // Configure mesh root to request IP from external router
            ESP_ERROR_CHECK(esp_mesh_post_toDS_state(true));
            ESP_LOGI(TAG, "Enabled mesh root to external DS (router)");
            // Ensure DHCP client is running on the mesh STA interface
            if (mesh_netif_sta) {
                esp_err_t derr = esp_netif_dhcpc_start(mesh_netif_sta);
                ESP_LOGI(TAG, "Ensured DHCP client on STA: %s", esp_err_to_name(derr));
            }
            
            handle_root_transition(esp_mesh_is_root());
        }
        break;
    }
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconn = (mesh_event_disconnected_t *)data;
        ESP_LOGW(TAG, "PARENT_DISCONNECTED, reason=%d, will scan for new parent", disconn->reason);
        break;
    }
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_connected_t *conn = (mesh_event_connected_t *)data;
        ESP_LOGI(TAG, "CHILD_CONNECTED: %02x:%02x:%02x:%02x:%02x:%02x",
                 conn->connected.bssid[0], conn->connected.bssid[1], conn->connected.bssid[2],
                 conn->connected.bssid[3], conn->connected.bssid[4], conn->connected.bssid[5]);
        // Don't add here (field may not reflect child's WiFi MAC). Ask for status; child will add itself properly via response
        const char *cmd_str = "{\"cmd\":\"status_request\"}";
        mesh_data_t data_req = {
            .data = (uint8_t*)cmd_str,
            .size = strlen(cmd_str),
            .proto = MESH_PROTO_BIN,
            .tos = MESH_TOS_P2P
        };
        mesh_addr_t bcast = {0};
        memset(bcast.addr, 0xFF, 6);
        bcast.mip.port = MESH_DATA_P2P;
        esp_mesh_send(&bcast, &data_req, MESH_DATA_P2P, NULL, 0);
        break;
    }
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_disconnected_t *disconn = (mesh_event_disconnected_t *)data;
        ESP_LOGW(TAG, "CHILD_DISCONNECTED, reason=%d", disconn->reason);
        break;
    }
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)data;
        ESP_LOGI(TAG, "ROOT_ADDRESS: %02x:%02x:%02x:%02x:%02x:%02x",
                 root_addr->addr[0], root_addr->addr[1], root_addr->addr[2],
                 root_addr->addr[3], root_addr->addr[4], root_addr->addr[5]);
        
        // Check if we are the root node
        mesh_addr_t self_addr;
        esp_mesh_get_id(&self_addr);
        if (memcmp(root_addr->addr, self_addr.addr, 6) == 0) {
            ESP_LOGI(TAG, "We are the root node - starting web services");
            handle_root_transition(true);
        }
        break;
    }
    case MESH_EVENT_VOTE_STARTED:
        ESP_LOGI(TAG, "ROOT_VOTE_STARTED");
        break;
    case MESH_EVENT_VOTE_STOPPED:
        ESP_LOGI(TAG, "ROOT_VOTE_STOPPED");
        break;
    case MESH_EVENT_ROOT_SWITCH_REQ:
        ESP_LOGI(TAG, "ROOT_SWITCH_REQ - preparing for potential root change");
        break;
    case MESH_EVENT_ROOT_SWITCH_ACK:
        ESP_LOGI(TAG, "ROOT_SWITCH_ACK - checking if we are new root");
        // Small delay to ensure mesh state is updated
        vTaskDelay(pdMS_TO_TICKS(100));
        handle_root_transition(esp_mesh_is_root());
        break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        int new_sz = esp_mesh_get_routing_table_size();
        ESP_LOGI(TAG, "ROUTING_TABLE_ADD, size=%d", new_sz);
        break;
    }
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        int new_sz = esp_mesh_get_routing_table_size();
        ESP_LOGI(TAG, "ROUTING_TABLE_REMOVE, size=%d", new_sz);
        break;
    }
    case MESH_EVENT_NO_PARENT_FOUND:
        if (event_count % 50 == 1) { // Only log every 50th occurrence to reduce spam
            ESP_LOGW(TAG, "NO_PARENT_FOUND - scanning for mesh network... (count: %lu)", event_count);
        }
        break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)data;
        ESP_LOGI(TAG, "LAYER_CHANGE, new_layer=%d", layer_change->new_layer);
        break;
    }
    default:
        if (event_count % 100 == 1) { // Only log unknown events occasionally
            ESP_LOGW(TAG, "Unknown mesh event: %ld (count: %lu)", id, event_count);
        }
        break;
    }
}

static void ip_check_task(void *arg) {
    int retry_count = 0;
    const int max_retries = 60; // Try for 60 seconds
    bool dhcp_kicked = false;
    
    ESP_LOGI(TAG, "Starting IP check task - waiting for DHCP assignment");
    log_all_netifs("ip_check_task start");
    
    while (retry_count < max_retries && is_root_node) {
        // On first pass, attempt to start DHCP on all netifs (harmless if already running)
        if (!dhcp_kicked) {
            try_start_dhcp_on_all();
            dhcp_kicked = true;
        }

        // Check all netifs for an assigned IP
        for (esp_netif_t *n = esp_netif_next_unsafe(NULL); n != NULL; n = esp_netif_next_unsafe(n)) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(n, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
                const char *ifkey = esp_netif_get_ifkey(n);
                ESP_LOGI(TAG, "IP assigned on [%s]! Address: " IPSTR, ifkey ? ifkey : "(null)", IP2STR(&ip_info.ip));
                ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
                ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
                ESP_LOGI(TAG, "Device should now be accessible at http://mesh-controller.local");
                ESP_LOGI(TAG, "Or directly at http://" IPSTR, IP2STR(&ip_info.ip));
                ip_check_active = false;
                vTaskDelete(NULL);
                return;
            }
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS); // Wait 1 second
        retry_count++;
        
        if (retry_count % 10 == 0) {
            ESP_LOGI(TAG, "Still waiting for IP assignment... (%d/%d)", retry_count, max_retries);
            log_all_netifs("waiting");
        }
    }
    
    if (retry_count >= max_retries) {
        ESP_LOGW(TAG, "Timeout waiting for IP assignment after %d seconds", max_retries);
    }
    
    ip_check_active = false;
    vTaskDelete(NULL);
}

static void log_all_netifs(const char *reason) {
    ESP_LOGI(TAG, "Netif scan (%s):", reason ? reason : "-");
    for (esp_netif_t *n = esp_netif_next_unsafe(NULL); n != NULL; n = esp_netif_next_unsafe(n)) {
        const char *ifkey = esp_netif_get_ifkey(n);
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        esp_netif_get_ip_info(n, &ip_info);
        ESP_LOGI(TAG, "  netif=%p ifkey=%s IP=" IPSTR " GW=" IPSTR,
                 (void*)n, ifkey ? ifkey : "(null)", IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
    }
}

static void try_start_dhcp_on_all(void) {
    for (esp_netif_t *n = esp_netif_next_unsafe(NULL); n != NULL; n = esp_netif_next_unsafe(n)) {
        const char *ifkey = esp_netif_get_ifkey(n);
        esp_err_t err = esp_netif_dhcpc_start(n);
        ESP_LOGI(TAG, "DHCP start on [%s]: %s", ifkey ? ifkey : "(null)", esp_err_to_name(err));
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "*** IP ASSIGNED! *** Address: " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
            ESP_LOGI(TAG, "=== DEVICE NOW ACCESSIBLE ===");
            ESP_LOGI(TAG, "mDNS: http://mesh-controller.local");
            ESP_LOGI(TAG, "Direct: http://" IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "=============================");
            
            // Stop the IP check task since we now have an IP
            ip_check_active = false;
            break;
        }
        case IP_EVENT_STA_LOST_IP:
            ESP_LOGI(TAG, "Lost IP address");
            break;
        default:
            break;
    }
}

static void rx_task(void *arg) {
    while (true) {
        mesh_addr_t from = {0};
        mesh_data_t data = {.data = rx_buf, .size = RX_BUF_SZ, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P};
        int flag = 0;
        mesh_opt_t opt[1] = {0};

        if (esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, opt, 1) == ESP_OK) {
            ESP_LOGI(TAG, "RX from %02x:%02x:%02x:%02x:%02x:%02x (%d bytes): %.*s",
                     from.addr[0], from.addr[1], from.addr[2], from.addr[3], from.addr[4], from.addr[5],
                     data.size, data.size, (char*)data.data);

            // Add sender to node registry
            add_or_update_node(&from, -1); // Layer will be updated by mesh events
            
            // Parse simple JSON commands using string matching
            char *msg = (char*)data.data;
            
            // Ensure null termination
            char msg_copy[256];
            int copy_len = (data.size < sizeof(msg_copy) - 1) ? data.size : sizeof(msg_copy) - 1;
            memcpy(msg_copy, msg, copy_len);
            msg_copy[copy_len] = '\0';
            
            if (strstr(msg_copy, "\"cmd\":\"toggle\"") || strstr(msg_copy, "\"cmd\":\"led_toggle\"")) {
                // Check if this command is for us
                char *target_mac_start = strstr(msg_copy, "\"target_mac\":\"");
                if (target_mac_start) {
                    target_mac_start += 14; // Skip past "target_mac":"
                    char target_mac[18];
                    strncpy(target_mac, target_mac_start, 17);
                    target_mac[17] = '\0';
                    
                    uint8_t self_addr[6];
                    esp_wifi_get_mac(WIFI_IF_STA, self_addr);
                    char self_mac[18];
                    snprintf(self_mac, sizeof(self_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                             self_addr[0], self_addr[1], self_addr[2],
                             self_addr[3], self_addr[4], self_addr[5]);
                    
                    if (strncmp(target_mac, self_mac, 17) == 0) {
                        led_toggle();
                        
                        // Send status response to root
            // Capture our current RSSI to parent/router
            int my_rssi = -127;
            wifi_ap_record_t aprec = {0};
            if (esp_wifi_sta_get_ap_info(&aprec) == ESP_OK) {
                my_rssi = aprec.rssi;
            }
            char resp_str[160];
            snprintf(resp_str, sizeof(resp_str),
                "{\"cmd\":\"status_response\",\"mac\":\"%s\",\"led_state\":%s,\"layer\":%d,\"rssi\":%d}",
                self_mac, led_state ? "true" : "false", esp_mesh_get_layer(), my_rssi);
                        
                        mesh_data_t resp_data = {
                            .data = (uint8_t*)resp_str,
                            .size = strlen(resp_str),
                            .proto = MESH_PROTO_BIN,
                            .tos = MESH_TOS_P2P
                        };
                        esp_mesh_send(&from, &resp_data, MESH_DATA_P2P, NULL, 0);
                    }
                } else {
                    // Legacy toggle command
                    led_toggle();
                }
            } else if (strstr(msg_copy, "\"cmd\":\"status_request\"")) {
                // Send our status back
                uint8_t self_addr[6];
                esp_wifi_get_mac(WIFI_IF_STA, self_addr);
                char self_mac[18];
                snprintf(self_mac, sizeof(self_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         self_addr[0], self_addr[1], self_addr[2],
                         self_addr[3], self_addr[4], self_addr[5]);
                
        int my_rssi = -127;
        wifi_ap_record_t aprec = {0};
        if (esp_wifi_sta_get_ap_info(&aprec) == ESP_OK) {
            my_rssi = aprec.rssi;
        }
        char resp_str[160];
        snprintf(resp_str, sizeof(resp_str),
            "{\"cmd\":\"status_response\",\"mac\":\"%s\",\"led_state\":%s,\"layer\":%d,\"rssi\":%d}",
            self_mac, led_state ? "true" : "false", esp_mesh_get_layer(), my_rssi);
                
                mesh_data_t resp_data = {
                    .data = (uint8_t*)resp_str,
                    .size = strlen(resp_str),
                    .proto = MESH_PROTO_BIN,
                    .tos = MESH_TOS_P2P
                };
                esp_mesh_send(&from, &resp_data, MESH_DATA_P2P, NULL, 0);
            } else if (strstr(msg_copy, "\"cmd\":\"status_response\"")) {
                // Parse MAC address from response
                char *mac_start = strstr(msg_copy, "\"mac\":\"");
                if (mac_start) {
                    mac_start += 7; // Skip past "mac":"
                    char resp_mac[18];
                    strncpy(resp_mac, mac_start, 17);
                    resp_mac[17] = '\0';
                    // Parse layer if present
                    int resp_layer = -1;
                    char *layer_ptr = strstr(msg_copy, "\"layer\":");
                    if (layer_ptr) {
                        resp_layer = atoi(layer_ptr + 8);
                    }
                    // Update/add this node using its WiFi MAC
                    uint8_t mac_bytes[6];
                    if (parse_mac_str(resp_mac, mac_bytes)) {
                        mesh_addr_t node_addr = {0};
                        memcpy(node_addr.addr, mac_bytes, 6);
                        add_or_update_node(&node_addr, resp_layer > 0 ? resp_layer : esp_mesh_get_layer());
                        // Record route hint from source
                        for (int i = 0; i < node_count; i++) {
                            if (memcmp(known_nodes[i].addr.addr, node_addr.addr, 6) == 0) {
                                known_nodes[i].last_from = from;
                                known_nodes[i].has_route = true;
                                break;
                            }
                        }
                    }
                    
                    // Parse LED state
                    bool resp_led_state = strstr(msg_copy, "\"led_state\":true") != NULL;
                    // Parse RSSI if present
                    int resp_rssi = -127;
                    char *rssi_ptr = strstr(msg_copy, "\"rssi\":");
                    if (rssi_ptr) {
                        resp_rssi = atoi(rssi_ptr + 7);
                    }
                    
                    // Update node registry
                    for (int i = 0; i < node_count; i++) {
                        char node_mac[18];
                        snprintf(node_mac, sizeof(node_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                                 known_nodes[i].addr.addr[0], known_nodes[i].addr.addr[1], known_nodes[i].addr.addr[2],
                                 known_nodes[i].addr.addr[3], known_nodes[i].addr.addr[4], known_nodes[i].addr.addr[5]);
                        
                        if (strcmp(node_mac, resp_mac) == 0) {
                            known_nodes[i].led_state = resp_led_state;
                            known_nodes[i].rssi = resp_rssi;
                            // don't break: update all duplicates if any
                        }
                    }
                }
            } else if (strstr(msg_copy, "\"cmd\":\"heartbeat\"")) {
                // Use heartbeat for discovery: update the node's presence and layer; record route; update LED state if provided
                char *mac_start = strstr(msg_copy, "\"mac\":\"");
                if (mac_start) {
                    mac_start += 7;
                    char hb_mac[18];
                    strncpy(hb_mac, mac_start, 17);
                    hb_mac[17] = '\0';
                    // Parse layer if present
                    int hb_layer = -1;
                    char *layer_ptr = strstr(msg_copy, "\"layer\":");
                    if (layer_ptr) {
                        hb_layer = atoi(layer_ptr + 8);
                    }
                    bool hb_led_state = strstr(msg_copy, "\"led_state\":true") != NULL;
                    int hb_rssi = -127;
                    char *rssi_ptr = strstr(msg_copy, "\"rssi\":");
                    if (rssi_ptr) {
                        hb_rssi = atoi(rssi_ptr + 7);
                    }
                    uint8_t mac_bytes[6];
                    if (parse_mac_str(hb_mac, mac_bytes)) {
                        mesh_addr_t node_addr = {0};
                        memcpy(node_addr.addr, mac_bytes, 6);
                        add_or_update_node(&node_addr, hb_layer > 0 ? hb_layer : esp_mesh_get_layer());
                        // Record route hint from source and update LED if we track this node
                        for (int i = 0; i < node_count; i++) {
                            if (memcmp(known_nodes[i].addr.addr, node_addr.addr, 6) == 0) {
                                known_nodes[i].last_from = from;
                                known_nodes[i].has_route = true;
                                known_nodes[i].led_state = hb_led_state;
                                known_nodes[i].rssi = hb_rssi;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

static void status_task(void *arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Every 10 seconds
        bool is_connected = esp_mesh_is_device_active();
        int layer = esp_mesh_get_layer();
        int table_size = esp_mesh_get_routing_table_size();
        
        ESP_LOGI(TAG, "STATUS: connected=%s, layer=%d, routing_table_size=%d", 
                 is_connected ? "YES" : "NO", layer, table_size);
        
        // Check IP address if we're root
        if (is_root_node && layer == 1) {
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (!netif) {
                netif = esp_netif_get_default_netif();
            }
            
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    if (ip_info.ip.addr != 0) {
                        ESP_LOGI(TAG, "Root IP: " IPSTR, IP2STR(&ip_info.ip));
                    } else {
                        ESP_LOGW(TAG, "Root node has no IP address assigned");
                    }
                }
            }
        }
        
        if (!is_connected && layer == 0) {
            ESP_LOGW(TAG, "Device not connected to mesh - check if root node is running with matching MESH_ID");
        }
    }
}

static void start_mesh(void) {
    // Wi-Fi & netif init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default MESH netifs: STA (upstream to router) and AP (downstream to children)
    ESP_LOGI(TAG, "Creating default mesh netifs (STA/AP)");
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_mesh_netifs(&mesh_netif_sta, &mesh_netif_ap));
    if (mesh_netif_sta == NULL || mesh_netif_ap == NULL) {
        ESP_LOGW(TAG, "Mesh netif creation returned NULL handles (sta=%p ap=%p)", (void*)mesh_netif_sta, (void*)mesh_netif_ap);
    } else {
        ESP_LOGI(TAG, "Mesh netifs created: STA=%p AP=%p", (void*)mesh_netif_sta, (void*)mesh_netif_ap);
    }

    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wicfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Mesh init
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

    // Use fully self-organized mesh with automatic root election
    ESP_LOGI(TAG, "Enabling automatic root election (self-organized mesh)");
    // Don't set explicit type - let the mesh auto-elect root
    // Enable full self-organization (healing AND root election)
    ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));

    // Mesh config
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    memcpy((uint8_t *)&cfg.mesh_id, MESH_ID, sizeof(cfg.mesh_id));

    // Backhaul (router) creds - ALL nodes need this for potential root election
    cfg.router.ssid_len = strlen(ROUTER_SSID);
    memcpy(cfg.router.ssid, ROUTER_SSID, cfg.router.ssid_len);
    memcpy(cfg.router.password, ROUTER_PASS, strlen(ROUTER_PASS));

    // SoftAP config for downstream children  
    cfg.mesh_ap.max_connection = 6;
    strcpy((char *)cfg.mesh_ap.password, "meshpassword"); // children auth
    
    // Apply configuration
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));

    // Start mesh (topology and IP behavior use defaults from config and self-organization)
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(TAG, "Mesh started, waiting for links...");
    xTaskCreate(rx_task, "rx_task", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status_task", 4096, NULL, 3, NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting mesh demo with dynamic root election");
    // Tame noisy logs from lower layers to make troubleshooting easier during self-heal
    esp_log_level_set("mesh", ESP_LOGW);
    esp_log_level_set("wifi", ESP_LOGW);
    esp_log_level_set("net80211", ESP_LOGW);
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // Initialize LED
    led_init();
    
    start_mesh();

    // Wait longer before sending test message to allow mesh to stabilize
    vTaskDelay(pdMS_TO_TICKS(15000));
    
    // Send periodic status announcements if we're connected to mesh
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Every 30 seconds
        
        // Periodic root status check (in case we missed the initial detection)
        static int check_count = 0;
        check_count++;
        if (check_count <= 3) { // Check first 3 times (first 90 seconds)
            bool current_root_status = esp_mesh_is_root();
            ESP_LOGI(TAG, "Periodic root check #%d: is_root=%s, web_active=%s", 
                     check_count, current_root_status ? "YES" : "NO", is_root_node ? "YES" : "NO");
            
            if (current_root_status && !is_root_node) {
                ESP_LOGW(TAG, "Root status mismatch detected - fixing web server state");
                handle_root_transition(true);
            }
        }
        
        if (esp_mesh_is_device_active()) {
            uint8_t self_addr[6];
            esp_wifi_get_mac(WIFI_IF_STA, self_addr);
            char self_mac[18];
            snprintf(self_mac, sizeof(self_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     self_addr[0], self_addr[1], self_addr[2],
                     self_addr[3], self_addr[4], self_addr[5]);
            
        // Attach RSSI to heartbeat for link quality visualization
        int my_rssi = -127;
        wifi_ap_record_t aprec = {0};
        if (esp_wifi_sta_get_ap_info(&aprec) == ESP_OK) {
        my_rssi = aprec.rssi;
        }
        char ann_str[160];
        snprintf(ann_str, sizeof(ann_str),
            "{\"cmd\":\"heartbeat\",\"mac\":\"%s\",\"led_state\":%s,\"layer\":%d,\"rssi\":%d}",
            self_mac, led_state ? "true" : "false", esp_mesh_get_layer(), my_rssi);
            
            mesh_data_t d = {
                .data = (uint8_t*)ann_str,
                .size = strlen(ann_str),
                .proto = MESH_PROTO_BIN,
                .tos = MESH_TOS_P2P
            };
            mesh_addr_t bcast = {0};
            bcast.mip.port = MESH_DATA_P2P;
            memset(bcast.addr, 0xFF, 6);
            esp_err_t r = esp_mesh_send(&bcast, &d, MESH_DATA_P2P, NULL, 0);
            
            if (r == ESP_OK) {
                ESP_LOGD(TAG, "Sent heartbeat broadcast");
            }
        }
    }
}
