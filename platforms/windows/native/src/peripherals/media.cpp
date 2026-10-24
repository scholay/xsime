#include "media.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
namespace rimes::windows::peripherals {
using Microsoft::WRL::ComPtr;
namespace {
constexpr const char* kFormats[] = {"CF_UNICODETEXT", "HTML Format", "Rich Text Format",
                                   "PNG", "CF_DIB", "CF_DIBV5"};
std::string BitmapFile(const std::string& dib) {
  if (dib.size() < sizeof(BITMAPINFOHEADER)) return {};
  BITMAPINFOHEADER h{};
  memcpy(&h, dib.data(), sizeof(h));
  if (h.biSize < sizeof(h) || h.biSize > dib.size() || h.biWidth <= 0 ||
      h.biWidth > 16384 || !h.biHeight || h.biHeight < -16384 ||
      h.biHeight > 16384 || h.biPlanes != 1 ||
      !(h.biBitCount == 1 || h.biBitCount == 4 || h.biBitCount == 8 ||
        h.biBitCount == 16 || h.biBitCount == 24 || h.biBitCount == 32) ||
      !(h.biCompression == BI_RGB || h.biCompression == BI_BITFIELDS)) return {};
  const std::uint64_t colors = h.biClrUsed ? h.biClrUsed
      : h.biBitCount <= 8 ? (1U << h.biBitCount) : 0;
  const auto offset = h.biSize + colors * sizeof(RGBQUAD) +
      (h.biCompression == BI_BITFIELDS && h.biSize == 40 ? 12 : 0);
  const auto stride = ((static_cast<std::uint64_t>(h.biWidth) * h.biBitCount + 31) / 32) * 4;
  if (offset > dib.size() || stride * static_cast<std::uint64_t>(std::abs(h.biHeight)) >
      dib.size() - offset) return {};
  BITMAPFILEHEADER file{};
  file.bfType = 0x4d42;
  file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
  file.bfOffBits = static_cast<DWORD>(sizeof(file) + offset);
  return std::string(reinterpret_cast<const char*>(&file), sizeof(file)) + dib;
}
std::vector<std::filesystem::path> DropPaths(const std::string& bytes) {
  if (bytes.size() < sizeof(DROPFILES)) throw std::runtime_error("剪贴板文件列表损坏。");
  DROPFILES drop{};
  memcpy(&drop, bytes.data(), sizeof(drop));
  if (drop.pFiles < sizeof(drop) || drop.pFiles >= bytes.size())
    throw std::runtime_error("剪贴板文件列表损坏。");
  std::wstring names;
  const auto size = bytes.size() - drop.pFiles;
  if (drop.fWide) {
    if (size % sizeof(wchar_t)) throw std::runtime_error("剪贴板文件列表损坏。");
    names.resize(size / sizeof(wchar_t));
    memcpy(names.data(), bytes.data() + drop.pFiles, size);
  } else {
    const auto count = MultiByteToWideChar(CP_ACP, 0, bytes.data() + drop.pFiles,
                                          static_cast<int>(size), nullptr, 0);
    if (count <= 0) throw std::runtime_error("剪贴板文件列表损坏。");
    names.resize(count);
    MultiByteToWideChar(CP_ACP, 0, bytes.data() + drop.pFiles,
                        static_cast<int>(size), names.data(), count);
  }
  std::vector<std::filesystem::path> paths;
  std::size_t offset = 0;
  while (offset < names.size() && names[offset]) {
    const auto end = names.find(L'\0', offset);
    if (end == names.npos || paths.size() == kClipboardFileLimit)
      throw std::runtime_error("剪贴板文件列表损坏或超过 16 项。");
    std::filesystem::path path(names.substr(offset, end - offset));
    if (!path.is_absolute() || path.native().size() >= 32768)
      throw std::runtime_error("剪贴板文件路径无效。");
    paths.push_back(std::move(path));
    offset = end + 1;
  }
  if (offset >= names.size()) throw std::runtime_error("剪贴板文件列表缺少结束标记。");
  return paths;
}
std::string UnicodeText(const std::string& bytes) {
  if (bytes.empty() || bytes.size() % sizeof(wchar_t))
    throw std::runtime_error("剪贴板文字格式无效。");
  std::wstring value(bytes.size() / sizeof(wchar_t), L'\0');
  memcpy(value.data(), bytes.data(), bytes.size());
  const auto end = value.find(L'\0');
  if (end == value.npos) throw std::runtime_error("剪贴板文字缺少结束标记。");
  value.resize(end);
  return workbench::Utf8(value);
}
}  // namespace
UINT ClipboardFormatId(const std::string& name) {
  if (name == "CF_UNICODETEXT") return CF_UNICODETEXT;
  if (name == "CF_DIB") return CF_DIB;
  if (name == "CF_DIBV5") return CF_DIBV5;
  if (name == "CF_HDROP") return CF_HDROP;
  if (name == "HTML Format" || name == "Rich Text Format" || name == "PNG")
    return RegisterClipboardFormatW(workbench::Wide(name).c_str());
  return 0;
}
void ValidateClipboardFormats(const std::vector<ClipboardFormat>& formats) {
  if (formats.empty() || formats.size() > std::size(kFormats) + 1)
    throw std::runtime_error("剪贴板归档格式数量无效。");
  std::size_t size = 0;
  std::set<std::string> names;
  for (const auto& format : formats) {
    if (!ClipboardFormatId(format.name) || format.bytes.empty() ||
        !names.insert(format.name).second || format.bytes.size() > kClipboardArchiveLimit - size)
      throw std::runtime_error("剪贴板归档格式无效或超过 64 MiB。");
    size += format.bytes.size();
    if (format.name == "CF_UNICODETEXT") (void)UnicodeText(format.bytes);
    if (format.name == "CF_HDROP") (void)DropPaths(format.bytes);
  }
}
std::string ClipboardFileDrop(const std::vector<std::filesystem::path>& paths) {
  if (paths.empty() || paths.size() > kClipboardFileLimit)
    throw std::runtime_error("剪贴板文件数量无效。");
  std::wstring names;
  for (const auto& path : paths) {
    if (!path.is_absolute() || path.native().size() >= 32768 ||
        path.native().find(L'\0') != std::wstring::npos)
      throw std::runtime_error("剪贴板文件路径无效。");
    names += path.native(); names += L'\0';
  }
  names += L'\0';
  DROPFILES drop{}; drop.pFiles = sizeof(drop); drop.fWide = TRUE;
  return std::string(reinterpret_cast<const char*>(&drop), sizeof(drop)) +
      std::string(reinterpret_cast<const char*>(names.data()), names.size() * sizeof(wchar_t));
}
bool InteractiveDesktop() {
  auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!desktop) return false;
  wchar_t name[128]{}; DWORD size = 0;
  const bool available = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) && _wcsicmp(name, L"Default") == 0;
  CloseDesktop(desktop); return available;
}
ClipboardContent ReadClipboard(HWND owner) {
  if (!OpenClipboard(owner)) return {};
  struct Close {
    ~Close() { CloseClipboard(); }
  } close;
  const auto format_id = [](const std::string& name) {
    const auto supported = ClipboardFormatId(name);
    return supported ? supported : RegisterClipboardFormatW(workbench::Wide(name).c_str());
  };
  ClipboardReadAccess access;
  access.available = [&](const auto& name) { return IsClipboardFormatAvailable(format_id(name)) != FALSE; };
  access.read = [&](const std::string& name, std::size_t limit) {
    std::string bytes;
    if (auto handle = GetClipboardData(format_id(name))) {
      const auto size = GlobalSize(handle);
      if (size > limit) throw std::runtime_error("剪贴板格式超过大小限制。");
      if (size)
        if (const auto* p = static_cast<const char*>(GlobalLock(handle))) {
          bytes.assign(p, size);
          GlobalUnlock(handle);
        }
    }
    return bytes;
  };
  return ReadClipboardSnapshot(access);
}
ClipboardContent ReadClipboardSnapshot(const ClipboardReadAccess& access) {
  struct Unavailable {};
  ClipboardContent result; result.opened = true;
  if (access.available("ExcludeClipboardContentFromMonitorProcessing")) return result;
  try {
  for (auto name :
       {"CanIncludeInClipboardHistory", "CanUploadToCloudClipboard"}) {
    if (!access.available(name)) continue;
    const auto policy = access.read(name, sizeof(DWORD));
    if (policy.empty()) throw Unavailable{};
    if (policy.size() != sizeof(DWORD) ||
        policy == std::string(sizeof(DWORD), '\0'))
      return result;
  }
  std::size_t total = 0;
  for (const auto* name : kFormats) {
    if (!access.available(name)) continue;
    auto bytes = access.read(name, std::string(name) == "CF_UNICODETEXT"
        ? 512 * 1024 : kClipboardArchiveLimit - total);
    if (bytes.empty()) throw Unavailable{};
    if (bytes.size() > kClipboardArchiveLimit - total)
      throw std::runtime_error("剪贴板归档过大。");
    total += bytes.size();
    if (std::string(name) == "CF_UNICODETEXT") result.text = UnicodeText(bytes);
    if (std::string(name) == "PNG") { result.bytes = bytes; result.extension = ".png"; }
    else if ((std::string(name) == "CF_DIB" || std::string(name) == "CF_DIBV5") && result.bytes.empty()) {
      result.bytes = BitmapFile(bytes);
      if (!result.bytes.empty()) result.extension = ".bmp";
    }
    result.formats.push_back({name, std::move(bytes)});
  }
  if (access.available("CF_HDROP")) {
    const auto files = access.read("CF_HDROP", 1024 * 1024);
    if (files.empty()) throw Unavailable{};
    result.files = DropPaths(files);
  }
  if (!result.formats.empty()) ValidateClipboardFormats(result.formats);
  } catch (const Unavailable&) {
    // Delayed rendering/temporary read failures retry the complete copy, never
    // admitting a partial snapshot or advancing the collection baseline.
    result = {};
  } catch (...) {
    // An oversize/malformed representation must not turn one copy into a
    // misleading partial card, and protected clipboard data stays unrecorded.
    result = {}; result.opened = true;
  }
  return result;
}
HBITMAP Thumbnail(const std::filesystem::path& path, unsigned maximum) {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)))
    return nullptr;
  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 32768 || h > 32768 ||
      static_cast<std::uint64_t>(w) * h > 128 * 1024 * 1024)
    return nullptr;
  const double ratio =
      (std::min)(1.0, static_cast<double>(maximum) / (std::max)(w, h));
  w = (std::max)(1U, static_cast<unsigned>(w * ratio));
  h = (std::max)(1U, static_cast<unsigned>(h * ratio));
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(frame.Get(), w, h,
                                WICBitmapInterpolationModeFant)) ||
      FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return nullptr;
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER),
                    static_cast<LONG>(w),
                    -static_cast<LONG>(h),
                    1,
                    32,
                    BI_RGB};
  void* pixels = nullptr;
  auto bitmap =
      CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (bitmap && FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4,
                                             static_cast<BYTE*>(pixels)))) {
    DeleteObject(bitmap);
    return nullptr;
  }
  return bitmap;
}
}  // namespace rimes::windows::peripherals
