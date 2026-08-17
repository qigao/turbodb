/*
Author: YangJian
Date: 2023-05-11
*/
#ifndef ORM_ENTITY_MODEL_DETAIL_HPP
#define ORM_ENTITY_MODEL_DETAIL_HPP
#include <type_traits>
#include <vector>
#include <tuple>
#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <ostream>
#include <stdexcept>

namespace orm::model {

#define MACRO_EXPAND(...) __VA_ARGS__
#define ADD_VIEW(str) std::string_view(#str, sizeof(#str) - 1)
#define SEPARATOR ,
#define CON_STR_1(element) ADD_VIEW(element)
#define CON_STR_2(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_1(__VA_ARGS__))
#define CON_STR_3(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_2(__VA_ARGS__))
#define CON_STR_4(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_3(__VA_ARGS__))
#define CON_STR_5(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_4(__VA_ARGS__))
#define CON_STR_6(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_5(__VA_ARGS__))
#define CON_STR_7(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_6(__VA_ARGS__))
#define CON_STR_8(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_7(__VA_ARGS__))
#define CON_STR_9(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_8(__VA_ARGS__))
#define CON_STR_10(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_9(__VA_ARGS__))
#define CON_STR_11(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_10(__VA_ARGS__))
#define CON_STR_12(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_11(__VA_ARGS__))
#define CON_STR_13(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_12(__VA_ARGS__))
#define CON_STR_14(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_13(__VA_ARGS__))
#define CON_STR_15(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_14(__VA_ARGS__))
#define CON_STR_16(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_15(__VA_ARGS__))
#define CON_STR_17(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_16(__VA_ARGS__))
#define CON_STR_18(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_17(__VA_ARGS__))
#define CON_STR_19(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_18(__VA_ARGS__))
#define CON_STR_20(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_19(__VA_ARGS__))
#define CON_STR_21(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_20(__VA_ARGS__))
#define CON_STR_22(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_21(__VA_ARGS__))
#define CON_STR_23(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_22(__VA_ARGS__))
#define CON_STR_24(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_23(__VA_ARGS__))
#define CON_STR_25(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_24(__VA_ARGS__))
#define CON_STR_26(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_25(__VA_ARGS__))
#define CON_STR_27(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_26(__VA_ARGS__))
#define CON_STR_28(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_27(__VA_ARGS__))
#define CON_STR_29(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_28(__VA_ARGS__))
#define CON_STR_30(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_29(__VA_ARGS__))
#define CON_STR_31(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_30(__VA_ARGS__))
#define CON_STR_32(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_31(__VA_ARGS__))
#define CON_STR_33(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_32(__VA_ARGS__))
#define CON_STR_34(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_33(__VA_ARGS__))
#define CON_STR_35(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_34(__VA_ARGS__))
#define CON_STR_36(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_35(__VA_ARGS__))
#define CON_STR_37(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_36(__VA_ARGS__))
#define CON_STR_38(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_37(__VA_ARGS__))
#define CON_STR_39(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_38(__VA_ARGS__))
#define CON_STR_40(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_39(__VA_ARGS__))
#define CON_STR_41(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_40(__VA_ARGS__))
#define CON_STR_42(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_41(__VA_ARGS__))
#define CON_STR_43(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_42(__VA_ARGS__))
#define CON_STR_44(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_43(__VA_ARGS__))
#define CON_STR_45(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_44(__VA_ARGS__))
#define CON_STR_46(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_45(__VA_ARGS__))
#define CON_STR_47(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_46(__VA_ARGS__))
#define CON_STR_48(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_47(__VA_ARGS__))
#define CON_STR_49(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_48(__VA_ARGS__))
#define CON_STR_50(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_49(__VA_ARGS__))
#define CON_STR_51(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_50(__VA_ARGS__))
#define CON_STR_52(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_51(__VA_ARGS__))
#define CON_STR_53(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_52(__VA_ARGS__))
#define CON_STR_54(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_53(__VA_ARGS__))
#define CON_STR_55(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_54(__VA_ARGS__))
#define CON_STR_56(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_55(__VA_ARGS__))
#define CON_STR_57(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_56(__VA_ARGS__))
#define CON_STR_58(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_57(__VA_ARGS__))
#define CON_STR_59(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_58(__VA_ARGS__))
#define CON_STR_60(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_59(__VA_ARGS__))
#define CON_STR_61(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_60(__VA_ARGS__))
#define CON_STR_62(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_61(__VA_ARGS__))
#define CON_STR_63(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_62(__VA_ARGS__))
#define CON_STR_64(element, ...) ADD_VIEW(element) SEPARATOR MACRO_EXPAND(CON_STR_63(__VA_ARGS__))

#define MACRO_FILTER(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, _N, ...) _N
#define RNG_N() 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1
#define MACRO_ARGS_INNER(...) MACRO_FILTER(__VA_ARGS__)
#define MACRO_ARGS_SIZE(...) MACRO_ARGS_INNER(__VA_ARGS__, RNG_N(), 0)

#define MACRO_CONCAT(a, b) a##_##b
#define FIELD(f) f

#define MAKE_ARG_LIST_1(op, arg) op(arg)
#define MAKE_ARG_LIST_2(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_1(op, __VA_ARGS__))
#define MAKE_ARG_LIST_3(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_2(op, __VA_ARGS__))
#define MAKE_ARG_LIST_4(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_3(op, __VA_ARGS__))
#define MAKE_ARG_LIST_5(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_4(op, __VA_ARGS__))
#define MAKE_ARG_LIST_6(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_5(op, __VA_ARGS__))
#define MAKE_ARG_LIST_7(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_6(op, __VA_ARGS__))
#define MAKE_ARG_LIST_8(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_7(op, __VA_ARGS__))
#define MAKE_ARG_LIST_9(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_8(op, __VA_ARGS__))
#define MAKE_ARG_LIST_10(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_9(op, __VA_ARGS__))
#define MAKE_ARG_LIST_11(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_10(op, __VA_ARGS__))
#define MAKE_ARG_LIST_12(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_11(op, __VA_ARGS__))
#define MAKE_ARG_LIST_13(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_12(op, __VA_ARGS__))
#define MAKE_ARG_LIST_14(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_13(op, __VA_ARGS__))
#define MAKE_ARG_LIST_15(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_14(op, __VA_ARGS__))
#define MAKE_ARG_LIST_16(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_15(op, __VA_ARGS__))
#define MAKE_ARG_LIST_17(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_16(op, __VA_ARGS__))
#define MAKE_ARG_LIST_18(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_17(op, __VA_ARGS__))
#define MAKE_ARG_LIST_19(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_18(op, __VA_ARGS__))
#define MAKE_ARG_LIST_20(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_19(op, __VA_ARGS__))
#define MAKE_ARG_LIST_21(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_20(op, __VA_ARGS__))
#define MAKE_ARG_LIST_22(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_21(op, __VA_ARGS__))
#define MAKE_ARG_LIST_23(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_22(op, __VA_ARGS__))
#define MAKE_ARG_LIST_24(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_23(op, __VA_ARGS__))
#define MAKE_ARG_LIST_25(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_24(op, __VA_ARGS__))
#define MAKE_ARG_LIST_26(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_25(op, __VA_ARGS__))
#define MAKE_ARG_LIST_27(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_26(op, __VA_ARGS__))
#define MAKE_ARG_LIST_28(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_27(op, __VA_ARGS__))
#define MAKE_ARG_LIST_29(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_28(op, __VA_ARGS__))
#define MAKE_ARG_LIST_30(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_29(op, __VA_ARGS__))
#define MAKE_ARG_LIST_31(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_30(op, __VA_ARGS__))
#define MAKE_ARG_LIST_32(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_31(op, __VA_ARGS__))
#define MAKE_ARG_LIST_33(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_32(op, __VA_ARGS__))
#define MAKE_ARG_LIST_34(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_33(op, __VA_ARGS__))
#define MAKE_ARG_LIST_35(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_34(op, __VA_ARGS__))
#define MAKE_ARG_LIST_36(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_35(op, __VA_ARGS__))
#define MAKE_ARG_LIST_37(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_36(op, __VA_ARGS__))
#define MAKE_ARG_LIST_38(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_37(op, __VA_ARGS__))
#define MAKE_ARG_LIST_39(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_38(op, __VA_ARGS__))
#define MAKE_ARG_LIST_40(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_39(op, __VA_ARGS__))
#define MAKE_ARG_LIST_41(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_40(op, __VA_ARGS__))
#define MAKE_ARG_LIST_42(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_41(op, __VA_ARGS__))
#define MAKE_ARG_LIST_43(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_42(op, __VA_ARGS__))
#define MAKE_ARG_LIST_44(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_43(op, __VA_ARGS__))
#define MAKE_ARG_LIST_45(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_44(op, __VA_ARGS__))
#define MAKE_ARG_LIST_46(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_45(op, __VA_ARGS__))
#define MAKE_ARG_LIST_47(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_46(op, __VA_ARGS__))
#define MAKE_ARG_LIST_48(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_47(op, __VA_ARGS__))
#define MAKE_ARG_LIST_49(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_48(op, __VA_ARGS__))
#define MAKE_ARG_LIST_50(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_49(op, __VA_ARGS__))
#define MAKE_ARG_LIST_51(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_50(op, __VA_ARGS__))
#define MAKE_ARG_LIST_52(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_51(op, __VA_ARGS__))
#define MAKE_ARG_LIST_53(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_52(op, __VA_ARGS__))
#define MAKE_ARG_LIST_54(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_53(op, __VA_ARGS__))
#define MAKE_ARG_LIST_55(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_54(op, __VA_ARGS__))
#define MAKE_ARG_LIST_56(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_55(op, __VA_ARGS__))
#define MAKE_ARG_LIST_57(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_56(op, __VA_ARGS__))
#define MAKE_ARG_LIST_58(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_57(op, __VA_ARGS__))
#define MAKE_ARG_LIST_59(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_58(op, __VA_ARGS__))
#define MAKE_ARG_LIST_60(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_59(op, __VA_ARGS__))
#define MAKE_ARG_LIST_61(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_60(op, __VA_ARGS__))
#define MAKE_ARG_LIST_62(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_61(op, __VA_ARGS__))
#define MAKE_ARG_LIST_63(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_62(op, __VA_ARGS__))
#define MAKE_ARG_LIST_64(op, arg, ...) op(arg), MACRO_EXPAND(MAKE_ARG_LIST_63(op, __VA_ARGS__))

#define MAKE_ARG_LIST(N, op, ...) MACRO_CONCAT(MAKE_ARG_LIST, N)(op, __VA_ARGS__)

#define MAKE_ENTITY_MODEL(class_name, ...)                                           \
inline auto entity_model_func(class_name const &){                                  \
  struct static_entity_model                                                        \
  {                                                                                 \
    constexpr decltype(auto) static apply_impl()                                    \
    {                                                                               \
      return std::make_tuple(__VA_ARGS__);                                          \
    }                                                                               \
    using size_type = std::integral_constant<size_t, MACRO_ARGS_SIZE(__VA_ARGS__)>; \
    constexpr static std::string_view name() { return name_##class_name; }          \
    constexpr static std::string_view fields() {  return fields_##class_name; }     \
    constexpr static std::string_view primary_key() {                              \
      return primary_key_##class_name;                                              \
    }                                                                               \
    constexpr static std::string_view version() {                                  \
      return version_##class_name;                                                  \
    }                                                                               \
    constexpr static size_t value() { return size_type::value; }                    \
    constexpr static std::array<std::string_view, size_type::value> arr()           \
    { return arr_##class_name; }                                                    \
  };                                                                                \
  return static_entity_model{};                                                     \
}

#define MAKE_META_DATA(class_name, table_name, N, ...)                  \
  constexpr std::array<std::string_view, N> arr_##class_name = { \
      MACRO_EXPAND(MACRO_CONCAT(CON_STR, N)(__VA_ARGS__)) };              \
  constexpr std::string_view fields_##class_name = { #__VA_ARGS__ }; \
  constexpr std::string_view name_##class_name = table_name; \
  constexpr std::string_view primary_key_##class_name = "id"; \
  constexpr std::string_view version_##class_name = {}; \
  MAKE_ENTITY_MODEL(class_name, MAKE_ARG_LIST(N, &class_name::FIELD, __VA_ARGS__))\

#define MAKE_ENTITY_META_DATA(class_name, table_name, primary_key_field, N, ...) \
  constexpr std::array<std::string_view, N> arr_##class_name = {                  \
      MACRO_EXPAND(MACRO_CONCAT(CON_STR, N)(__VA_ARGS__)) };                       \
  constexpr std::string_view fields_##class_name = { #__VA_ARGS__ };              \
  constexpr std::string_view name_##class_name = table_name;                       \
  constexpr std::string_view primary_key_##class_name = #primary_key_field;        \
  constexpr std::string_view version_##class_name = {};                            \
  MAKE_ENTITY_MODEL(class_name,                                                     \
                       MAKE_ARG_LIST(N, &class_name::FIELD, __VA_ARGS__))

#define MAKE_VERSIONED_ENTITY_META_DATA(class_name, table_name, primary_key_field,  \
                                        version_field, N, ...)                      \
  constexpr std::array<std::string_view, N> arr_##class_name = {                   \
      MACRO_EXPAND(MACRO_CONCAT(CON_STR, N)(__VA_ARGS__)) };                        \
  constexpr std::string_view fields_##class_name = { #__VA_ARGS__ };               \
  constexpr std::string_view name_##class_name = table_name;                        \
  constexpr std::string_view primary_key_##class_name = #primary_key_field;         \
  constexpr std::string_view version_##class_name = #version_field;                 \
  MAKE_ENTITY_MODEL(class_name,                                                      \
                       MAKE_ARG_LIST(N, &class_name::FIELD, __VA_ARGS__))


