// Disposable decoder. A malformed document cannot stall the IME or GTK loop.
#include "capsule_resources.hpp"
#include "capsule_rich_text.hpp"
#include "capsule_text.hpp"
#include <algorithm>
#include <cmath>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poppler.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace rimes::capsule;
namespace {
void Limit(int resource, rlim_t value) {
    const rlimit limit{value, value};
    if (setrlimit(resource, &limit))
        throw std::runtime_error("preview limit unavailable");
}
cairo_status_t Png(void* closure, const unsigned char* bytes, unsigned int size) {
    auto& output = *static_cast<std::string*>(closure);
    if (output.size() + size > 11 * 1024 * 1024)
        return CAIRO_STATUS_WRITE_ERROR;
    output.append(reinterpret_cast<const char*>(bytes), size);
    return CAIRO_STATUS_SUCCESS;
}
} // namespace
int main(int argc, char** argv) {
    try {
        Limit(RLIMIT_AS, 1024ULL * 1024 * 1024);
        Limit(RLIMIT_CPU, 5);
        Limit(RLIMIT_CORE, 0);
        Limit(RLIMIT_FSIZE, 16 * 1024 * 1024);
        if (argc != 2)
            return 2;
        const std::filesystem::path path(argv[1]);
        const auto ext = LowerText(path.extension().string());
        std::atomic<bool> cancelled{false};
        nlohmann::json value = {{"text", ""}, {"png", ""}, {"message", ""}, {"pages", 0}};
        std::string png;
        if (ext == ".pdf") {
            auto bytes = ReadResource(path, 256 * 1024 * 1024, cancelled);
            auto* data = g_bytes_new_static(bytes.data(), bytes.size());
            auto* document = poppler_document_new_from_bytes(data, nullptr, nullptr);
            g_bytes_unref(data);
            if (!document)
                throw std::runtime_error("PDF 无法预览，可能已加密或损坏");
            const auto pages = poppler_document_get_n_pages(document);
            auto* page = pages > 0 ? poppler_document_get_page(document, 0) : nullptr;
            if (!page) {
                g_object_unref(document);
                throw std::runtime_error("PDF 没有可预览页面");
            }
            double width = 0, height = 0;
            poppler_page_get_size(page, &width, &height);
            if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
                return 3;
            const auto scale = 1600.0 / std::max(width, height);
            auto* surface =
                cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(1, int(width * scale)),
                                           std::max(1, int(height * scale)));
            auto* cr = cairo_create(surface);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
            cairo_scale(cr, scale, scale);
            poppler_page_render(page, cr);
            const auto result = cairo_surface_write_to_png_stream(surface, Png, &png);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            g_object_unref(page);
            g_object_unref(document);
            if (result != CAIRO_STATUS_SUCCESS)
                throw std::runtime_error("PDF 预览超过限制");
            value["pages"] = pages;
            value["message"] = "第 1 页 / 共 " + std::to_string(pages) + " 页";
        } else if (ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".mkv") {
            const int fd = OpenResource(path);
            struct stat info {};
            if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 ||
                info.st_size > 1024LL * 1024 * 1024) {
                if (fd >= 0)
                    close(fd);
                throw std::runtime_error("视频不可用或超过 1 GiB");
            }
            gst_init(nullptr, nullptr);
            GError* error = nullptr;
            auto* pipeline =
                gst_parse_launch("uridecodebin name=decoder ! videoconvert ! videoscale ! "
                                 "video/x-raw,format=RGB,width=640,height=360,pixel-aspect-ratio=1/"
                                 "1 ! appsink name=frame max-buffers=1 drop=true sync=false",
                                 &error);
            if (error || !pipeline) {
                g_clear_error(&error);
                if (pipeline)
                    gst_object_unref(pipeline);
                close(fd);
                throw std::runtime_error("视频预览组件不可用");
            }
            auto* decoder = gst_bin_get_by_name(GST_BIN(pipeline), "decoder");
            g_object_set(decoder, "uri", ("file:///proc/self/fd/" + std::to_string(fd)).c_str(),
                         nullptr);
            gst_object_unref(decoder);
            auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "frame");
            gst_element_set_state(pipeline, GST_STATE_PAUSED);
            auto* sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 6 * GST_SECOND);
            if (sample) {
                GstVideoInfo video{};
                auto* buffer = gst_sample_get_buffer(sample);
                GstMapInfo map{};
                if (gst_video_info_from_caps(&video, gst_sample_get_caps(sample)) &&
                    gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    const bool valid =
                        video.width == 640 && video.height == 360 &&
                        GST_VIDEO_INFO_FORMAT(&video) == GST_VIDEO_FORMAT_RGB &&
                        video.stride[0] >= 640 * 3 &&
                        map.size >= static_cast<std::size_t>(video.stride[0]) * 359 + 640 * 3;
                    auto* picture =
                        valid
                            ? gdk_pixbuf_new_from_data(map.data, GDK_COLORSPACE_RGB, FALSE, 8, 640,
                                                       360, video.stride[0], nullptr, nullptr)
                            : nullptr;
                    char* bytes = nullptr;
                    gsize length = 0;
                    if (picture && gdk_pixbuf_save_to_buffer(picture, &bytes, &length, "png",
                                                             nullptr, nullptr))
                        png.assign(bytes, length);
                    g_free(bytes);
                    if (picture)
                        g_object_unref(picture);
                    gst_buffer_unmap(buffer, &map);
                }
                gst_sample_unref(sample);
            }
            gst_object_unref(sink);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            close(fd);
            if (png.empty())
                throw std::runtime_error("视频首帧无法预览");
            value["message"] = "视频首帧预览";
        } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
                   ext == ".webp" || ext == ".bmp" || ext == ".tif" || ext == ".tiff" ||
                   ext == ".heic") {
            const auto bytes = ReadResource(path, 32 * 1024 * 1024, cancelled);
            auto* loader = gdk_pixbuf_loader_new();
            g_signal_connect(
                loader, "size-prepared",
                G_CALLBACK(+[](GdkPixbufLoader* loader, int width, int height, gpointer) {
                    const auto scale =
                        std::min({1.0, 1600.0 / std::max(1, width), 1600.0 / std::max(1, height)});
                    gdk_pixbuf_loader_set_size(loader, std::max(1, int(width * scale)),
                                               std::max(1, int(height * scale)));
                }),
                nullptr);
            const bool wrote = gdk_pixbuf_loader_write(
                loader, reinterpret_cast<const guchar*>(bytes.data()), bytes.size(), nullptr);
            const bool closed = gdk_pixbuf_loader_close(loader, nullptr);
            auto* picture = wrote && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
            char* data = nullptr;
            gsize length = 0;
            const bool saved = picture && gdk_pixbuf_save_to_buffer(picture, &data, &length, "png",
                                                                    nullptr, nullptr);
            if (saved && length <= 11 * 1024 * 1024)
                png.assign(data, length);
            g_free(data);
            g_object_unref(loader);
            if (png.empty())
                throw std::runtime_error("图片格式不可用或超过预览限制");
            value["message"] = "图片预览";
        } else {
            const auto bytes = ReadResource(path, 1024 * 1024, cancelled);
            if (bytes.find('\0') != bytes.npos ||
                !g_utf8_validate(bytes.data(), bytes.size(), nullptr))
                throw std::runtime_error("此文件没有 UTF-8 文本预览；仍可复制文件");
            const auto text = CharacterPrefix(bytes, 16000);
            value["text"] = text;
            value["message"] =
                text.size() < bytes.size() ? "显示前 16000 字；文件内容未改动" : "文本预览";
        }
        value["png"] = Base64Encode(png);
        std::cout << value.dump();
        return 0;
    } catch (const std::exception& error) {
        std::cout << nlohmann::json{{"message", error.what()}}.dump();
        return 0;
    }
}
