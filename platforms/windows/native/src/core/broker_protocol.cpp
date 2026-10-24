#include "broker_protocol.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace rimes::windows::core {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

bool IsKnownMessageType(MessageType type) {
  switch (type) {
    case MessageType::kClientHello:
    case MessageType::kBrokerHello:
    case MessageType::kPing:
    case MessageType::kPong:
    case MessageType::kError:
    case MessageType::kOpenInputSession:
    case MessageType::kInputSessionOpened:
    case MessageType::kCloseInputSession:
    case MessageType::kInputSessionClosed:
    case MessageType::kKeyEvent:
    case MessageType::kInputState:
    case MessageType::kControl:
    case MessageType::kControlState:
      return true;
    case MessageType::kInvalid:
      return false;
  }
  return false;
}

bool AreFrameFlagsConsistent(MessageType type, std::uint32_t flags) {
  const bool is_response =
      (flags & static_cast<std::uint32_t>(FrameFlags::kResponse)) != 0;
  const bool is_error =
      (flags & static_cast<std::uint32_t>(FrameFlags::kError)) != 0;
  if (is_error && (!is_response || type != MessageType::kError)) {
    return false;
  }
  if (type == MessageType::kError && !is_error) {
    return false;
  }
  return true;
}

bool IsKnownBrokerErrorCode(BrokerErrorCode code) {
  switch (code) {
    case BrokerErrorCode::kMalformedPayload:
    case BrokerErrorCode::kUnsupportedMessage:
    case BrokerErrorCode::kUnauthorizedClient:
    case BrokerErrorCode::kInternalError:
      return true;
  }
  return false;
}

void AppendU16(std::vector<std::byte>* output, std::uint16_t value) {
  output->push_back(static_cast<std::byte>(value & 0xffU));
  output->push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void AppendU32(std::vector<std::byte>* output, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void AppendU64(std::vector<std::byte>* output, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    output->push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

std::optional<std::uint32_t> Utf16Length(std::string_view value);

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool ReadU16(std::uint16_t* value) {
    if (!CanRead(2)) {
      return false;
    }
    *value = static_cast<std::uint16_t>(ByteAt(offset_)) |
             static_cast<std::uint16_t>(ByteAt(offset_ + 1) << 8U);
    offset_ += 2;
    return true;
  }

  bool ReadU32(std::uint32_t* value) {
    if (!CanRead(4)) {
      return false;
    }
    *value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
      *value |= ByteAt(offset_ + index) << (index * 8U);
    }
    offset_ += 4;
    return true;
  }

  bool ReadU64(std::uint64_t* value) {
    if (!CanRead(8)) {
      return false;
    }
    *value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
      *value |= static_cast<std::uint64_t>(ByteAt(offset_ + index))
                << (index * 8U);
    }
    offset_ += 8;
    return true;
  }

  bool ReadString(std::size_t maximum_bytes, std::string* value) {
    std::uint16_t length = 0;
    if (!ReadU16(&length) || length > maximum_bytes || !CanRead(length)) {
      return false;
    }
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
    value->assign(begin, begin + length);
    offset_ += length;
    return value->find('\0') == std::string::npos && Utf16Length(*value);
  }

  [[nodiscard]] bool AtEnd() const { return offset_ == bytes_.size(); }

 private:
  [[nodiscard]] bool CanRead(std::size_t count) const {
    return count <= bytes_.size() - offset_;
  }

  [[nodiscard]] std::uint32_t ByteAt(std::size_t index) const {
    return std::to_integer<std::uint32_t>(bytes_[index]);
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_ = 0;
};

bool ValidateText(std::string_view value, std::size_t maximum_bytes,
                  const char* field_name, std::string* error) {
  if (value.size() > maximum_bytes ||
      value.size() > std::numeric_limits<std::uint16_t>::max()) {
    SetError(error, std::string(field_name) + " exceeds its wire limit");
    return false;
  }
  if (value.find('\0') != std::string_view::npos) {
    SetError(error, std::string(field_name) + " contains an embedded NUL");
    return false;
  }
  if (!Utf16Length(value)) {
    SetError(error, std::string(field_name) + " is not valid UTF-8");
    return false;
  }
  return true;
}

std::optional<std::uint32_t> Utf16Length(std::string_view value) {
  std::uint32_t units = 0;
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::uint32_t code_point = 0;
    std::size_t width = 0;
    if (first <= 0x7fU) {
      code_point = first;
      width = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      code_point = first & 0x1fU;
      width = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      code_point = first & 0x0fU;
      width = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      code_point = first & 0x07U;
      width = 4;
    } else {
      return std::nullopt;
    }
    if (width > value.size() - index) {
      return std::nullopt;
    }
    for (std::size_t continuation = 1; continuation < width; ++continuation) {
      const auto byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return std::nullopt;
      }
      code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    // Reject overlong forms, UTF-16 surrogate values, and values outside the
    // Unicode scalar range.
    if ((width == 2 && code_point < 0x80U) ||
        (width == 3 && code_point < 0x800U) ||
        (width == 4 && code_point < 0x10000U) ||
        (code_point >= 0xd800U && code_point <= 0xdfffU) ||
        code_point > 0x10ffffU) {
      return std::nullopt;
    }
    units += code_point > 0xffffU ? 2U : 1U;
    index += width;
  }
  return units;
}

