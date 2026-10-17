#pragma once
#include "capsule_history.hpp"
#include <algorithm>
#include <gtk/gtk.h>
#include <memory>
namespace rimes::capsule {
inline std::shared_ptr<GdkPixbuf> ClipboardThumbnail(const ClipboardArchive& archive,
                                                     int maximum = 512) {
    for (const auto& value : archive.representations) {
        if (value.type.rfind("image/", 0) != 0 || value.bytes.size() > 32 * 1024 * 1024)
            continue;
        auto* loader = gdk_pixbuf_loader_new();
        g_signal_connect(
            loader, "size-prepared",
            G_CALLBACK(+[](GdkPixbufLoader* loader, int width, int height, gpointer raw) {
                const auto bound = GPOINTER_TO_INT(raw);
                const double scale = std::min({1.0, static_cast<double>(bound) / std::max(1, width),
                                               static_cast<double>(bound) / std::max(1, height)});
                gdk_pixbuf_loader_set_size(loader, std::max(1, static_cast<int>(width * scale)),
                                           std::max(1, static_cast<int>(height * scale)));
            }),
            GINT_TO_POINTER(maximum));
        GdkPixbuf* image = nullptr;
        if (gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(value.bytes.data()),
                                    value.bytes.size(), nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr)) {
            image = gdk_pixbuf_loader_get_pixbuf(loader);
            if (image)
                g_object_ref(image);
        } else
            gdk_pixbuf_loader_close(loader, nullptr);
        g_object_unref(loader);
        if (image)
            return {image, [](GdkPixbuf* picture) { g_object_unref(picture); }};
    }
    return {};
}
} // namespace rimes::capsule
