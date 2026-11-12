#include <fstream>
#include "test/mocks/server/factory_context.h"
#include "test/test_common/utility.h"
#include "test/test_common/environment.h"

#include "contrib/envoy/extensions/filters/http/http_dubbo_transcoder/v3/http_dubbo_transcoder.pb.h"
#include "contrib/envoy/extensions/filters/http/http_dubbo_transcoder/v3/http_dubbo_transcoder.pb.validate.h"
#include "contrib/http_dubbo_transcoder/filters/http/source/config.h"
#include "contrib/http_dubbo_transcoder/filters/http/source/dubbo_transcoder_filter.h"
#include "contrib/http_dubbo_transcoder/filters/http/source/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "hessian2/object.hpp"

using testing::_;
using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HttpDubboTranscoder {

class TranscodeFilterTest : public testing::Test {
public:
  TranscodeFilterTest() = default;

  void setConfiguration() {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param
      mapping_type: "java.lang.String"
)EOF";

    setConfiguration(yaml_string);
  }

  void setConfiguration(const std::string& yaml_string) {
    envoy::extensions::filters::http::http_dubbo_transcoder::v3::HttpDubboTranscoder proto_config;
    TestUtility::loadFromYaml(yaml_string, proto_config);

    time_system_.setSystemTime(std::chrono::seconds(1610503040));
    config_ =
        std::make_shared<DubboTranscoderConfig>(proto_config, "http_dubbo_transcoder", *scope_.rootScope());
  }

  void setFilter() { setFilter(std::make_shared<TranscodeFilter>(*config_)); }

  void setFilter(std::shared_ptr<TranscodeFilter> filter) {
    filter_ = filter;
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
  }

  std::string readHexStream(std::string hex_stream) {
    ASSERT(hex_stream.size() % 2 == 0);
    std::stringstream ss;
    for (size_t i = 0; i < hex_stream.size(); i += 2) {
      std::string str_byte = hex_stream.substr(i, 2);
      char chr = static_cast<char>(strtol(str_byte.c_str(), NULL, 16));
      ss << chr;
    }
    return ss.str();
  }

  Stats::TestUtil::TestStore scope_;
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  NiceMock<Http::MockStreamEncoderFilterCallbacks> encoder_callbacks_;
  std::shared_ptr<DubboTranscoderConfig> config_;
  std::shared_ptr<TranscodeFilter> filter_;
};

class MockObject : public Hessian2::Object {
public:
  MOCK_METHOD(Hessian2::Object::Type, type, (), (const));
  MOCK_METHOD(size_t, hash, (), (const));
  MOCK_METHOD(bool, equal, (const Object&), (const));
};

TEST_F(TranscodeFilterTest, NormalHttpGetMethod) {
  setConfiguration();
  setFilter();

  EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
}

TEST_F(TranscodeFilterTest, RestoreOriginalMethodOnEncodeHeaders) {
  setConfiguration();
  setFilter();

  EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);

  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
  EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
  EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
}

TEST_F(TranscodeFilterTest, AllowUnknownMethodAndParameter) {
  setConfiguration();
  setFilter();

  {
    // the path mismatch.
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "POST"},
                                                   {":path", "/mytest.service/test?my_param=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Post, request_headers.getMethodValue());
  }

  {
    // the parameter mismatch.
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/mytest.service/test?my_test=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
}

TEST_F(TranscodeFilterTest, RejectUnknownMethodAndParameter) {
  const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: true
  reject_unknown_method: true
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param
      mapping_type: "java.lang.String"
)EOF";

  setConfiguration(yaml_string);
  setFilter();

  {
    // the path mismatch.
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(1);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "POST"},
                                                   {":path", "/mytest.service/test?my_param=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Post, request_headers.getMethodValue());
  }

  {
    // the parameter mismatch.
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(1);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_test=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
}

TEST_F(TranscodeFilterTest, ExtractParameterKeyFromQuery) {
  const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param1
      mapping_type: "java.lang.String"
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param2
      mapping_type: "java.lang.Long"
)EOF";
  setConfiguration(yaml_string);
  setFilter();

  {
    // normal request
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param1=test&my_param2=12345"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }
  {
    // the request path don't include a query
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::InternalServerError, _, _, _, _))
        .Times(1);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/mytest.service/sayHello"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }

  {
    // query key don't match the extract_key
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::NotFound, _, _, _, _)).Times(1);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param1=test&my_param4=45645"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
}

