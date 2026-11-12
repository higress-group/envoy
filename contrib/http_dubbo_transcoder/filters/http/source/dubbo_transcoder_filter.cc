#include "contrib/http_dubbo_transcoder/filters/http/source/dubbo_transcoder_filter.h"

#include "source/common/common/assert.h"
#include "source/common/common/hex.h"
#include "source/common/common/regex.h"
#include "source/common/http/status.h"
#include "source/common/http/utility.h"
#include "source/extensions/filters/http/well_known_names.h"

#include "absl/status/status.h"
#include "absl/strings/str_split.h"

#include "contrib/http_dubbo_transcoder/filters/http/source/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HttpDubboTranscoder {

static const std::string HTTPResponseKey = "result";
static const std::string HTTPResponseErrorKey = "error";
static const std::string HTTPResponseAttachmentKey = "attachment";

static const std::string DubboGenericMethodName = "$invoke";
static const std::string DubboGenericParamTypes =
    "Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/Object;";

static const std::string DubboDefaultProtocolVsersion = "2.7.1";
static const std::string DubboDefaultMethodVersion = "0.0.0";

[[maybe_unused]] static const std::string AttachmentPathKey = "path";
[[maybe_unused]] static const std::string AttachmentGenericKey = "generic";
[[maybe_unused]] static const std::string AttachmentInterfaceKey = "interface";
[[maybe_unused]] static const std::string AttachmentVersionKey = "version";
[[maybe_unused]] static const std::string AttachmentTrueValue = "true";
[[maybe_unused]] static const std::string AttachmentGroupKey = "group";
static const std::string ContentTypeHeaderValue = "application/json; charset=utf-8";
static std::atomic_ulong RequestId{0};

DubboTranscoderConfig::DubboTranscoderConfig(
    const envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder&
        proto_config,
    const std::string& stat_prefix, Stats::Scope& scope)
    : stats_(generateStats(stat_prefix, scope)) {

  disabled_ = proto_config.services_mapping().empty();
  if (disabled_) {
    return;
  }

  request_validate_options_ = proto_config.request_validation_options();

  // build path matcher
  ::google::grpc::transcoding::PathMatcherBuilder<MethodInfoSharedPtr> pmb;
  for (const auto& service : proto_config.services_mapping()) {
    for (const auto& method : service.method_mapping()) {
      MethodInfoSharedPtr method_info =
          createMethodInfo(service.name(), service.version(), service.group(), method);
      pmb.Register(method_info->match_http_method_, method_info->match_pattern_, "", method_info);
    }
  }
  switch (proto_config.url_unescape_spec()) {
    PANIC_ON_PROTO_ENUM_SENTINEL_VALUES;
  case envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
      ALL_CHARACTERS_EXCEPT_RESERVED:
    pmb.SetUrlUnescapeSpec(
        google::grpc::transcoding::UrlUnescapeSpec::kAllCharactersExceptReserved);
    break;
  case envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
      ALL_CHARACTERS_EXCEPT_SLASH:
    pmb.SetUrlUnescapeSpec(google::grpc::transcoding::UrlUnescapeSpec::kAllCharactersExceptSlash);
    break;
  case envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
      ALL_CHARACTERS:
    pmb.SetUrlUnescapeSpec(google::grpc::transcoding::UrlUnescapeSpec::kAllCharacters);
    break;
  }
  path_matcher_ = pmb.Build();
}

