//===-- wrapper_function_utils.h - Utilities for wrapper funcs --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of the ORC runtime support library.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_WRAPPER_FUNCTION_UTILS_H
#define ORC_RT_WRAPPER_FUNCTION_UTILS_H

#include "c_api.h"
#include "common.h"
#include "error.h"
#include "executor_address.h"
#include "simple_packed_serialization.h"
#include <type_traits>

namespace __orc_rt {

/// C++ wrapper function result: Same as CWrapperFunctionResult but
/// auto-releases memory.
class WrapperFunctionResult {
public:
  /// Create a default WrapperFunctionResult.
  WrapperFunctionResult() { __orc_rt_CWrapperFunctionResultInit(&R); }

  /// Create a WrapperFunctionResult from a CWrapperFunctionResult. This
  /// instance takes ownership of the result object and will automatically
  /// call dispose on the result upon destruction.
  WrapperFunctionResult(__orc_rt_CWrapperFunctionResult R) : R(R) {}

  WrapperFunctionResult(const WrapperFunctionResult &) = delete;
  WrapperFunctionResult &operator=(const WrapperFunctionResult &) = delete;

  WrapperFunctionResult(WrapperFunctionResult &&Other) {
    __orc_rt_CWrapperFunctionResultInit(&R);
    std::swap(R, Other.R);
  }

  WrapperFunctionResult &operator=(WrapperFunctionResult &&Other) {
    __orc_rt_CWrapperFunctionResult Tmp;
    __orc_rt_CWrapperFunctionResultInit(&Tmp);
    std::swap(Tmp, Other.R);
    std::swap(R, Tmp);
    return *this;
  }

  ~WrapperFunctionResult() { __orc_rt_DisposeCWrapperFunctionResult(&R); }

  /// Relinquish ownership of and return the
  /// __orc_rt_CWrapperFunctionResult.
  __orc_rt_CWrapperFunctionResult release() {
    __orc_rt_CWrapperFunctionResult Tmp;
    __orc_rt_CWrapperFunctionResultInit(&Tmp);
    std::swap(R, Tmp);
    return Tmp;
  }

  /// Get a pointer to the data contained in this instance.
  char *data() { return __orc_rt_CWrapperFunctionResultData(&R); }

  /// Returns the size of the data contained in this instance.
  size_t size() const { return __orc_rt_CWrapperFunctionResultSize(&R); }

  /// Returns true if this value is equivalent to a default-constructed
  /// WrapperFunctionResult.
  bool empty() const { return __orc_rt_CWrapperFunctionResultEmpty(&R); }

  /// Create a WrapperFunctionResult with the given size and return a pointer
  /// to the underlying memory.
  static WrapperFunctionResult allocate(size_t Size) {
    WrapperFunctionResult R;
    R.R = __orc_rt_CWrapperFunctionResultAllocate(Size);
    return R;
  }

  /// Copy from the given char range.
  static WrapperFunctionResult copyFrom(const char *Source, size_t Size) {
    return __orc_rt_CreateCWrapperFunctionResultFromRange(Source, Size);
  }

  /// Copy from the given null-terminated string (includes the null-terminator).
  static WrapperFunctionResult copyFrom(const char *Source) {
    return __orc_rt_CreateCWrapperFunctionResultFromString(Source);
  }