void AppendString(std::vector<std::byte>* output, std::string_view value) {
  AppendU16(output, static_cast<std::uint16_t>(value.size()));
  output->insert(
      output->end(), reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

bool ReadDtoPrefix(Reader* reader, std::string* error) {
  std::uint16_t version = 0;
  std::uint16_t reserved = 0;
  if (!reader->ReadU16(&version) || !reader->ReadU16(&reserved)) {
    SetError(error, "truncated DTO prefix");
    return false;
  }
  if (version != kControlDtoVersion) {
    SetError(error, "unsupported DTO version");
    return false;
  }
  if (reserved != 0) {
    SetError(error, "DTO reserved field must be zero");
    return false;
  }
  return true;
}

void AppendDtoPrefix(std::vector<std::byte>* output) {
  AppendU16(output, kControlDtoVersion);
  AppendU16(output, 0);
}

bool EncodeHelloPayload(std::uint32_t process_id, std::uint32_t session_id,
                        std::uint64_t capabilities, std::string_view label,
                        std::size_t maximum_label_bytes, const char* field_name,
                        std::vector<std::byte>* payload, std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  // Windows Session 0 is valid for explicitly enabled automation. Endpoint
  // policy and verified pipe identities decide whether it may connect; this
  // OS identifier is distinct from a non-zero Rime input-session handle.
  if (process_id == 0 || label.empty()) {
    SetError(error, "hello process ID and label must not be zero or empty");
    return false;
  }
  if (!ValidateText(label, maximum_label_bytes, field_name, error)) {
    return false;
  }

  std::vector<std::byte> encoded;
  encoded.reserve(4 + 4 + 4 + 8 + 2 + label.size());
  AppendDtoPrefix(&encoded);
  AppendU32(&encoded, process_id);
  AppendU32(&encoded, session_id);
  AppendU64(&encoded, capabilities);
  AppendString(&encoded, label);
  *payload = std::move(encoded);
  return true;
}

bool DecodeHelloPayload(std::span<const std::byte> payload,
                        std::size_t maximum_label_bytes,
                        std::uint32_t* process_id, std::uint32_t* session_id,
                        std::uint64_t* capabilities, std::string* label,
                        std::string* error) {
  Reader reader(payload);
  if (!ReadDtoPrefix(&reader, error)) {
    return false;
  }
  if (!reader.ReadU32(process_id) || !reader.ReadU32(session_id) ||
      !reader.ReadU64(capabilities) ||
      !reader.ReadString(maximum_label_bytes, label)) {
    SetError(error, "malformed hello DTO");
    return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "hello DTO has trailing bytes");
    return false;
  }
  if (*process_id == 0 || label->empty()) {
    SetError(error, "hello process ID and label must not be zero or empty");
    return false;
  }
  return true;
}

bool EncodeSessionIdPayload(std::uint64_t session_id,
                            std::vector<std::byte>* payload,
                            std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (session_id == 0) {
    SetError(error, "session_id must not be zero");
    return false;
  }
  std::vector<std::byte> encoded;
  encoded.reserve(12);
  AppendDtoPrefix(&encoded);
  AppendU64(&encoded, session_id);
  *payload = std::move(encoded);
  return true;
}

bool DecodeSessionIdPayload(std::span<const std::byte> payload,
                            std::uint64_t* session_id, std::string* error) {
  Reader reader(payload);
  if (!ReadDtoPrefix(&reader, error) || !reader.ReadU64(session_id)) {
    SetError(error, "malformed session DTO");
    return false;
  }
  if (*session_id == 0) {
    SetError(error, "session_id must not be zero");
    return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "session DTO has trailing bytes");
    return false;
  }
  return true;
}