MethodInfoSharedPtr DubboTranscoderConfig::createMethodInfo(
    const std::string& service_name, const std::string& service_version,
    const std::string& service_group,
    const envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
        DubboMethodMapping& method_mapping) {
  MethodInfoSharedPtr method_info = std::make_shared<MethodInfo>();
  method_info->service_name_ = service_name;
  method_info->service_version_ = service_version;
  method_info->service_group_ = service_group;
  method_info->name_ = method_mapping.name();

  if (method_mapping.has_path_matcher()) {
    std::string http_method_spec = envoy::extensions::filters::http::http_dubbo_transcoder::v3::
        HttpDubboTranscoder_DubboMethodMapping_MatchHttpMethodSpec_Name(
            method_mapping.path_matcher().match_http_method_spec());
    method_info->match_http_method_ = std::string(absl::StripPrefix(http_method_spec, "ALL_"));
    method_info->match_pattern_ = method_mapping.path_matcher().match_pattern();
  } else {
    // Default matching path: service/method, http method: get.
    std::string http_method_spec = envoy::extensions::filters::http::http_dubbo_transcoder::v3::
        HttpDubboTranscoder_DubboMethodMapping_MatchHttpMethodSpec_Name(
            envoy::extensions::filters::http::http_dubbo_transcoder::v3::
                HttpDubboTranscoder_DubboMethodMapping_MatchHttpMethodSpec_ALL_GET);
    method_info->match_http_method_ = std::string(absl::StripPrefix(http_method_spec, "ALL_"));
    method_info->match_pattern_ = fmt::format("/{}/{}", service_name, method_info->name_);
  }

  ENVOY_LOG(debug, "http method: {}, match pattern {}", method_info->match_http_method_,
            method_info->match_pattern_);

  if (!method_mapping.parameter_mapping().empty()) {
    method_info->parameter_mapping_ = method_mapping.parameter_mapping();
  }

  if (method_mapping.has_passthrough_setting()) {
    const auto& passthrough_setting = method_mapping.passthrough_setting();

    using PassthroughSetting = envoy::extensions::filters::http::http_dubbo_transcoder::v3::
        HttpDubboTranscoder::DubboMethodMapping::PassthroughSetting;
    switch (method_mapping.passthrough_setting().headers_setting_case()) {
    case PassthroughSetting::kPassthroughAllHeaders:
      method_info->passthrough_all_headers_ = passthrough_setting.passthrough_all_headers();
      break;
    case PassthroughSetting::kPassthroughHeaders:
      method_info->passthrough_header_keys_ = passthrough_setting.passthrough_headers().keys();
      break;
    case PassthroughSetting::HEADERS_SETTING_NOT_SET:
      PANIC_DUE_TO_PROTO_UNSET;
    }
  }

  return method_info;
}

std::tuple<absl::Status, Http2DubboTranscoder*>
DubboTranscoderConfig::createTranscoder(Http::RequestHeaderMap& headers) const {
  ASSERT(!disabled_);

  const std::string method(headers.getMethodValue());
  std::string path(headers.getPathValue());
  std::string args;

  const size_t pos = path.find('?');
  if (pos != std::string::npos) {
    args = path.substr(pos + 1);
    path = path.substr(0, pos);
  }

  ENVOY_LOG(debug, "path is {} args is {} method is {}", path, args, method);

  std::vector<google::grpc::transcoding::VariableBinding> grpc_variable_bindings;
  std::string body_field_path;
  auto method_info =
      path_matcher_->Lookup(method, path, args, &grpc_variable_bindings, &body_field_path);
  if (!method_info) {
    return {absl::NotFoundError(fmt::format("Could not resolve {} to a method", path)), nullptr};
  }

  // Convert grpc_transcoding::VariableBinding to local VariableBinding
  std::vector<VariableBinding> variable_bindings;
  variable_bindings.reserve(grpc_variable_bindings.size());
  for (const auto& grpc_binding : grpc_variable_bindings) {
    VariableBinding local_binding;
    local_binding.field_path.assign(grpc_binding.field_path.begin(), grpc_binding.field_path.end());
    local_binding.value = grpc_binding.value;
    variable_bindings.push_back(std::move(local_binding));
  }

  return {absl::OkStatus(), new Http2DubboTranscoder(*method_info, std::move(variable_bindings))};
}

Http2DubboTranscoder::Http2DubboTranscoder(const MethodInfo& method_info,
                                           std::vector<VariableBinding>&& bindings)
    : method_info_(method_info), bindings_(std::move(bindings)) {
  ENVOY_LOG(debug, "method name is {} method args count is {}", method_info_.name_,
            method_info_.parameter_mapping_.size());
};

