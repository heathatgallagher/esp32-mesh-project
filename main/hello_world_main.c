#include <string.h>
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mesh.h"
#include "esp_mesh_internal.h"
#include "esp_netif.h"
#include "esp_mac.h"

static const char *TAG = "MESH_UNIFIED";

// --- Demo config (later: load these from NVS/captive portal) ---
static const uint8_t MESH_ID[6] = { 0x11,0x22,0x33,0x44,0x55,0x66 };  // same for all nodes
static const char *ROUTER_SSID   = "IsolationSwitchWiFi";
static const char *ROUTER_PASS   = "Cutoutswitch1";
// Flip this at build time for your initial tests: true = gateway/root; false = regular node
static const bool   FORCE_ROOT   = false;

// Simple RX buffer
#define RX_BUF_SZ 256
static uint8_t rx_buf[RX_BUF_SZ];

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
        break;
    }
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *conn = (mesh_event_connected_t *)data;
        ESP_LOGI(TAG, "PARENT_CONNECTED, layer=%d, parent=%02x:%02x:%02x:%02x:%02x:%02x",
                 esp_mesh_get_layer(), 
                 conn->connected.bssid[0], conn->connected.bssid[1], conn->connected.bssid[2],
                 conn->connected.bssid[3], conn->connected.bssid[4], conn->connected.bssid[5]);
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
        break;
    }
    case MESH_EVENT_VOTE_STARTED:
        ESP_LOGI(TAG, "ROOT_VOTE_STARTED");
        break;
    case MESH_EVENT_VOTE_STOPPED:
        ESP_LOGI(TAG, "ROOT_VOTE_STOPPED");
        break;
    case MESH_EVENT_ROOT_SWITCH_REQ:
        ESP_LOGI(TAG, "ROOT_SWITCH_REQ");
        break;
    case MESH_EVENT_ROOT_SWITCH_ACK:
        ESP_LOGI(TAG, "ROOT_SWITCH_ACK");
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

            // tiny command: {"cmd":"toggle"}
            if (data.size >= 17 && strstr((char*)data.data, "\"toggle\"")) {
                // TODO: toggle your GPIO here
                ESP_LOGI(TAG, "Toggling local output (stub)");
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
        
        if (!is_connected && layer == 0) {
            ESP_LOGW(TAG, "Device not connected to mesh - check if root node is running with matching MESH_ID");
        }
    }
}

static void start_mesh(void) {
    // Wi-Fi init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wicfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Mesh init
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));

    // For self-organized mesh, we DON'T set explicit type - let it auto-elect
    if (FORCE_ROOT) {
        ESP_LOGI(TAG, "Forcing this device to be ROOT");
        ESP_ERROR_CHECK(esp_mesh_set_type(MESH_ROOT));
        // Only use self-organization for automatic healing, not root selection
        ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
    } else {
        ESP_LOGI(TAG, "Allowing automatic root election (self-organized mesh)");
        // Don't set explicit type - let the mesh auto-elect root
        // Enable full self-organization (healing AND root election)
        ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, true));
    }

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

    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(TAG, "Mesh started, waiting for links...");
    xTaskCreate(rx_task, "rx_task", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status_task", 4096, NULL, 3, NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting mesh demo - FORCE_ROOT=%s", FORCE_ROOT ? "true" : "false");
    ESP_ERROR_CHECK(nvs_flash_init());
    start_mesh();

    // Wait longer before sending test message to allow mesh to stabilize
    vTaskDelay(pdMS_TO_TICKS(15000));
    
    // Only send broadcast if we're connected to mesh
    if (esp_mesh_is_device_active()) {
        ESP_LOGI(TAG, "Sending test broadcast message...");
        mesh_data_t d = { .data = (uint8_t*)"hello-mesh", .size = 10, .proto = MESH_PROTO_BIN, .tos = MESH_TOS_P2P };
        mesh_addr_t bcast = {0};
        bcast.mip.port = MESH_DATA_P2P;
        memset(bcast.addr, 0xFF, 6); // broadcast to descendants (simple demo)
        esp_err_t r = esp_mesh_send(&bcast, &d, MESH_DATA_P2P, NULL, 0);
        ESP_LOGI(TAG, "esp_mesh_send(broadcast) -> %s", esp_err_to_name(r));
    } else {
        ESP_LOGW(TAG, "Device not connected to mesh, skipping test broadcast");
    }
}