  /// Copy from the given std::string (includes the null terminator).
  static WrapperFunctionResult copyFrom(const std::string &Source) {
    return copyFrom(Source.c_str());
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandError(const char *Msg) {
    return __orc_rt_CreateCWrapperFunctionResultFromOutOfBandError(Msg);
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandError(const std::string &Msg) {
    return createOutOfBandError(Msg.c_str());
  }

  /// If this value is an out-of-band error then this returns the error message,
  /// otherwise returns nullptr.
  const char *getOutOfBandError() const {
    return __orc_rt_CWrapperFunctionResultGetOutOfBandError(&R);
  }

private:
  __orc_rt_CWrapperFunctionResult R;
};

namespace detail {

template <typename SPSArgListT, typename... ArgTs>
WrapperFunctionResult
serializeViaSPSToWrapperFunctionResult(const ArgTs &...Args) {
  auto Result = WrapperFunctionResult::allocate(SPSArgListT::size(Args...));
  SPSOutputBuffer OB(Result.data(), Result.size());
  if (!SPSArgListT::serialize(OB, Args...))
    return WrapperFunctionResult::createOutOfBandError(
        "Error serializing arguments to blob in call");
  return Result;
}

template <typename RetT> class WrapperFunctionHandlerCaller {
public:
  template <typename HandlerT, typename ArgTupleT, std::size_t... I>
  static decltype(auto) call(HandlerT &&H, ArgTupleT &Args,
                             std::index_sequence<I...>) {
    return std::forward<HandlerT>(H)(std::get<I>(Args)...);
  }
};

template <> class WrapperFunctionHandlerCaller<void> {
public:
  template <typename HandlerT, typename ArgTupleT, std::size_t... I>
  static SPSEmpty call(HandlerT &&H, ArgTupleT &Args,
                       std::index_sequence<I...>) {
    std::forward<HandlerT>(H)(std::get<I>(Args)...);
    return SPSEmpty();
  }
};

template <typename WrapperFunctionImplT,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper
    : public WrapperFunctionHandlerHelper<
          decltype(&std::remove_reference_t<WrapperFunctionImplT>::operator()),
          ResultSerializer, SPSTagTs...> {};

template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                   SPSTagTs...> {
public:
  using ArgTuple = std::tuple<std::decay_t<ArgTs>...>;
  using ArgIndices = std::make_index_sequence<std::tuple_size<ArgTuple>::value>;

  template <typename HandlerT>
  static WrapperFunctionResult apply(HandlerT &&H, const char *ArgData,
                                     size_t ArgSize) {
    ArgTuple Args;
    if (!deserialize(ArgData, ArgSize, Args, ArgIndices{}))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not deserialize arguments for wrapper function call");

    auto HandlerResult = WrapperFunctionHandlerCaller<RetT>::call(
        std::forward<HandlerT>(H), Args, ArgIndices{});

    return ResultSerializer<decltype(HandlerResult)>::serialize(
        std::move(HandlerResult));
  }

private:
  template <std::size_t... I>
  static bool deserialize(const char *ArgData, size_t ArgSize, ArgTuple &Args,
                          std::index_sequence<I...>) {
    SPSInputBuffer IB(ArgData, ArgSize);
    return SPSArgList<SPSTagTs...>::deserialize(IB, std::get<I>(Args)...);
  }
};

// Map function pointers to function types.
template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (*)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map non-const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...) const,
                                   ResultSerializer, SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

template <typename SPSRetTagT, typename RetT> class ResultSerializer {
public:
  static WrapperFunctionResult serialize(RetT Result) {
    return serializeViaSPSToWrapperFunctionResult<SPSArgList<SPSRetTagT>>(
        Result);
  }
};

template <typename SPSRetTagT> class ResultSerializer<SPSRetTagT, Error> {
public:
  static WrapperFunctionResult serialize(Error Err) {
    return serializeViaSPSToWrapperFunctionResult<SPSArgList<SPSRetTagT>>(
        toSPSSerializable(std::move(Err)));
  }
};

template <typename SPSRetTagT, typename T>
class ResultSerializer<SPSRetTagT, Expected<T>> {
public:
  static WrapperFunctionResult serialize(Expected<T> E) {
    return serializeViaSPSToWrapperFunctionResult<SPSArgList<SPSRetTagT>>(
        toSPSSerializable(std::move(E)));
  }
};

template <typename SPSRetTagT, typename RetT> class ResultDeserializer {
public:
  static void makeSafe(RetT &Result) {}

  static Error deserialize(RetT &Result, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    if (!SPSArgList<SPSRetTagT>::deserialize(IB, Result))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    return Error::success();
  }
};

template <> class ResultDeserializer<SPSError, Error> {
public:
  static void makeSafe(Error &Err) { cantFail(std::move(Err)); }

  static Error deserialize(Error &Err, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableError BSE;
    if (!SPSArgList<SPSError>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    Err = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

template <typename SPSTagT, typename T>
class ResultDeserializer<SPSExpected<SPSTagT>, Expected<T>> {
public:
  static void makeSafe(Expected<T> &E) { cantFail(E.takeError()); }

  static Error deserialize(Expected<T> &E, const char *ArgData,
                           size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableExpected<T> BSE;
    if (!SPSArgList<SPSExpected<SPSTagT>>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    E = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

} // end namespace detail

template <typename SPSSignature> class WrapperFunction;

template <typename SPSRetTagT, typename... SPSTagTs>
class WrapperFunction<SPSRetTagT(SPSTagTs...)> {
private:
  template <typename RetT>
  using ResultSerializer = detail::ResultSerializer<SPSRetTagT, RetT>;

public:
  template <typename RetT, typename... ArgTs>
  static Error call(const void *FnTag, RetT &Result, const ArgTs &...Args) {

    // RetT might be an Error or Expected value. Set the checked flag now:
    // we don't want the user to have to check the unused result if this
    // operation fails.
    detail::ResultDeserializer<SPSRetTagT, RetT>::makeSafe(Result);

    if (ORC_RT_UNLIKELY(!&__orc_rt_jit_dispatch_ctx))
      return make_error<StringError>("__orc_rt_jit_dispatch_ctx not set");
    if (ORC_RT_UNLIKELY(!&__orc_rt_jit_dispatch))
      return make_error<StringError>("__orc_rt_jit_dispatch not set");

    auto ArgBuffer =
        detail::serializeViaSPSToWrapperFunctionResult<SPSArgList<SPSTagTs...>>(
            Args...);
    if (const char *ErrMsg = ArgBuffer.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    WrapperFunctionResult ResultBuffer = __orc_rt_jit_dispatch(
        &__orc_rt_jit_dispatch_ctx, FnTag, ArgBuffer.data(), ArgBuffer.size());
    if (auto ErrMsg = ResultBuffer.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    return detail::ResultDeserializer<SPSRetTagT, RetT>::deserialize(
        Result, ResultBuffer.data(), ResultBuffer.size());
  }

  template <typename HandlerT>
  static WrapperFunctionResult handle(const char *ArgData, size_t ArgSize,
                                      HandlerT &&Handler) {
    using WFHH =
        detail::WrapperFunctionHandlerHelper<std::remove_reference_t<HandlerT>,
                                             ResultSerializer, SPSTagTs...>;
    return WFHH::apply(std::forward<HandlerT>(Handler), ArgData, ArgSize);
  }

private:
  template <typename T> static const T &makeSerializable(const T &Value) {
    return Value;
  }

  static detail::SPSSerializableError makeSerializable(Error Err) {
    return detail::toSPSSerializable(std::move(Err));
  }

  template <typename T>
  static detail::SPSSerializableExpected<T> makeSerializable(Expected<T> E) {
    return detail::toSPSSerializable(std::move(E));
  }
};

template <typename... SPSTagTs>
class WrapperFunction<void(SPSTagTs...)>
    : private WrapperFunction<SPSEmpty(SPSTagTs...)> {
public:
  template <typename... ArgTs>
  static Error call(const void *FnTag, const ArgTs &...Args) {
    SPSEmpty BE;
    return WrapperFunction<SPSEmpty(SPSTagTs...)>::call(FnTag, BE, Args...);
  }

  using WrapperFunction<SPSEmpty(SPSTagTs...)>::handle;
};

/// A function object that takes an ExecutorAddr as its first argument,
/// casts that address to a ClassT*, then calls the given method on that
/// pointer passing in the remaining function arguments. This utility
/// removes some of the boilerplate from writing wrappers for method calls.
///
///   @code{.cpp}
///   class MyClass {
///   public:
///     void myMethod(uint32_t, bool) { ... }
///   };
///
///   // SPS Method signature -- note MyClass object address as first argument.
///   using SPSMyMethodWrapperSignature =
///     SPSTuple<SPSExecutorAddr, uint32_t, bool>;
///
///   WrapperFunctionResult
///   myMethodCallWrapper(const char *ArgData, size_t ArgSize) {
///     return WrapperFunction<SPSMyMethodWrapperSignature>::handle(
///        ArgData, ArgSize, makeMethodWrapperHandler(&MyClass::myMethod));
///   }
///   @endcode
///
template <typename RetT, typename ClassT, typename... ArgTs>
class MethodWrapperHandler {
public:
  using MethodT = RetT (ClassT::*)(ArgTs...);
  MethodWrapperHandler(MethodT M) : M(M) {}
  RetT operator()(ExecutorAddr ObjAddr, ArgTs &...Args) {
    return (ObjAddr.toPtr<ClassT *>()->*M)(std::forward<ArgTs>(Args)...);
  }

private:
  MethodT M;
};

/// Create a MethodWrapperHandler object from the given method pointer.
template <typename RetT, typename ClassT, typename... ArgTs>
MethodWrapperHandler<RetT, ClassT, ArgTs...>
makeMethodWrapperHandler(RetT (ClassT::*Method)(ArgTs...)) {
  return MethodWrapperHandler<RetT, ClassT, ArgTs...>(Method);
}

} // end namespace __orc_rt

#endif // ORC_RT_WRAPPER_FUNCTION_UTILS_H
