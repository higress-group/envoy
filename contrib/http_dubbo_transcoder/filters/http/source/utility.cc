#include "utility.h"

#include <iostream>
#include <vector>

#include "envoy/http/codes.h"
#include "envoy/http/query_params.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/regex.h"

#include "absl/strings/str_split.h"
#include "hessian2/object.hpp"
#include "include/nlohmann/json.hpp"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HttpDubboTranscoder {

absl::optional<json> DubboUtility::convertStringToTypeValue(absl::string_view value,
                                                            std::string type) {

  // the return value converted value maybe int boolean string, so use json to store the value
  if (type == JsonType2JavaType.at(json::value_t::boolean)) {
    if (value == "true" || value == "false") {
      return {json(value == "true" ? true : false)};
    }
    return absl::nullopt;
  } else if (type == JsonType2JavaType.at(json::value_t::number_float) ||
             type == "java.lang.Float" || type == "java.math.BigDecimal") {
    envoy::type::matcher::v3::RegexMatcher matcher;
    *matcher.mutable_google_re2() = envoy::type::matcher::v3::RegexMatcher::GoogleRE2();
    matcher.set_regex("^-?([1-9]\\d*\\.\\d*|0\\.\\d*[1-9]\\d*|0?\\.0+|0)$");
    Regex::GoogleReEngine engine;
    const auto compiled_matcher = Regex::Utility::parseRegex(matcher, engine);
    if (!compiled_matcher.ok() || !(*compiled_matcher)->match(value)) {
      return absl::nullopt;
    }
    return {json(strtod(value.data(), nullptr))};
  } else if (type == JsonType2JavaType.at(json::value_t::number_integer) ||
             type == "java.lang.Integer" || type == "java.lang.Short" || type == "java.util.Date") {
    envoy::type::matcher::v3::RegexMatcher matcher;
    *matcher.mutable_google_re2() = envoy::type::matcher::v3::RegexMatcher::GoogleRE2();
    matcher.set_regex("^(0|[1-9][0-9]*|-[1-9][0-9]*)$");
    Regex::GoogleReEngine engine;
    const auto compiled_matcher = Regex::Utility::parseRegex(matcher, engine);
    if (!compiled_matcher.ok() || !(*compiled_matcher)->match(value)) {
      return absl::nullopt;
    }
    return {json(strtoll(value.data(), nullptr, 10))};
  } else if (type == JsonType2JavaType.at(json::value_t::string)) {
    return {json(value)};
  } else if (type == JsonType2JavaType.at(json::value_t::array) || type == "java.util.Set") {
    json array_json;
    array_json.emplace_back(std::string(value));
    return array_json;
  } else {
    return absl::nullopt;
  }
}

Http::Code DubboUtility::convertStatusToHttpCode(absl::StatusCode status) {
  Http::Code ret_http_code;

  switch (status) {
  case absl::StatusCode::kInternal:
    ret_http_code = Http::Code::InternalServerError;
    break;
  case absl::StatusCode::kNotFound:
    ret_http_code = Http::Code::NotFound;
    break;
  case absl::StatusCode::kOutOfRange:
    ret_http_code = Http::Code::RangeNotSatisfiable;
    break;
  case absl::StatusCode::kUnimplemented:
    ret_http_code = Http::Code::NotImplemented;
    break;
  case absl::StatusCode::kInvalidArgument:
    ret_http_code = Http::Code::BadRequest;
    break;
  case absl::StatusCode::kDataLoss:
    ret_http_code = Http::Code::BadRequest;
    break;
  default:
    ret_http_code = Http::Code::NotFound;
  }

  return ret_http_code;
}

std::tuple<bool, bool, bool> DubboUtility::resolveResponseFlag(RpcResponseType flag) {
  bool has_value = false, has_excption = false, has_attachment = false;

  switch (flag) {
  case RpcResponseType::ResponseWithException:
    has_excption = true;
    break;
  case RpcResponseType::ResponseWithExceptionWithAttachments:
    has_excption = true;
    has_attachment = true;
    break;
  case RpcResponseType::ResponseWithNullValue:
    has_value = false;
    break;
  case RpcResponseType::ResponseNullValueWithAttachments:
    has_value = false;
    has_attachment = true;
    break;
  case RpcResponseType::ResponseWithValue:
    has_value = true;
    break;
  case RpcResponseType::ResponseValueWithAttachments:
    has_value = true;
    has_attachment = true;
    break;
  }

  return {has_value, has_excption, has_attachment};
}

