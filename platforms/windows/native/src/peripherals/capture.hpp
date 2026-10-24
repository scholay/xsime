#pragma once
#include <functional>
#include <memory>

#include "media.hpp"

namespace rimes::windows::peripherals {
struct CaptureImage {
  int width = 0, height = 0;
  std::vector<std::uint32_t> pixels;  // top-down, opaque BGRA
  bool Valid() const;
};
RECT CaptureSelection(POINT a, POINT b, int width, int height);
CaptureImage CropCapture(const CaptureImage&, RECT);
CaptureImage LoadCapture(const std::filesystem::path&);
std::string EncodeCapture(const CaptureImage&);
// Both PNG and DIB are published so editors and chat applications receive
// pixels.
ClipboardWriteResult CopyCapture(HWND, const CaptureImage&);
ClipboardWriteResult CopyCapsuleAsset(HWND, Store&, const Json&);
CaptureImage RenderCapture(const CaptureImage&, const Json& operations);

class CaptureWindows {
 public:
  using Screenshot = std::function<std::pair<CaptureImage, RECT>()>;
  using Copy = std::function<ClipboardWriteResult(HWND, Store&, const Json&)>;
  CaptureWindows(Store&, std::function<void()> changed,
                 std::function<std::string()> theme, Screenshot = {}, Copy = {});
  ~CaptureWindows();
  void Start();
  void Edit(const Json&);
  void Protect();
  bool HandleMessage(MSG*);
  HWND SelectionWindow() const;
  HWND StackWindow() const;
  HWND EditorWindow() const;
  bool Preparing() const;
  void RefreshTheme();
  std::set<std::string> ActiveRecords() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace rimes::windows::peripherals