bool ValidateKeyEvent(const KeyEvent& dto, std::string* error) {
  if (dto.session_id == 0 || dto.sequence_id == 0) {
    SetError(error, "key event session_id and sequence_id must not be zero");
    return false;
  }
  if (dto.virtual_key == 0 || dto.virtual_key > 0xffU ||
      dto.scan_code > 0xffffU || dto.repeat_count == 0 ||
      dto.repeat_count > 0xffffU) {
    SetError(error, "key event contains an invalid virtual key or scan code");
    return false;
  }
  if ((dto.modifiers & ~kKnownKeyModifiers) != 0 ||
      (dto.event_flags & ~kKnownKeyEventFlags) != 0) {
    SetError(error, "key event contains unknown flags");
    return false;
  }
  const bool repeat = (dto.event_flags &
                       static_cast<std::uint32_t>(KeyEventFlags::kRepeat)) != 0;
  const bool key_down =
      (dto.event_flags & static_cast<std::uint32_t>(KeyEventFlags::kKeyDown)) !=
      0;
  if (repeat && !key_down) {
    SetError(error, "a repeated key event must be a key-down event");
    return false;
  }
  if (repeat != (dto.repeat_count > 1)) {
    SetError(error, "repeat flag and repeat_count disagree");
    return false;
  }
  if ((dto.event_flags & static_cast<std::uint32_t>(KeyEventFlags::kShiftTap)) != 0) {
    constexpr auto incompatible =
        static_cast<std::uint32_t>(KeyEventFlags::kKeyDown) |
        static_cast<std::uint32_t>(KeyEventFlags::kRepeat) |
        static_cast<std::uint32_t>(KeyEventFlags::kTestOnly) |
        static_cast<std::uint32_t>(KeyEventFlags::kPreservedKey) |
        static_cast<std::uint32_t>(KeyEventFlags::kSystemKey);
    constexpr auto commands = static_cast<std::uint32_t>(KeyModifiers::kControl) |
        static_cast<std::uint32_t>(KeyModifiers::kAlt) |
        static_cast<std::uint32_t>(KeyModifiers::kWindows) |
        static_cast<std::uint32_t>(KeyModifiers::kAltGr);
    if ((dto.virtual_key != 0x10 && dto.virtual_key != 0xa0 && dto.virtual_key != 0xa1) ||
        (dto.event_flags & incompatible) != 0 || (dto.modifiers & commands) != 0) {
      SetError(error, "Shift tap must be a bare real Shift release");
      return false;
    }
  }
  return true;
}

