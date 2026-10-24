#include "../ui/layered_frame.hpp"
#include "../ui/buffer_paint.hpp"
#include "../ui/icons.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace rimes::windows;
using Microsoft::WRL::ComPtr;
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void Save(ui::LayeredFrame& frame, const std::filesystem::path& path) {
  ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmap> bitmap; ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> image;
  Check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))), "WIC factory");
  Check(SUCCEEDED(factory->CreateBitmapFromHBITMAP(frame.bitmap(), nullptr, WICBitmapUsePremultipliedAlpha, &bitmap)), "WIC surface");
  factory->CreateStream(&stream); Check(SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)), "PNG path");
  factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder); encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  encoder->CreateNewFrame(&image, nullptr); image->Initialize(nullptr);
  Check(SUCCEEDED(image->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(image->Commit()) && SUCCEEDED(encoder->Commit()), "PNG frame");
}
int wmain(int argc, wchar_t** argv) {
  try {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<ID2D1Factory> factory; ComPtr<IDWriteFactory> write;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.GetAddressOf()));
    ComPtr<IDWriteTextFormat> body, label;
    write->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12, L"zh-CN", &body);
    write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10, L"zh-CN", &label);
    ui::BufferPaintState composition;
    composition.source_blocks = {{L"已有内容", false}};
    composition.insertion = 1;
    const auto start = ui::MeasureBufferCaret(write.Get(), body.Get(), composition, true);
    for (const auto* preedit : {L"n", L"nihao", L"ni"}) {
      composition.preedit = preedit;
      composition.preedit_caret = static_cast<unsigned>(composition.preedit.size());
      Check(ui::MeasureBufferCaret(write.Get(), body.Get(), composition, true) == start,
          "Buffer candidate anchor remains at composition start while editing");
      Check(ui::MeasureBufferCaret(write.Get(), body.Get(), composition) > start,
          "Buffer editing caret still advances independently of candidate placement");
    }
    const HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", L"Surface fixture", WS_POPUP, 0, 0, 760, 120, nullptr, nullptr, nullptr, nullptr);
    Check(window != nullptr, "layered fixture window");
    for (const int dpi : {96, 120, 144, 192, 240}) for (int theme = 0; theme < 4; ++theme) for (int scenario=0; scenario<6; ++scenario) {
      const auto mode = scenario % 3;
      ui::BufferPaintState state; state.theme = static_cast<ui::ThemeId>(theme);
      state.visible = state.bound = state.capturing = state.translate = true;
      state.mode = ui::BufferMode::kTranslate; state.source_blocks = {{L"今天我们去公园散步。", false}};
      if (mode==1) {
        state.mode=ui::BufferMode::kInput; state.translate=false; state.plugin_name=L"Default";
        state.auto_send_seconds=3; state.close_after_send=true;
        state.live_metrics=L"124 字/分  ·  码长 2.34  ·  击键 4.8/秒  ·  回删 2";
      } else if (mode==2) {
        state.mode=ui::BufferMode::kGenerate; state.translate=false; state.plugin_name=L"ChatGPT";
        state.show_ai_choices=true; state.ai_model=L"GPT-6 Astra"; state.ai_effort=L"High";
        state.busy=true; state.send_enabled=false; state.copy_enabled=false;
        state.result_blocks={{L"第一块回复。",true},{L"第二块正在生成…",true}};
        state.reasoning_summary=L"整理已提供的信息，并检查回答的关键步骤";
      }
      if (scenario >= 3) { state.folded = true; state.bound = state.capturing = false; }
      const auto layout = ui::LayoutBuffer(state, 760);
      if (state.folded) Check(!layout.show_source && !layout.show_result &&
          !layout.show_live_metrics && !layout.show_reasoning, "fold hides every content rail while retaining its state");
      const int width = MulDiv(760, dpi, 96), height = MulDiv(static_cast<int>(layout.height_dip), dpi, 96);
      ui::LayeredFrame frame; Check(frame.Resize(width, height), "allocate native DPI frame");
      ComPtr<ID2D1DCRenderTarget> target; ComPtr<ID2D1SolidColorBrush> brush;
      const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), float(dpi), float(dpi));
      Check(SUCCEEDED(factory->CreateDCRenderTarget(&properties, &target)), "premultiplied render target");
      RECT rect{0,0,width,height}; target->BindDC(frame.dc(), &rect); target->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
      if (theme == 0 && scenario == 0) {
        // Exercise the private font through DirectWrite at every desktop scale.
        // Missing/fallback glyphs must fail before a blank toolbar can ship.
        for (const auto glyph : {ui::FluentIcon::kGrid, ui::FluentIcon::kChevronDown,
            ui::FluentIcon::kSwap, ui::FluentIcon::kSpeakerOn, ui::FluentIcon::kSpeakerOff,
            ui::FluentIcon::kArrowLeft, ui::FluentIcon::kTimerOff, ui::FluentIcon::kClose,
            ui::FluentIcon::kPaste, ui::FluentIcon::kCopy, ui::FluentIcon::kSparkle,
            ui::FluentIcon::kSend, ui::FluentIcon::kSpinner}) {
          target->BeginDraw(); target->Clear(D2D1::ColorF(0,0));
          target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
          Check(ui::DrawFluentIcon(target.Get(), write.Get(), brush.Get(), glyph,
              D2D1::RectF(8,8,28,28), D2D1::ColorF(1,1,1)), "embedded Fluent glyph resolves");
          Check(SUCCEEDED(target->EndDraw()), "Fluent glyph draw completes");
          unsigned ink = 0;
          for (int py = 0; py < height; ++py) for (int px = 0; px < width; ++px) {
            if (!(frame.pixels()[py * width + px] >> 24U)) continue;
            ++ink;
            const float x = px * 96.f / dpi, y = py * 96.f / dpi;
            Check(x >= 7 && x <= 29 && y >= 7 && y <= 29, "Fluent glyph stays inside its control");
          }
          Check(ink > 8, "Fluent glyph has visible ink");
        }
      }
      const auto box = layout.association; const float scale = dpi / 96.f;
      const LONG x = static_cast<LONG>((box.left + 2) * scale), y = static_cast<LONG>((box.top + 2) * scale);
      const LONG size = static_cast<LONG>(18 * scale); RECT icon_box{x,y,x+size,y+size};
      const HICON icon = LoadIconW(nullptr, IDI_INFORMATION);
      std::vector<std::uint32_t> badge;
      for (int tick = 0; tick < 8; ++tick) {
        state.busy = mode==0 ? tick%2 : mode==2; state.status = state.busy ? L"生成中…" : L"等待输入";
        state.translation_speech = tick % 2 != 0;
        target->BeginDraw(); target->Clear(D2D1::ColorF(0,0));
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ui::DrawBufferWorkbench({target.Get(), write.Get(), body.Get(), label.Get(), brush.Get(), float(dpi)}, state, ui::LayoutBuffer(state, 760));
        Check(SUCCEEDED(target->EndDraw()), "complete buffer draw"); frame.DrawIcon(icon, icon_box);
        std::vector<std::uint32_t> current;
        for (LONG py=y; py<y+size; ++py) for (LONG px=x; px<x+size; ++px) current.push_back(frame.pixels()[py*width+px]);
        if (tick) Check(current == badge, "target badge is unchanged across translation refreshes"); else badge = current;
        Check(frame.Present(window), "present entire layered frame without HWND region");
        Check(GetForegroundWindow() != window, "frame presentation never activates Buffer");
        if (argc == 2 && scenario == 0 && (tick == 0 || tick == 7)) {
          std::filesystem::create_directories(argv[1]);
          Save(frame, std::filesystem::path(argv[1]) / (L"speech-" + std::wstring(state.translation_speech ? L"on" : L"off") +
              L"-" + std::to_wstring(theme) + L"-" + std::to_wstring(dpi) + L".png"));
        }
      }
      for (int column=0; column<width; ++column) Check(frame.pixels()[column] == 0, "no stray drawing above the Buffer chrome");
      if (state.folded) {
        const int corner_column = static_cast<int>(std::ceil(2.5f * scale));
        for (int corner_row = height - static_cast<int>(std::ceil(3 * scale)); corner_row < height; ++corner_row)
          Check(frame.pixels()[corner_row * width + corner_column] == 0, "folded toolbar has no divider crossing its rounded bottom corner");
      }
      unsigned partial = 0;
      for (int i=0; i<width*height; ++i) {
        const auto p = frame.pixels()[i], a = p >> 24U;
        Check((p & 255U) <= a && ((p >> 8U) & 255U) <= a && ((p >> 16U) & 255U) <= a, "valid premultiplied alpha");
        if (a > 0 && a < 255) ++partial;
      }
      Check(frame.pixels()[0] == 0 && partial > 20, "transparent corner and fractional antialiased outer edge");
      if (argc == 2) { std::filesystem::create_directories(argv[1]); Save(frame, std::filesystem::path(argv[1]) / (L"buffer-" + std::to_wstring(scenario) + L"-" + std::to_wstring(theme) + L"-" + std::to_wstring(dpi) + L".png")); }
      std::cout << "DPI " << dpi << " theme " << theme << ": " << partial << " translucent edge pixels; stable badge\n";
    }
    DestroyWindow(window);
    ui::LayeredFrame gdi; Check(gdi.Resize(120,60), "GDI frame");
    ui::FillRectColor(gdi.dc(), {0,0,120,60}, RGB(240,240,240));
    ui::FillRoundRect(gdi.dc(), {10,10,110,50}, 12, RGB(40,40,40)); GdiFlush();
    unsigned blended=0;
    for (int i=0;i<7200;++i) { const auto c=gdi.pixels()[i]&255U; if(c>40&&c<240) ++blended; }
    Check(blended > 20, "shared rounded button painter antialiases curves");
    gdi.RoundAlpha(12); Check(gdi.pixels()[0]==0 && (gdi.pixels()[30*120+60]>>24U)==255, "candidate corner transparent and interior opaque");
    std::cout << "Surface rendering tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