/* Entity-model macros must be used at namespace scope and require an unqualified,
 * non-template class name (no `ns::Type` or template-id), because they paste the
 * class token into generated symbol names. They support 1..64 fields.
 *
 * fields() returns the full #__VA_ARGS__ string (comma-joined), while arr()
 * returns the individual field names.
 */
#define ORM_MODEL(class_name, ...) \
    MAKE_META_DATA(class_name, #class_name, MACRO_ARGS_SIZE(__VA_ARGS__), __VA_ARGS__) \

#define ORM_MODEL_WITH_NAME(class_name, table_name, ...) \
    MAKE_META_DATA(class_name, table_name, MACRO_ARGS_SIZE(__VA_ARGS__), __VA_ARGS__) \

#define ORM_MODEL_ENTITY(class_name, table_name, primary_key_field, ...)            \
    MAKE_ENTITY_META_DATA(class_name, table_name, primary_key_field,                 \
                          MACRO_ARGS_SIZE(__VA_ARGS__), __VA_ARGS__)                 \

#define ORM_MODEL_VERSIONED_ENTITY(class_name, table_name, primary_key_field,       \
                                   version_field, ...)                              \
    MAKE_VERSIONED_ENTITY_META_DATA(class_name, table_name, primary_key_field,       \
                                    version_field, MACRO_ARGS_SIZE(__VA_ARGS__),     \
                                    __VA_ARGS__)                                    \