bool ValidateInputState(const InputState& dto, std::string* error) {
  if (dto.session_id == 0 || dto.sequence_id == 0) {
    SetError(error, "input state session_id and sequence_id must not be zero");
    return false;
  }
  if ((dto.state_flags & ~kKnownInputStateFlags) != 0) {
    SetError(error, "input state contains unknown flags");
    return false;
  }
  if (!ValidateText(dto.composition, kMaxCompositionBytes, "composition",
                    error) ||
      !ValidateText(dto.commit_text, kMaxCommitTextBytes, "commit_text",
                    error)) {
    return false;
  }
  if (dto.candidates.size() > kMaxCandidateCount) {
    SetError(error, "candidate count exceeds its wire limit");
    return false;
  }

  const bool composing =
      (dto.state_flags &
       static_cast<std::uint32_t>(InputStateFlags::kComposing)) != 0;
  const bool candidates_visible =
      (dto.state_flags &
       static_cast<std::uint32_t>(InputStateFlags::kCandidatesVisible)) != 0;
  const bool handled = (dto.state_flags & static_cast<std::uint32_t>(
                                              InputStateFlags::kHandled)) != 0;
  const bool modifier_snapshot = (dto.state_flags & static_cast<std::uint32_t>(
      InputStateFlags::kModifierSnapshot)) != 0;
  if (handled && modifier_snapshot) {
    SetError(error, "modifier snapshot cannot own a host key");
    return false;
  }
  const std::uint32_t composition_units =
      Utf16Length(dto.composition).value_or(0);
  if (static_cast<std::uint64_t>(dto.caret_utf16) + dto.selection_length_utf16 >
      composition_units) {
    SetError(error, "composition selection is outside the UTF-16 text range");
    return false;
  }
  if (!composing && (!dto.composition.empty() || dto.caret_utf16 != 0 ||
                     dto.selection_length_utf16 != 0 || candidates_visible ||
                     !dto.candidates.empty())) {
    SetError(error, "non-composing state contains composition data");
    return false;
  }
  if (!handled && !modifier_snapshot && (composing || !dto.commit_text.empty())) {
    SetError(error, "unhandled input state contains a text mutation");
    return false;
  }
  if (candidates_visible != !dto.candidates.empty()) {
    SetError(error, "candidate visibility and candidate data disagree");
    return false;
  }
  if (!candidates_visible &&
      (dto.highlighted_candidate != kNoCandidateSelected ||
       dto.page_start != 0 || dto.page_size != 0 ||
       (dto.state_flags & kCandidatePagingStateFlags))) {
    SetError(error, "hidden candidates contain selection or paging data");
    return false;
  }
  if (candidates_visible) {
    const auto valid_page_key = [&](InputStateFlags up, InputStateFlags down, InputStateFlags paging) {
      const bool previous = (dto.state_flags & static_cast<std::uint32_t>(up)) != 0;
      const bool next = (dto.state_flags & static_cast<std::uint32_t>(down)) != 0;
      return !(previous && next) && ((previous || next) ||
          !(dto.state_flags & static_cast<std::uint32_t>(paging)));
    };
    if (!valid_page_key(InputStateFlags::kMinusPageUp, InputStateFlags::kMinusPageDown, InputStateFlags::kMinusPagingOnly) ||
        !valid_page_key(InputStateFlags::kEqualPageUp, InputStateFlags::kEqualPageDown, InputStateFlags::kEqualPagingOnly)) {
      SetError(error, "candidate paging key metadata is inconsistent");
      return false;
    }
    if (dto.highlighted_candidate != kNoCandidateSelected &&
        dto.highlighted_candidate >= dto.candidates.size()) {
      SetError(error, "highlighted candidate is outside the candidate list");
      return false;
    }
    if (dto.page_size == 0 || dto.page_start >= dto.candidates.size() ||
        static_cast<std::size_t>(dto.page_start) + dto.page_size >
            dto.candidates.size()) {
      SetError(error, "candidate page is outside the candidate list");
      return false;
    }
    if (dto.highlighted_candidate != kNoCandidateSelected &&
        (dto.highlighted_candidate < dto.page_start ||
         dto.highlighted_candidate >=
             static_cast<std::size_t>(dto.page_start) + dto.page_size)) {
      SetError(error, "highlighted candidate is outside the visible page");
      return false;
    }
  }
  for (const Candidate& candidate : dto.candidates) {
    if (candidate.text.empty()) {
      SetError(error, "candidate text must not be empty");
      return false;
    }
    if (!ValidateText(candidate.text, kMaxCandidateTextBytes, "candidate text",
                      error) ||
        !ValidateText(candidate.comment, kMaxCandidateCommentBytes,
                      "candidate comment", error) ||
        !ValidateText(candidate.label, kMaxCandidateLabelBytes,
                      "candidate label", error)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool EncodeFrame(const Frame& frame, std::vector<std::byte>* encoded,
                 std::string* error) {
  if (encoded == nullptr) {
    SetError(error, "encoded output is null");
    return false;
  }
  if (frame.header.protocol_major != kProtocolMajor ||
      frame.header.protocol_minor > kProtocolMinor) {
    SetError(error, "unsupported protocol version");
    return false;
  }
  if (!IsKnownMessageType(frame.header.message_type)) {
    SetError(error, "unknown message type");
    return false;
  }
  if ((frame.header.flags & ~kKnownFrameFlags) != 0) {
    SetError(error, "unknown frame flags");
    return false;
  }
  if (!AreFrameFlagsConsistent(frame.header.message_type, frame.header.flags)) {
    SetError(error, "frame flags are inconsistent with the message type");
    return false;
  }
  if (frame.payload.size() > kMaxPayloadSize) {
    SetError(error, "payload exceeds the maximum frame size");
    return false;
  }
  if (frame.header.payload_size != 0 &&
      frame.header.payload_size != frame.payload.size()) {
    SetError(error, "header payload size does not match the payload");
    return false;
  }

  std::vector<std::byte> output;
  output.reserve(kFrameHeaderSize + frame.payload.size());
  AppendU32(&output, kFrameMagic);
  AppendU16(&output, kFrameHeaderSize);
  AppendU16(&output, frame.header.protocol_major);
  AppendU16(&output, frame.header.protocol_minor);
  AppendU16(&output, static_cast<std::uint16_t>(frame.header.message_type));
  AppendU32(&output, frame.header.flags);
  AppendU32(&output, frame.header.request_id);
  AppendU32(&output, static_cast<std::uint32_t>(frame.payload.size()));
  output.insert(output.end(), frame.payload.begin(), frame.payload.end());
  *encoded = std::move(output);
  return true;
}

HeaderDecodeResult DecodeFrameHeader(std::span<const std::byte> bytes) {
  HeaderDecodeResult result;
  if (bytes.size() < kFrameHeaderSize) {
    return result;
  }

  Reader reader(bytes.first(kFrameHeaderSize));
  std::uint32_t magic = 0;
  std::uint16_t header_size = 0;
  std::uint16_t raw_message_type = 0;
  if (!reader.ReadU32(&magic) || !reader.ReadU16(&header_size) ||
      !reader.ReadU16(&result.header.protocol_major) ||
      !reader.ReadU16(&result.header.protocol_minor) ||
      !reader.ReadU16(&raw_message_type) ||
      !reader.ReadU32(&result.header.flags) ||
      !reader.ReadU32(&result.header.request_id) ||
      !reader.ReadU32(&result.header.payload_size)) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "malformed frame header";
    return result;
  }

  result.header.message_type = static_cast<MessageType>(raw_message_type);
  if (magic != kFrameMagic) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "invalid frame magic";
  } else if (header_size != kFrameHeaderSize) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "unsupported frame header size";
  } else if (result.header.protocol_major != kProtocolMajor ||
             result.header.protocol_minor > kProtocolMinor) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "unsupported protocol version";
  } else if (!IsKnownMessageType(result.header.message_type)) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "unknown message type";
  } else if ((result.header.flags & ~kKnownFrameFlags) != 0) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "unknown frame flags";
  } else if (!AreFrameFlagsConsistent(result.header.message_type,
                                      result.header.flags)) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "frame flags are inconsistent with the message type";
  } else if (result.header.payload_size > kMaxPayloadSize) {
    result.status = DecodeStatus::kInvalidFrame;
    result.error = "payload exceeds the maximum frame size";
  } else {
    result.status = DecodeStatus::kComplete;
  }
  return result;
}

