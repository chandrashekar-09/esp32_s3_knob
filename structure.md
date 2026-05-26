┌────────────────────────────────────────────────────────┐
│             LAYER 4: FreeRTOS Mutex Coordination       │
├───────────────────────────┬────────────────────────────┤
│    CORE 0 (Networking)    │      CORE 1 (User Space)   │
├───────────────────────────┼────────────────────────────┤
│ • ESP-WIFI-MESH Stack     │ • LVGL Graphics Engine     │
│ • Mesh Event Loop         │ • Hardware PCNT Driver     │
│ • TX / RX Packet Queues   │ • CST816S Touch Polling    │
└───────────────────────────┴────────────────────────────┘

1. Multi-Core Separation

    Core 0: Dedicate exclusively to the background mesh routing loops and LwIP TCP/IP processing.

    Core 1: Dedicate to your existing components (LVGL, Touch scanning, and the mechanical rotary encoder pulse processing).

2. Mesh-Safe Dynamic Allocations

ESP-WIFI-MESH demands a significant chunk of dynamic internal SRAM to cache the structural routing tables of adjacent nodes. Since you are building on a 16MB Flash / 8MB PSRAM architecture (n16r8), you must force your LVGL drawing canvas buffers into the external PSRAM (MALLOC_CAP_SPIRAM), freeing up the limited internal SRAM exclusively for the mesh network.
3. Decouple via Thread-Safe Struct Buffers

Never let your mesh network task write values directly to UI labels. Instead, define a static data structures hub (mesh_payload_t). Let Core 0 modify this raw payload struct upon receiving a valid packet, and shield it using a FreeRTOS SemaphoreHandle_t. Core 1 can then pull data from this safe struct at its own refresh rate.