template <typename T>
using entity_model = decltype(entity_model_func(std::declval<T>()));

template <typename T, typename = void>
struct is_entity : std::false_type {};

template <typename T>
struct is_entity<T, std::void_t<decltype(entity_model<T>::arr())>>
    : std::true_type {};

template <typename T, typename = void>
struct has_relations : std::false_type {};

template <typename T>
struct has_relations<T, std::void_t<decltype(entity_model<T>::relations())>>
    : std::true_type {};


template <template <typename...> class U, typename T>
struct is_template_instant_of : std::false_type {};

template <template <typename...> class U, typename... args>
struct is_template_instant_of<U, U<args...>> : std::true_type {};

template <typename T>
struct is_stdstring : is_template_instant_of<std::basic_string, T> {};

template <typename T>
struct is_tuple : is_template_instant_of<std::tuple, T> {};

template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<T>())>>
    : std::true_type {};

template <typename T>
inline constexpr bool is_streamable_v = is_streamable<T>::value;

template <typename>
inline constexpr bool always_false_v = false;


template <typename T>
inline constexpr bool is_entity_v = is_entity<T>::value;

template <typename T>
inline constexpr bool has_relations_v = has_relations<T>::value;

template <typename... Args, typename A, typename F, std::size_t... Idx>
constexpr void for_each(const std::tuple<Args...>& t, const A& arr, F&& f,
                        std::index_sequence<Idx...>) {
  (std::forward<F>(f)(std::get<Idx>(t), arr[Idx], std::integral_constant<size_t, Idx>{}),
   ...);
}