std::string DubboUtility::hessianType2String(Hessian2::Object::Type type) {
  switch (type) {
  case Hessian2::Object::Type::Binary:
    return "Binary";
  case Hessian2::Object::Type::Boolean:
    return "Boolean";
  case Hessian2::Object::Type::Date:
    return "Date";
  case Hessian2::Object::Type::Double:
    return "Double";
  case Hessian2::Object::Type::Integer:
    return "Integer";
  case Hessian2::Object::Type::Long:
    return "Long";
  case Hessian2::Object::Type::Null:
    return "Null";
  case Hessian2::Object::Type::Ref:
    return "Ref";
  case Hessian2::Object::Type::String:
    return "String";
  case Hessian2::Object::Type::TypedList:
    return "TypedList";
  case Hessian2::Object::Type::UntypedList:
    return "UntypedList";
  case Hessian2::Object::Type::TypedMap:
    return "TypedMap";
  case Hessian2::Object::Type::UntypedMap:
    return "UntypedMap";
  case Hessian2::Object::Type::Class:
    return "Class";
  default:
    return "Unknown";
  }
}

json DubboUtility::badCastErrorMessageJson(const std::string& type) {
  json error_message_json;
  error_message_json["error"] = absl::StrFormat(
      "The data returned by dubbo service does not comply with the hessian protocol, data type: %s",
      type);
  return error_message_json;
}

json DubboUtility::hessian2Json(Object* input) {
  json out;

  if (input == nullptr) {
    return nullptr;
  }

  switch (input->type()) {
  case Object::Type::TypedMap: {
    if (dynamic_cast<Hessian2::TypedMapObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      for (auto& item : *(static_cast<Hessian2::TypedMapObject*>(input))) {
        Hessian2::StringObject& key = item.first->asType<Hessian2::StringObject>();
        auto key_ref = key.toMutableString();
        if (key_ref.has_value()) {
          out[*key_ref] = hessian2Json(item.second.get());
        }
      }
    }
  } break;
  case Object::Type::UntypedMap: {
    if (dynamic_cast<Hessian2::UntypedMapObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      for (auto& item : *(static_cast<Hessian2::UntypedMapObject*>(input))) {
        Hessian2::StringObject& key = item.first->asType<Hessian2::StringObject>();
        auto key_ref = key.toMutableString();
        if (key_ref.has_value()) {
          std::string key_str = (*key_ref).get();
          if (key_str != ClassKey) {
            out[key_str] = hessian2Json(item.second.get());
          }
        }
      }
    }
  } break;
  case Object::Type::UntypedList: {
    if (dynamic_cast<Hessian2::UntypedListObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      for (auto& item : *(static_cast<Hessian2::UntypedListObject*>(input))) {
        json j = hessian2Json(item.get());
        out.push_back(j);
      }
    }
  } break;
  case Object::Type::TypedList: {
    if (dynamic_cast<Hessian2::TypedListObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      for (auto& item : *(static_cast<Hessian2::TypedListObject*>(input))) {
        json j = hessian2Json(item.get());
        out.push_back(j);
      }
    }
  } break;

  case Object::Type::String: {
    if (dynamic_cast<Hessian2::StringObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = *(static_cast<Hessian2::StringObject*>(input)->toMutableString());
    }
  } break;

  case Object::Type::Double: {
    if (dynamic_cast<Hessian2::DoubleObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = (*(static_cast<Hessian2::DoubleObject*>(input)->toMutableDouble())).get();
    }
  } break;

  case Object::Type::Integer: {
    if (dynamic_cast<Hessian2::IntegerObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = (*(static_cast<Hessian2::IntegerObject*>(input)->toMutableInteger())).get();
    }
  } break;

  case Object::Type::Long: {
    if (dynamic_cast<Hessian2::LongObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = (*(static_cast<Hessian2::LongObject*>(input)->toMutableLong())).get();
    }
  } break;

  case Object::Type::Boolean: {
    if (dynamic_cast<Hessian2::BooleanObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = (*(static_cast<Hessian2::BooleanObject*>(input)->toMutableBoolean())).get();
    }
  } break;

  case Object::Type::Ref: {
    if (dynamic_cast<Hessian2::RefObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      Hessian2::Object* obj = static_cast<Hessian2::RefObject*>(input)->toRefDest().value();
      out = absl::StrFormat("Type: Ref, target Object Type: %s", hessianType2String(obj->type()));
    }
  } break;

  case Object::Type::Class: {
    if (dynamic_cast<Hessian2::ClassInstanceObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      auto class_instance_ref =
          static_cast<Hessian2::ClassInstanceObject*>(input)->toClassInstance();
      const Hessian2::Object::ClassInstance* class_instance = &(*class_instance_ref).get();
      RELEASE_ASSERT(class_instance->def_->field_names_.size() == class_instance->data_.size(),
                     "The size of def_->field_names_ and data_ of class_instance is inconsistent");
      out[ClassKey] = class_instance->def_->type_;
      for (int i = 0; i < static_cast<int>(class_instance->def_->field_names_.size()); i++) {
        out[class_instance->def_->field_names_[i]] = hessian2Json(class_instance->data_[i].get());
      }
    }
  } break;

  case Object::Type::Date: {
    if (dynamic_cast<Hessian2::DateObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = static_cast<Hessian2::DateObject*>(input)->toMutableDate()->count();
    }
  } break;

  case Object::Type::Null: {
    out = nullptr;
  } break;

  case Object::Type::Binary: {
    if (dynamic_cast<Hessian2::BinaryObject*>(input) == nullptr) {
      out = badCastErrorMessageJson(hessianType2String(input->type()));
    } else {
      out = (*(static_cast<Hessian2::BinaryObject*>(input)->toMutableBinary())).get();
    }
  } break;

  default:
    break;
  }

  return out;
}

