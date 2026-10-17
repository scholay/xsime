// Test-only pointer for the isolated Sway region selector. Not installed.
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
struct Fixture {
    struct wl_seat* seat;
    struct zwlr_virtual_pointer_manager_v1* manager;
};
static void global(void* raw, struct wl_registry* registry, uint32_t id, const char* name,
                   uint32_t version) {
    (void)version;
    struct Fixture* self = raw;
    if (!strcmp(name, "wl_seat"))
        self->seat = wl_registry_bind(registry, id, &wl_seat_interface, 1);
    if (!strcmp(name, "zwlr_virtual_pointer_manager_v1"))
        self->manager =
            wl_registry_bind(registry, id, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void removed(void* raw, struct wl_registry* registry, uint32_t id) {
    (void)raw;
    (void)registry;
    (void)id;
}
int main(void) {
    if (!getenv("RIMES_PRIVATE_WAYLAND_CAPTURE_TEST"))
        return 2;
    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 1;
    struct Fixture state = {0};
    struct wl_registry* registry = wl_display_get_registry(display);
    const struct wl_registry_listener listener = {global, removed};
    wl_registry_add_listener(registry, &listener, &state);
    wl_display_roundtrip(display);
    if (!state.manager || !state.seat)
        return 1;
    struct zwlr_virtual_pointer_v1* pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(state.manager, state.seat);
    wl_display_roundtrip(display);
    usleep(200000);
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 1, 100, 80, 800, 600);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_button(pointer, 2, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_motion_absolute(pointer, 3, 500, 380, 800, 600);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_button(pointer, 4, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(pointer);
    wl_display_roundtrip(display);
    usleep(100000);
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwlr_virtual_pointer_manager_v1_destroy(state.manager);
    wl_seat_destroy(state.seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