template <typename...Args, typename F, std::size_t...Idx>
constexpr void for_each(std::tuple<Args...>& t, F&& f, std::index_sequence<Idx...>)
{
  (std::forward<F>(f)(std::get<Idx>(t), std::integral_constant<size_t, Idx>{}), ...);
}

template <typename...Args, typename F, std::size_t...Idx>
constexpr void for_each(const std::tuple<Args...>& t, F&& f, std::index_sequence<Idx...>)
{
  (std::forward<F>(f)(std::get<Idx>(t), std::integral_constant<size_t, Idx>{}), ...);
}

template <typename T, typename F>
constexpr std::enable_if_t<is_entity<T>::value> for_each(T&& t, F&& f) {
  using M = decltype(entity_model_func(std::forward<T>(t)));
  for_each(M::apply_impl(), M::arr(), std::forward<F>(f),
           std::make_index_sequence<M::value()>{});
}

template<typename T, typename F>
constexpr std::enable_if_t<!is_entity<T>::value && is_tuple<std::decay_t<T>>::value> for_each(T&& t, F&& f) {
  for_each(std::forward<T>(t),
           std::forward<F>(f),
           std::make_index_sequence<std::tuple_size_v<std::decay_t<T>>>{}
          );
}


template <typename T>
constexpr void set_param_values(std::ostream& os, const std::string_view& field,
                                T &&value, size_t)
{
  os << field<< ":"<< value <<" ";
}