void DubboUtility::json2Hessian(json&& object, Hessian2::Encoder& encoder) {
  auto type = object.type();
  switch (type) {
  case json::value_t::object: {
    encoder.encodeMapBegin("");
    for (auto& el : object.items()) {
      encoder.encode<std::string>(el.key());
      json2Hessian(el.value(), encoder);
    }
    encoder.encodeMapEnd();
  } break;

  case json::value_t::boolean:
    encoder.encode<bool>(object.get<bool>());
    break;

  case json::value_t::number_integer:
  case json::value_t::number_unsigned:
    encoder.encode<int64_t>(object.get<int64_t>());
    break;

  case json::value_t::number_float:
    encoder.encode<double>(object.get<double>());
    break;

  case json::value_t::array: {
    Hessian2::Object::UntypedList untyped_list;
    for (auto& item : object.items()) {
      createUntypedListObjcet(item.value(), untyped_list);
    }
    Hessian2::UntypedListObject untyped_list_object(std::move(untyped_list));
    encoder.encode<Hessian2::UntypedListObject>(untyped_list_object);
  } break;

  case json::value_t::string:
    encoder.encode<std::string>(object.get<std::string>());
    break;

  case json::value_t::binary:
    encoder.encode<std::vector<uint8_t>>(object.get_binary());
    break;

  case json::value_t::null:
  default:
    encoder.encode<Object>(Hessian2::NullObject());
    break;
  }
}

void DubboUtility::json2Hessian(json& j, Hessian2::Encoder& encoder) {
  DubboUtility::json2Hessian(std::move(j), encoder);
}

void DubboUtility::encodeParameterList(json& j, Hessian2::Encoder& encoder) {
  encoder.encodeVarListBegin("");
  for (auto& item : j.items()) {
    json2Hessian(item.value(), encoder);
  }
  encoder.encodeVarListEnd();
}

void DubboUtility::createUntypedListObjcet(const json& object,
                                           Hessian2::Object::UntypedList& untyped_list) {
  auto type = object.type();
  switch (type) {
  case json::value_t::string:
    untyped_list.emplace_back(std::make_unique<Hessian2::StringObject>(object.get<std::string>()));
    break;
  case json::value_t::number_unsigned:
  case json::value_t::number_integer:
    untyped_list.emplace_back(std::make_unique<Hessian2::IntegerObject>(object.get<int64_t>()));
    break;
  case json::value_t::number_float:
    untyped_list.emplace_back(std::make_unique<Hessian2::DoubleObject>(object.get<double>()));
    break;
  case json::value_t::boolean:
    untyped_list.emplace_back(std::make_unique<Hessian2::BooleanObject>(object.get<bool>()));
    break;
  case json::value_t::binary:
    untyped_list.emplace_back(std::make_unique<Hessian2::BinaryObject>(object.get_binary()));
    break;
  case json::value_t::null:
  default:
    untyped_list.emplace_back(std::make_unique<Hessian2::NullObject>());
    break;
  }
}

} // namespace HttpDubboTranscoder
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
