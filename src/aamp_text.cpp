/**
 * Copyright (C) 2020 leoetlino
 *
 * This file is part of oead.
 *
 * oead is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * oead is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with oead.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <absl/algorithm/container.h>
#include <absl/strings/match.h>
#include <absl/strings/str_format.h>
#include <array>
#include <tuple>

#include <c4/std/string.hpp>
#include <ryml.hpp>
#include "../lib/libyaml/include/yaml.h"

#include <cmrc/cmrc.hpp>

#include <oead/aamp.h>
#include <oead/errors.h>
#include "util/iterator_utils.h"
#include "util/variant_utils.h"
#include "yaml.h"

CMRC_DECLARE(oead::res);

namespace oead::aamp {

template <typename Callback>
static void SplitStringByLine(std::string_view data, Callback cb) {
  size_t line_start_pos = 0;
  while (line_start_pos < data.size()) {
    const auto line_end_pos = data.find('\n', line_start_pos);
    if (line_start_pos != line_end_pos)
      cb(data.substr(line_start_pos, line_end_pos - line_start_pos));
    if (line_end_pos == std::string_view::npos)
      break;
    line_start_pos = line_end_pos + 1;
  }
}

NameTable::NameTable(bool with_botw_strings) {
  if (!with_botw_strings)
    return;

  const auto fs = cmrc::oead::res::get_filesystem();

  const auto hashed_names_f = fs.open("data/botw_hashed_names.txt");
  SplitStringByLine({hashed_names_f.begin(), hashed_names_f.size()}, [&](std::string_view name) {
    // No need to copy the string data since it is in the constant data section of the executable.
    names.emplace(util::crc32(name), name);
  });

  const auto numbered_names_f = fs.open("data/botw_numbered_names.txt");
  SplitStringByLine({numbered_names_f.begin(), numbered_names_f.size()},
                    [&](std::string_view name) { numbered_names.emplace_back(name); });
}

std::optional<std::string_view> NameTable::GetName(u32 hash, int index, u32 parent_name_hash) {
  using namespace std::string_view_literals;

  if (const auto it = names.find(hash); it != names.end())
    return it->second;

  if (const auto it = owned_names.find(hash); it != owned_names.end())
    return it->second;

  // Try to guess the name from the parent structure if possible.
  if (const auto it = names.find(parent_name_hash); it != names.end()) {
    const auto test_names = [&](std::string_view prefix) -> std::optional<std::string_view> {
      static constexpr std::array<std::string_view, 6> formats = {{
          "%s%d",
          "%s_%d",
          "%s%02d",
          "%s_%02d",
          "%s%03d",
          "%s_%03d",
      }};
      for (int i : {index, index + 1}) {
        for (const auto format : formats) {
          auto candidate = absl::StrFormat(format, prefix, i);
          if (util::crc32(candidate) == hash)
            return AddName(hash, std::move(candidate));
        }
      }
      return std::nullopt;
    };

    const std::string_view parent_name = it->second;
    if (const auto match = test_names(parent_name))
      return *match;
    // Sometimes the parent name is plural and the object names are singular.
    if (const auto match = test_names("Children"sv))
      return *match;
    for (std::string_view suffix : {"s"sv, "es"sv, "List"sv}) {
      if (!absl::EndsWith(parent_name, suffix))
        continue;
      if (const auto match = test_names(parent_name.substr(0, parent_name.size() - suffix.size())))
        return *match;
    }
  }

  // Last resort: test all numbered names.
  for (std::string_view name : numbered_names) {
    for (int i = 0; i < index + 2; ++i) {
      auto candidate = absl::StrFormat(name, i);
      if (util::crc32(candidate) == hash)
        return AddName(hash, std::move(candidate));
    }
  }

  return std::nullopt;
}

std::string_view NameTable::AddName(u32 hash, std::string name) {
  const auto& [it, added] = owned_names.emplace(hash, std::move(name));
  return it->second;
}

void NameTable::AddNameReference(std::string_view name) {
  names.emplace(util::crc32(name), name);
}

NameTable& GetDefaultNameTable() {
  static NameTable s_table{true};
  return s_table;
}

static std::optional<oead::yml::TagBasedType> RecognizeTag(const std::string_view tag) {
  if (util::IsAnyOf(tag, "!str32", "!str64", "!str256"))
    return yml::TagBasedType::Str;
  if (util::IsAnyOf(tag, "!u"))
    return yml::TagBasedType::Int;
  return std::nullopt;
}

static Parameter ScalarToValue(std::string_view tag, yml::Scalar&& scalar) {
  return util::Match(
      std::move(scalar), [](bool value) -> Parameter { return value; },
      [&](std::string&& value) -> Parameter {
        if (tag == "!str32")
          return FixedSafeString<32>(value);
        if (tag == "!str64")
          return FixedSafeString<64>(value);
        if (tag == "!str256")
          return FixedSafeString<256>(value);
        return Parameter{std::move(value)};
      },
      [&](u64 value) -> Parameter {
        if (tag == "!u")
          return U32(value);
        return int(value);
      },
      [&](f64 value) -> Parameter { return float(value); },
      [](std::nullptr_t) -> Parameter { throw InvalidDataError("Unexpected scalar type"); });
}

template <typename T>
static T ParseIntOrFloat(const ryml::NodeRef& node) {
  using YmlType = std::conditional_t<std::is_integral_v<T>, u64, f64>;
  return T(std::get<YmlType>(yml::ParseScalar(node, RecognizeTag)));
}

template <typename T>
static T ReadSequenceForNumericalStruct(const ryml::NodeRef& node) {
  T value;
  auto fields = value.fields();
  if (node.num_children() != std::tuple_size<decltype(fields)>())
    throw InvalidDataError("Unexpected number of children");
  auto child = node.children().begin();
  std::apply(
      [&](auto&&... x) {
        ((x = ParseIntOrFloat<std::decay_t<decltype(x)>>(*child), ++child), ...);
      },
      fields);
  return value;
}

template <typename T>
static std::vector<T> ReadSequenceForBuffer(const ryml::NodeRef& node) {
  std::vector<T> vector;
  vector.reserve(node.num_children());
  for (const auto& child : node)
    vector.emplace_back(ParseIntOrFloat<T>(child));
  return vector;
}

template <size_t N>
static std::array<Curve, N> ReadSequenceForCurve(const ryml::NodeRef& node) {
  std::array<Curve, N> curves;
  size_t i = 0;
  for (Curve& curve : curves) {
    curve.a = ParseIntOrFloat<u32>(node[i++]);
    curve.b = ParseIntOrFloat<u32>(node[i++]);
    for (float& x : curve.floats)
      x = ParseIntOrFloat<f32>(node[i++]);
  }
  return curves;
}

bool read(const ryml::NodeRef& node, Parameter* v) {
  if (node.is_seq()) {
    const auto tag = yml::RymlSubstrToStrView(node.val_tag());
    if (tag == "!vec2") {
      *v = ReadSequenceForNumericalStruct<Vector2f>(node);
    } else if (tag == "!vec3") {
      *v = ReadSequenceForNumericalStruct<Vector3f>(node);
    } else if (tag == "!vec4") {
      *v = ReadSequenceForNumericalStruct<Vector4f>(node);
    } else if (tag == "!color") {
      *v = ReadSequenceForNumericalStruct<Color4f>(node);
    } else if (tag == "!curve") {
      constexpr size_t NumElementsPerCurve = 32;
      switch (node.num_children()) {
      case 1 * NumElementsPerCurve:
        *v = ReadSequenceForCurve<1>(node);
        break;
      case 2 * NumElementsPerCurve:
        *v = ReadSequenceForCurve<2>(node);
        break;
      case 3 * NumElementsPerCurve:
        *v = ReadSequenceForCurve<3>(node);
        break;
      case 4 * NumElementsPerCurve:
        *v = ReadSequenceForCurve<4>(node);
        break;
      default:
        throw InvalidDataError("Invalid curve: unexpected number of children");
      }
    } else if (tag == "!buffer_int") {
      *v = ReadSequenceForBuffer<int>(node);
    } else if (tag == "!buffer_f32") {
      *v = ReadSequenceForBuffer<f32>(node);
    } else if (tag == "!buffer_u32") {
      *v = ReadSequenceForBuffer<u32>(node);
    } else if (tag == "!buffer_binary") {
      *v = ReadSequenceForBuffer<u8>(node);
    } else if (tag == "!quat") {
      *v = ReadSequenceForNumericalStruct<Quatf>(node);
    } else {
      throw InvalidDataError(absl::StrFormat("Unexpected sequence tag (or no tag): %s", tag));
    }
    return true;
  }

  if (node.has_val()) {
    *v = ScalarToValue(yml::RymlGetValTag(node), yml::ParseScalar(node, RecognizeTag));
    return true;
  }

  return false;
}

template <typename T, typename Map>
static void ReadMap(const ryml::NodeRef& node, Map& map) {
  for (const auto& child : node) {
    const auto key = yml::ParseScalarKey(child, RecognizeTag);
    T structure;
    child >> structure;
    if (const auto* hash = std::get_if<u64>(&key))
      map.emplace(static_cast<u32>(*hash), std::move(structure));
    else if (const auto* str = std::get_if<std::string>(&key))
      map.emplace(*str, std::move(structure));
    else
      throw InvalidDataError("Unexpected key scalar type");
  }
}

bool read(const ryml::NodeRef& node, ParameterObject* v) {
  if (!node.is_map())
    return false;

  ParameterObject object;
  ReadMap<Parameter>(node, object.params);
  *v = std::move(object);
  return true;
}

bool read(const ryml::NodeRef& node, ParameterList* v) {
  if (!node.is_map() || !node.has_child("objects") || !node.has_child("lists"))
    return false;

  ParameterList list;
  ReadMap<ParameterObject>(node["objects"], list.objects);
  ReadMap<ParameterList>(node["lists"], list.lists);
  *v = std::move(list);
  return true;
}

bool read(const ryml::NodeRef& node, ParameterIO* v) {
  if (!node.is_map() || !node.has_child("version") || !node.has_child("type") ||
      !node.has_child("param_root")) {
    return false;
  }

  ParameterIO pio;
  pio.version = ParseIntOrFloat<u32>(node["version"]);
  pio.type = std::move(std::get<std::string>(yml::ParseScalar(node["type"], RecognizeTag)));
  ParameterList param_root;
  node["param_root"] >> param_root;
  pio.objects = std::move(param_root.objects);
  pio.lists = std::move(param_root.lists);
  *v = std::move(pio);
  return true;
}

ParameterIO ParameterIO::FromText(std::string_view yml_text) {
  yml::InitRymlIfNeeded();
  ryml::Tree tree = ryml::parse(yml::StrViewToRymlSubstr(yml_text));
  ParameterIO pio;
  tree.rootref() >> pio;
  return pio;
}

class TextEmitter {
public:
  std::string Emit(const ParameterIO& pio) {
    m_extra_name_table = {};
    BuildExtraNameTable(pio);

    yaml_event_t event;

    yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING);
    emitter.Emit(event);

    yaml_document_start_event_initialize(&event, nullptr, nullptr, nullptr, 1);
    emitter.Emit(event);

    EmitParameterIO(pio);

    yaml_document_end_event_initialize(&event, 1);
    emitter.Emit(event);

    yaml_stream_end_event_initialize(&event);
    emitter.Emit(event);
    return std::move(emitter.GetOutput());
  }

private:
  /// Populates the extra name table with strings from the given parameter IO.
  void BuildExtraNameTable(const ParameterList& list) {
    for (const auto& [obj_name, obj] : list.objects) {
      for (const auto& [param_name, param] : obj.params) {
        if (IsStringType(param.GetType()))
          m_extra_name_table.AddNameReference(param.GetStringView());
      }
    }
    for (const auto& [sub_list_name, sub_list] : list.lists)
      BuildExtraNameTable(sub_list);
  }

  void EmitName(Name name, int index, Name parent_name) {
    NameTable& table = GetDefaultNameTable();
    if (const auto name_str = m_extra_name_table.GetName(name, index, parent_name))
      emitter.EmitString(*name_str);
    else if (const auto name_str = table.GetName(name, index, parent_name))
      emitter.EmitString(*name_str);
    else
      emitter.EmitInt(name);
  }

  void EmitParameter(const Parameter& param) {
    util::Match(
        param.GetVariant().v,                    //
        [&](bool v) { emitter.EmitBool(v); },    //
        [&](float v) { emitter.EmitFloat(v); },  //
        [&](int v) { emitter.EmitInt(v); },      //
        [&](const Vector2f& v) { emitter.EmitSimpleSequence(v.fields(), "!vec2"); },
        [&](const Vector3f& v) { emitter.EmitSimpleSequence(v.fields(), "!vec3"); },
        [&](const Vector4f& v) { emitter.EmitSimpleSequence(v.fields(), "!vec4"); },
        [&](const Color4f& v) { emitter.EmitSimpleSequence(v.fields(), "!color"); },
        [&](const FixedSafeString<32>& v) { emitter.EmitString(v, "!str32"); },
        [&](const FixedSafeString<64>& v) { emitter.EmitString(v, "!str64"); },
        [&](const std::array<Curve, 1>& v) { EmitCurves(v); },
        [&](const std::array<Curve, 2>& v) { EmitCurves(v); },
        [&](const std::array<Curve, 3>& v) { EmitCurves(v); },
        [&](const std::array<Curve, 4>& v) { EmitCurves(v); },
        [&](const std::vector<int>& v) { emitter.EmitSimpleSequence<int>(v, "!buffer_int"); },
        [&](const std::vector<f32>& v) { emitter.EmitSimpleSequence<f32>(v, "!buffer_f32"); },
        [&](const FixedSafeString<256>& v) { emitter.EmitString(v, "!str256"); },
        [&](const Quatf& v) { emitter.EmitSimpleSequence(v.fields(), "!quat"); },
        [&](U32 v) { emitter.EmitInt(v, "!u"); },
        [&](const std::vector<u32>& v) { emitter.EmitSimpleSequence<u32>(v, "!buffer_u32"); },
        [&](const std::vector<u8>& v) { emitter.EmitSimpleSequence<u8>(v, "!buffer_binary"); },
        [&](const std::string& v) { emitter.EmitString(v); });
  }

  void EmitParameterObject(const ParameterObject& pobject, Name parent_name) {
    yml::LibyamlEmitter::MappingScope scope{emitter, "!obj", YAML_BLOCK_MAPPING_STYLE};
    size_t i = 0;
    for (const auto& [name, param] : pobject.params) {
      EmitName(name, i++, parent_name);
      EmitParameter(param);
    }
  }

  void EmitParameterList(const ParameterList& plist, Name parent_name) {
    yml::LibyamlEmitter::MappingScope scope{emitter, "!list", YAML_BLOCK_MAPPING_STYLE};

    emitter.EmitString("objects");
    {
      yml::LibyamlEmitter::MappingScope subscope{emitter, {}, YAML_BLOCK_MAPPING_STYLE};
      size_t i = 0;
      for (const auto& [name, object] : plist.objects) {
        EmitName(name, i++, parent_name);
        EmitParameterObject(object, name);
      }
    }

    emitter.EmitString("lists");
    {
      yml::LibyamlEmitter::MappingScope subscope{emitter, {}, YAML_BLOCK_MAPPING_STYLE};
      size_t i = 0;
      for (const auto& [name, list] : plist.lists) {
        EmitName(name, i++, parent_name);
        EmitParameterList(list, name);
      }
    }
  }

  void EmitParameterIO(const ParameterIO& pio) {
    yml::LibyamlEmitter::MappingScope scope{emitter, "!io", YAML_BLOCK_MAPPING_STYLE};

    emitter.EmitString("version");
    emitter.EmitInt(pio.version);

    emitter.EmitString("type");
    emitter.EmitString(pio.type);

    emitter.EmitString("param_root");
    EmitParameterList(pio, ParameterIO::ParamRootKey);
  }

  void EmitCurves(tcb::span<const Curve> curves) {
    yaml_event_t event;
    yaml_sequence_start_event_initialize(&event, nullptr, (const u8*)"!curve", 0,
                                         YAML_FLOW_SEQUENCE_STYLE);
    emitter.Emit(event);

    for (const Curve& curve : curves) {
      emitter.EmitInt(curve.a);
      emitter.EmitInt(curve.b);
      for (float v : curve.floats)
        emitter.EmitFloat(v);
    }

    yaml_sequence_end_event_initialize(&event);
    emitter.Emit(event);
  }

  NameTable m_extra_name_table{false};
  yml::LibyamlEmitterWithStorage<std::string> emitter;
};

std::string ParameterIO::ToText() const {
  TextEmitter emitter;
  return emitter.Emit(*this);
}

}  // namespace oead::aamp