template <typename T>
std::string serialize(const T& t)
{
  std::stringstream ss;
  for_each(t,
          [&t, &ss](auto item, auto field, auto) {
            using value_type = decltype(t.*item);
            if constexpr (is_streamable_v<value_type>) {
              set_param_values(ss, field, t.*item, 0);
            } else {
              static_assert(always_false_v<value_type>,
                            "orm::model::serialize: member type is not streamable");
            }
          });
  return ss.str();
}

template<typename T>
constexpr typename std::enable_if<!is_entity<T>::value, std::size_t>::type get_value()
{
  return 0;
}

template<typename T>
constexpr typename std::enable_if<is_entity<T>::value, std::size_t>::type get_value()
{
  using M = decltype(entity_model_func(std::declval<T>()));
  return M::value();
}

template<typename T>
constexpr std::size_t get_index(std::string_view field)
{
  using M = decltype(entity_model_func(std::declval<T>()));
  const auto arr = M::arr();
  const auto it = std::find_if(arr.begin(), arr.end(), [&field](auto f) {
    return std::string_view(f) == field;
  });
  return it == arr.end() ? std::string_view::npos
                         : static_cast<std::size_t>(std::distance(arr.begin(), it));
}

template<typename T>
constexpr auto get_array()
{
  using M = decltype(entity_model_func(std::declval<T>()));
  return M::arr();
}

template<typename T>
constexpr std::string_view get_field()
{
  using M = decltype(entity_model_func(std::declval<T>()));
  return M::fields();
}

template<typename T>
constexpr auto get_name()
{
  using M = decltype(entity_model_func(std::declval<T>()));
  return M::name();
}

template <typename T, typename = void>
struct has_primary_key_metadata : std::false_type {};

template <typename T>
struct has_primary_key_metadata<
    T, std::void_t<decltype(entity_model<T>::primary_key())>> : std::true_type {};

template <typename T>
constexpr std::string_view get_primary_key()
{
  using M = entity_model<T>;
  if constexpr (has_primary_key_metadata<T>::value) {
    return M::primary_key();
  } else {
    return "id";
  }
}