absl::Status Http2DubboTranscoder::translateDubboToHttp(Buffer::Instance& data) {
  if (data.length() < DUBBO_MAGIC_SIZE || !validateMagicNumber(data)) {
    return absl::UnknownError("Service unachievable or not dubbo message");
  }

  if (data.length() < DUBBO_HEADER_SIZE) {
    data.drain(data.length());
    return absl::DataLossError("Dubbo message data is incomplete");
  }

  int32_t dubbo_data_length = data.peekBEInt<uint32_t>(DUBBO_LENGTH_OFFSET);
  data.drain(DUBBO_HEADER_SIZE);
  std::string response;
  response.reserve(dubbo_data_length);
  response.resize(dubbo_data_length);
  data.copyOut(0, dubbo_data_length, &response[0]);
  data.drain(data.length());
  Hessian2::Decoder decoder(response);
  auto type_value = decoder.decode<int32_t>();
  if (type_value == nullptr) {
    return absl::InternalError("Cannot parse RpcResult type from buffer");
  }

  auto type = static_cast<RpcResponseType>(*type_value);
  auto [has_value, has_exception, has_attachment] = DubboUtility::resolveResponseFlag(type);
  json http_json;
  if (has_exception || has_value) {
    auto response_value = decoder.decode<Object>();
    http_json[has_value ? HTTPResponseKey : HTTPResponseErrorKey] =
        DubboUtility::hessian2Json(response_value.get());
  }

  if (has_attachment) {
    auto attachment_value = decoder.decode<Object>();
    http_json[HTTPResponseAttachmentKey] = DubboUtility::hessian2Json(attachment_value.get());
  }

  data.add(http_json.dump());
  return absl::OkStatus();
}