TEST_F(TranscodeFilterTest, ExtractParameterKeyFromHeader) {
  const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_HEADER
      extract_key: my_param1
      mapping_type: "java.lang.String"
    - extract_key_spec: ALL_HEADER
      extract_key: my_param2
      mapping_type: "java.lang.Double"
)EOF";
  setConfiguration(yaml_string);
  setFilter();
  {
    // normal request
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/mytest.service/sayHello"},
                                                   {"my_param1", "test"},
                                                   {"my_param2", "0.234"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }
  {
    // extract_key my_param1 cannot be found in headers
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::NotFound, _, _, _, _)).Times(1);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/mytest.service/sayHello"},
                                                   {"param", "test"},
                                                   {"my_param2", "0.234"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
  {
    // my_param2's mapping type is Double, but given String
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::BadRequest, _, _, _, _)).Times(1);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/mytest.service/sayHello"},
                                                   {"my_param1", "test"},
                                                   {"my_param2", "abc"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
}

TEST_F(TranscodeFilterTest, DefaultMatchingPathAndHttpMethod) {
  const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    parameter_mapping:
    - extract_key_spec: ALL_HEADER
      extract_key: my_param1
      mapping_type: "java.lang.String"
)EOF";
  setConfiguration(yaml_string);
  setFilter();

  {
    // normal request
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/common.sayHello/sayHello"}, {"my_param1", "test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }

  {
    // extract_key my_param1 cannot be found in headers
    EXPECT_CALL(decoder_callbacks_, sendLocalReply(Http::Code::NotFound, _, _, _, _)).Times(1);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(0);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/common.sayHello/sayHello"}, {"param", "test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::StopIteration,
              filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Get, request_headers.getMethodValue());
  }
}

TEST_F(TranscodeFilterTest, ExtractParameterKeyFromBody) {
  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    parameter_mapping:
    - extract_key_spec: ALL_BODY
      extract_key: name
      mapping_type: "java.lang.String"
    - extract_key_spec: ALL_HEADER
      extract_key: my_param1
      mapping_type: "java.lang.String"
)EOF";
    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, _)).Times(0);
    std::string json_string = R"EOF(
      {
        "age": 10,
        "name" : "test"
      }
    )EOF";
    Buffer::OwnedImpl data(json_string);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/common.sayHello/sayHello"},
        {"my_param1", "test"},
        {"Content-Length", std::to_string(data.length())}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));

    std::string encoded_data(data.toString());
    EXPECT_TRUE(encoded_data.find("test") != std::string::npos);
  }

  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    parameter_mapping:
    - extract_key_spec: ALL_BODY
      mapping_type: "java.util.Map"
)EOF";
    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, _)).Times(0);

    // if there is not Content-Length header, filter will conly parse the first package
    Http::TestRequestHeaderMapImpl request_headers{{":method", "GET"},
                                                   {":path", "/common.sayHello/sayHello"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

    std::string json_string = R"EOF(
      {
        "age": 10,
        "name" : "test"
      }
    )EOF";
    Buffer::OwnedImpl data(json_string);
    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));

    std::string encoded_data(data.toString());
    EXPECT_TRUE(encoded_data.find("age") != std::string::npos);
  }
}