FrameDecodeResult DecodeFrame(std::span<const std::byte> bytes) {
  FrameDecodeResult result;
  const HeaderDecodeResult header_result = DecodeFrameHeader(bytes);
  if (header_result.status != DecodeStatus::kComplete) {
    result.status = header_result.status;
    result.error = header_result.error;
    return result;
  }

  const std::size_t frame_size =
      kFrameHeaderSize + header_result.header.payload_size;
  if (bytes.size() < frame_size) {
    return result;
  }

  result.status = DecodeStatus::kComplete;
  result.frame.header = header_result.header;
  result.frame.payload.assign(bytes.begin() + kFrameHeaderSize,
                              bytes.begin() + frame_size);
  result.bytes_consumed = frame_size;
  return result;
}

bool EncodeClientHello(const ClientHello& dto, std::vector<std::byte>* payload,
                       std::string* error) {
  return EncodeHelloPayload(dto.process_id, dto.session_id, dto.capabilities,
                            dto.client_name, kMaxClientNameBytes, "client_name",
                            payload, error);
}

bool DecodeClientHello(std::span<const std::byte> payload, ClientHello* dto,
                       std::string* error) {
  if (dto == nullptr) {
    SetError(error, "ClientHello output is null");
    return false;
  }
  ClientHello decoded;
  if (!DecodeHelloPayload(payload, kMaxClientNameBytes, &decoded.process_id,
                          &decoded.session_id, &decoded.capabilities,
                          &decoded.client_name, error)) {
    return false;
  }
  *dto = std::move(decoded);
  return true;
}

