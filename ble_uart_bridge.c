/*
 * BLE UART Bridge for Flipper Zero
 *
 * Bridges BLE serial (phone) <-> GPIO UART (ESP32 Marauder).
 * Replaces the built-in UART Bridge which only bridges USB <-> UART.
 *
 * UART: USART1 (GPIO pins 13 TX / 14 RX), 115200 baud
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <furi_ble/profile_interface.h>
#include <bt/bt_service/bt.h>
#include <profiles/serial_profile.h>

#define BRIDGE_UART_ID FuriHalSerialIdUsart
#define BRIDGE_BAUD    115200
#define WORKER_STACK   2048
#define UART_BUF_SIZE  512

typedef struct {
    Bt* bt;
    FuriHalBleProfileBase* ble_profile;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* uart_rx_stream;
    FuriThread* worker_thread;
    Gui* gui;
    ViewPort* view_port;
    volatile bool running;
    volatile uint32_t rx_count;
    volatile uint32_t tx_count;
} BleUartBridge;

/* ---------- UART RX callback (ISR context) ---------- */

static void uart_rx_callback(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    BleUartBridge* app = context;

    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t byte = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(app->uart_rx_stream, &byte, 1, 0);
        }
    }
}

/* ---------- BLE RX callback (phone -> ESP32) ---------- */

static uint16_t ble_rx_callback(SerialServiceEvent event, void* context) {
    BleUartBridge* app = context;

    if(event.event == SerialServiceEventTypeDataReceived) {
        if(app->serial && event.data.size > 0) {
            furi_hal_serial_tx(app->serial, event.data.buffer, event.data.size);
            app->tx_count += event.data.size;
        }
    }

    return 0;
}

/* ---------- Worker thread: UART RX -> BLE TX ---------- */

static int32_t bridge_worker(void* context) {
    BleUartBridge* app = context;
    uint8_t buf[BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX];

    while(app->running) {
        size_t len = furi_stream_buffer_receive(
            app->uart_rx_stream, buf, sizeof(buf), 50);
        if(len > 0 && app->ble_profile) {
            ble_profile_serial_tx(app->ble_profile, buf, (uint16_t)len);
            app->rx_count += len;
        }
    }

    return 0;
}

/* ---------- GUI ---------- */

static void draw_callback(Canvas* canvas, void* context) {
    BleUartBridge* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "BLE UART Bridge");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 26, "USART1  115200 8N1");

    char buf[40];
    snprintf(buf, sizeof(buf), "ESP32 -> Phone: %lu B", (unsigned long)app->rx_count);
    canvas_draw_str(canvas, 2, 40, buf);
    snprintf(buf, sizeof(buf), "Phone -> ESP32: %lu B", (unsigned long)app->tx_count);
    canvas_draw_str(canvas, 2, 52, buf);

    canvas_draw_str(canvas, 2, 64, "Press BACK to exit");
}

static void input_callback(InputEvent* event, void* context) {
    BleUartBridge* app = context;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        app->running = false;
    }
}

/* ---------- App entry ---------- */

int32_t ble_uart_bridge_app(void* p) {
    UNUSED(p);
    BleUartBridge* app = malloc(sizeof(BleUartBridge));
    memset(app, 0, sizeof(BleUartBridge));
    app->running = true;

    // --- GUI ---
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // --- BLE: switch to serial profile ---
    app->bt = furi_record_open(RECORD_BT);
    app->ble_profile = bt_profile_start(app->bt, ble_profile_serial, NULL);
    if(!app->ble_profile) {
        app->running = false;
    }

    if(app->running) {
        ble_profile_serial_set_event_callback(
            app->ble_profile, UART_BUF_SIZE, ble_rx_callback, app);
    }

    // --- UART ---
    app->uart_rx_stream = furi_stream_buffer_alloc(UART_BUF_SIZE, 1);
    app->serial = furi_hal_serial_control_acquire(BRIDGE_UART_ID);
    if(app->serial) {
        furi_hal_serial_init(app->serial, BRIDGE_BAUD);
        furi_hal_serial_async_rx_start(app->serial, uart_rx_callback, app, false);
    } else {
        app->running = false;
    }

    // --- Worker thread ---
    app->worker_thread = furi_thread_alloc_ex("BleUartWorker", WORKER_STACK, bridge_worker, app);
    furi_thread_start(app->worker_thread);

    // --- Main loop ---
    while(app->running) {
        view_port_update(app->view_port);
        furi_delay_ms(200);
    }

    // --- Cleanup ---
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    if(app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    furi_stream_buffer_free(app->uart_rx_stream);

    if(app->ble_profile) {
        bt_profile_restore_default(app->bt);
    }
    furi_record_close(RECORD_BT);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    free(app);
    return 0;
}
