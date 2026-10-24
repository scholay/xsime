#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rimes::windows::core {

// The wire format is deliberately independent of compiler packing and host
// endianness. Every integer is encoded little-endian by broker_protocol.cpp.
inline constexpr std::uint32_t kFrameMagic = 0x50574252;  // "RBWP"
inline constexpr std::uint16_t kFrameHeaderSize = 24;
inline constexpr std::uint16_t kProtocolMajor = 2;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::uint32_t kMaxPayloadSize = 1024U * 1024U;
inline constexpr std::uint32_t kMaxFrameSize =
    kFrameHeaderSize + kMaxPayloadSize;

enum class MessageType : std::uint16_t {
  kInvalid = 0,
  kClientHello = 1,
  kBrokerHello = 2,
  kPing = 3,
  kPong = 4,
  kError = 5,
  kOpenInputSession = 10,
  kInputSessionOpened = 11,
  kCloseInputSession = 12,
  kInputSessionClosed = 13,
  kKeyEvent = 14,
  kInputState = 15,
  kControl = 20,
  kControlState = 21,
};

enum class FrameFlags : std::uint32_t {
  kNone = 0,
  kResponse = 1U << 0,
  kError = 1U << 1,
};

inline constexpr std::uint32_t kKnownFrameFlags =
    static_cast<std::uint32_t>(FrameFlags::kResponse) |
    static_cast<std::uint32_t>(FrameFlags::kError);

struct FrameHeader {
  std::uint16_t protocol_major = kProtocolMajor;
  std::uint16_t protocol_minor = kProtocolMinor;
  MessageType message_type = MessageType::kInvalid;
  std::uint32_t flags = 0;
  std::uint32_t request_id = 0;
  std::uint32_t payload_size = 0;
};

struct Frame {
  FrameHeader header;
  std::vector<std::byte> payload;
};

enum class DecodeStatus {
  kComplete,
  kNeedMoreData,
  kInvalidFrame,
};

struct HeaderDecodeResult {
  DecodeStatus status = DecodeStatus::kNeedMoreData;
  FrameHeader header;
  std::string error;
};

struct FrameDecodeResult {
  DecodeStatus status = DecodeStatus::kNeedMoreData;
  Frame frame;
  std::size_t bytes_consumed = 0;
  std::string error;
};

// Encodes one complete frame. The function rejects unsupported protocol
// versions, message types, flags, and payloads larger than kMaxPayloadSize.
bool EncodeFrame(const Frame& frame, std::vector<std::byte>* encoded,
                 std::string* error = nullptr);

// Decodes and validates the fixed header without allocating for its payload.
// Pipe readers should call this before accepting payload_size from a peer.
HeaderDecodeResult DecodeFrameHeader(std::span<const std::byte> bytes);

// Decodes the first complete frame in bytes. bytes may contain following
// frames; bytes_consumed identifies exactly how much belongs to this frame.
FrameDecodeResult DecodeFrame(std::span<const std::byte> bytes);

inline constexpr std::uint16_t kControlDtoVersion = 1;
inline constexpr std::size_t kMaxClientNameBytes = 128;
inline constexpr std::size_t kMaxBrokerVersionBytes = 128;
inline constexpr std::size_t kMaxErrorMessageBytes = 1024;
inline constexpr std::size_t kMaxSchemaIdBytes = 128;
inline constexpr std::size_t kMaxCompositionBytes = 16U * 1024U;
inline constexpr std::size_t kMaxCommitTextBytes = 65535U;
inline constexpr std::size_t kMaxCandidateTextBytes = 1024;
inline constexpr std::size_t kMaxCandidateCommentBytes = 512;
inline constexpr std::size_t kMaxCandidateLabelBytes = 32;
inline constexpr std::size_t kMaxCandidateCount = 64;

// Negotiated separately from the original workbench-control capability. A
// legacy peer never receives pass-through modifier snapshots or offers Shift.
inline constexpr std::uint64_t kModifierSnapshotsCapability = 1ULL << 1;
// Separately negotiated: legacy peers must not receive the session-mode
// extension or kAsciiMode flag they cannot decode.
inline constexpr std::uint64_t kKeyRoutingCapability = 1ULL << 2;
// Pending physical chords need command routing before librime has preedit.
// Old DLLs retain librime's legacy chord composer until they are restarted.
inline constexpr std::uint64_t kPendingChordCapability = 1ULL << 3;
// Last-page metadata is opt-in so an old, still-loaded DLL can decode updates.
inline constexpr std::uint64_t kCandidatePagingCapability = 1ULL << 4;

struct ClientHello {
  std::uint32_t process_id = 0;
  // Windows logon session, including 0 for explicitly permitted automation.
  std::uint32_t session_id = 0;
  std::uint64_t capabilities = 0;
  std::string client_name;
};