bool EncodeBrokerHello(const BrokerHello& dto, std::vector<std::byte>* payload,
                       std::string* error) {
  return EncodeHelloPayload(dto.process_id, dto.session_id, dto.capabilities,
                            dto.broker_version, kMaxBrokerVersionBytes,
                            "broker_version", payload, error);
}

bool DecodeBrokerHello(std::span<const std::byte> payload, BrokerHello* dto,
                       std::string* error) {
  if (dto == nullptr) {
    SetError(error, "BrokerHello output is null");
    return false;
  }
  BrokerHello decoded;
  if (!DecodeHelloPayload(payload, kMaxBrokerVersionBytes, &decoded.process_id,
                          &decoded.session_id, &decoded.capabilities,
                          &decoded.broker_version, error)) {
    return false;
  }
  *dto = std::move(decoded);
  return true;
}

bool EncodeErrorResponse(const ErrorResponse& dto,
                         std::vector<std::byte>* payload, std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (!IsKnownBrokerErrorCode(dto.code)) {
    SetError(error, "unknown broker error code");
    return false;
  }
  if (!ValidateText(dto.message, kMaxErrorMessageBytes, "error message",
                    error)) {
    return false;
  }

  std::vector<std::byte> encoded;
  encoded.reserve(4 + 4 + 2 + dto.message.size());
  AppendDtoPrefix(&encoded);
  AppendU32(&encoded, static_cast<std::uint32_t>(dto.code));
  AppendString(&encoded, dto.message);
  *payload = std::move(encoded);
  return true;
}

bool DecodeErrorResponse(std::span<const std::byte> payload, ErrorResponse* dto,
                         std::string* error) {
  if (dto == nullptr) {
    SetError(error, "ErrorResponse output is null");
    return false;
  }
  Reader reader(payload);
  if (!ReadDtoPrefix(&reader, error)) {
    return false;
  }

  std::uint32_t raw_code = 0;
  ErrorResponse decoded;
  if (!reader.ReadU32(&raw_code) ||
      !reader.ReadString(kMaxErrorMessageBytes, &decoded.message)) {
    SetError(error, "malformed error DTO");
    return false;
  }
  switch (static_cast<BrokerErrorCode>(raw_code)) {
    case BrokerErrorCode::kMalformedPayload:
    case BrokerErrorCode::kUnsupportedMessage:
    case BrokerErrorCode::kUnauthorizedClient:
    case BrokerErrorCode::kInternalError:
      decoded.code = static_cast<BrokerErrorCode>(raw_code);
      break;
    default:
      SetError(error, "unknown broker error code");
      return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "error DTO has trailing bytes");
    return false;
  }
  *dto = std::move(decoded);
  return true;
}

bool EncodeOpenInputSession(const OpenInputSession& dto,
                            std::vector<std::byte>* payload,
                            std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (dto.context_id == 0) {
    SetError(error, "context_id must not be zero");
    return false;
  }
  if (!ValidateText(dto.schema_id, kMaxSchemaIdBytes, "schema_id", error)) {
    return false;
  }
  std::vector<std::byte> encoded;
  encoded.reserve(4 + 8 + 2 + dto.schema_id.size());
  AppendDtoPrefix(&encoded);
  AppendU64(&encoded, dto.context_id);
  AppendString(&encoded, dto.schema_id);
  *payload = std::move(encoded);
  return true;
}

bool DecodeOpenInputSession(std::span<const std::byte> payload,
                            OpenInputSession* dto, std::string* error) {
  if (dto == nullptr) {
    SetError(error, "OpenInputSession output is null");
    return false;
  }
  Reader reader(payload);
  OpenInputSession decoded;
  if (!ReadDtoPrefix(&reader, error) || !reader.ReadU64(&decoded.context_id) ||
      !reader.ReadString(kMaxSchemaIdBytes, &decoded.schema_id)) {
    SetError(error, "malformed open-input-session DTO");
    return false;
  }
  if (decoded.context_id == 0) {
    SetError(error, "context_id must not be zero");
    return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "open-input-session DTO has trailing bytes");
    return false;
  }
  *dto = std::move(decoded);
  return true;
}

