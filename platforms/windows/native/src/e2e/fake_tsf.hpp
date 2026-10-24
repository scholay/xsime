#pragma once

#include <Windows.h>
#include <msctf.h>
#include <textstor.h>
#include <inputscope.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace rimes::windows::e2e {

struct FakeDocument {
  std::wstring text;
  std::wstring last_commit;
  std::wstring composition;
  bool composing = false;
  // Non-owning pointers, valid only while the fake host has a composition.
  ITfComposition* active_composition = nullptr;
  ITfCompositionSink* composition_sink = nullptr;
  RECT caret_rect{120, 180, 122, 204};
  // Opt-in text geometry: each ACP advances by this many pixels. This makes
  // the selection caret and a collapsed composition start distinguishable.
  LONG range_advance = 0;
  LONG selection_acp = -1;
  // Chromium-family hosts can refuse every caret source while composing.
  // When set, GetTextExt reports failure so cached candidate placement is used.
  bool refuse_caret = false;
  bool write_caret_before_failure = false;
  HWND view_window = nullptr;
};

class FakeThreadMgr final : public ITfThreadMgr, public ITfKeystrokeMgr,
                            public ITfUIElementMgr {
 public:
  FakeThreadMgr() noexcept = default;
  bool thread_focus = true;
  bool ui_enabled = false, ui_show = false, ui_fail_begin = false;
  unsigned ui_begin = 0, ui_update = 0, ui_end = 0;
  DWORD ui_first_flags = 0;
  ITfUIElement* ui_element = nullptr;
  std::function<void()> on_ui_begin;
  std::function<void()> on_ui_update;
  HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement*, BOOL*, DWORD*) override;
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD) override;
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override;
  HRESULT STDMETHODCALLTYPE GetUIElement(DWORD, ITfUIElement**) override;
  HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements**) override { return E_NOTIMPL; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE Activate(TfClientId* client_id) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;
  HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr**) override;
  HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs**) override;
  HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr**) override;
  HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr*) override;
  HRESULT STDMETHODCALLTYPE AssociateFocus(HWND, ITfDocumentMgr*,
                                           ITfDocumentMgr**) override;
  HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID,
                                                ITfFunctionProvider**) override;
  HRESULT STDMETHODCALLTYPE
  EnumFunctionProviders(IEnumTfFunctionProviders**) override;
  HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr**) override;

  HRESULT STDMETHODCALLTYPE AdviseKeyEventSink(TfClientId, ITfKeyEventSink*,
                                               BOOL) override;
  HRESULT STDMETHODCALLTYPE UnadviseKeyEventSink(TfClientId) override;
  HRESULT STDMETHODCALLTYPE GetForeground(CLSID*) override;
  HRESULT STDMETHODCALLTYPE TestKeyDown(WPARAM, LPARAM, BOOL*) override;
  HRESULT STDMETHODCALLTYPE TestKeyUp(WPARAM, LPARAM, BOOL*) override;
  HRESULT STDMETHODCALLTYPE KeyDown(WPARAM, LPARAM, BOOL*) override;
  HRESULT STDMETHODCALLTYPE KeyUp(WPARAM, LPARAM, BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetPreservedKey(ITfContext*, const TF_PRESERVEDKEY*,
                                            GUID*) override;
  HRESULT STDMETHODCALLTYPE IsPreservedKey(REFGUID, const TF_PRESERVEDKEY*,
                                           BOOL*) override;
  HRESULT STDMETHODCALLTYPE PreserveKey(TfClientId, REFGUID,
                                        const TF_PRESERVEDKEY*, const WCHAR*,
                                        ULONG) override;
  HRESULT STDMETHODCALLTYPE UnpreserveKey(REFGUID,
                                          const TF_PRESERVEDKEY*) override;
  HRESULT STDMETHODCALLTYPE SetPreservedKeyDescription(REFGUID, const WCHAR*,
                                                       ULONG) override;
  HRESULT STDMETHODCALLTYPE GetPreservedKeyDescription(REFGUID, BSTR*) override;
  HRESULT STDMETHODCALLTYPE SimulatePreservedKey(ITfContext*, REFGUID,
                                                 BOOL*) override;

 private:
  ~FakeThreadMgr();
  ITfDocumentMgr* focus_ = nullptr;
  std::atomic_ulong reference_count_{1};
  TfClientId client_id_ = 1;
};

