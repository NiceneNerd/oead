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

// To be included in every translation unit.

#pragma once

#include <nonstd/span.h>
#include <optional>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>

#include <oead/types.h>
#include "pybind11_variant_caster.h"

namespace py = pybind11;
using namespace py::literals;

#define OEAD_MAKE_OPAQUE(NAME, ...)                                                                \
  namespace pybind11::detail {                                                                     \
  template <>                                                                                      \
  class type_caster<__VA_ARGS__> : public type_caster_base<__VA_ARGS__> {                          \
  public:                                                                                          \
    static constexpr auto name = _(NAME);                                                          \
  };                                                                                               \
  }

#define OEAD_MAKE_VARIANT_CASTER(...)                                                              \
  namespace pybind11::detail {                                                                     \
  template <>                                                                                      \
  struct type_caster<__VA_ARGS__::Storage> : oead_variant_caster<__VA_ARGS__::Storage> {};         \
  template <>                                                                                      \
  struct type_caster<__VA_ARGS__> : oead_variant_wrapper_caster<__VA_ARGS__> {};                   \
  }

namespace oead::bind {
inline tcb::span<u8> PyBytesToSpan(py::bytes b) {
  return {reinterpret_cast<u8*>(PYBIND11_BYTES_AS_STRING(b.ptr())),
          size_t(PYBIND11_BYTES_SIZE(b.ptr()))};
}

inline tcb::span<u8> PyBufferToSpan(py::buffer b) {
  const py::buffer_info buffer = b.request();
  if (buffer.itemsize != 1 || buffer.ndim != 1 || buffer.size <= 0)
    throw py::value_error("Expected a non-empty bytes-like object");
  return {static_cast<u8*>(buffer.ptr), size_t(buffer.size)};
}

template <typename T, typename Caster>
T CastFromPython(const py::handle& handle) {
  Caster caster;
  if (!caster.load(handle, true))
    throw py::cast_error("Failed to cast Python instance to C++ type");
  return static_cast<T&&>(std::move(caster));
}

template <typename Caster, typename T>
py::object CastToPython(const T& value,
                        py::return_value_policy policy = py::return_value_policy::reference,
                        py::handle parent = {}) {
  Caster caster;
  return py::reinterpret_steal<py::object>(caster.cast(value, policy, parent));
}

template <typename Vector, typename holder_type = std::unique_ptr<Vector>, typename... Args>
py::class_<Vector, holder_type> BindVector(py::handle scope, const std::string& name,
                                           Args&&... args) {
  using Value = typename Vector::value_type;
  using Caster = py::detail::list_caster<Vector, Value>;

  const auto as_list = [](const Vector& vector) { return CastToPython<Caster>(vector); };

  auto cl = py::bind_vector<Vector, holder_type>(scope, name, std::forward<Args>(args)...)
                .def("__str__", [&](const Vector& v) { return py::str(as_list(v)); })
                .def("__repr__", [name, as_list](const Vector& v) {
                  return "{}({!r})"_s.format(name, as_list(v));
                });
  py::implicitly_convertible<py::iterable, Vector>();
  return cl;
}

template <typename Map, typename Key, typename CastFn>
static Map MapFromIter(py::iterator it, CastFn cast_value) {
  Map map;
  while (it != py::iterator::sentinel()) {
    auto pair = py::cast<std::pair<py::handle, py::handle>>(*it);
    map.emplace(pair.first.cast<Key>(), cast_value(pair.second));
    ++it;
  }
  return map;
}

template <typename Map, typename Key, typename CastFn>
static Map MapFromDict(py::dict dict, CastFn cast_value) {
  Map map;
  for (std::pair<py::handle, py::handle> pair : dict)
    map.emplace(pair.first.cast<Key>(), cast_value(pair.second));
  return map;
}

template <typename Map, typename Key, typename Value>
static Value MapCastValue(py::handle handle) {
  if constexpr (std::is_convertible<Map, Value>()) {
    if (py::isinstance<py::dict>(handle))
      return MapFromDict<Map, Key>(handle.cast<py::dict>(), MapCastValue<Map, Key, Value>);
    if (py::isinstance<py::iterator>(handle))
      return MapFromIter<Map, Key>(handle.cast<py::iterator>(), MapCastValue<Map, Key, Value>);
  }
  return handle.cast<Value>();
}

template <typename Map, typename holder_type = std::unique_ptr<Map>, typename... Args>
py::class_<Map, holder_type> BindMap(py::handle scope, const std::string& name, Args&&... args) {
  using Key = typename Map::key_type;
  using Value = typename Map::mapped_type;
  using Caster = py::detail::map_caster<Map, Key, Value>;

  const auto as_dict = [](const Map& map) { return CastToPython<Caster>(map); };
  auto cl =
      py::bind_map<Map, holder_type>(scope, name, std::forward<Args>(args)...)
          .def(py::init([&](py::iterator it) {
                 return MapFromIter<Map, Key>(it, MapCastValue<Map, Key, Value>);
               }),
               "iterator"_a)
          .def(py::init([&](py::dict dict) {
                 return MapFromDict<Map, Key>(dict, MapCastValue<Map, Key, Value>);
               }),
               "dictionary"_a)
          .def("__str__", [as_dict](const Map& map) { return py::str(as_dict(map)); })
          .def("__repr__",
               [name, as_dict](const Map& map) { return "{}({!s})"_s.format(name, as_dict(map)); })
          .def("clear", &Map::clear)
          .def(
              "get",
              [](const Map& map, const Key& key, const std::optional<Value>& value) {
                const auto it = map.find(key);
                if (it == map.cend())
                  return value;
                return std::make_optional(it->second);
              },
              py::return_value_policy::reference_internal, "key"_a, "default"_a = std::nullopt)
          .def(
              "keys", [](const Map& map) { return py::make_key_iterator(map.begin(), map.end()); },
              py::keep_alive<0, 1>());
  py::implicitly_convertible<py::iterator, Map>();
  py::implicitly_convertible<py::dict, Map>();
  return cl;
}
}  // namespace oead::bind

OEAD_MAKE_OPAQUE("oead.Bytes", std::vector<u8>);
OEAD_MAKE_OPAQUE("oead.BufferInt", std::vector<int>);
OEAD_MAKE_OPAQUE("oead.BufferF32", std::vector<f32>);
OEAD_MAKE_OPAQUE("oead.BufferU32", std::vector<u32>);