bool EncodeInputSessionOpened(const InputSessionOpened& dto,
                              std::vector<std::byte>* payload,
                              std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (dto.session_id == 0) {
    SetError(error, "session_id must not be zero");
    return false;
  }
  if (!ValidateText(dto.active_schema_id, kMaxSchemaIdBytes, "active_schema_id",
                    error)) {
    return false;
  }
  std::vector<std::byte> encoded;
  encoded.reserve(4 + 8 + 2 + dto.active_schema_id.size());
  AppendDtoPrefix(&encoded);
  AppendU64(&encoded, dto.session_id);
  AppendString(&encoded, dto.active_schema_id);
  if (dto.has_key_routing) AppendU32(&encoded, dto.ascii_mode ? 1U : 0U);
  *payload = std::move(encoded);
  return true;
}

bool DecodeInputSessionOpened(std::span<const std::byte> payload,
                              InputSessionOpened* dto, std::string* error) {
  if (dto == nullptr) {
    SetError(error, "InputSessionOpened output is null");
    return false;
  }
  Reader reader(payload);
  InputSessionOpened decoded;
  if (!ReadDtoPrefix(&reader, error) || !reader.ReadU64(&decoded.session_id) ||
      !reader.ReadString(kMaxSchemaIdBytes, &decoded.active_schema_id)) {
    SetError(error, "malformed input-session-opened DTO");
    return false;
  }
  if (decoded.session_id == 0) {
    SetError(error, "session_id must not be zero");
    return false;
  }
  if (!reader.AtEnd()) {
    std::uint32_t mode = 0;
    if (!reader.ReadU32(&mode) || mode > 1 || !reader.AtEnd()) {
      SetError(error, "invalid input-session-opened mode extension");
      return false;
    }
    decoded.has_key_routing = true;
    decoded.ascii_mode = mode != 0;
  }
  *dto = std::move(decoded);
  return true;
}

bool EncodeCloseInputSession(const CloseInputSession& dto,
                             std::vector<std::byte>* payload,
                             std::string* error) {
  return EncodeSessionIdPayload(dto.session_id, payload, error);
}

bool DecodeCloseInputSession(std::span<const std::byte> payload,
                             CloseInputSession* dto, std::string* error) {
  if (dto == nullptr) {
    SetError(error, "CloseInputSession output is null");
    return false;
  }
  CloseInputSession decoded;
  if (!DecodeSessionIdPayload(payload, &decoded.session_id, error)) {
    return false;
  }
  *dto = decoded;
  return true;
}

bool EncodeInputSessionClosed(const InputSessionClosed& dto,
                              std::vector<std::byte>* payload,
                              std::string* error) {
  return EncodeSessionIdPayload(dto.session_id, payload, error);
}

bool DecodeInputSessionClosed(std::span<const std::byte> payload,
                              InputSessionClosed* dto, std::string* error) {
  if (dto == nullptr) {
    SetError(error, "InputSessionClosed output is null");
    return false;
  }
  InputSessionClosed decoded;
  if (!DecodeSessionIdPayload(payload, &decoded.session_id, error)) {
    return false;
  }
  *dto = decoded;
  return true;
}

bool EncodeKeyEvent(const KeyEvent& dto, std::vector<std::byte>* payload,
                    std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (!ValidateKeyEvent(dto, error)) {
    return false;
  }
  std::vector<std::byte> encoded;
  encoded.reserve(48);
  AppendDtoPrefix(&encoded);
  AppendU64(&encoded, dto.session_id);
  AppendU64(&encoded, dto.sequence_id);
  AppendU64(&encoded, dto.timestamp_millis);
  AppendU32(&encoded, dto.virtual_key);
  AppendU32(&encoded, dto.scan_code);
  AppendU32(&encoded, dto.repeat_count);
  AppendU32(&encoded, dto.modifiers);
  AppendU32(&encoded, dto.event_flags);
  *payload = std::move(encoded);
  return true;
}

bool DecodeKeyEvent(std::span<const std::byte> payload, KeyEvent* dto,
                    std::string* error) {
  if (dto == nullptr) {
    SetError(error, "KeyEvent output is null");
    return false;
  }
  Reader reader(payload);
  KeyEvent decoded;
  if (!ReadDtoPrefix(&reader, error) || !reader.ReadU64(&decoded.session_id) ||
      !reader.ReadU64(&decoded.sequence_id) ||
      !reader.ReadU64(&decoded.timestamp_millis) ||
      !reader.ReadU32(&decoded.virtual_key) ||
      !reader.ReadU32(&decoded.scan_code) ||
      !reader.ReadU32(&decoded.repeat_count) ||
      !reader.ReadU32(&decoded.modifiers) ||
      !reader.ReadU32(&decoded.event_flags)) {
    SetError(error, "malformed key-event DTO");
    return false;
  }
  if (!reader.AtEnd()) {
    SetError(error, "key-event DTO has trailing bytes");
    return false;
  }
  if (!ValidateKeyEvent(decoded, error)) {
    return false;
  }
  *dto = decoded;
  return true;
}

