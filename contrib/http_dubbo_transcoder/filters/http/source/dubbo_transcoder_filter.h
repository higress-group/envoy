#pragma once

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "contrib/envoy/extensions/filters/http/http_dubbo_transcoder/v3/http_dubbo_transcoder.pb.h"
#include "contrib/envoy/extensions/filters/http/http_dubbo_transcoder/v3/http_dubbo_transcoder.pb.validate.h"
#include "contrib/http_dubbo_transcoder/filters/http/source/transcoder.h"
#include "contrib/http_dubbo_transcoder/filters/http/source/utility.h"

#include "envoy/api/api.h"
#include "envoy/http/filter.h"
#include "envoy/type/matcher/v3/regex.pb.h"

#include "source/common/common/logger_impl.h"
#include "source/common/common/logger.h"
#include "source/common/common/regex.h"
#include "source/common/http/codes.h"
#include "source/common/http/header_map_impl.h"

#include "grpc_transcoding/path_matcher.h"

#include "hessian2/basic_codec/object_codec.hpp"
#include "hessian2/codec.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HttpDubboTranscoder {

class Http2DubboTranscoder;

/**
 * All http_dubbo_transcoder stats.
 */
#define ALL_HTTP_DUBBO_TRANSCODER_STATS(COUNTER)                                                   \
  COUNTER(response_protocol_error)                                                                 \
  COUNTER(response_incomplete)                                                                     \
  COUNTER(response_type_error)                                                                     \
  COUNTER(extract_parameter_error)                                                                 \
  COUNTER(resolve_method_error)                                                                    \
  COUNTER(response_success)                                                                        \
  COUNTER(dubbo_req_total)

struct HttpDubboTranscoderStats {
  ALL_HTTP_DUBBO_TRANSCODER_STATS(GENERATE_COUNTER_STRUCT)
};

/***
 * transcoder config
 */
class DubboTranscoderConfig : public Router::RouteSpecificFilterConfig,
                              public Logger::Loggable<Logger::Id::config> {
public:
  /***
   * resolve the global enable falg in the config
   */
  DubboTranscoderConfig(
      const envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder&
          config,
      const std::string& stat_prefix, Stats::Scope& scope);

  /***
   * this function will create the corresponding transcoder acccording to the
   * headers, mainly according to the content-type field
   *
   * @return nullptr if the any thing wrong when create transcoder
   */
  std::tuple<absl::Status, Http2DubboTranscoder*>
  createTranscoder(Http::RequestHeaderMap& headers) const;

  MethodInfoSharedPtr
  createMethodInfo(const std::string& service_name, const std::string& service_version,
                   const std::string& service_group,
                   const envoy::extensions::filters::http::http_dubbo_transcoder::v3::
                       HttpDubboTranscoder::DubboMethodMapping& method_mapping);

  /***
   * wether enable the transcoder
   */
  bool disabled() const { return disabled_; }

  const envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
      RequestValidateOptions&
      requestValidateOptions() const {
    return request_validate_options_;
  }

  HttpDubboTranscoderStats stats_;

private:
  HttpDubboTranscoderStats generateStats(const std::string& prefix, Stats::Scope& scope) {
    return HttpDubboTranscoderStats{
        ALL_HTTP_DUBBO_TRANSCODER_STATS(POOL_COUNTER_PREFIX(scope, prefix))};
  }

  bool disabled_{false};
  envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder::
      RequestValidateOptions request_validate_options_;
  google::grpc::transcoding::PathMatcherPtr<MethodInfoSharedPtr> path_matcher_;
};

using DubboTranscoderConfigSharedPtr = std::shared_ptr<DubboTranscoderConfig>;

/***
 * the error maybe occured while peoccsing the header or body
 * 0 means no error
 */
enum class ParamsErrorCode : int8_t {
  OK = 0,
  CountError = 1,
  TypeError = 2,
  ParseError = 4,
  MethodNotFound = 5,
  OthersError = 7,
};