struct BrokerHello {
  std::uint32_t process_id = 0;
  // Windows logon session; not a Rime input-session handle.
  std::uint32_t session_id = 0;
  std::uint64_t capabilities = 0;
  std::string broker_version;
};

enum class BrokerErrorCode : std::uint32_t {
  kMalformedPayload = 1,
  kUnsupportedMessage = 2,
  kUnauthorizedClient = 3,
  kInternalError = 4,
};

struct ErrorResponse {
  BrokerErrorCode code = BrokerErrorCode::kInternalError;
  std::string message;
};

// context_id is an opaque, client-generated identifier for a TSF document
// context. It is diagnostic only; the broker allocates the authoritative
// session_id returned by InputSessionOpened.
struct OpenInputSession {
  std::uint64_t context_id = 0;
  std::string schema_id;
};

struct InputSessionOpened {
  std::uint64_t session_id = 0;
  std::string active_schema_id;
  bool has_key_routing = false;
  bool ascii_mode = false;
};

struct CloseInputSession {
  std::uint64_t session_id = 0;
};

struct InputSessionClosed {
  std::uint64_t session_id = 0;
};

enum class KeyModifiers : std::uint32_t {
  kShift = 1U << 0,
  kControl = 1U << 1,
  kAlt = 1U << 2,
  kWindows = 1U << 3,
  kCapsLock = 1U << 4,
  kNumLock = 1U << 5,
  kAltGr = 1U << 6,
};

inline constexpr std::uint32_t kKnownKeyModifiers =
    static_cast<std::uint32_t>(KeyModifiers::kShift) |
    static_cast<std::uint32_t>(KeyModifiers::kControl) |
    static_cast<std::uint32_t>(KeyModifiers::kAlt) |
    static_cast<std::uint32_t>(KeyModifiers::kWindows) |
    static_cast<std::uint32_t>(KeyModifiers::kCapsLock) |
    static_cast<std::uint32_t>(KeyModifiers::kNumLock) |
    static_cast<std::uint32_t>(KeyModifiers::kAltGr);

enum class KeyEventFlags : std::uint32_t {
  kKeyDown = 1U << 0,
  kRepeat = 1U << 1,
  kExtended = 1U << 2,
  kSystemKey = 1U << 3,
  // TSF calls OnTestKey* before OnKey*. A test-only frame must never mutate
  // the authoritative input session; it is a query for whether TSF should eat
  // the subsequent real event.
  kTestOnly = 1U << 4,
  kPreservedKey = 1U << 5,
  // A physically observed, uninterrupted Shift tap, delivered only on its
  // real release. Requires kModifierSnapshotsCapability on both peers.
  kShiftTap = 1U << 6,
};

inline constexpr std::uint32_t kKnownKeyEventFlags =
    static_cast<std::uint32_t>(KeyEventFlags::kKeyDown) |
    static_cast<std::uint32_t>(KeyEventFlags::kRepeat) |
    static_cast<std::uint32_t>(KeyEventFlags::kExtended) |
    static_cast<std::uint32_t>(KeyEventFlags::kSystemKey) |
    static_cast<std::uint32_t>(KeyEventFlags::kTestOnly) |
    static_cast<std::uint32_t>(KeyEventFlags::kPreservedKey) |
    static_cast<std::uint32_t>(KeyEventFlags::kShiftTap);

struct KeyEvent {
  std::uint64_t session_id = 0;
  // Monotonically increasing per session. It lets the TSF client discard a
  // stale response after focus/context changes.
  std::uint64_t sequence_id = 0;
  std::uint64_t timestamp_millis = 0;
  std::uint32_t virtual_key = 0;
  std::uint32_t scan_code = 0;
  std::uint32_t repeat_count = 1;
  std::uint32_t modifiers = 0;
  std::uint32_t event_flags = 0;
};

struct Candidate {
  std::uint32_t id = 0;
  std::string text;
  std::string comment;
  std::string label;
};

enum class InputStateFlags : std::uint32_t {
  kHandled = 1U << 0,
  kComposing = 1U << 1,
  kCandidatesVisible = 1U << 2,
  kBufferCapture = 1U << 3,
  // A real modifier may update librime while remaining a host key. Mutually
  // exclusive with kHandled; this is an authoritative snapshot, not an eat.
  kModifierSnapshot = 1U << 4,
  kAsciiMode = 1U << 5,
  kPendingChord = 1U << 6,
  kLastCandidatePage = 1U << 7,
  kMinusPageUp = 1U << 8,
  kMinusPageDown = 1U << 9,
  kMinusPagingOnly = 1U << 10,
  kEqualPageUp = 1U << 11,
  kEqualPageDown = 1U << 12,
  kEqualPagingOnly = 1U << 13,
};