TEST_F(TranscodeFilterTest, ExtractParameterKeyFromBigBody) {
  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/common.sayHello/sayHello"
      match_http_method_spec: ALL_POST
    parameter_mapping:
    - extract_key_spec: ALL_BODY
      mapping_type: "java.util.Map"
    - extract_key_spec: ALL_HEADER
      extract_key: my_param1
      mapping_type: "java.lang.String"
)EOF";

    std::ifstream file(TestEnvironment::substitute(
        "{{ test_rundir "
        "}}/contrib/http_dubbo_transcoder/filters/http/test/test_data/big_reqeust_body"));
    ASSERT_TRUE(file.fail() == false);

    std::string json_body;
    std::getline(file, json_body);
    int pos = json_body.length() / 2;
    std::string body_part1 = json_body.substr(0, pos);
    std::string body_part2 = json_body.substr(pos);
    Buffer::OwnedImpl data_part1(body_part1);
    Buffer::OwnedImpl data_part2(body_part2);

    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, _)).Times(0);

    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "POST"},
        {":path", "/common.sayHello/sayHello"},
        {"my_param1", "test"},
        {"Content-Length", std::to_string(data_part1.length() + data_part2.length())}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

    // The first part of body will be buffed, the second part of body will be parsed along with the
    // first part.
    EXPECT_EQ(Http::FilterDataStatus::StopIterationAndBuffer,
              filter_->decodeData(data_part1, false));
    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_part2, true));
  }
}

TEST_F(TranscodeFilterTest, PassthroughSetting) {
  {
    const std::string yaml_string = R"EOF(
  url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
  request_validation_options:
    reject_unknown_query_parameters: false
    reject_unknown_method: false
  services_mapping:
  - name: "common.sayHello"
    version: "0.0.0"
    group: "dev"
    method_mapping:
      name: "sayHello"
      parameter_mapping:
      - extract_key_spec: ALL_BODY
        extract_key: name
        mapping_type: "java.lang.String"
      passthrough_setting:
        passthrough_all_headers: true
  )EOF";
    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    std::string json_string = R"EOF(
        {
          "age": 10,
          "name" : "test"
        }
      )EOF";
    Buffer::OwnedImpl data(json_string);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/common.sayHello/sayHello"},
        {"my_param1", "test"},
        {"Content-Length", std::to_string(data.length())}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));

    std::string encoded_data(data.toString());
    EXPECT_TRUE(encoded_data.find("my_param1") != std::string::npos);
    EXPECT_TRUE(encoded_data.find("sayHello") != std::string::npos);
  }

  {
    const std::string yaml_string = R"EOF(
  url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
  request_validation_options:
    reject_unknown_query_parameters: false
    reject_unknown_method: false
  services_mapping:
  - name: "common.sayHello"
    version: "0.0.0"
    group: "dev"
    method_mapping:
      name: "sayHello"
      parameter_mapping:
      - extract_key_spec: ALL_BODY
        extract_key: name
        mapping_type: "java.lang.String"
      passthrough_setting:
        passthrough_all_headers: false
  )EOF";
    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, sendLocalReply(_, _, _, _, _)).Times(0);
    std::string json_string = R"EOF(
        {
          "age": 10,
          "name" : "test"
        }
      )EOF";
    Buffer::OwnedImpl data(json_string);
    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"},
        {":path", "/common.sayHello/sayHello"},
        {"my_param1", "test"},
        {"Content-Length", std::to_string(data.length())}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());

    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data, true));

    std::string encoded_data(data.toString());
    EXPECT_TRUE(encoded_data.find("my_param1") == std::string::npos);
    EXPECT_TRUE(encoded_data.find("GET") == std::string::npos);
  }
}

TEST(DobboUtilityTest, convertStringToTypeValueTest) {
  {
    absl::string_view value{"0.234234"};
    std::string type{"java.lang.Double"};
    nlohmann::json result = 0.234234;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"-0.234234"};
    std::string type{"java.lang.Double"};
    nlohmann::json result = -0.234234;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"0.0"};
    std::string type{"java.lang.Double"};
    nlohmann::json result = 0;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }

  {
    absl::string_view value{"837465"};
    std::string type{"java.lang.Long"};
    nlohmann::json result = 837465;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"-34534"};
    std::string type{"java.lang.Long"};
    nlohmann::json result = -34534;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"0"};
    std::string type{"java.lang.Long"};
    nlohmann::json result = 0;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"true"};
    std::string type{"java.lang.Boolean"};
    nlohmann::json result = true;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
  {
    absl::string_view value{"false"};
    std::string type{"java.lang.Boolean"};
    nlohmann::json result = false;
    EXPECT_EQ(result, DubboUtility::convertStringToTypeValue(value, type).value());
  }
}