constexpr uint64_t DUBBO_HEADER_SIZE = 16;
constexpr uint64_t DUBBO_MAGIC_SIZE = 2;
constexpr uint64_t DUBBO_TYPE_SIZE = 1;
constexpr uint64_t DUBBO_STATE_SIZE = 1;
constexpr uint64_t DUBBO_REQID_SIZE = 8;
constexpr uint64_t DUBBO_PACKETLEN_SIZE = 4;
constexpr uint16_t DUBBO_MAGIC = 0xdabb;
constexpr uint64_t DUBBO_LENGTH_OFFSET = 12;
constexpr uint8_t DEFAULT_REQUEST_STAT = 0;
constexpr int64_t DEFAULT_REQUEST_ID = 1;
/**
 *
 * | req or response | 2 way | event | Serializtion |
 * |       1         |    1  |    0  |      2       |
 * |       1         |    1  |    0  |    00010     |
 * more details:
 * https://dubbo.apache.org/en/blog/2018/10/05/introduction-to-the-dubbo-protocol/
 */
constexpr uint8_t TYPE_INFO = 0xc2;

// this type point to the state_ field in the Header struct
enum class ResponseStatus : uint8_t {
  Ok = 20,
  ClientTimeout = 30,
  ServerTimeout = 31,
  BadRequest = 40,
  BadResponse = 50,
  ServiceNotFound = 60,
  ServiceError = 70,
  ServerError = 80,
  ClientError = 90,
  ServerThreadpoolExhaustedError = 100,
};

/***
 * Rpc response represent used by DubboDecoder
 */
struct RpcResponse {
  std::string body_;
  Envoy::Http::Code code_;
};

using RpcResponsePtr = std::unique_ptr<RpcResponse>;

/***
 * this class transcode the http request to dubbo request
 * the transcode support http2Dubbo specification
 * the transcode split into 2 condition:
 * 1. service map (one path corresponding to a set methods of a service)
 * 2. method map (one path corresponding to one method)
 */
class Http2DubboTranscoder : public Logger::Loggable<Logger::Id::filter> {
public:
  /***
   * @param method_info_vec the correspond methodinfo of the request,come from sharedPtr of
   * pathmatcher
   * @param config the transcoder config
   */
  Http2DubboTranscoder(const MethodInfo& method_info, std::vector<VariableBinding>&& bindings);

  std::string getName() const { return "http_dubbo_transcoder"; }

  absl::Status translateDubboToHttp(Buffer::Instance& data);
  absl::Status extractTranscoderParameters(Http::RequestHeaderMap& headers, Buffer::Instance& body);

  void encodeDubboFrameWithGenericCall(Buffer::Instance& data);

private:
  bool validateMagicNumber(Buffer::Instance& data);

  struct TypedParamsWithAttachment {
    std::vector<std::string> parameter_types_;
    std::vector<json> arguments_;
    nlohmann::json attachment_;
  };

  const MethodInfo& method_info_;
  const std::vector<VariableBinding> bindings_;
  Buffer::OwnedImpl request_buffer_{};
  absl::optional<TypedParamsWithAttachment> current_params_;
};

using Http2DubboTranscoderPtr = std::unique_ptr<Http2DubboTranscoder>;

/***
 * Transcoder Filter
 */
class TranscodeFilter : public Http::StreamFilter, public Logger::Loggable<Logger::Id::filter> {
public:
  TranscodeFilter(DubboTranscoderConfig& config) : config_(config){};
  // Http::StreamFilterBase
  void onDestroy() override{};

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override;
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    decoder_callbacks_ = &callbacks;
  }

  // Http::StreamEncoderFilter
  Http::Filter1xxHeadersStatus encode1xxHeaders(Http::ResponseHeaderMap&) override {
    return Http::Filter1xxHeadersStatus::Continue;
  }
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers, bool) override;
  Http::FilterDataStatus encodeData(Buffer::Instance&, bool) override;
  Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap&) override;
  Http::FilterMetadataStatus encodeMetadata(Http::MetadataMap&) override {
    return Http::FilterMetadataStatus::Continue;
  }
  void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) override {
    encoder_callbacks_ = &callbacks;
  }

private:
  void initPerRouteConfig();

  Http2DubboTranscoderPtr transcoder_;
  DubboTranscoderConfig& config_;
  const DubboTranscoderConfig* per_route_config_{};
  Http::StreamEncoderFilterCallbacks* encoder_callbacks_{};
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{};
  Http::RequestHeaderMap* request_header_{};
  std::unique_ptr<Buffer::OwnedImpl> request_body_buffer_{};

  bool error_{false};
  std::string original_method_;
};

} // namespace HttpDubboTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