absl::Status Http2DubboTranscoder::extractTranscoderParameters(Http::RequestHeaderMap& headers,
                                                               Buffer::Instance& body) {
  ASSERT(!current_params_.has_value());

  ENVOY_LOG(debug, "method name is {} method args count is {}", method_info_.name_,
            method_info_.parameter_mapping_.size());

  TypedParamsWithAttachment params_and_attachment;
  params_and_attachment.parameter_types_.resize(method_info_.parameter_mapping_.size());
  params_and_attachment.arguments_.resize(method_info_.parameter_mapping_.size());

  uint8_t current_path_binding_index = 1;
  uint8_t current_params_index = 0;
  using ParameterMapping = envoy::extensions::filters::http::http_dubbo_transcoder::v3::
      HttpDubboTranscoder::DubboMethodMapping::ParameterMapping;
  json body_json;
  if (!body.toString().empty()) {
    // If the exception is not caught, a core dump error may occur
    try {
      body_json = json::parse(body.toString());
    } catch (json::parse_error& e) {
      ENVOY_LOG(warn, "json::parse throw exception : {}", e.what());
    }
  }

  for (const auto& parameter : method_info_.parameter_mapping_) {
    const auto& extract_key = parameter.extract_key();
    ENVOY_LOG(debug, "parameter extract key {}", extract_key);

    std::string parameter_value;
    switch (parameter.extract_key_spec()) {
    case ParameterMapping::ALL_QUERY_PARAMETER: {
      auto params = Http::Utility::QueryParamsMulti::parseQueryString(headers.getPathValue());
      if (params.data().empty()) {
        return absl::InternalError("Error parsing query parameters");
      }
      auto value = params.getFirstValue(extract_key);
      if (!value.has_value()) {
        return absl::NotFoundError(fmt::format("The parameter {} could not be found", extract_key));
      }
      parameter_value = value.value();
      break;
    }
    case ParameterMapping::ALL_HEADER: {
      auto result = headers.get(Http::LowerCaseString(extract_key));
      if (result.empty()) {
        return absl::NotFoundError(fmt::format("The header {} could not be found", extract_key));
      }
      parameter_value = std::string(result[0]->value().getStringView());
      break;
    }
    case ParameterMapping::ALL_PATH: {
      if (current_path_binding_index > bindings_.size()) {
        return absl::OutOfRangeError("Error parsing query parameters");
      }
      parameter_value = bindings_.at(current_path_binding_index - 1).value;
      current_path_binding_index++;
      break;
    }
    case ParameterMapping::ALL_BODY: {
      if (body_json.is_discarded() || body_json.is_null()) {
        return absl::InvalidArgumentError("the body can not be parsed as json or body is empty.");
      } else {
        if (!extract_key.empty()) {
          auto key = body_json.find(extract_key);
          if (key == body_json.end()) {
            return absl::NotFoundError(
                fmt::format("The parameter {} could not be found", extract_key));
          }
          params_and_attachment.arguments_[current_params_index] = *key;
        } else {
          params_and_attachment.arguments_[current_params_index] = body_json;
        }
        params_and_attachment.parameter_types_[current_params_index] = parameter.mapping_type();
      }
      current_params_index++;
      continue;
    }
    default:
      return absl::UnimplementedError("Unsupported types");
    }

    ENVOY_LOG(debug, "parameter extract value {}, type {}", parameter_value,
              parameter.mapping_type());

    absl::optional<json> result =
        DubboUtility::convertStringToTypeValue(parameter_value, parameter.mapping_type());
    if (!result) {
      return absl::InvalidArgumentError(
          "can not transcode the request because the given param not match the type");
    }

    params_and_attachment.arguments_[current_params_index] = result.value();
    params_and_attachment.parameter_types_[current_params_index] = parameter.mapping_type();
    current_params_index++;
  }

  ENVOY_LOG(debug, "method name is {} method args count is {}", method_info_.name_,
            method_info_.parameter_mapping_.size());

  if (method_info_.passthrough_all_headers_.has_value()) {
    if (method_info_.passthrough_all_headers_.value()) {
      headers.iterate(
          [&params_and_attachment](const Http::HeaderEntry& header) -> Http::HeaderMap::Iterate {
            const std::string& header_key = {header.key().getStringView().begin(),
                                             header.key().getStringView().end()};
            params_and_attachment.attachment_[header_key] = header.value().getStringView();
            return Http::HeaderMap::Iterate::Continue;
          });
    }
  } else {
    if (method_info_.passthrough_header_keys_.has_value()) {
      for (const auto& key : method_info_.passthrough_header_keys_.value()) {

        auto result = headers.get(Http::LowerCaseString(key));
        if (result.empty()) {
          return absl::NotFoundError(fmt::format("The header {} could not be found", key));
        }
        params_and_attachment.attachment_[key] = result[0]->value().getStringView();
      }
    } else {
      ENVOY_LOG(debug, "passthrough_header_keys has no value");
    }
  }

  if (!method_info_.service_group_.empty()) {
    params_and_attachment.attachment_[AttachmentGroupKey] = method_info_.service_group_;
  }

  current_params_.emplace(params_and_attachment);
  return absl::OkStatus();
}