TEST(DobboUtilityTest, HessianToJsonBadCast) {
  const std::string ERROR_KEY = "error";
  const std::string ERROR_VALUE_TEMP =
      "The data returned by dubbo service does not comply with the hessian protocol, data type: ";
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Binary));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Binary));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Boolean));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Boolean));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Date));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Date));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Double));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Double));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Integer));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Integer));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Long));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Long));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::Ref));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::Ref));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::String));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY],
              ERROR_VALUE_TEMP + DubboUtility::hessianType2String(Hessian2::Object::Type::String));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::TypedList));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY], ERROR_VALUE_TEMP + DubboUtility::hessianType2String(
                                                            Hessian2::Object::Type::TypedList));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::UntypedList));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY], ERROR_VALUE_TEMP + DubboUtility::hessianType2String(
                                                            Hessian2::Object::Type::UntypedList));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::TypedMap));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY], ERROR_VALUE_TEMP + DubboUtility::hessianType2String(
                                                            Hessian2::Object::Type::TypedMap));
  }
  {
    NiceMock<MockObject> mock_obj;
    EXPECT_CALL(mock_obj, type()).WillRepeatedly(Return(Hessian2::Object::Type::UntypedMap));
    nlohmann::json error_json = DubboUtility::hessian2Json(&mock_obj);
    EXPECT_EQ(error_json[ERROR_KEY], ERROR_VALUE_TEMP + DubboUtility::hessianType2String(
                                                            Hessian2::Object::Type::UntypedMap));
  }
} // namespace HttpDubboTranscoder

