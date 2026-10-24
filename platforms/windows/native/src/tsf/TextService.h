#pragma once

#include <Windows.h>
#include <msctf.h>

#include <atomic>
#include <bitset>
#include <memory>
#include <string>

#include "BrokerClient.h"
#include "CandidateWindow.h"
#include "CandidateUIElement.h"
#include "InputModeIndicator.h"
#include "TsfInterfaces.h"
#include "candidate_navigation.hpp"

namespace rimes::windows::tsf {

class TextService final : public ITfTextInputProcessorEx,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfThreadMgrEventSink,
                          public ITfTextLayoutSink,
                          public ITfTextEditSink {
 public:
  TextService() noexcept;
  explicit TextService(std::unique_ptr<BrokerClient> broker_client) noexcept;

  TextService(const TextService&) = delete;
  TextService& operator=(const TextService&) = delete;

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id,
                                           void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // ITfTextInputProcessor / ITfTextInputProcessorEx
  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* thread_manager,
                                     TfClientId client_id) override;
  HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* thread_manager,
                                       TfClientId client_id,
                                       DWORD flags) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;

  // ITfKeyEventSink
  HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context,
                                          WPARAM virtual_key, LPARAM key_data,
                                          BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM virtual_key,
                                      LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM virtual_key,
                                        LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM virtual_key,
                                    LPARAM key_data, BOOL* eaten) override;
  HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext* context, REFGUID key,
                                           BOOL* eaten) override;

  // ITfCompositionSink
  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
      TfEditCookie write_cookie, ITfComposition* composition) override;

  // ITfDisplayAttributeProvider
  HRESULT STDMETHODCALLTYPE
  EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumerator) override;
  HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(
      REFGUID guid, ITfDisplayAttributeInfo** info) override;

  HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override;
  HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr*) override;
  HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr*,
                                       ITfDocumentMgr*) override;
  HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext*) override;
  HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext*) override;
  HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext*, TfLayoutCode,
                                           ITfContextView*) override;

  HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext*, TfEditCookie,
                                      ITfEditRecord*) override;

  [[nodiscard]] bool IsBrokerConnected() const noexcept;

 private:
  friend struct CaretRegressionProbe;
  friend struct CandidateUIRegressionProbe;
  ~TextService();

  HRESULT HandleKey(BrokerKeyPhase phase, ITfContext* context,
                    WPARAM virtual_key, LPARAM key_data, BOOL* eaten) noexcept;

  HRESULT ApplyDocumentState(ITfContext* context,
                             const BrokerInputState& state,
                             TfEditCookie cookie = TF_INVALID_EDIT_COOKIE) noexcept;
  void SelectCandidate(std::size_t index) noexcept;
  void PageCandidate(int delta) noexcept;
  void MoveCandidatePage(ITfContext* context, int delta,
                         TfEditCookie cookie = TF_INVALID_EDIT_COOKIE);
  bool IsFocusedContext() const noexcept;
  HRESULT CommitCandidate(ITfContext* context, CandidateNavigation::Selection selection,
                          TfEditCookie cookie = TF_INVALID_EDIT_COOKIE) noexcept;
  std::optional<BrokerInputState> StepCandidatePage(int delta) noexcept;
  bool HandleCandidateKey(BrokerKeyPhase phase, ITfContext* context,
                          WPARAM virtual_key) noexcept;
  HRESULT CommitText(ITfContext* context, const std::wstring& text) noexcept;
  HRESULT UpdateComposition(ITfContext* context,
                            const BrokerInputState& state) noexcept;
  HRESULT EndComposition(ITfContext* context) noexcept;
  void UpdateCandidateWindow(ITfContext* context,
                             const BrokerInputState& state,
                             TfEditCookie cookie = TF_INVALID_EDIT_COOKIE) noexcept;
  void EndCandidateUI() noexcept;
  void PublishCandidateUI(ITfContext*, const CandidateSnapshot&);
  HRESULT CandidateUIAction(CandidateUIElement::Action, UINT);
  RECT QueryCaretRect(ITfContext* context,
                      TfEditCookie cookie = TF_INVALID_EDIT_COOKIE,
                      bool composition_start = false) noexcept;
  RECT candidate_anchor_{};
  void ClearCompositionPointer() noexcept;
  bool BindContext(ITfContext*) noexcept;
  void SetCapture(bool enabled);
  bool capture_active_ = false;
  RECT buffer_caret_{};
  std::uint64_t buffer_opening_ = 0;
  unsigned buffer_route_ = 0;
  bool buffer_focus_follow_ = false;
  std::uint64_t buffer_focus_request_ = 0, buffer_focus_deadline_ = 0;
  void TryResumeBufferFocus();
  void CancelBufferFocusRequest();
  void OnBrokerNotification();
  void RefreshBrokerConnection() noexcept;
  void UpdateInputMode() noexcept;
  void ToggleInputMode() noexcept;
  InputModeIndicator* input_mode_ = nullptr;
  static LRESULT CALLBACK NotificationProcedure(HWND, UINT, WPARAM, LPARAM);
  HWND notification_window_ = nullptr;
  std::uint64_t last_delivery_ = 0;
  std::uint64_t broker_generation_ = 0;
  WPARAM buffer_owned_key_ = 0;
  void RevokeContext() noexcept;
  ITfContext* active_context_ = nullptr;
  IUnknown* context_identity_ = nullptr;
  std::uint64_t context_generation_ = 0;
  std::shared_ptr<std::atomic_bool> edit_valid_;
  DWORD thread_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD edit_sink_cookie_ = TF_INVALID_COOKIE;
  bool own_buffer_edit_ = false;
  DWORD layout_sink_cookie_ = TF_INVALID_COOKIE;
  BrokerInputState last_state_;
  CandidateNavigation candidate_navigation_;
  std::bitset<256> candidate_owned_keys_;
  std::uint64_t candidate_epoch_ = 0;
  bool candidate_compact_paged_ = false;

  std::atomic_ulong reference_count_{1};
  ITfThreadMgr* thread_manager_ = nullptr;
  ITfKeystrokeMgr* keystroke_manager_ = nullptr;
  ITfComposition* composition_ = nullptr;
  TfClientId client_id_ = kNullClientId;
  DWORD activation_flags_ = 0;
  bool key_event_sink_advised_ = false;
  bool ending_composition_ = false;
  std::unique_ptr<BrokerClient> broker_client_;
  CandidateWindow candidate_window_;
  ITfUIElementMgr* ui_manager_ = nullptr;
  CandidateUIElement* candidate_ui_ = nullptr;
  DWORD candidate_ui_id_ = TF_INVALID_UIELEMENTID;
};

}  // namespace rimes::windows::tsf