void Http2DubboTranscoder::encodeDubboFrameWithGenericCall(Buffer::Instance& data) {
  // Encode dubbo data.
  std::string encoded_data;
  Hessian2::Encoder encoder(encoded_data);

  // Write dubbo header.
  {
    // Write the dubbo protocol: magic-number\type\serialization id\status.
    data.writeBEInt(static_cast<uint16_t>(DUBBO_MAGIC));
    data.writeBEInt(static_cast<uint8_t>(TYPE_INFO));
    data.writeBEInt(static_cast<uint8_t>(DEFAULT_REQUEST_STAT));

    // Write the request id.
    // TODO(zhaobingkun.zbk)
    if (RequestId == ULONG_MAX) {
      RequestId = 0;
    }
    data.writeBEInt(static_cast<int64_t>(++RequestId));
  }

  // Encode dubbo body.
  {
    // Encode: dubbo version\service name\service version.
    encoder.encode(DubboDefaultProtocolVsersion);
    encoder.encode(method_info_.service_name_);
    encoder.encode(method_info_.service_version_.empty() ? DubboDefaultMethodVersion
                                                         : method_info_.service_version_);
    // Encode: method name\parameter type, use generic call.
    encoder.encode(DubboGenericMethodName);
    encoder.encode(DubboGenericParamTypes);

    // Encode: arguments.
    encoder.encode(method_info_.name_);
    if (current_params_.has_value()) {
      auto type_j = json(current_params_.value().parameter_types_);
      DubboUtility::encodeParameterList(type_j, encoder);

      auto params_j = json(current_params_.value().arguments_);
      DubboUtility::encodeParameterList(params_j, encoder);
    } else {
      ENVOY_LOG(debug, "The parameter is empty");
    }
  }

  // Encode attachment.
  {
    if (!current_params_.value().attachment_.is_null()) {
      DubboUtility::json2Hessian(current_params_.value().attachment_, encoder);
    } else {
      encoder.encodeMapBegin("");
      encoder.encodeMapEnd();
    }
  }

  // Write the message data length.
  data.writeBEInt(static_cast<uint32_t>(encoded_data.size()));

  // Write body and attachment data.
  data.add(encoded_data.c_str(), encoded_data.size());

  ENVOY_LOG(debug, "encoded data is {} size is {} ", data.toString(), data.length());
}

inline bool Http2DubboTranscoder::validateMagicNumber(Buffer::Instance& data) {
  return data.peekBEInt<uint16_t>() == DUBBO_MAGIC;
}

void TranscodeFilter::initPerRouteConfig() {
  const auto* route_local =
      Http::Utility::resolveMostSpecificPerFilterConfig<DubboTranscoderConfig>(decoder_callbacks_);

  per_route_config_ = route_local ? route_local : &config_;
}

