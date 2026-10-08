// BLE-MIDI peripheral (NimBLE), host + controller pinned to core 0 so the radio never
// preempts the USB/audio/CV work on core 1. Control-rate only: feeds the same router as USB-MIDI.
#include <string.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "app.h"
#include "ble_midi.h"

// 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 / 7772E5DB-3868-4112-A1A9-F2669D106BF3 (little-endian)
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
                                                        0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t chr_uuid = BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
                                                        0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);
static uint16_t chr_handle, conn = BLE_HS_CONN_HANDLE_NONE;
static uint8_t own_addr_type;
static blemidi_rx_t rx;

static void emit(void *u, const uint8_t *msg, int len, uint16_t ts) {
    (void)u; (void)ts;
    midi_router_rx(TRANSPORT_BLE, msg, len);
}

static int chr_access(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)ch; (void)ah; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) == 0) blemidi_rx_packet(&rx, buf, n, emit, NULL);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;   // spec: read returns no payload
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &chr_uuid.u, .access_cb = chr_access, .val_handle = &chr_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY},
         {0}}},
    {0},
};

static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) { conn = ev->connect.conn_handle; g_app.ble_connected = true; }
        else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE;
        g_app.ble_connected = false;
        app_lock(); mcv_all_off(&g_app.mapper); app_unlock();   // transport lost: gates low
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    const char *name = ble_svc_gap_device_name();
    r.name = (const uint8_t *)name;
    r.name_len = (uint8_t)strlen(name);
    r.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = 0x20, .itvl_max = 0x40};
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &own_addr_type);
    advertise();
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_midi_start(void) {
    blemidi_rx_init(&rx);
    if (nimble_port_init() != ESP_OK) return;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
    ble_svc_gap_device_name_set("CS CV Interface");
    nimble_port_freertos_init(host_task);   // core per CONFIG_BT_NIMBLE_PINNED_TO_CORE (0)
}

static void notify_pkt(void *u, const uint8_t *p, size_t n) {
    (void)u;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
    if (om) ble_gatts_notify_custom(conn, chr_handle, om);
}

void ble_midi_send(const uint8_t *msg, size_t n) {
    if (conn == BLE_HS_CONN_HANDLE_NONE || !n) return;
    uint16_t mtu = ble_att_mtu(conn);
    size_t payload = mtu > 3 ? (size_t)mtu - 3 : 20;
    blemidi_encode(msg, n, (uint16_t)(app_ms() & 0x1FFF), payload, notify_pkt, NULL);
}