template <typename T, typename = void>
struct has_primary_keys_metadata : std::false_type {};

template <typename T>
struct has_primary_keys_metadata<
    T, std::void_t<decltype(entity_model<T>::primary_keys())>>
    : std::true_type {};

template <typename T>
constexpr auto get_primary_keys() {
  using M = entity_model<T>;
  if constexpr (has_primary_keys_metadata<T>::value) {
    return M::primary_keys();
  } else {
    return std::array<std::string_view, 1>{get_primary_key<T>()};
  }
}

template <typename T, typename = void>
struct has_version_metadata : std::false_type {};

template <typename T>
struct has_version_metadata<
    T, std::void_t<decltype(entity_model<T>::version())>> : std::true_type {};

template <typename T>
constexpr std::string_view get_version()
{
  using M = entity_model<T>;
  if constexpr (has_version_metadata<T>::value) {
    return M::version();
  } else {
    return {};
  }
}

template <typename T, typename = void>
struct has_discriminator_metadata : std::false_type {};

template <typename T>
struct has_discriminator_metadata<
    T, std::void_t<decltype(entity_model<T>::discriminator_column()),
                   decltype(entity_model<T>::discriminator_value())>>
    : std::true_type {};

template <typename T>
inline constexpr bool has_discriminator_metadata_v =
    has_discriminator_metadata<T>::value;

template <typename T>
constexpr std::string_view get_discriminator_column() {
  static_assert(has_discriminator_metadata_v<T>,
                "ORM entity has no discriminator metadata");
  return entity_model<T>::discriminator_column();
}

template <typename T>
constexpr std::string_view get_discriminator_value() {
  static_assert(has_discriminator_metadata_v<T>,
                "ORM entity has no discriminator metadata");
  return entity_model<T>::discriminator_value();
}

template <typename T, typename = void>
struct has_inheritance_hierarchy : std::false_type {};

template <typename T>
struct has_inheritance_hierarchy<
    T, std::void_t<typename entity_model<T>::polymorphic_type,
                   decltype(entity_model<T>::discriminator_cases())>>
    : std::true_type {};

template <typename T>
inline constexpr bool has_inheritance_hierarchy_v =
    has_inheritance_hierarchy<T>::value;

template <typename T, typename = void>
struct has_inheritance_strategy : std::false_type {};

template <typename T>
struct has_inheritance_strategy<
    T, std::void_t<decltype(entity_model<T>::inheritance())>>
    : std::true_type {};

template <typename T>
inline constexpr bool has_inheritance_strategy_v =
    has_inheritance_strategy<T>::value;

template <typename T, typename = void>
struct has_inheritance_tables : std::false_type {};

template <typename T>
struct has_inheritance_tables<
    T, std::void_t<decltype(entity_model<T>::inheritance_tables()),
                   decltype(entity_model<T>::column_tables())>>
    : std::true_type {};

template <typename T>
inline constexpr bool has_inheritance_tables_v =
    has_inheritance_tables<T>::value;

template<typename T>
constexpr auto get_name(size_t idx)
{
  using M = decltype(entity_model_func(std::declval<T>()));
  return M::arr().at(idx);
}

template<typename T, std::size_t I>
constexpr auto get_name()
{
  using M = decltype(entity_model_func(std::declval<T>()));
  static_assert(I < M::value(), "index out of range");
  return M::arr()[I];
}

template<typename T>
std::string_view get_name_impl(const T&, std::size_t i)
{
    return get_name<T>(i);
}

template <size_t I, typename T>
constexpr decltype(auto) get(T&& t) {
  using M = decltype(entity_model_func(std::forward<T>(t)));
  using U = decltype(std::forward<T>(t).*(std::get<I>(M::apply_impl())));

  if constexpr (std::is_array_v<U>) {
    auto s = std::forward<T>(t).*(std::get<I>(M::apply_impl()));
    std::array<char, sizeof(U)> arr;
    memcpy(arr.data(), s, arr.size());
    return arr;
  }
  else
    return std::forward<T>(t).*(std::get<I>(M::apply_impl()));
}


}  // namespace orm::model

#endif  // ORM_ENTITY_MODEL_DETAIL_HPP
