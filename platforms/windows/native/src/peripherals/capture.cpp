#include "capture.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <cmath>
#include <future>
#include <atomic>

#include "appearance.hpp"
#include "../ui/layered_frame.hpp"

namespace rimes::windows::peripherals {
using Microsoft::WRL::ComPtr;
using workbench::Wide;
namespace {
void Require(bool ok) {
  if (!ok) throw std::runtime_error("图像处理失败，原图已保留。");
}
void Require(HRESULT hr) { Require(SUCCEEDED(hr)); }
ComPtr<IWICImagingFactory> Factory() {
  ComPtr<IWICImagingFactory> factory;
  Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  return factory;
}
HGLOBAL Memory(const void* bytes, std::size_t size) {
  auto handle = GlobalAlloc(GMEM_MOVEABLE, size);
  void* data = handle ? GlobalLock(handle) : nullptr;
  if (!data) {
    if (handle) GlobalFree(handle);
    return nullptr;
  }
  memcpy(data, bytes, size);
  GlobalUnlock(handle);
  return handle;
}
std::wstring Stamp() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  wchar_t text[64]{};
  swprintf_s(text, L"截图 %04u-%02u-%02u %02u-%02u-%02u", time.wYear,
             time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
  return text;
}
POINT Point(const Json& p) {
  Require(p.is_array() && p.size() == 2 && p[0].is_number_integer() &&
          p[1].is_number_integer());
  auto x = p[0].get<long long>(), y = p[1].get<long long>();
  Require(x >= 0 && y >= 0 && x <= 32768 && y <= 32768);
  return {static_cast<LONG>(x), static_cast<LONG>(y)};
}
void DrawOperation(Gdiplus::Graphics& g, const Json& op) {
  const auto tool = op.at("tool").get<std::string>();
  const auto& points = op.at("points");
  Require(points.is_array() && points.size() >= 2 && points.size() <= 20000);
  std::vector<Gdiplus::Point> path;
  for (const auto& p : points) {
    const auto q = Point(p);
    path.emplace_back(q.x, q.y);
  }
  const auto a = path.front(), b = path.back();
  const auto color = op.value("color", 0xffff4254U);
  const auto width = op.value("width", 4);
  Require(width >= 1 && width <= 64);
  Gdiplus::Pen pen(Gdiplus::Color(color), static_cast<float>(width));
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  const int left = (std::min)(a.X, b.X), top = (std::min)(a.Y, b.Y);
  const int w = std::abs(a.X - b.X), h = std::abs(a.Y - b.Y);
  if (tool == "pen")
    g.DrawLines(&pen, path.data(), static_cast<int>(path.size()));
  else if (tool == "rectangle")
    g.DrawRectangle(&pen, left, top, w, h);
  else if (tool == "arrow") {
    g.DrawLine(&pen, a, b);
    const auto angle = std::atan2(static_cast<double>(b.Y - a.Y),
                                  static_cast<double>(b.X - a.X));
    const auto length = (std::max)(12., width * 4.);
    for (const double offset : {-0.5, 0.5})
      g.DrawLine(&pen, b.X, b.Y,
                 b.X - static_cast<int>(std::cos(angle + offset) * length),
                 b.Y - static_cast<int>(std::sin(angle + offset) * length));
  } else if (tool == "redact") {
    // Solid, opaque pixels; copying/exporting can never expose covered pixels.
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 20, 20, 20));
    g.FillRectangle(&brush, left, top, w, h);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  } else if (tool != "crop")
    throw std::runtime_error("此编辑工具尚未实现。");
}
std::pair<CaptureImage, RECT> DesktopImage() {
  RECT rect{GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
  CaptureImage image;
  image.width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  image.height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  rect.right = rect.left + image.width;
  rect.bottom = rect.top + image.height;
  Require(image.width > 0 && image.height > 0 &&
          static_cast<std::uint64_t>(image.width) * image.height <= 32000000);
  BITMAPINFO info{};
  info.bmiHeader = {
      sizeof(BITMAPINFOHEADER), image.width, -image.height, 1, 32, BI_RGB};
  auto screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
  void* pixels = nullptr;
  auto bitmap =
      CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  bool ok = false;
  if (dc && bitmap) {
    auto old = SelectObject(dc, bitmap);
    ok = BitBlt(dc, 0, 0, image.width, image.height, screen, rect.left,
                rect.top, SRCCOPY | CAPTUREBLT) != FALSE;
    if (ok)
      image.pixels.assign(
          static_cast<std::uint32_t*>(pixels),
          static_cast<std::uint32_t*>(pixels) +
              static_cast<std::size_t>(image.width) * image.height);
    SelectObject(dc, old);
  }
  if (bitmap) DeleteObject(bitmap);
  if (dc) DeleteDC(dc);
  if (screen) ReleaseDC(nullptr, screen);
  Require(ok);
  for (auto& pixel : image.pixels) pixel |= 0xff000000U;
  return {std::move(image), rect};
}
void DrawImage(HDC dc, const CaptureImage& image, RECT rect) {
  if (!image.Valid()) return;
  Gdiplus::Bitmap bitmap(
      image.width, image.height, image.width * 4, PixelFormat32bppARGB,
      reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(image.pixels.data())));
  Gdiplus::Graphics g(dc);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.DrawImage(&bitmap,
              Gdiplus::Rect(rect.left, rect.top, rect.right - rect.left,
                            rect.bottom - rect.top));
}
RECT Fit(const CaptureImage& image, RECT box) {
  const double scale =
      (std::min)(static_cast<double>(box.right - box.left) / image.width,
                 static_cast<double>(box.bottom - box.top) / image.height);
  const LONG w = (std::max)(1L, static_cast<LONG>(image.width * scale)),
             h = (std::max)(1L, static_cast<LONG>(image.height * scale));
  const LONG x = box.left + (box.right - box.left - w) / 2,
             y = box.top + (box.bottom - box.top - h) / 2;
  return {x, y, x + w, y + h};
}
// Reuse the desktop and composition surfaces for the entire selection. Mouse
// movement must not allocate or resample a multi-monitor screenshot.
struct CaptureSurface {
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ old = nullptr;
  std::uint32_t* pixels = nullptr;
  ~CaptureSurface() { Reset(); }
  void Reset() {
    if (dc && old) SelectObject(dc, old);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    dc = nullptr; bitmap = nullptr; old = nullptr; pixels = nullptr;
  }
  void Create(int width, int height) {
    Reset();
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    dc = CreateCompatibleDC(nullptr);
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&pixels), nullptr, 0);
    Require(dc && bitmap && pixels);
    old = SelectObject(dc, bitmap);
  }
};
CaptureImage Thumbnail(const CaptureImage& image) {
  const auto r = Fit(image, {0, 0, 512, 364});
  CaptureImage preview;
  preview.width = r.right - r.left; preview.height = r.bottom - r.top;
  preview.pixels.resize(static_cast<std::size_t>(preview.width) * preview.height);
  Gdiplus::Bitmap source(image.width, image.height, image.width * 4,
      PixelFormat32bppARGB, reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(image.pixels.data())));
  Gdiplus::Bitmap target(preview.width, preview.height, preview.width * 4,
      PixelFormat32bppARGB, reinterpret_cast<BYTE*>(preview.pixels.data()));
  Gdiplus::Graphics graphics(&target);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.DrawImage(&source, 0, 0, preview.width, preview.height);
  return preview;
}
class CaptureDragSource final : public IDropSource {
 public:
  explicit CaptureDragSource(std::shared_ptr<std::atomic<bool>> cancelled)
      : cancelled_(std::move(cancelled)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropSource) return E_NOINTERFACE;
    *out = static_cast<IDropSource*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto refs = --references_; if (!refs) delete this; return refs;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape || cancelled_->load()) return DRAGDROP_S_CANCEL;
    return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
 private:
  ULONG references_ = 1;
  std::shared_ptr<std::atomic<bool>> cancelled_;
};
}  // namespace