inline constexpr std::uint32_t kCandidatePageKeyFlags =
    static_cast<std::uint32_t>(InputStateFlags::kMinusPageUp) |
    static_cast<std::uint32_t>(InputStateFlags::kMinusPageDown) |
    static_cast<std::uint32_t>(InputStateFlags::kMinusPagingOnly) |
    static_cast<std::uint32_t>(InputStateFlags::kEqualPageUp) |
    static_cast<std::uint32_t>(InputStateFlags::kEqualPageDown) |
    static_cast<std::uint32_t>(InputStateFlags::kEqualPagingOnly);
inline constexpr std::uint32_t kCandidatePagingStateFlags =
    static_cast<std::uint32_t>(InputStateFlags::kLastCandidatePage) | kCandidatePageKeyFlags;

inline constexpr std::uint32_t kKnownInputStateFlags =
    static_cast<std::uint32_t>(InputStateFlags::kHandled) |
    static_cast<std::uint32_t>(InputStateFlags::kComposing) |
    static_cast<std::uint32_t>(InputStateFlags::kCandidatesVisible) |
    static_cast<std::uint32_t>(InputStateFlags::kBufferCapture) |
    static_cast<std::uint32_t>(InputStateFlags::kModifierSnapshot) |
    static_cast<std::uint32_t>(InputStateFlags::kAsciiMode) |
    static_cast<std::uint32_t>(InputStateFlags::kPendingChord) |
    kCandidatePagingStateFlags;
inline constexpr std::uint16_t kNoCandidateSelected = 0xffffU;

// One atomic response contains every text mutation for a key event. Wire text
// is UTF-8; caret and selection values are UTF-16 code-unit offsets so the TSF
// client can apply them without a lossy coordinate conversion.
struct InputState {
  std::uint64_t session_id = 0;
  std::uint64_t sequence_id = 0;
  std::uint64_t revision = 0;
  std::uint32_t state_flags = 0;
  std::uint32_t caret_utf16 = 0;
  std::uint32_t selection_length_utf16 = 0;
  std::uint16_t highlighted_candidate = kNoCandidateSelected;
  std::uint16_t page_start = 0;
  std::uint16_t page_size = 0;
  std::string composition;
  std::string commit_text;
  std::vector<Candidate> candidates;
};

bool EncodeClientHello(const ClientHello& dto, std::vector<std::byte>* payload,
                       std::string* error = nullptr);
bool DecodeClientHello(std::span<const std::byte> payload, ClientHello* dto,
                       std::string* error = nullptr);

bool EncodeBrokerHello(const BrokerHello& dto, std::vector<std::byte>* payload,
                       std::string* error = nullptr);
bool DecodeBrokerHello(std::span<const std::byte> payload, BrokerHello* dto,
                       std::string* error = nullptr);

bool EncodeErrorResponse(const ErrorResponse& dto,
                         std::vector<std::byte>* payload,
                         std::string* error = nullptr);
bool DecodeErrorResponse(std::span<const std::byte> payload, ErrorResponse* dto,
                         std::string* error = nullptr);

bool EncodeOpenInputSession(const OpenInputSession& dto,
                            std::vector<std::byte>* payload,
                            std::string* error = nullptr);
bool DecodeOpenInputSession(std::span<const std::byte> payload,
                            OpenInputSession* dto,
                            std::string* error = nullptr);
bool EncodeInputSessionOpened(const InputSessionOpened& dto,
                              std::vector<std::byte>* payload,
                              std::string* error = nullptr);
bool DecodeInputSessionOpened(std::span<const std::byte> payload,
                              InputSessionOpened* dto,
                              std::string* error = nullptr);
bool EncodeCloseInputSession(const CloseInputSession& dto,
                             std::vector<std::byte>* payload,
                             std::string* error = nullptr);
bool DecodeCloseInputSession(std::span<const std::byte> payload,
                             CloseInputSession* dto,
                             std::string* error = nullptr);
bool EncodeInputSessionClosed(const InputSessionClosed& dto,
                              std::vector<std::byte>* payload,
                              std::string* error = nullptr);
bool DecodeInputSessionClosed(std::span<const std::byte> payload,
                              InputSessionClosed* dto,
                              std::string* error = nullptr);
bool EncodeKeyEvent(const KeyEvent& dto, std::vector<std::byte>* payload,
                    std::string* error = nullptr);
bool DecodeKeyEvent(std::span<const std::byte> payload, KeyEvent* dto,
                    std::string* error = nullptr);
bool EncodeInputState(const InputState& dto, std::vector<std::byte>* payload,
                      std::string* error = nullptr);
bool DecodeInputState(std::span<const std::byte> payload, InputState* dto,
                      std::string* error = nullptr);

}  // namespace rimes::windows::core