// transcoder filter impl
Http::FilterHeadersStatus TranscodeFilter::decodeHeaders(Http::RequestHeaderMap& header,
                                                         bool end_stream) {
  initPerRouteConfig();
  if (per_route_config_->disabled()) {
    return Http::FilterHeadersStatus::Continue;
  }

  per_route_config_->stats_.dubbo_req_total_.inc();
  ENVOY_STREAM_LOG(debug, "decodeHeaders:", *decoder_callbacks_);

  auto [status, transcoder] = per_route_config_->createTranscoder(header);
  if (!status.ok()) {
    per_route_config_->stats_.resolve_method_error_.inc();
    ENVOY_STREAM_LOG(debug, "Failed to transcode request headers: {}", *decoder_callbacks_,
                     status.ToString());

    if (status.code() == absl::StatusCode::kNotFound &&
        !per_route_config_->requestValidateOptions().reject_unknown_method()) {
      ENVOY_LOG(debug, "Request is passed through without transcoding because it cannot be mapped "
                       "to a Dubbo method.");
      return Http::FilterHeadersStatus::Continue;
    }
    error_ = true;
    decoder_callbacks_->sendLocalReply(static_cast<Http::Code>(Http::Code::InternalServerError),
                                       status.ToString(), nullptr, absl::nullopt, "");
    return Http::FilterHeadersStatus::StopIteration;
  }

  transcoder_.reset(transcoder);

  if (end_stream) {
    Buffer::OwnedImpl empty_data;
    status = transcoder_->extractTranscoderParameters(header, empty_data);
    if (!status.ok()) {
      per_route_config_->stats_.extract_parameter_error_.inc();
      ENVOY_LOG(warn, "Failed to resolve headers, error is {}", status.ToString());

      // TODO(zhaobingkun.zbk)
      Http::Code http_code = DubboUtility::convertStatusToHttpCode(status.code());
      error_ = true;
      decoder_callbacks_->sendLocalReply(static_cast<Http::Code>(http_code), status.ToString(),
                                         nullptr, absl::nullopt, "");
      return Http::FilterHeadersStatus::StopIteration;
    }

    Buffer::OwnedImpl data;
    transcoder_->encodeDubboFrameWithGenericCall(data);
    decoder_callbacks_->addDecodedData(data, true);
    ENVOY_STREAM_LOG(debug, "sent dubbo frame", *decoder_callbacks_);
  }

  // CONNECT is only needed on the upstream request path; restore the original
  // downstream method before Envoy evaluates CONNECT response tunneling.
  original_method_ = std::string(header.getMethodValue());
  header.setMethod(Http::Headers::get().MethodValues.Connect);
  request_header_ = &header;
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus TranscodeFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  if (!transcoder_ || error_) {
    ENVOY_STREAM_LOG(debug, "Transcoder does not exist or an error occurred, end_stream: {}",
                     *decoder_callbacks_, end_stream);

    return Http::FilterDataStatus::Continue;
  }

  if (!request_body_buffer_) {
    request_body_buffer_ = std::make_unique<Buffer::OwnedImpl>();
  }
  request_body_buffer_->move(data);
  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationAndBuffer;
  }

  const auto status =
      transcoder_->extractTranscoderParameters(*request_header_, *request_body_buffer_);
  if (!status.ok()) {
    per_route_config_->stats_.extract_parameter_error_.inc();
    ENVOY_LOG(warn, "Failed to auto mapping body, error is {}", status.ToString());

    // TODO(zhaobingkun.zbk)
    Http::Code http_code = DubboUtility::convertStatusToHttpCode(status.code());
    error_ = true;
    decoder_callbacks_->sendLocalReply(static_cast<Http::Code>(http_code), status.ToString(),
                                       nullptr, absl::nullopt, "");
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  data.drain(data.length());
  transcoder_->encodeDubboFrameWithGenericCall(data);

  ENVOY_STREAM_LOG(debug, "encoded dubbo frame, length: {}, data: {}", *decoder_callbacks_,
                   data.length(), data.toString());

  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus TranscodeFilter::decodeTrailers(Http::RequestTrailerMap&) {
  return Http::FilterTrailersStatus::Continue;
}

Http::FilterHeadersStatus TranscodeFilter::encodeHeaders(Http::ResponseHeaderMap& headers, bool) {
  if (transcoder_) {
    headers.setReferenceContentType(ContentTypeHeaderValue);
    // Prevent the downstream HTTP/1.1 keep-alive connection from being treated
    // as a CONNECT tunnel after the upstream Dubbo response is encoded.
    if (request_header_ != nullptr && !original_method_.empty()) {
      request_header_->setMethod(original_method_);
    }
  }
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus TranscodeFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  ENVOY_STREAM_LOG(debug, "Recieve data from remote {} length is {} end_stream is {}",
                   *decoder_callbacks_, data.toString(), data.length(), end_stream);

  if (transcoder_) {
    absl::Status status = transcoder_->translateDubboToHttp(data);
    switch (status.code()) {
    case absl::StatusCode::kUnknown:
      per_route_config_->stats_.response_protocol_error_.inc();
      break;
    case absl::StatusCode::kDataLoss:
      per_route_config_->stats_.response_incomplete_.inc();
      break;
    case absl::StatusCode::kInternal:
      per_route_config_->stats_.response_type_error_.inc();
      break;
    case absl::StatusCode::kOk:
      per_route_config_->stats_.response_success_.inc();
      break;
    default:
      break;
    }
    if (status.code() != absl::StatusCode::kOk && status.code() != absl::StatusCode::kUnknown) {
      ENVOY_STREAM_LOG(debug, "translateDubboToHttp failed, faliled reason {}", *decoder_callbacks_,
                       status.message());
      data.add(status.message());
    }
  }

  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus TranscodeFilter::encodeTrailers(Http::ResponseTrailerMap&) {
  if (transcoder_) {
    transcoder_.reset();
  }

  return Http::FilterTrailersStatus::Continue;
}

} // namespace HttpDubboTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