bool CaptureImage::Valid() const {
  return width > 0 && height > 0 && width <= 32768 && height <= 32768 &&
         static_cast<std::uint64_t>(width) * height <= 32000000 &&
         pixels.size() == static_cast<std::size_t>(width) * height;
}
RECT CaptureSelection(POINT a, POINT b, int w, int h) {
  return {std::clamp((std::min)(a.x, b.x), 0L, static_cast<LONG>(w)),
          std::clamp((std::min)(a.y, b.y), 0L, static_cast<LONG>(h)),
          std::clamp((std::max)(a.x, b.x), 0L, static_cast<LONG>(w)),
          std::clamp((std::max)(a.y, b.y), 0L, static_cast<LONG>(h))};
}
CaptureImage CropCapture(const CaptureImage& input, RECT r) {
  Require(input.Valid());
  r = CaptureSelection({r.left, r.top}, {r.right, r.bottom}, input.width,
                       input.height);
  CaptureImage out;
  out.width = r.right - r.left;
  out.height = r.bottom - r.top;
  Require(out.width > 0 && out.height > 0);
  out.pixels.resize(static_cast<std::size_t>(out.width) * out.height);
  for (int y = 0; y < out.height; ++y)
    std::copy_n(input.pixels.data() +
                    static_cast<std::size_t>(y + r.top) * input.width + r.left,
                out.width,
                out.pixels.data() + static_cast<std::size_t>(y) * out.width);
  return out;
}
CaptureImage LoadCapture(const std::filesystem::path& path) {
  // ReadFile enforces the same private-path/reparse checks as Capsule storage.
  auto bytes = ReadFile(path, 64 * 1024 * 1024);
  auto factory = Factory();
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  Require(factory->CreateStream(&stream));
  Require(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()),
                                       static_cast<DWORD>(bytes.size())));
  Require(factory->CreateDecoderFromStream(
      stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
  Require(decoder->GetFrame(0, &frame));
  UINT w = 0, h = 0;
  Require(frame->GetSize(&w, &h));
  Require(w && h && w <= 32768 && h <= 32768 &&
          static_cast<std::uint64_t>(w) * h <= 32000000);
  CaptureImage image;
  image.width = w;
  image.height = h;
  image.pixels.resize(static_cast<std::size_t>(w) * h);
  Require(factory->CreateFormatConverter(&converter));
  Require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
  Require(converter->CopyPixels(nullptr, w * 4, w * h * 4,
                                reinterpret_cast<BYTE*>(image.pixels.data())));
  // Composite imported transparency over white to match the editor/export.
  for (auto& pixel : image.pixels) {
    const auto a = pixel >> 24;
    std::uint32_t out = 0xff000000U;
    for (unsigned shift : {0U, 8U, 16U})
      out |= ((((pixel >> shift) & 255) * a / 255 + 255 - a) << shift);
    pixel = out;
  }
  return image;
}
std::string EncodeCapture(const CaptureImage& image) {
  Require(image.Valid());
  auto factory = Factory();
  ComPtr<IStream> stream;
  Require(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  Require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
  Require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  Require(encoder->CreateNewFrame(&frame, nullptr));
  Require(frame->Initialize(nullptr));
  Require(frame->SetSize(image.width, image.height));
  auto format = GUID_WICPixelFormat32bppBGRA;
  Require(frame->SetPixelFormat(&format));
  Require(IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) != 0);
  Require(frame->WritePixels(
      image.height, image.width * 4, static_cast<UINT>(image.pixels.size() * 4),
      reinterpret_cast<BYTE*>(
          const_cast<std::uint32_t*>(image.pixels.data()))));
  Require(frame->Commit());
  Require(encoder->Commit());
  STATSTG stat{};
  Require(stream->Stat(&stat, STATFLAG_NONAME));
  Require(stat.cbSize.QuadPart <= 64 * 1024 * 1024);
  HGLOBAL memory = nullptr;
  Require(GetHGlobalFromStream(stream.Get(), &memory));
  const auto* bytes = static_cast<const char*>(GlobalLock(memory));
  Require(bytes != nullptr);
  std::string result(bytes, static_cast<std::size_t>(stat.cbSize.QuadPart));
  GlobalUnlock(memory);
  return result;
}
ClipboardWriteResult CopyCapture(HWND owner, const CaptureImage& image) {
  auto png = EncodeCapture(image);
  BITMAPINFOHEADER header{sizeof(BITMAPINFOHEADER),
                          image.width,
                          -image.height,
                          1,
                          32,
                          BI_RGB,
                          static_cast<DWORD>(image.pixels.size() * 4)};
  std::string dib(reinterpret_cast<const char*>(&header), sizeof(header));
  dib.append(reinterpret_cast<const char*>(image.pixels.data()),
             image.pixels.size() * 4);
  auto pngMemory = Memory(png.data(), png.size()),
       dibMemory = Memory(dib.data(), dib.size());
  ClipboardWriteResult copied;
  if (pngMemory && dibMemory && OpenClipboard(owner)) {
    if (EmptyClipboard()) {
      if (SetClipboardData(RegisterClipboardFormatW(L"PNG"), pngMemory)) {
        copied.success = true;
        pngMemory = nullptr;
      }
      if (SetClipboardData(CF_DIB, dibMemory)) {
        copied.success = true;
        dibMemory = nullptr;
      }
    }
    if (copied) copied.sequence = GetClipboardSequenceNumber();
    CloseClipboard();
  }
  if (pngMemory) GlobalFree(pngMemory);
  if (dibMemory) GlobalFree(dibMemory);
  return copied;
}
ClipboardWriteResult CopyCapsuleAsset(HWND owner, Store& store, const Json& item) {
  if (item.value("kind", "") == "image")
    return CopyCapture(owner, LoadCapture(store.Asset(item)));
  const auto path = store.Asset(item).wstring();
  DROPFILES header{};
  header.pFiles = sizeof(header);
  header.fWide = TRUE;
  std::string bytes(reinterpret_cast<const char*>(&header), sizeof(header));
  bytes.append(reinterpret_cast<const char*>(path.c_str()),
               (path.size() + 1) * sizeof(wchar_t));
  bytes.append(sizeof(wchar_t), '\0');
  auto memory = Memory(bytes.data(), bytes.size());
  ClipboardWriteResult result;
  if (memory && OpenClipboard(owner)) {
    result.success = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
    if (result) result.sequence = GetClipboardSequenceNumber();
    CloseClipboard();
  }
  if (!result && memory) GlobalFree(memory);
  return result;
}
CaptureImage RenderCapture(const CaptureImage& base, const Json& operations) {
  Require(base.Valid() && operations.is_array() && operations.size() <= 500);
  auto session = ui::AcquireSmoothShapes();
  auto result = base;
  for (const auto& op : operations) {
    if (op.at("tool") == "crop") {
      const auto& points = op.at("points");
      Require(points.is_array() && points.size() == 2);
      result = CropCapture(result,
                           CaptureSelection(Point(points[0]), Point(points[1]),
                                            result.width, result.height));
    } else {
      Gdiplus::Bitmap bitmap(result.width, result.height, result.width * 4,
                             PixelFormat32bppARGB,
                             reinterpret_cast<BYTE*>(result.pixels.data()));
      Gdiplus::Graphics g(&bitmap);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      DrawOperation(g, op);
    }
  }
  return result;
}

struct CaptureWindows::Impl {
  Store& store;
  std::function<void()> changed;
  std::function<std::string()> theme;
  Screenshot screenshot;
  Copy copy;
  std::shared_ptr<ui::SmoothShapesSession> shapes = ui::AcquireSmoothShapes();
  HWND selection = nullptr, stack = nullptr, editor = nullptr;
  HFONT font = nullptr;
  CaptureImage desktop, original, rendered;
  CaptureSurface selection_image, selection_dim, selection_frame;
  struct PreparedCapture { std::string png; CaptureImage preview; };
  std::future<PreparedCapture> preparation;
  bool discard_preparation = false, laying_out = false;
  RECT desktop_rect{}, canvas{};
  POINT start{}, current{};
  bool dragging = false, dirty = false;
  std::string tool = "arrow";
  Json pending, operations = Json::array(), redo = Json::array(), record;
  std::vector<Json> cards;
  std::vector<CaptureImage> previews;
  std::vector<HWND> card_windows;
  HWND hovered_card = nullptr;
  HWND drag_card = nullptr;
  POINT drag_start{};
  std::string dragged_record;
  std::shared_ptr<std::atomic<bool>> drag_cancelled;
  unsigned color_index = 0;
  std::wstring status;
  std::map<HWND, std::wstring> card_status;
  HMONITOR monitor = nullptr;
  HWND previous_foreground = nullptr;
  static constexpr std::uint32_t colors[] = {0xffff4254U, 0xff22c55eU,
                                             0xff2787ffU, 0xff141414U};
  Impl(Store& s, std::function<void()> c, std::function<std::string()> t,
       Screenshot shot, Copy writer)
      : store(s),
        changed(std::move(c)),
        theme(std::move(t)),
        screenshot(shot ? std::move(shot) : DesktopImage),
        copy(writer ? std::move(writer) : CopyCapsuleAsset) {}
  ~Impl() {
    for (auto h : card_windows) DestroyWindow(h);
    for (auto h : {selection, editor})
      if (h) DestroyWindow(h);
    if (font) DeleteObject(font);
  }
  bool IsCard(HWND h) const {
    return std::find(card_windows.begin(), card_windows.end(), h) != card_windows.end();
  }
  void Hover(HWND h) {
    if (hovered_card == h) return;
    const auto old = hovered_card;
    hovered_card = h;
    for (auto card : {old, h}) if (card && IsWindow(card)) {
      for (int id = 200; id <= 204; ++id)
        ShowWindow(GetDlgItem(card, id), card == h ? SW_SHOWNOACTIVATE : SW_HIDE);
      RedrawWindow(card, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
  }
  void TrackHover(HWND h) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    Hover(IsCard(h) ? h : GetParent(h));
  }
  void LeaveHover() {
    POINT point{}; GetCursorPos(&point);
    const auto hit = WindowFromPoint(point);
    const auto root = GetAncestor(hit, GA_ROOT);
    Hover(IsCard(root) ? root : nullptr);
  }
  void DragCard(HWND h) {
    const auto index = static_cast<std::size_t>(std::find(card_windows.begin(), card_windows.end(), h) - card_windows.begin());
    if (index >= cards.size() || !store.Enabled(Module::Capture)) return;
    const auto item = store.Get(cards[index].at("id"));
    const auto path = store.Asset(item);
    const auto initialized = OleInitialize(nullptr);
    Require(initialized);
    auto pidl = ILCreateFromPathW(path.c_str());
    if (!pidl) { OleUninitialize(); throw std::runtime_error("图片已不可用，请刷新捕获历史。"); }
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    auto result = SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child);
    ComPtr<IDataObject> data;
    if (SUCCEEDED(result)) {
      result = folder->GetUIObjectOf(h, 1, &child, IID_IDataObject, nullptr,
          reinterpret_cast<void**>(data.GetAddressOf()));
      folder->Release();
    }
    if (SUCCEEDED(result)) {
      dragged_record = item.at("id");
      drag_cancelled = std::make_shared<std::atomic<bool>>(false);
      auto* source = new CaptureDragSource(drag_cancelled);
      DWORD effect = 0;
      // Copy-only: dropping a thumbnail must never move a managed asset.
      DoDragDrop(data.Get(), source, DROPEFFECT_COPY, &effect);
      source->Release();
      dragged_record.clear(); drag_cancelled.reset();
    }
    ILFree(pidl);
    data.Reset(); OleUninitialize();
    Require(result);
  }
  static LRESULT CALLBACK CardButton(HWND h, UINT msg, WPARAM w, LPARAM l,
      UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<Impl*>(data);
    if (msg == WM_MOUSEMOVE) self->TrackHover(h);
    if (msg == WM_MOUSELEAVE) self->LeaveHover();
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    // Native buttons retain input/accessibility, but all visual content belongs
    // to the parent's single buffered frame. Independently exposed child
    // backgrounds used to flash while hover controls appeared or disappeared.
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
      PAINTSTRUCT paint{}; BeginPaint(h, &paint); EndPaint(h, &paint); return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, CardButton, 1);
    const auto parent = GetParent(h);
    const auto result = DefSubclassProc(h, msg, w, l);
    if (IsWindow(parent) && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_CAPTURECHANGED || msg == BM_SETSTATE || msg == WM_SETFOCUS || msg == WM_KILLFOCUS))
      InvalidateRect(parent, nullptr, FALSE);
    return result;
  }
  HWND Make(const wchar_t* name, const wchar_t* title, DWORD style, DWORD ex,
            int x, int y, int w, int h) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    auto hwnd = CreateWindowExW(ex, name, title, style, x, y, w, h, nullptr,
                                nullptr, wc.hInstance, this);
    Require(hwnd != nullptr);
    return hwnd;
  }
  void Button(HWND parent, int id, const wchar_t* text) {
    auto h = CreateWindowExW(
        0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    Require(h != nullptr);
  }
  int S(HWND h, int v) { return MulDiv(v, GetDpiForWindow(h), 96); }
  RECT SelectionHint(POINT point) const {
    const LONG x = (std::max)(0L, (std::min)(point.x + 18, desktop.width - 370L));
    const LONG y = (std::max)(0L, (std::min)(point.y + 22, desktop.height - 48L));
    return {x, y, (std::min)(static_cast<LONG>(desktop.width), x + 360),
        (std::min)(static_cast<LONG>(desktop.height), y + 36)};
  }
  void MoveSelection(POINT next) {
    const auto previous = current;
    current = next;
    if (previous.x == next.x && previous.y == next.y) return;
    for (const auto point : {previous, next}) {
      auto hint = SelectionHint(point);
      InvalidateRect(selection, &hint, FALSE);
      if (dragging) {
        auto area = CaptureSelection(start, point, desktop.width, desktop.height);
        InflateRect(&area, 3, 3);
        InvalidateRect(selection, &area, FALSE);
      }
    }
  }
  void Place(HWND h, int id, int x, int y, int w, int height,
             bool visible = true) {
    SetWindowPos(GetDlgItem(h, id), nullptr, S(h, x), S(h, y), S(h, w),
                 S(h, height),
                 SWP_NOZORDER | SWP_NOACTIVATE |
                     (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  }
  void Error(HWND h, const std::exception& e) {
    if (IsCard(h))
      card_status[h] = Wide(e.what());
    else
      status = Wide(e.what());
    if (h == selection) {
      CancelSelection();
      MessageBoxW(nullptr, status.c_str(), L"截图未完成", MB_OK | MB_ICONERROR);
    } else
      InvalidateRect(h, nullptr, FALSE);
  }
  void CancelSelection() {
    dragging = false;
    if (GetCapture() == selection) ReleaseCapture();
    if (selection) ShowWindow(selection, SW_HIDE);
    desktop = {};
    selection_image.Reset(); selection_dim.Reset(); selection_frame.Reset();
    LayoutStack();
    if (previous_foreground && IsWindow(previous_foreground))
      SetForegroundWindow(previous_foreground);
  }
  void Start() {
    if (!store.Enabled(Module::Capture))
      throw std::runtime_error("捕获模块已停用，请先在插件中启用。");
    if (selection && IsWindowVisible(selection)) return;
    if (preparation.valid()) return;
    if (editor && IsWindowVisible(editor)) {
      SetForegroundWindow(editor);
      throw std::runtime_error("请先保存或关闭当前截图编辑。");
    }
    previous_foreground = GetForegroundWindow();
    Hover(nullptr);
    for (auto h : card_windows) ShowWindow(h, SW_HIDE);
    DwmFlush();
    try {
      auto shot = screenshot();
      desktop = std::move(shot.first);
      desktop_rect = shot.second;
      Require(desktop.Valid());
      selection_image.Create(desktop.width, desktop.height);
      selection_dim.Create(desktop.width, desktop.height);
      selection_frame.Create(desktop.width, desktop.height);
      memcpy(selection_image.pixels, desktop.pixels.data(), desktop.pixels.size() * 4);
      for (std::size_t i = 0; i < desktop.pixels.size(); ++i) {
        const auto pixel = desktop.pixels[i];
        selection_dim.pixels[i] = 0xff000000U |
            (((pixel >> 16 & 255) * 145 / 255) << 16) |
            (((pixel >> 8 & 255) * 145 / 255) << 8) | ((pixel & 255) * 145 / 255);
      }
    } catch (...) {
      LayoutStack();
      throw;
    }
    if (!selection)
      selection =
          Make(L"Rimes.Capture.Selection", L"框选截图 · 拖动选择，Esc 取消",
               WS_POPUP, WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 0, 0, 1, 1);
    dragging = false;
    start = current = {};
    SetWindowPos(selection, HWND_TOPMOST, desktop_rect.left, desktop_rect.top,
                 desktop.width, desktop.height, SWP_SHOWWINDOW);
    SetForegroundWindow(selection);
    SetFocus(selection);
    InvalidateRect(selection, nullptr, FALSE);
  }
  void FinishSelection() {
    const auto rect =
        CaptureSelection(start, current, desktop.width, desktop.height);
    if (rect.right - rect.left < 2 || rect.bottom - rect.top < 2) {
      CancelSelection();
      return;
    }
    auto image = std::move(desktop);
    POINT location{desktop_rect.left + rect.left, desktop_rect.top + rect.top};
    if (!monitor || cards.empty()) monitor = MonitorFromPoint(location, MONITOR_DEFAULTTONEAREST);
    dragging = false;
    ReleaseCapture();
    ShowWindow(selection, SW_HIDE);
    desktop = {};
    selection_image.Reset(); selection_dim.Reset(); selection_frame.Reset();
    discard_preparation = false;
    preparation = std::async(std::launch::async, [image = std::move(image), rect] {
      const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      try {
        auto cropped = CropCapture(image, rect);
        PreparedCapture result{EncodeCapture(cropped), Thumbnail(cropped)};
        if (SUCCEEDED(com)) CoUninitialize();
        return result;
      } catch (...) {
        if (SUCCEEDED(com)) CoUninitialize();
        throw;
      }
    });
    SetTimer(selection, 1, 15, nullptr);
    LayoutStack();
    if (previous_foreground && IsWindow(previous_foreground))
      SetForegroundWindow(previous_foreground);
  }
  void CompleteCapture() {
    if (!preparation.valid() || preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    KillTimer(selection, 1);
    if (discard_preparation || !store.Enabled(Module::Capture)) {
      try { preparation.get(); } catch (...) {}
      return;
    }
    auto ready = preparation.get();
    auto item = store.ImportData(Module::Capture, workbench::Utf8(Stamp()),
        ".png", ready.png, "", true);
    ShowCard(item, std::move(ready.preview));
    if (changed) changed();
  }
  void ShowCard(const Json& item, CaptureImage preview = {}) {
    const auto id = item.at("id");
    if (!preview.Valid()) preview = Thumbnail(LoadCapture(store.Asset(item)));
    for (std::size_t i = 0; i < cards.size(); ++i)
      if (cards[i].at("id") == id) {
        cards[i] = item;
        previews[i] = std::move(preview);
        card_status.erase(card_windows[i]);
        InvalidateRect(card_windows[i], nullptr, FALSE);
        return; // Editing updates this card in place, never its stack identity.
      }
    auto h = Make(L"Rimes.Capture.Stack", L"截图堆栈", WS_POPUP,
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED, 0, 0, 1, 1);
    const wchar_t* labels[] = {L"复制并移除卡片", L"标注", L"保存", L"关闭卡片", L"贴图（尚未实现）"};
    for (int i = 0; i < 5; ++i) {
      Button(h, 200 + i, labels[i]);
      auto button = GetDlgItem(h, 200 + i);
      SetWindowLongPtrW(button, GWL_EXSTYLE, GetWindowLongPtrW(button, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
      ShowWindow(button, SW_HIDE);
      SetWindowSubclass(button, CardButton, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    auto tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW,
        nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, h, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (int i = 0; i < 5; ++i) {
      TOOLINFOW tool_info{sizeof(tool_info)};
      tool_info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      tool_info.hwnd = h;
      tool_info.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(h, 200 + i));
      tool_info.lpszText = const_cast<wchar_t*>(labels[i]);
      SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool_info));
    }
    EnableWindow(GetDlgItem(h, 204), FALSE);
    // Exclude our own chrome where the OS supports capture exclusion.
    SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    cards.insert(cards.begin(), item);
    previews.insert(previews.begin(), std::move(preview));
    card_windows.insert(card_windows.begin(), h);
    stack = h;
    if (cards.size() > 50) {
      if (hovered_card == card_windows.back()) Hover(nullptr);
      card_status.erase(card_windows.back());
      DestroyWindow(card_windows.back());
      card_windows.pop_back();
      cards.pop_back();
      previews.pop_back();
    }
    LayoutStack();
  }
  void LayoutStack() {
    if (!stack || laying_out) return;
    laying_out = true;
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info))
      GetMonitorInfoW(MonitorFromWindow(stack, MONITOR_DEFAULTTOPRIMARY),
                      &info);
    int bottom = info.rcWork.bottom - S(stack, 18);
    for (auto h : card_windows) {
      // Move onto the destination display before measuring its DPI.
      SetWindowPos(h, nullptr, info.rcWork.left + S(h, 18), bottom - S(h, 148),
          0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      const int width = S(h, 208), height = S(h, 148);
      const bool fits = bottom - height >= info.rcWork.top + S(h, 18) &&
          width <= info.rcWork.right - info.rcWork.left - S(h, 36);
      SetWindowPos(h, HWND_TOPMOST, info.rcWork.left + S(h, 18), bottom - height,
          width, height, SWP_NOACTIVATE | (fits ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
      const bool controls = h == hovered_card && fits;
      Place(h, 200, 78, 44, 52, 26, controls);
      Place(h, 202, 78, 78, 52, 26, controls);
      Place(h, 203, 7, 7, 22, 22, controls);
      Place(h, 201, 7, 119, 22, 22, controls);
      Place(h, 204, 179, 7, 22, 22, controls);
      InvalidateRect(h, nullptr, FALSE);
      bottom -= height + S(h, 8);
    }
    laying_out = false;
  }
  void Edit(const Json& item) {
    if (!store.Enabled(Module::Capture))
      throw std::runtime_error("捕获模块已停用。");
    if (editor && IsWindowVisible(editor) && dirty) {
      SetForegroundWindow(editor);
      throw std::runtime_error("请先保存或放弃当前编辑。");
    }
    record = store.Get(item.at("id"));
    auto source = record;
    operations = Json::array();
    redo = Json::array();
    if (record.contains("captureDocument")) {
      const auto& doc = record.at("captureDocument");
      Require(doc.value("version", 0) == 1);
      source["asset"] = doc.at("source");
      operations = doc.at("operations");
    }
    original = LoadCapture(store.Asset(source));
    rendered = RenderCapture(original, operations);
    dirty = false;
    pending = nullptr;
    dragging = false;
    status =
        L"拖动标注或裁剪 · Ctrl+Z 撤销 · Ctrl+Y 重做 · Ctrl+S 保存 · 原图保留";
    if (!editor) {
      editor = Make(L"Rimes.Capture.Editor", L"Capsule · 截图编辑",
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, WS_EX_APPWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, 1040, 720);
      for (const auto& entry :
           std::vector<std::pair<int, const wchar_t*>>{{100, L"箭头"},
                                                       {101, L"矩形"},
                                                       {102, L"画笔"},
                                                       {103, L"遮盖"},
                                                       {104, L"裁剪"},
                                                       {105, L"换颜色"},
                                                       {106, L"撤销"},
                                                       {107, L"重做"},
                                                       {108, L"保存"},
                                                       {109, L"复制"},
                                                       {110, L"另存 PNG"},
                                                       {111, L"更多"}})
        Button(editor, entry.first, entry.second);
    }
    LayoutEditor();
    ShowWindow(editor, SW_SHOWNORMAL);
    SetForegroundWindow(editor);
    SetFocus(editor);
  }
  void LayoutEditor() {
    if (!editor) return;
    RECT r{};
    GetClientRect(editor, &r);
    int x = 10, y = 10, width = MulDiv(r.right, 96, GetDpiForWindow(editor));
    for (int id = 100; id <= 111; ++id) {
      const int w = id == 110 ? 90 : 68;
      if (x + w > width - 10) {
        x = 10;
        y += 36;
      }
      Place(editor, id, x, y, w, 28);
      x += w + 5;
    }
    if (rendered.Valid())
      canvas = Fit(rendered,
                   {S(editor, 16), S(editor, y + 46), r.right - S(editor, 16),
                    (std::max)(static_cast<LONG>(S(editor, y + 60)),
                               r.bottom - S(editor, 46))});
    EnableWindow(GetDlgItem(editor, 106), !operations.empty());
    EnableWindow(GetDlgItem(editor, 107), !redo.empty());
    EnableWindow(GetDlgItem(editor, 108), dirty);
    InvalidateRect(editor, nullptr, FALSE);
  }
  POINT ImagePoint(POINT p) {
    return {std::clamp(MulDiv(p.x - canvas.left, rendered.width,
                              (std::max)(1L, canvas.right - canvas.left)),
                       0, rendered.width),
            std::clamp(MulDiv(p.y - canvas.top, rendered.height,
                              (std::max)(1L, canvas.bottom - canvas.top)),
                       0, rendered.height)};
  }
  void RefreshEdit() {
    rendered = RenderCapture(original, operations);
    dirty = true;
    LayoutEditor();
  }
  void Save() {
    if (!dirty) return;
    const auto asset = NewId() + ".png";
    auto path = store.Asset(Json{{"asset", asset}});
    AtomicWrite(path, EncodeCapture(rendered));
    auto next = record;
    next["asset"] = asset;
    next["kind"] = "image";
    next["captureDocument"] = {
        {"version", 1},
        {"source", record.contains("captureDocument")
                       ? record["captureDocument"]["source"]
                       : record["asset"]},
        {"operations", operations}};
    try {
      record = store.Save(next, record.at("revision"));
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      throw;
    }
    dirty = false;
    ShowCard(record);
    status = L"已保存 · 可从 Capsule 或堆栈再次编辑";
    if (changed) changed();
    LayoutEditor();
  }
  void Export(HWND owner, const CaptureImage& image) {
    wchar_t path[32768] = L"截图.png";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"PNG 图片\0*.png\0\0";
    dialog.lpstrDefExt = L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) {
      AtomicWrite(path, EncodeCapture(image));
      (IsCard(owner) ? card_status[owner] : status) = L"已导出 PNG";
      InvalidateRect(owner, nullptr, FALSE);
    }
  }
  void CloseEditor() {
    if (dirty) {
      const auto answer =
          MessageBoxW(editor, L"保存本次截图编辑？", L"截图编辑",
                      MB_YESNOCANCEL | MB_DEFBUTTON1);
      if (answer == IDCANCEL) return;
      if (answer == IDYES) Save();
    }
    ShowWindow(editor, SW_HIDE);
    original = {};
    rendered = {};
    operations = Json::array();
    redo = Json::array();
    dirty = false;
  }
  void Command(HWND h, int id) {
    if (!store.Enabled(Module::Capture)) {
      Protect();
      return;
    }
    if (IsCard(h)) {
      if (id < 200 || id > 203) return;
      const auto index = static_cast<std::size_t>(
          std::find(card_windows.begin(), card_windows.end(), h) - card_windows.begin());
      if (index >= cards.size()) return;
      const auto item = store.Get(cards[index].at("id"));
      const int action = id - 200;
      if (action == 1) {
        Edit(item);
        return;
      }
      if (action == 2) {
        Export(h, LoadCapture(store.Asset(item)));
        return;
      }
      if (action == 0 && !copy(h, store, item))
        throw std::runtime_error("剪贴板忙，卡片已保留，请重试。");
      if (hovered_card == h) Hover(nullptr);
      cards.erase(cards.begin() + index);
      previews.erase(previews.begin() + index);
      card_windows.erase(card_windows.begin() + index);
      card_status.erase(h);
      DestroyWindow(h);
      stack = card_windows.empty() ? nullptr : card_windows.front();
      LayoutStack();
      return;
    }
    if (id >= 100 && id <= 104) {
      const char* tools[] = {"arrow", "rectangle", "pen", "redact", "crop"};
      tool = tools[id - 100];
    }
    if (id == 105) color_index = (color_index + 1) % 4;
    if (id == 106 && !operations.empty()) {
      redo.push_back(operations.back());
      operations.erase(operations.end() - 1);
      RefreshEdit();
    }
    if (id == 107 && !redo.empty()) {
      operations.push_back(redo.back());
      redo.erase(redo.end() - 1);
      RefreshEdit();
    }
    if (id == 108) Save();
    if (id == 109) {
      if (!CopyCapture(editor, rendered))
        throw std::runtime_error("剪贴板忙，请重试。");
      status = L"已复制图片";
    }
    if (id == 110) Export(editor, rendered);
    if (id == 111) {
      auto menu = CreatePopupMenu();
      for (const auto* text : {L"文字 / OCR（尚未实现）", L"录屏（尚未实现）",
                               L"贴图 / 多图拼接（尚未实现）",
                               L"模糊 / 像素化（尚未实现，请使用遮盖）"})
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, text);
      POINT p{};
      GetCursorPos(&p);
      TrackPopupMenu(menu, TPM_NONOTIFY, p.x, p.y, 0, editor, nullptr);
      DestroyMenu(menu);
    }
    RedrawWindow(editor, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
  }
  void Paint(HWND h, HDC dc) {
    RECT r{};
    GetClientRect(h, &r);
    const auto& palette =
        ui::Palette(ui::ThemeIdOrDefault(theme ? theme() : "night"));
    appearance::Paint p(dc, GetDpiForWindow(h));
    if (h == selection) {
      if (!selection_dim.dc) return;
      BitBlt(dc, 0, 0, r.right, r.bottom, selection_dim.dc, 0, 0, SRCCOPY);
      if (dragging) {
        auto area =
            CaptureSelection(start, current, desktop.width, desktop.height);
        BitBlt(dc, area.left, area.top, area.right - area.left,
            area.bottom - area.top, selection_image.dc, area.left, area.top, SRCCOPY);
        ui::StrokeRoundRect(dc, area, 0, RGB(34, 197, 94), 2);
      }
      auto text =
          dragging ? std::to_wstring(std::abs(current.x - start.x)) + L" × " +
                         std::to_wstring(std::abs(current.y - start.y)) +
                         L"  松开完成 · Esc 取消"
                   : L"拖动选择截图区域 · Esc 取消";
      const auto hint = SelectionHint(current);
      p.Fill(hint, 0x202020, 8);
      p.Text(text, hint, 12, 0xffffff, FW_NORMAL,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (IsCard(h)) {
      const auto index = std::find(card_windows.begin(), card_windows.end(), h) - card_windows.begin();
      const auto& image = previews[index];
      p.Fill(r, palette.surface_secondary, 8);
      DrawImage(dc, image, Fit(image, r));
      if (hovered_card == h) {
        {
          Gdiplus::Graphics graphics(dc);
          Gdiplus::SolidBrush shade(Gdiplus::Color(110, 0, 0, 0));
          graphics.FillRectangle(&shade, 0, 0, r.right, r.bottom);
        }
        const ui::FluentIcon icons[] = {ui::FluentIcon::kCopy, ui::FluentIcon::kEdit,
            ui::FluentIcon::kArrowDown, ui::FluentIcon::kClose, ui::FluentIcon::kPin};
        for (int action = 0; action < 5; ++action) {
          auto button = GetDlgItem(h, 200 + action);
          RECT box{}; GetWindowRect(button, &box);
          MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&box), 2);
          const bool down = (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
          p.Fill(box, down ? palette.accent : palette.surface_secondary, 5);
          if (GetFocus() == button) p.Stroke(box, palette.accent, 5);
          const int size = p.S(16);
          box.left += (box.right - box.left - size) / 2;
          box.top += (box.bottom - box.top - size) / 2;
          box.right = box.left + size; box.bottom = box.top + size;
          ui::DrawFluentIcon(dc, icons[action], box,
              ui::ToColorRef(IsWindowEnabled(button) ? palette.text_primary : palette.text_muted));
        }
        const auto notice = card_status.find(h);
        if (notice != card_status.end()) p.Text(notice->second, p.R(36, 118, 165, 25),
            10, 0xffffff, FW_NORMAL, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      }
      p.Stroke({0, 0, r.right - 1, r.bottom - 1}, palette.border, 8);
    } else if (rendered.Valid()) {
      p.Fill(r, palette.settings_background);
      DrawImage(dc, rendered, canvas);
      if (dragging && pending.is_object()) {
        if (tool == "crop") {
          auto a = Point(pending["points"].front()),
               b = Point(pending["points"].back());
          RECT box{canvas.left +
                       MulDiv(a.x, canvas.right - canvas.left, rendered.width),
                   canvas.top +
                       MulDiv(a.y, canvas.bottom - canvas.top, rendered.height),
                   canvas.left +
                       MulDiv(b.x, canvas.right - canvas.left, rendered.width),
                   canvas.top + MulDiv(b.y, canvas.bottom - canvas.top,
                                       rendered.height)};
          if (box.left > box.right) std::swap(box.left, box.right);
          if (box.top > box.bottom) std::swap(box.top, box.bottom);
          ui::StrokeRoundRect(dc, box, 0, RGB(34, 197, 94), 2);
        } else {
          Gdiplus::Graphics g(dc);
          g.SetClip(Gdiplus::Rect(canvas.left, canvas.top,
                                  canvas.right - canvas.left,
                                  canvas.bottom - canvas.top));
          g.TranslateTransform(static_cast<float>(canvas.left),
                               static_cast<float>(canvas.top));
          g.ScaleTransform(
              static_cast<float>(canvas.right - canvas.left) / rendered.width,
              static_cast<float>(canvas.bottom - canvas.top) / rendered.height);
          g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
          DrawOperation(g, pending);
        }
      }
      p.Text(status,
             {S(h, 14), r.bottom - S(h, 34), r.right - S(h, 14),
              r.bottom - S(h, 6)},
             11, palette.text_secondary, FW_NORMAL,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
  }
  void Protect() {
    if (drag_cancelled) drag_cancelled->store(true);
    if (drag_card && GetCapture() == drag_card) ReleaseCapture();
    drag_card = nullptr;
    discard_preparation = true;
    Hover(nullptr);
    dragging = false;
    if (GetCapture() == selection || GetCapture() == editor) ReleaseCapture();
    for (auto h : card_windows) DestroyWindow(h);
    card_windows.clear(); stack = nullptr;
    for (auto h : {selection, editor})
      if (h) ShowWindow(h, SW_HIDE);
    desktop = {};
    selection_image.Reset(); selection_dim.Reset(); selection_frame.Reset();
    original = {};
    rendered = {};
    pending = nullptr;
    operations = Json::array();
    redo = Json::array();
    dirty = false;
    cards.clear();
    previews.clear();
    card_status.clear();
  }
  static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      self = static_cast<Impl*>(
          reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, msg, w, l);
    try {
      switch (msg) {
        case WM_ERASEBKGND:
          return 1;
        case WM_PAINT: {
          PAINTSTRUCT ps{};
          auto dc = BeginPaint(h, &ps);
          RECT r{};
          GetClientRect(h, &r);
          if (self->IsCard(h)) {
            ui::LayeredFrame frame;
            if (frame.Resize(r.right, r.bottom)) {
              self->Paint(h, frame.dc());
              frame.RoundAlpha(static_cast<float>(self->S(h, 8)));
              frame.Present(h);
            }
            EndPaint(h, &ps);
            return 0;
          }
          if (h == self->selection && self->selection_frame.dc) {
            auto frame = self->selection_frame.dc;
            const auto saved = SaveDC(frame);
            IntersectClipRect(frame, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
            self->Paint(h, frame);
            RestoreDC(frame, saved);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                frame, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            EndPaint(h, &ps);
            return 0;
          }
          auto frame = CreateCompatibleDC(dc);
          auto bitmap = CreateCompatibleBitmap(dc, (std::max)(1L, r.right),
                                               (std::max)(1L, r.bottom));
          auto old = SelectObject(frame, bitmap);
          self->Paint(h, frame);
          BitBlt(dc, 0, 0, r.right, r.bottom, frame, 0, 0, SRCCOPY);
          SelectObject(frame, old);
          DeleteObject(bitmap);
          DeleteDC(frame);
          EndPaint(h, &ps);
          return 0;
        }
        case WM_PRINTCLIENT:
          self->Paint(h, reinterpret_cast<HDC>(w));
          return 0;
        case WM_DRAWITEM: {
          const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
          auto& pal = ui::Palette(
              ui::ThemeIdOrDefault(self->theme ? self->theme() : "night"));
          if (self->IsCard(h)) {
            // Painted atomically with the thumbnail in Paint(), never as a
            // second independent surface on top of a just-exposed background.
            return TRUE;
          }
          bool selected = false;
          const char* names[] = {"arrow", "rectangle", "pen", "redact", "crop"};
          if (item->CtlID >= 100 && item->CtlID <= 104)
            selected = self->tool == names[item->CtlID - 100];
          ui::FillRectColor(item->hDC, item->rcItem,
                            ui::ToColorRef(pal.settings_background));
          ui::FillRoundRect(
              item->hDC, item->rcItem, self->S(h, 6),
              ui::ToColorRef(selected || (item->itemState & ODS_SELECTED)
                                 ? pal.clipboard_selected
                                 : pal.surface_secondary));
          ui::StrokeRoundRect(
              item->hDC, item->rcItem, self->S(h, 6),
              ui::ToColorRef((item->itemState & ODS_FOCUS) ? pal.accent
                                                           : pal.border));
          wchar_t text[64]{};
          GetWindowTextW(item->hwndItem, text, 64);
          appearance::Paint p(item->hDC, GetDpiForWindow(h));
          p.Text(text, item->rcItem, 11,
                 (item->itemState & ODS_DISABLED) ? pal.text_muted
                 : item->CtlID == 105 ? colors[self->color_index] & 0xffffff
                                      : pal.text_primary,
                 FW_NORMAL, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          return TRUE;
        }
        case WM_COMMAND:
          if (HIWORD(w) == BN_CLICKED) self->Command(h, LOWORD(w));
          return 0;
        case WM_SIZE:
          if (h == self->editor) self->LayoutEditor();
          return 0;
        case WM_GETMINMAXINFO:
          if (h == self->editor) {
            auto* m = reinterpret_cast<MINMAXINFO*>(l);
            m->ptMinTrackSize = {self->S(h, 640), self->S(h, 450)};
            return 0;
          }
          break;
        case WM_DPICHANGED:
          if (h != self->selection) {
            const auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->IsCard(h))
              self->LayoutStack();
            else
              self->LayoutEditor();
          }
          return 0;
        case WM_DISPLAYCHANGE:
          if (h == self->selection && IsWindowVisible(h))
            self->CancelSelection();
          else if (self->IsCard(h))
            self->LayoutStack();
          return 0;
        case WM_MOUSEACTIVATE:
          if (self->IsCard(h)) return MA_NOACTIVATE;
          break;
        case WM_SETCURSOR:
          if (h == self->selection) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
          }
          break;
        case WM_LBUTTONDOWN: {
          POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
          if (self->IsCard(h)) {
            self->drag_card = h; self->drag_start = pt; SetCapture(h);
          } else if (h == self->selection) {
            self->start = self->current = pt;
            self->dragging = true;
            SetCapture(h);
          } else if (h == self->editor && PtInRect(&self->canvas, pt)) {
            self->dragging = true;
            auto p = self->ImagePoint(pt);
            self->pending = {
                {"tool", self->tool},
                {"color", colors[self->color_index]},
                {"width", 4},
                {"points", Json::array({Json::array({p.x, p.y}),
                                        Json::array({p.x, p.y})})}};
            SetCapture(h);
            SetFocus(h);
          }
          InvalidateRect(h, nullptr, FALSE);
          return 0;
        }
        case WM_MOUSEMOVE:
          if (self->IsCard(h)) self->TrackHover(h);
          if (h == self->drag_card && (w & MK_LBUTTON) &&
              (std::abs(GET_X_LPARAM(l) - self->drag_start.x) >= GetSystemMetrics(SM_CXDRAG) ||
               std::abs(GET_Y_LPARAM(l) - self->drag_start.y) >= GetSystemMetrics(SM_CYDRAG))) {
            self->drag_card = nullptr; ReleaseCapture(); self->DragCard(h); return 0;
          }
          if (h == self->selection) {
            self->MoveSelection({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
          }
          if (self->dragging) {
            self->current = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (h == self->editor) {
              auto p = self->ImagePoint(self->current);
              auto& points = self->pending["points"];
              if (self->tool == "pen" && points.size() < 20000)
                points.push_back({p.x, p.y});
              else
                points.back() = {p.x, p.y};
            }
            InvalidateRect(h, nullptr, FALSE);
          }
          return 0;
        case WM_MOUSELEAVE:
          if (self->IsCard(h)) self->LeaveHover();
          return 0;
        case WM_TIMER:
          if (h == self->selection && w == 1) self->CompleteCapture();
          return 0;
        case WM_LBUTTONDBLCLK:
          if (self->IsCard(h)) self->Command(h, 201);
          return 0;
        case WM_LBUTTONUP:
          if (h == self->drag_card) {
            self->drag_card = nullptr; ReleaseCapture(); return 0;
          }
          if (self->dragging) {
            self->current = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (h == self->selection)
              self->FinishSelection();
            else if (h == self->editor) {
              auto p = self->ImagePoint(self->current);
              self->pending["points"].back() = {p.x, p.y};
              self->dragging = false;
              ReleaseCapture();
              auto next = self->operations;
              next.push_back(self->pending);
              auto result = RenderCapture(self->original, next);
              self->operations = std::move(next);
              self->rendered = std::move(result);
              self->redo = Json::array();
              self->pending = nullptr;
              self->dirty = true;
              self->LayoutEditor();
            }
            return 0;
          }
          break;
        case WM_CAPTURECHANGED:
          if (h == self->drag_card) self->drag_card = nullptr;
          if (self->dragging) {
            self->dragging = false;
            self->pending = nullptr;
            InvalidateRect(h, nullptr, FALSE);
          }
          return 0;
        case WM_ACTIVATE:
          if (h == self->selection && LOWORD(w) == WA_INACTIVE &&
              IsWindowVisible(h))
            self->CancelSelection();
          break;
        case WM_CLOSE:
          if (h == self->editor)
            self->CloseEditor();
          else if (h == self->selection)
            self->CancelSelection();
          else if (self->IsCard(h))
            self->Command(h, 203);
          else
            ShowWindow(h, SW_HIDE);
          return 0;
        case WM_KEYDOWN:
          if (w == VK_ESCAPE) {
            if (self->dragging && h == self->editor) {
              self->dragging = false;
              self->pending = nullptr;
              ReleaseCapture();
              InvalidateRect(h, nullptr, FALSE);
            } else
              SendMessageW(h, WM_CLOSE, 0, 0);
            return 0;
          }
          break;
      }
    } catch (const std::exception& e) {
      self->Error(h, e);
      return 0;
    }
    return DefWindowProcW(h, msg, w, l);
  }
};
CaptureWindows::CaptureWindows(Store& s, std::function<void()> c,
                                 std::function<std::string()> t, Screenshot shot, Copy writer)
    : impl_(std::make_unique<Impl>(s, std::move(c), std::move(t),
                                   std::move(shot), std::move(writer))) {}
CaptureWindows::~CaptureWindows() = default;
void CaptureWindows::Start() { impl_->Start(); }
void CaptureWindows::Edit(const Json& record) { impl_->Edit(record); }
void CaptureWindows::Protect() { impl_->Protect(); }
HWND CaptureWindows::SelectionWindow() const { return impl_->selection; }
HWND CaptureWindows::StackWindow() const { return impl_->stack; }
HWND CaptureWindows::EditorWindow() const { return impl_->editor; }
bool CaptureWindows::Preparing() const { return impl_->preparation.valid(); }
void CaptureWindows::RefreshTheme() {
  auto handles = impl_->card_windows; handles.push_back(impl_->editor);
  for (auto h : handles) if (h)
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
std::set<std::string> CaptureWindows::ActiveRecords() const {
  std::set<std::string> ids;
  if (!impl_->dragged_record.empty()) ids.insert(impl_->dragged_record);
  if (IsWindowVisible(impl_->editor) && impl_->record.contains("id")) ids.insert(impl_->record.at("id"));
  for (const auto& item : impl_->cards) ids.insert(item.at("id"));
  return ids;
}
bool CaptureWindows::HandleMessage(MSG* msg) {
  auto handles = impl_->card_windows;
  handles.push_back(impl_->selection); handles.push_back(impl_->editor);
  for (auto h : handles)
    if (h && (msg->hwnd == h || IsChild(h, msg->hwnd))) {
      if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE) {
        SendMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
        return true;
      }
      if (h == impl_->editor && msg->message == WM_KEYDOWN &&
          (GetKeyState(VK_CONTROL) & 0x8000)) {
        const int id = msg->wParam == 'S'   ? 108
                       : msg->wParam == 'C' ? 109
                       : msg->wParam == 'Z' ? 106
                       : msg->wParam == 'Y' ? 107
                                            : 0;
        if (id) {
          SendMessageW(h, WM_COMMAND, id, 0);
          return true;
        }
      }
      return h == impl_->selection ? false : IsDialogMessageW(h, msg) != FALSE;
    }
  return false;
}
}  // namespace rimes::windows::peripherals