TEST_F(TranscodeFilterTest, EncodeDataFromDubboServer) {
  setConfiguration();
  setFilter();

  // initialize filter_->transcoder by calling filter->decodeHeaders
  Http::TestRequestHeaderMapImpl request_headers{
      {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  {
    // 1. Normal dubbo request message
    Buffer::OwnedImpl buffer;
    buffer.add(std::string({'\xda', '\xbb', 0x42, 20}));
    buffer.writeBEInt(static_cast<uint64_t>(1));
    std::string content({'I', 0x00, 0x00, 0x00, 0x01, 0x05, 'h', 'e', 'l', 'l', 'o'});
    buffer.writeBEInt(static_cast<uint32_t>(content.size()));
    buffer.add(content);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
  }

  {
    // 2. Protocol error
    Buffer::OwnedImpl buffer;
    buffer.add("Not dubbo message");
    filter_->encodeData(buffer, true);
    EXPECT_EQ(buffer.toString(), "Not dubbo message");
  }

  {
    // 3. The length of dubbo message is less than DUBBO_HEADER_SIZE
    Buffer::OwnedImpl buffer;
    buffer.add(std::string({'\xda', '\xbb', 0x42}));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(buffer.toString(), "Dubbo message data is incomplete");
  }

  {
    // 4. Cannot parse RpcResult type from buffer
    Buffer::OwnedImpl buffer;
    buffer.add(std::string({'\xda', '\xbb', 0x42, 20}));
    buffer.writeBEInt(static_cast<uint64_t>(1));
    std::string content({0x00, 0x00, 0x00, 0x01, 0x05, 'h', 'e', 'l', 'l', 'o'});
    buffer.writeBEInt(static_cast<uint32_t>(content.size()));
    buffer.add(content);
    filter_->encodeData(buffer, true);
    EXPECT_EQ(buffer.toString(), "Cannot parse RpcResult type from buffer");
  }

  {
    // 5. In the Hessian protocol, if an object is empty, it is represented by the character 'N'.
    //    When decoding, we interpret it as null instead of the string "null".
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb02140000000000000001000000509148046e616d654e05636c617373302e636f6"
                             "d2e616c69626162612e6e61636f732e6578616d706c652e647562626f2e7365727669"
                             "63652e506572736f6e03616765900a7365636f6e644e616d654e5a"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":{\"age\":0,\"name\":null,\"secondName\":null}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 6. The java backend returns a fastjson object, which corresponds to TypedMap in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream(
        "dabb0214000000000000000c0000021e914d1f636f6d2e616c69626162612e666173746a736f6e2e4a534f4e4f"
        "626a656374036d7367024f4b04636f6465c8c804646174614d90036d7367077375636365737304636f64659104"
        "6461746172136a6176612e7574696c2e41727261794c6973744d90046d6369641e597a703763587736496a6736"
        "4c6930714b69346f5231394b4f6d5525334403696d67304868747470733a2f2f646174612e30303776696e2e63"
        "6f6d2f737463696d67732f696d672f343738636164373934653466623164633965373836613464303637613563"
        "38322e6a70670a636f6c6f7276616c756591036e756d0131056c6162656c1954686520656e67696e652f467565"
        "6c20747970652f746f6f6c096272616e64436f646506746f796f74615a4d90046d6369641e597a703763587736"
        "496a67364c6930714b69346f5231394b4f6d5525334403696d67304868747470733a2f2f646174612e30303776"
        "696e2e636f6d2f737463696d67732f696d672f6565666438376236326436363539316566616336303835356261"
        "6232656163622e6a70670a636f6c6f7276616c756591036e756d0132056c6162656c1944726976652074726169"
        "6e2f4368617373697320636c617373096272616e64436f646506746f796f74615a066c656e677468940474696d"
        "651231303139383730362e3131333138323234350a71756572795f74696d6514302e3031333330303230363531"
        "323231323735335a0773756363657373545a"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response =
        "{\"result\":{\"code\":200,\"data\":{\"code\":1,\"data\":[{\"brandCode\":\"toyota\","
        "\"colorvalue\":1,\"img\":\"https://data.007vin.com/stcimgs/img/"
        "478cad794e4fb1dc9e786a4d067a5c82.jpg\",\"label\":\"The engine/Fuel "
        "type/"
        "tool\",\"mcid\":\"Yzp7cXw6Ijg6Li0qKi4oR19KOmU%3D\",\"num\":\"1\"},{\"brandCode\":"
        "\"toyota\",\"colorvalue\":1,\"img\":\"https://data.007vin.com/stcimgs/img/"
        "eefd87b62d66591efac60855bab2eacb.jpg\",\"label\":\"Drive train/Chassis "
        "class\",\"mcid\":\"Yzp7cXw6Ijg6Li0qKi4oR19KOmU%3D\",\"num\":\"2\"}],\"length\":4,\"msg\":"
        "\"success\",\"query_time\":\"0.013300206512212753\",\"time\":\"10198706.113182245\"},"
        "\"msg\":\"OK\",\"success\":true}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 7. The java backend returns a java.math.BigDecimal object, which corresponds to ClassInstance
    // in hessian serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb02140000000000000001000000329143146a6176612e6d6174682e42696744656"
                             "3696d616c910576616c7565601231303139383730362e313133313832323435"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response =
        "{\"result\":{\"class\":\"java.math.BigDecimal\",\"value\":\"10198706.113182245\"}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 8. The java backend returns a byte[] object, which corresponds to Binary in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb0214000000000000000a00000011912f010203010203010203010203010203"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":[1,2,3,1,2,3,1,2,3,1,2,3,1,2,3]}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 9. The java backend returns a java.util.Date object, which corresponds to Date in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream(
        "dabb021400000000000000020000009b914806706572736f6e48046974656d5190046e616d654e05636c617373"
        "302e636f6d2e616c69626162612e6e61636f732e6578616d706c652e647562626f2e736572766963652e506572"
        "736f6e03616765900a7365636f6e644e616d654e5a05636c617373302c636f6d2e616c69626162612e6e61636f"
        "732e6578616d706c652e647562626f2e736572766963652e4974656d056f726465724e5a"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response =
        "{\"result\":{\"order\":null,\"person\":{\"age\":0,\"item\":\"Type: Ref, target Object "
        "Type: UntypedMap\",\"name\":null,\"secondName\":null}}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 10. The java backend returns an object, which has a circular reference problem. At this time,
    // a Ref object will appear in the Hessian serialization protocol
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream(
        "dabb021400000000000000020000009b914806706572736f6e48046974656d5190046e616d654e05636c617373"
        "302e636f6d2e616c69626162612e6e61636f732e6578616d706c652e647562626f2e736572766963652e506572"
        "736f6e03616765900a7365636f6e644e616d654e5a05636c617373302c636f6d2e616c69626162612e6e61636f"
        "732e6578616d706c652e647562626f2e736572766963652e4974656d056f726465724e5a"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response =
        "{\"result\":{\"order\":null,\"person\":{\"age\":0,\"item\":\"Type: Ref, target Object "
        "Type: UntypedMap\",\"name\":null,\"secondName\":null}}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 11. The java backend returns a boolean object, which corresponds to Boolean in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb02140000000000000002000000029146"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":false}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 12. The java backend returns a double object, which corresponds to Double in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb021400000000000000040000000a9144402877e90ff97247"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":12.2342}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 13. The java backend returns a int object, which corresponds to Integer in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb021400000000000000060000000491d5e162"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":123234}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 14. The java backend returns a long object, which corresponds to Long in hessian
    // serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb02140000000000000008000000069159117e0c07"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":293473287}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 15. The java backend returns a java.lang.String object, which corresponds to String in
    // hessian serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb0214000000000000000a00000021911f6162636465736173636e756b736e63697"
                             "57366686175686461657569646861"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":\"abcdesascnuksnciusfhauhdaeuidha\"}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 16. The java backend returns a ArrayList<String>, which corresponds to TypedList in
    // hessian serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb02140000000000000002000000229173136a6176612e7574696c2e41727261794"
                             "c697374036162630362636403636465"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":[\"abc\",\"bcd\",\"cde\"]}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }

  {
    // 17. The java backend returns a Map<String, Integer>, which corresponds to UntypedMap in
    // hessian serialization protocol.
    Buffer::OwnedImpl buffer;
    buffer.add(readHexStream("dabb021400000000000000020000001c91480373696649000e51d4036a6e6749000f0"
                             "32703616263d5e1625a"));
    filter_->encodeData(buffer, true);
    EXPECT_EQ(filter_->encodeData(buffer, true), Http::FilterDataStatus::Continue);
    std::string expected_response = "{\"result\":{\"abc\":123234,\"jng\":983847,\"sif\":938452}}";
    EXPECT_EQ(buffer.toString(), expected_response);
  }
}

TEST_F(TranscodeFilterTest, ServiceVersionAndGroup) {
  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param
      mapping_type: "java.lang.String"
)EOF";

    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);

    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }
  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  version: "0.0.0"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param
      mapping_type: "java.lang.String"
)EOF";

    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);

    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }
  {
    const std::string yaml_string = R"EOF(
url_unescape_spec: ALL_CHARACTERS_EXCEPT_RESERVED
request_validation_options:
  reject_unknown_query_parameters: false
  reject_unknown_method: false
services_mapping:
- name: "common.sayHello"
  group: "dev"
  method_mapping:
    name: "sayHello"
    path_matcher:
      match_pattern: "/mytest.service/sayHello"
      match_http_method_spec: ALL_GET
    parameter_mapping:
    - extract_key_spec: ALL_QUERY_PARAMETER
      extract_key: my_param
      mapping_type: "java.lang.String"
)EOF";

    setConfiguration(yaml_string);
    setFilter();

    EXPECT_CALL(decoder_callbacks_, addDecodedData(_, true)).Times(1);

    Http::TestRequestHeaderMapImpl request_headers{
        {":method", "GET"}, {":path", "/mytest.service/sayHello?my_param=test"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));
    EXPECT_EQ(Http::Headers::get().MethodValues.Connect, request_headers.getMethodValue());
  }
}

} // namespace HttpDubboTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
