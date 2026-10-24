#include "fake_tsf.hpp"

#include <algorithm>
#include <new>

namespace rimes::windows::e2e {
namespace {

HRESULT NotImpl() { return E_NOTIMPL; }

}  // namespace

HRESULT STDMETHODCALLTYPE FakeThreadMgr::QueryInterface(REFIID interface_id,
                                                        void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
      InlineIsEqualGUID(interface_id, IID_ITfThreadMgr)) {
    *object = static_cast<ITfThreadMgr*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfKeystrokeMgr)) {
    *object = static_cast<ITfKeystrokeMgr*>(this);
  } else if (ui_enabled && interface_id == IID_ITfUIElementMgr) {
    *object = static_cast<ITfUIElementMgr*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE FakeThreadMgr::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE FakeThreadMgr::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE FakeThreadMgr::Activate(TfClientId* client_id) {
  if (client_id == nullptr) {
    return E_POINTER;
  }
  *client_id = client_id_;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::Deactivate() { return S_OK; }
HRESULT STDMETHODCALLTYPE FakeThreadMgr::CreateDocumentMgr(ITfDocumentMgr**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE
FakeThreadMgr::EnumDocumentMgrs(IEnumTfDocumentMgrs**) {
  return NotImpl();
}
FakeThreadMgr::~FakeThreadMgr() {
  if (ui_element) ui_element->Release();
  if (focus_) focus_->Release();
}

HRESULT FakeThreadMgr::BeginUIElement(ITfUIElement* element, BOOL* show, DWORD* id) {
  if (!element || !show || !id) return E_POINTER;
  if (ui_fail_begin) return E_FAIL;
  if (ui_element) return E_UNEXPECTED;
  ++ui_begin;
  ui_element = element;
  element->AddRef();
  *id = 42;
  *show = ui_show;
  if (on_ui_begin) on_ui_begin();
  return S_OK;
}
HRESULT FakeThreadMgr::UpdateUIElement(DWORD id) {
  if (id != 42 || !ui_element) return E_INVALIDARG;
  ++ui_update;
  ITfCandidateListUIElement* candidates = nullptr;
  if (SUCCEEDED(ui_element->QueryInterface(IID_ITfCandidateListUIElement,
                                           reinterpret_cast<void**>(&candidates)))) {
    if (ui_update == 1) candidates->GetUpdatedFlags(&ui_first_flags);
    candidates->Release();
  }
  if (on_ui_update) on_ui_update();
  return S_OK;
}
HRESULT FakeThreadMgr::EndUIElement(DWORD id) {
  if (id != 42 || !ui_element) return E_INVALIDARG;
  ++ui_end;
  auto* element = ui_element;
  ui_element = nullptr;
  element->Release();
  return S_OK;
}
HRESULT FakeThreadMgr::GetUIElement(DWORD id, ITfUIElement** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  if (id != 42 || !ui_element) return E_INVALIDARG;
  *out = ui_element;
  (*out)->AddRef();
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::GetFocus(ITfDocumentMgr** focus) {
  if (!focus) return E_POINTER;
  *focus = focus_;
  if (focus_) focus_->AddRef();
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::SetFocus(ITfDocumentMgr* focus) {
  if (focus) focus->AddRef();
  if (focus_) focus_->Release();
  focus_ = focus;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeDocumentMgr::QueryInterface(REFIID iid,
                                                          void** object) {
  if (!object) return E_POINTER;
  *object = nullptr;
  if (iid != IID_IUnknown && iid != IID_ITfDocumentMgr) return E_NOINTERFACE;
  *object = static_cast<ITfDocumentMgr*>(this);
  AddRef();
  return S_OK;
}
FakeDocumentMgr::FakeDocumentMgr(ITfContext* context) {
  if (context) Push(context);
}
FakeDocumentMgr::~FakeDocumentMgr() { Pop(TF_POPF_ALL); }
HRESULT STDMETHODCALLTYPE FakeDocumentMgr::Push(ITfContext* context) {
  if (!context) return E_INVALIDARG;
  if (context_) return TF_E_STACKFULL;
  auto* fake = dynamic_cast<FakeContext*>(context);
  if (fake && fake->document_manager_) return E_INVALIDARG;
  context_ = context;
  context_->AddRef();
  if (fake) fake->document_manager_ = this;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeDocumentMgr::Pop(DWORD) {
  if (!context_) return S_FALSE;
  if (auto* fake = dynamic_cast<FakeContext*>(context_))
    fake->document_manager_ = nullptr;
  context_->Release();
  context_ = nullptr;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::AssociateFocus(HWND, ITfDocumentMgr*,
                                                        ITfDocumentMgr**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::IsThreadFocus(BOOL* focus) {
  if (focus != nullptr) {
    *focus = thread_focus ? TRUE : FALSE;
  }
  return S_OK;
}
HRESULT STDMETHODCALLTYPE
FakeThreadMgr::GetFunctionProvider(REFCLSID, ITfFunctionProvider**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE
FakeThreadMgr::EnumFunctionProviders(IEnumTfFunctionProviders**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE
FakeThreadMgr::GetGlobalCompartment(ITfCompartmentMgr**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::AdviseKeyEventSink(TfClientId,
                                                            ITfKeyEventSink*,
                                                            BOOL) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::UnadviseKeyEventSink(TfClientId) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::GetForeground(CLSID*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::TestKeyDown(WPARAM, LPARAM, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::TestKeyUp(WPARAM, LPARAM, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::KeyDown(WPARAM, LPARAM, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::KeyUp(WPARAM, LPARAM, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::GetPreservedKey(ITfContext*,
                                                         const TF_PRESERVEDKEY*,
                                                         GUID*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::IsPreservedKey(REFGUID,
                                                        const TF_PRESERVEDKEY*,
                                                        BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::PreserveKey(TfClientId, REFGUID,
                                                     const TF_PRESERVEDKEY*,
                                                     const WCHAR*, ULONG) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::UnpreserveKey(REFGUID,
                                                       const TF_PRESERVEDKEY*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE
FakeThreadMgr::SetPreservedKeyDescription(REFGUID, const WCHAR*, ULONG) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::GetPreservedKeyDescription(REFGUID,
                                                                    BSTR*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeThreadMgr::SimulatePreservedKey(ITfContext*,
                                                              REFGUID, BOOL*) {
  return NotImpl();
}

FakeContext::FakeContext(FakeDocument* document) noexcept
    : document_(document) {}

HRESULT STDMETHODCALLTYPE FakeContext::QueryInterface(REFIID interface_id,
                                                      void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
      InlineIsEqualGUID(interface_id, IID_ITfContext)) {
    *object = static_cast<ITfContext*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfInsertAtSelection)) {
    *object = static_cast<ITfInsertAtSelection*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfContextComposition)) {
    *object = static_cast<ITfContextComposition*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfContextView)) {
    *object = static_cast<ITfContextView*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfProperty)) {
    *object = static_cast<ITfProperty*>(this);
  } else if (InlineIsEqualGUID(interface_id, IID_ITfInputScope)) {
    *object = static_cast<ITfInputScope*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE FakeContext::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE FakeContext::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE FakeContext::RequestEditSession(
    TfClientId, ITfEditSession* session, DWORD flags, HRESULT* result) {
  if (session == nullptr || result == nullptr) {
    return E_POINTER;
  }
  if ((flags & TF_ES_READWRITE) == TF_ES_READ) {
    ++read_requests;
    if (refuse_read_edits) {
      *result = TF_E_SYNCHRONOUS;
      return S_OK;
    }
  }
  if ((flags & TF_ES_READWRITE) == TF_ES_READWRITE) {
    ++write_requests;
    if (refuse_write_edits) {
      *result = TF_E_LOCKED;
      return S_OK;
    }
  }
  if (defer_edits && ((flags & TF_ES_READWRITE) == TF_ES_READWRITE)) {
    if (flags & TF_ES_SYNC) {
      *result = TF_E_SYNCHRONOUS;
      return S_OK;
    }
    session->AddRef();
    delayed_edits.push_back(session);
    *result = TF_S_ASYNC;
    return S_OK;
  }
  *result = session->DoEditSession(1);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::InWriteSession(TfClientId, BOOL* value) {
  if (value != nullptr) {
    *value = TRUE;
  }
  return S_OK;
}

void FakeContext::DrainEdits() {
  auto edits = std::move(delayed_edits);
  delayed_edits.clear();
  for (auto* edit : edits) {
    edit->DoEditSession(1);
    edit->Release();
  }
}

void FakeContext::TerminateComposition() {
  auto* composition = document_->active_composition;
  auto* sink = document_->composition_sink;
  if (!composition || !sink) return;
  composition->AddRef();
  sink->AddRef();
  sink->OnCompositionTerminated(1, composition);
  composition->EndComposition(1);
  sink->Release();
  composition->Release();
}
HRESULT STDMETHODCALLTYPE FakeContext::GetSelection(TfEditCookie, ULONG, ULONG,
                                                    TF_SELECTION* selection,
                                                    ULONG* fetched) {
  if (!selection || !fetched) return E_POINTER;
  selection->range = new (std::nothrow)
      FakeRange(document_, document_->range_advance && document_->selection_acp >= 0
          ? document_->selection_acp : static_cast<LONG>(document_->text.size()), 0);
  selection->style = {TF_AE_NONE, FALSE};
  *fetched = selection->range ? 1 : 0;
  return selection->range ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE FakeContext::SetSelection(TfEditCookie, ULONG count,
                                                    const TF_SELECTION* selection) {
  if (document_->range_advance && count && selection && selection->range) {
    ITfRangeACP* acp = nullptr;
    if (SUCCEEDED(selection->range->QueryInterface(IID_ITfRangeACP,
            reinterpret_cast<void**>(&acp))) && acp) {
      LONG length = 0;
      acp->GetExtent(&document_->selection_acp, &length);
      acp->Release();
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetStart(TfEditCookie,
                                                ITfRange** range) {
  if (range == nullptr) {
    return E_POINTER;
  }
  *range =
      static_cast<ITfRange*>(new (std::nothrow) FakeRange(document_, 0, 0));
  return *range != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetEnd(TfEditCookie, ITfRange** range) {
  return GetStart(1, range);
}

HRESULT STDMETHODCALLTYPE FakeContext::GetActiveView(ITfContextView** view) {
  if (view == nullptr) {
    return E_POINTER;
  }
  *view = static_cast<ITfContextView*>(this);
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::EnumViews(IEnumTfContextViews**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::GetDocumentMgr(ITfDocumentMgr** manager) {
  if (!manager) return E_POINTER;
  *manager = document_manager_;
  if (document_manager_) document_manager_->AddRef();
  return document_manager_ ? S_OK : S_FALSE;
}
HRESULT STDMETHODCALLTYPE FakeContext::GetStatus(TS_STATUS* value) {
  if (!value) return E_POINTER;
  *value = {0, read_only ? TS_SD_READONLY : 0U};
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetProperty(REFGUID,
                                                   ITfProperty** property) {
  if (property == nullptr) {
    return E_POINTER;
  }
  *property = static_cast<ITfProperty*>(this);
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetAppProperty(REFGUID guid,
                                                      ITfReadOnlyProperty** property) {
  if (!property) return E_POINTER;
  *property = nullptr;
  if (!password_scope || guid != GUID_PROP_INPUTSCOPE) return NotImpl();
  *property = static_cast<ITfProperty*>(this); AddRef(); return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::TrackProperties(const GUID**, ULONG,
                                                       const GUID**, ULONG,
                                                       ITfReadOnlyProperty**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::EnumProperties(IEnumTfProperties**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::CreateRangeBackup(TfEditCookie,
                                                         ITfRange*,
                                                         ITfRangeBackup**) {
  return NotImpl();
}

HRESULT STDMETHODCALLTYPE FakeContext::InsertTextAtSelection(TfEditCookie,
                                                             DWORD flags,
                                                             const WCHAR* text,
                                                             LONG count,
                                                             ITfRange** range) {
  if (range != nullptr) {
    *range = nullptr;
  }
  if ((flags & TF_IAS_QUERYONLY) != 0) {
    const LONG start = static_cast<LONG>(document_->text.size());
    if (range != nullptr) {
      *range = static_cast<ITfRange*>(new (std::nothrow)
                                          FakeRange(document_, start, 0));
      if (*range == nullptr) {
        return E_OUTOFMEMORY;
      }
    }
    return S_OK;
  }
  if (text != nullptr && count > 0) {
    document_->text.append(text, static_cast<std::size_t>(count));
    document_->last_commit.assign(text, static_cast<std::size_t>(count));
  }
  if (range != nullptr) {
    const LONG start =
        static_cast<LONG>(document_->text.size() - (count > 0 ? count : 0));
    *range = static_cast<ITfRange*>(new (std::nothrow)
                                        FakeRange(document_, start, count));
    if (*range == nullptr) {
      return E_OUTOFMEMORY;
    }
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::InsertEmbeddedAtSelection(TfEditCookie,
                                                                 DWORD,
                                                                 IDataObject*,
                                                                 ITfRange**) {
  return NotImpl();
}

HRESULT STDMETHODCALLTYPE FakeContext::StartComposition(
    TfEditCookie, ITfRange* range, ITfCompositionSink* sink,
    ITfComposition** composition) {
  if (composition == nullptr) {
    return E_POINTER;
  }
  *composition = nullptr;
  auto* typed = static_cast<FakeRange*>(range);
  if (typed == nullptr) {
    typed = new (std::nothrow)
        FakeRange(document_, static_cast<LONG>(document_->text.size()), 0);
    if (typed == nullptr) {
      return E_OUTOFMEMORY;
    }
  } else {
    typed->AddRef();
  }
  document_->composing = true;
  auto* created = new (std::nothrow) FakeComposition(document_, typed);
  typed->Release();
  if (created == nullptr) {
    return E_OUTOFMEMORY;
  }
  *composition = created;
  document_->active_composition = created;
  document_->composition_sink = sink;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
FakeContext::EnumCompositions(IEnumITfCompositionView**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::FindComposition(
    TfEditCookie, ITfRange*, IEnumITfCompositionView**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::TakeOwnership(TfEditCookie,
                                                     ITfCompositionView*,
                                                     ITfCompositionSink*,
                                                     ITfComposition**) {
  return NotImpl();
}

HRESULT STDMETHODCALLTYPE FakeContext::GetRangeFromPoint(TfEditCookie,
                                                         const POINT*, DWORD,
                                                         ITfRange**) {
  return NotImpl();
}

HRESULT STDMETHODCALLTYPE FakeContext::GetTextExt(TfEditCookie, ITfRange* range,
                                                  RECT* rect, BOOL* clipped) {
  if (rect == nullptr) {
    return E_POINTER;
  }
  if (document_->refuse_caret) {
    // A failed call's output is unspecified and must not become an anchor.
    if (document_->write_caret_before_failure) {
      *rect = document_->caret_rect;
    }
    return E_FAIL;
  }
  *rect = document_->caret_rect;
  if (document_->range_advance && range) {
    ITfRangeACP* acp = nullptr;
    if (SUCCEEDED(range->QueryInterface(IID_ITfRangeACP,
            reinterpret_cast<void**>(&acp))) && acp) {
      LONG start = 0, length = 0;
      acp->GetExtent(&start, &length);
      acp->Release();
      rect->left += start * document_->range_advance;
      rect->right += (start + length) * document_->range_advance;
    }
  }
  if (clipped != nullptr) {
    *clipped = FALSE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetScreenExt(RECT* rect) {
  if (rect == nullptr) {
    return E_POINTER;
  }
  *rect = document_->caret_rect;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetWnd(HWND* window) {
  if (window != nullptr) {
    *window = document_->view_window;
  }
  return document_->view_window ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetType(GUID* guid) {
  if (guid != nullptr) {
    *guid = GUID_PROP_ATTRIBUTE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::GetContext(ITfContext** context) {
  if (context == nullptr) {
    return E_POINTER;
  }
  *context = static_cast<ITfContext*>(this);
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeContext::EnumRanges(TfEditCookie, IEnumTfRanges**,
                                                  ITfRange*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeContext::GetValue(TfEditCookie, ITfRange*,
                                                VARIANT* value) {
  if (!password_scope) return NotImpl();
  if (!value) return E_POINTER;
  VariantInit(value); value->vt = VT_UNKNOWN;
  value->punkVal = static_cast<ITfInputScope*>(this); AddRef(); return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::GetInputScopes(InputScope** scopes, UINT* count) {
  if (!scopes || !count) return E_POINTER;
  *count = 0;
  *scopes = static_cast<InputScope*>(CoTaskMemAlloc(sizeof(InputScope)));
  if (!*scopes) return E_OUTOFMEMORY;
  **scopes = password_scope ? IS_PASSWORD : IS_DEFAULT; *count = 1; return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::SetValue(TfEditCookie, ITfRange*,
                                                const VARIANT*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::SetValueStore(TfEditCookie, ITfRange*,
                                                     ITfPropertyStore*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::Clear(TfEditCookie, ITfRange*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeContext::FindRange(TfEditCookie, ITfRange*,
                                                 ITfRange**, TfAnchor) {
  return NotImpl();
}
FakeRange::FakeRange(FakeDocument* document, LONG start, LONG length) noexcept
    : document_(document), start_(start), length_(length) {}

HRESULT STDMETHODCALLTYPE FakeRange::QueryInterface(REFIID interface_id,
                                                    void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (InlineIsEqualGUID(interface_id, IID_IUnknown) ||
      InlineIsEqualGUID(interface_id, IID_ITfRange) ||
      InlineIsEqualGUID(interface_id, IID_ITfRangeACP)) {
    *object = static_cast<ITfRangeACP*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE FakeRange::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE FakeRange::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE FakeRange::GetText(TfEditCookie, DWORD, WCHAR* buffer,
                                             ULONG buffer_size, ULONG* copied) {
  const std::wstring& text =
      document_->composing ? document_->composition : document_->text;
  const ULONG n = static_cast<ULONG>(
      (std::min)(static_cast<std::size_t>(buffer_size), text.size()));
  if (buffer != nullptr && n > 0) {
    text.copy(buffer, n);
  }
  if (copied != nullptr) {
    *copied = n;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeRange::SetText(TfEditCookie, DWORD,
                                             const WCHAR* text, LONG count) {
  if (text == nullptr || count < 0) {
    document_->composition.clear();
    return S_OK;
  }
  document_->composition.assign(text, static_cast<std::size_t>(count));
  document_->composing = true;
  length_ = count;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeRange::GetFormattedText(TfEditCookie,
                                                      IDataObject**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::GetEmbedded(TfEditCookie, REFGUID, REFIID,
                                                 IUnknown**) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::InsertEmbedded(TfEditCookie, DWORD,
                                                    IDataObject*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftStart(TfEditCookie, LONG, LONG*,
                                                const TF_HALTCOND*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftEnd(TfEditCookie, LONG, LONG*,
                                              const TF_HALTCOND*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftStartToRange(TfEditCookie, ITfRange*,
                                                       TfAnchor) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftEndToRange(TfEditCookie, ITfRange*,
                                                     TfAnchor) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftStartRegion(TfEditCookie, TfShiftDir,
                                                      BOOL*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::ShiftEndRegion(TfEditCookie, TfShiftDir,
                                                    BOOL*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::IsEmpty(TfEditCookie, BOOL* empty) {
  if (empty != nullptr) {
    *empty = length_ == 0 ? TRUE : FALSE;
  }
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::Collapse(TfEditCookie, TfAnchor anchor) {
  if (anchor == TF_ANCHOR_END) start_ += length_;
  length_ = 0;
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::IsEqualStart(TfEditCookie, ITfRange*,
                                                  TfAnchor, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::IsEqualEnd(TfEditCookie, ITfRange*,
                                                TfAnchor, BOOL*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::CompareStart(TfEditCookie, ITfRange*,
                                                  TfAnchor, LONG*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::CompareEnd(TfEditCookie, ITfRange*,
                                                TfAnchor, LONG*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::AdjustForInsert(TfEditCookie, ULONG,
                                                     BOOL*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeRange::GetGravity(TfGravity*, TfGravity*) {
  return NotImpl();
}
HRESULT STDMETHODCALLTYPE FakeRange::SetGravity(TfEditCookie, TfGravity,
                                                TfGravity) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeRange::Clone(ITfRange** range) {
  if (range == nullptr) {
    return E_POINTER;
  }
  *range = static_cast<ITfRange*>(new (std::nothrow)
                                      FakeRange(document_, start_, length_));
  return *range != nullptr ? S_OK : E_OUTOFMEMORY;
}

HRESULT STDMETHODCALLTYPE FakeRange::GetContext(ITfContext**) {
  return NotImpl();
}

HRESULT STDMETHODCALLTYPE FakeRange::GetExtent(LONG* start, LONG* length) {
  if (start != nullptr) {
    *start = start_;
  }
  if (length != nullptr) {
    *length = length_;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeRange::SetExtent(LONG start, LONG length) {
  start_ = start;
  length_ = length;
  return S_OK;
}

FakeComposition::FakeComposition(FakeDocument* document,
                                 FakeRange* range) noexcept
    : document_(document), range_(range) {
  if (range_ != nullptr) {
    range_->AddRef();
  }
}

FakeComposition::~FakeComposition() {
  if (document_->active_composition == this) {
    document_->active_composition = nullptr;
    document_->composition_sink = nullptr;
  }
  if (range_ != nullptr) {
    range_->Release();
  }
}

HRESULT STDMETHODCALLTYPE FakeComposition::QueryInterface(REFIID interface_id,
                                                          void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (!InlineIsEqualGUID(interface_id, IID_IUnknown) &&
      !InlineIsEqualGUID(interface_id, IID_ITfComposition)) {
    return E_NOINTERFACE;
  }
  *object = static_cast<ITfComposition*>(this);
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE FakeComposition::AddRef() {
  return reference_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE FakeComposition::Release() {
  const ULONG remaining =
      reference_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (remaining == 0) {
    delete this;
  }
  return remaining;
}

HRESULT STDMETHODCALLTYPE FakeComposition::GetRange(ITfRange** range) {
  if (range == nullptr) {
    return E_POINTER;
  }
  if (range_ == nullptr) {
    *range =
        static_cast<ITfRange*>(new (std::nothrow) FakeRange(document_, 0, 0));
    return *range != nullptr ? S_OK : E_OUTOFMEMORY;
  }
  range_->AddRef();
  *range = static_cast<ITfRange*>(range_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeComposition::ShiftStart(TfEditCookie, ITfRange*) {
  return S_OK;
}
HRESULT STDMETHODCALLTYPE FakeComposition::ShiftEnd(TfEditCookie, ITfRange*) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE FakeComposition::EndComposition(TfEditCookie) {
  if (document_ != nullptr) {
    if (document_->active_composition == this) {
      document_->active_composition = nullptr;
      document_->composition_sink = nullptr;
    }
    if (!document_->composition.empty()) {
      document_->last_commit = document_->composition;
      document_->text.append(document_->composition);
    }
    document_->composition.clear();
    document_->composing = false;
  }
  return S_OK;
}

}  // namespace rimes::windows::e2e