class FakeDocumentMgr final : public ITfDocumentMgr {
 public:
  explicit FakeDocumentMgr(ITfContext* context);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto refs = --refs_;
    if (!refs) delete this;
    return refs;
  }
  HRESULT STDMETHODCALLTYPE CreateContext(TfClientId, DWORD, IUnknown*,
                                          ITfContext**, TfEditCookie*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Push(ITfContext*) override;
  HRESULT STDMETHODCALLTYPE Pop(DWORD) override;
  HRESULT STDMETHODCALLTYPE GetTop(ITfContext** context) override {
    if (!context) return E_POINTER;
    *context = context_;
    if (context_) context_->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBase(ITfContext** context) override {
    return GetTop(context);
  }
  HRESULT STDMETHODCALLTYPE EnumContexts(IEnumTfContexts**) override {
    return E_NOTIMPL;
  }
 private:
  ~FakeDocumentMgr();
  std::atomic_ulong refs_{1};
  ITfContext* context_ = nullptr;
};

class FakeContext final : public ITfContext,
                          public ITfInsertAtSelection,
                          public ITfContextComposition,
                          public ITfContextView,
                          public ITfProperty, public ITfInputScope {
 public:
  explicit FakeContext(FakeDocument* document) noexcept;
  bool defer_edits = false, read_only = false;
  bool password_scope = false;
  bool refuse_write_edits = false;
  bool refuse_read_edits = false;
  unsigned read_requests = 0;
  unsigned write_requests = 0;
  void DrainEdits();
  void TerminateComposition();
  std::vector<ITfEditSession*> delayed_edits;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE RequestEditSession(TfClientId, ITfEditSession*,
                                               DWORD, HRESULT*) override;
  HRESULT STDMETHODCALLTYPE InWriteSession(TfClientId, BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetSelection(TfEditCookie, ULONG, ULONG,
                                         TF_SELECTION*, ULONG*) override;
  HRESULT STDMETHODCALLTYPE SetSelection(TfEditCookie, ULONG,
                                         const TF_SELECTION*) override;
  HRESULT STDMETHODCALLTYPE GetStart(TfEditCookie, ITfRange**) override;
  HRESULT STDMETHODCALLTYPE GetEnd(TfEditCookie, ITfRange**) override;
  HRESULT STDMETHODCALLTYPE GetActiveView(ITfContextView**) override;
  HRESULT STDMETHODCALLTYPE EnumViews(IEnumTfContextViews**) override;
  HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) override;
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS*) override;
  HRESULT STDMETHODCALLTYPE GetProperty(REFGUID, ITfProperty**) override;
  HRESULT STDMETHODCALLTYPE GetAppProperty(REFGUID,
                                           ITfReadOnlyProperty**) override;
  HRESULT STDMETHODCALLTYPE TrackProperties(const GUID**, ULONG, const GUID**,
                                            ULONG,
                                            ITfReadOnlyProperty**) override;
  HRESULT STDMETHODCALLTYPE EnumProperties(IEnumTfProperties**) override;
  HRESULT STDMETHODCALLTYPE CreateRangeBackup(TfEditCookie, ITfRange*,
                                              ITfRangeBackup**) override;

  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(TfEditCookie, DWORD,
                                                  const WCHAR*, LONG,
                                                  ITfRange**) override;
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(TfEditCookie, DWORD,
                                                      IDataObject*,
                                                      ITfRange**) override;

  HRESULT STDMETHODCALLTYPE StartComposition(TfEditCookie, ITfRange*,
                                             ITfCompositionSink*,
                                             ITfComposition**) override;
  HRESULT STDMETHODCALLTYPE
  EnumCompositions(IEnumITfCompositionView**) override;
  HRESULT STDMETHODCALLTYPE FindComposition(TfEditCookie, ITfRange*,
                                            IEnumITfCompositionView**) override;
  HRESULT STDMETHODCALLTYPE TakeOwnership(TfEditCookie, ITfCompositionView*,
                                          ITfCompositionSink*,
                                          ITfComposition**) override;

  HRESULT STDMETHODCALLTYPE GetRangeFromPoint(TfEditCookie, const POINT*, DWORD,
                                              ITfRange**) override;
  HRESULT STDMETHODCALLTYPE GetTextExt(TfEditCookie, ITfRange*, RECT*,
                                       BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetScreenExt(RECT*) override;
  HRESULT STDMETHODCALLTYPE GetWnd(HWND*) override;

  HRESULT STDMETHODCALLTYPE GetType(GUID*) override;
  HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) override;
  HRESULT STDMETHODCALLTYPE EnumRanges(TfEditCookie, IEnumTfRanges**,
                                       ITfRange*) override;
  HRESULT STDMETHODCALLTYPE GetValue(TfEditCookie, ITfRange*,
                                     VARIANT*) override;
  HRESULT STDMETHODCALLTYPE SetValue(TfEditCookie, ITfRange*,
                                     const VARIANT*) override;
  HRESULT STDMETHODCALLTYPE SetValueStore(TfEditCookie, ITfRange*,
                                          ITfPropertyStore*) override;
  HRESULT STDMETHODCALLTYPE Clear(TfEditCookie, ITfRange*) override;
  HRESULT STDMETHODCALLTYPE FindRange(TfEditCookie, ITfRange*, ITfRange**,
                                      TfAnchor) override;

  FakeDocument* document() noexcept { return document_; }
  HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope** scopes, UINT* count) override;
  HRESULT STDMETHODCALLTYPE GetPhrase(BSTR**, UINT*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetRegularExpression(BSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSRGS(BSTR*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetXML(BSTR*) override { return E_NOTIMPL; }

 private:
  friend class FakeDocumentMgr;
  ~FakeContext() = default;
  std::atomic_ulong reference_count_{1};
  FakeDocument* document_;
  ITfDocumentMgr* document_manager_ = nullptr;  // Non-owning stack membership.
};

class FakeRange final : public ITfRangeACP {
 public:
  FakeRange(FakeDocument* document, LONG start, LONG length) noexcept;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE GetText(TfEditCookie, DWORD, WCHAR*, ULONG,
                                    ULONG*) override;
  HRESULT STDMETHODCALLTYPE SetText(TfEditCookie, DWORD, const WCHAR*,
                                    LONG) override;
  HRESULT STDMETHODCALLTYPE GetFormattedText(TfEditCookie,
                                             IDataObject**) override;
  HRESULT STDMETHODCALLTYPE GetEmbedded(TfEditCookie, REFGUID, REFIID,
                                        IUnknown**) override;
  HRESULT STDMETHODCALLTYPE InsertEmbedded(TfEditCookie, DWORD,
                                           IDataObject*) override;
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, LONG, LONG*,
                                       const TF_HALTCOND*) override;
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, LONG, LONG*,
                                     const TF_HALTCOND*) override;
  HRESULT STDMETHODCALLTYPE ShiftStartToRange(TfEditCookie, ITfRange*,
                                              TfAnchor) override;
  HRESULT STDMETHODCALLTYPE ShiftEndToRange(TfEditCookie, ITfRange*,
                                            TfAnchor) override;
  HRESULT STDMETHODCALLTYPE ShiftStartRegion(TfEditCookie, TfShiftDir,
                                             BOOL*) override;
  HRESULT STDMETHODCALLTYPE ShiftEndRegion(TfEditCookie, TfShiftDir,
                                           BOOL*) override;
  HRESULT STDMETHODCALLTYPE IsEmpty(TfEditCookie, BOOL*) override;
  HRESULT STDMETHODCALLTYPE Collapse(TfEditCookie, TfAnchor) override;
  HRESULT STDMETHODCALLTYPE IsEqualStart(TfEditCookie, ITfRange*, TfAnchor,
                                         BOOL*) override;
  HRESULT STDMETHODCALLTYPE IsEqualEnd(TfEditCookie, ITfRange*, TfAnchor,
                                       BOOL*) override;
  HRESULT STDMETHODCALLTYPE CompareStart(TfEditCookie, ITfRange*, TfAnchor,
                                         LONG*) override;
  HRESULT STDMETHODCALLTYPE CompareEnd(TfEditCookie, ITfRange*, TfAnchor,
                                       LONG*) override;
  HRESULT STDMETHODCALLTYPE AdjustForInsert(TfEditCookie, ULONG,
                                            BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetGravity(TfGravity*, TfGravity*) override;
  HRESULT STDMETHODCALLTYPE SetGravity(TfEditCookie, TfGravity,
                                       TfGravity) override;
  HRESULT STDMETHODCALLTYPE Clone(ITfRange**) override;
  HRESULT STDMETHODCALLTYPE GetContext(ITfContext**) override;
  HRESULT STDMETHODCALLTYPE GetExtent(LONG*, LONG*) override;
  HRESULT STDMETHODCALLTYPE SetExtent(LONG, LONG) override;

 private:
  ~FakeRange() = default;
  std::atomic_ulong reference_count_{1};
  FakeDocument* document_;
  LONG start_ = 0;
  LONG length_ = 0;
};

class FakeComposition final : public ITfComposition {
 public:
  FakeComposition(FakeDocument* document, FakeRange* range) noexcept;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE GetRange(ITfRange** range) override;
  HRESULT STDMETHODCALLTYPE ShiftStart(TfEditCookie, ITfRange*) override;
  HRESULT STDMETHODCALLTYPE ShiftEnd(TfEditCookie, ITfRange*) override;
  HRESULT STDMETHODCALLTYPE EndComposition(TfEditCookie) override;

 private:
  ~FakeComposition();
  std::atomic_ulong reference_count_{1};
  FakeDocument* document_;
  FakeRange* range_;
};

}  // namespace rimes::windows::e2e