bool EncodeInputState(const InputState& dto, std::vector<std::byte>* payload,
                      std::string* error) {
  if (payload == nullptr) {
    SetError(error, "payload output is null");
    return false;
  }
  if (!ValidateInputState(dto, error)) {
    return false;
  }

  std::vector<std::byte> encoded;
  encoded.reserve(48 + dto.composition.size() + dto.commit_text.size() +
                  dto.candidates.size() * 16);
  AppendDtoPrefix(&encoded);
  AppendU64(&encoded, dto.session_id);
  AppendU64(&encoded, dto.sequence_id);
  AppendU64(&encoded, dto.revision);
  AppendU32(&encoded, dto.state_flags);
  AppendU32(&encoded, dto.caret_utf16);
  AppendU32(&encoded, dto.selection_length_utf16);
  AppendU16(&encoded, dto.highlighted_candidate);
  AppendU16(&encoded, dto.page_start);
  AppendU16(&encoded, dto.page_size);
  AppendU16(&encoded, static_cast<std::uint16_t>(dto.candidates.size()));
  AppendString(&encoded, dto.composition);
  AppendString(&encoded, dto.commit_text);
  for (const Candidate& candidate : dto.candidates) {
    AppendU32(&encoded, candidate.id);
    AppendString(&encoded, candidate.text);
    AppendString(&encoded, candidate.comment);
    AppendString(&encoded, candidate.label);
  }
  if (encoded.size() > kMaxPayloadSize) {
    SetError(error, "encoded input state exceeds the maximum payload size");
    return false;
  }
  *payload = std::move(encoded);
  return true;
}

bool DecodeInputState(std::span<const std::byte> payload, InputState* dto,
                      std::string* error) {
  if (dto == nullptr) {
    SetError(error, "InputState output is null");
    return false;
  }
  Reader reader(payload);
  InputState decoded;
  std::uint16_t candidate_count = 0;
  if (!ReadDtoPrefix(&reader, error) || !reader.ReadU64(&decoded.session_id) ||
      !reader.ReadU64(&decoded.sequence_id) ||
      !reader.ReadU64(&decoded.revision) ||
      !reader.ReadU32(&decoded.state_flags) ||
      !reader.ReadU32(&decoded.caret_utf16) ||
      !reader.ReadU32(&decoded.selection_length_utf16) ||
      !reader.ReadU16(&decoded.highlighted_candidate) ||
      !reader.ReadU16(&decoded.page_start) ||
      !reader.ReadU16(&decoded.page_size) ||
      !reader.ReadU16(&candidate_count) ||
      !reader.ReadString(kMaxCompositionBytes, &decoded.composition) ||
      !reader.ReadString(kMaxCommitTextBytes, &decoded.commit_text)) {
    SetError(error, "malformed input-state DTO");
    return false;
  }
  if (candidate_count > kMaxCandidateCount) {
    SetError(error, "candidate count exceeds its wire limit");
    return false;
  }
  decoded.candidates.reserve(candidate_count);
  for (std::uint16_t index = 0; index < candidate_count; ++index) {
    Candidate candidate;
    if (!reader.ReadU32(&candidate.id) ||
        !reader.ReadString(kMaxCandidateTextBytes, &candidate.text) ||
        !reader.ReadString(kMaxCandidateCommentBytes, &candidate.comment) ||
        !reader.ReadString(kMaxCandidateLabelBytes, &candidate.label)) {
      SetError(error, "malformed candidate DTO");
      return false;
    }
    decoded.candidates.push_back(std::move(candidate));
  }
  if (!reader.AtEnd()) {
    SetError(error, "input-state DTO has trailing bytes");
    return false;
  }
  if (!ValidateInputState(decoded, error)) {
    return false;
  }
  *dto = std::move(decoded);
  return true;
}

}  // namespace rimes::windows::core
