// Copyright 2019 The Fuchsia Authors. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#pragma once

#include <zxtest/zxtest.h>

#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace mock_function {

// This class mocks a single function.  The Expect*() functions are used by the test to set
// expectations, and Call() is used by the code under test.  There are three variants:
//
// * ExpectCall(return_value, arg1, arg2, ...) sets the expectation that the call will be made with
//   arguments `arg1`, `arg2`, etc., each compared using operator==. `return_value` will be returned
//   unconditionally, or is omitted if the function returns void.
// * ExpectCallWithMatcher(return_value, matcher) uses a `matcher` validate the arguments. The
//   matcher will be called with the arguments to the mocked function call.
// * ExpectNoCall() expects that the function will not be called.
//
// Example:
//
// class SomeClassTest : SomeClass {
// public:
//     zx_status_t CallsSomeMethod();
//
//     mock_function::MockFunction<zx_status_t, uint32_t, uint32_t>& mock_SomeMethod() {
//         return mock_some_method_;
//     }
//
// private:
//     zx_status_t SomeMethod(uint32_t a, uint32_t b) override {
//         return mock_some_method_.Call(a, b);
//     }
//
//     mock_function::MockFunction<zx_status_t, uint32_t, uint32_t> mock_some_method_;
// };
//
// TEST(SomeDriver, SomeTest) {
//     SomeClassTest test;
//     test.mock_SomeMethod().ExpectCall(ZX_OK, 100, 30);
//     test.mock_SomeMethod().ExpectCallWithMatcher(ZX_OK, [](uint32_t a, uint32_t b) {
//         EXPECT_EQ(200, a);
//         EXPECT_EQ(60, b);
//     });
//
//     EXPECT_OK(test.CallsSomeMethod(100, 30));
//     EXPECT_OK(test.CallsSomeMethod(200, 60));
//
//     test.mock_SomeMethod().VerifyAndClear();
// }

template <typename R, typename... Ts>
class MockFunction {
 public:
  template <typename... As>
  MockFunction& ExpectCall(R retval, As&&... args) {
    static_assert(sizeof...(As) == sizeof...(Ts), "wrong number of arguments to ExpectCall");
    using ArgsTuple = std::tuple<typename std::decay<Ts>::type...>;
    auto equals_matcher = [expected_args =
                               ArgsTuple(std::forward<As>(args)...)](Ts... actual_args) {
      EXPECT_EQ(expected_args, std::tie(actual_args...));
    };
    expectations_.emplace_back(
        MakeExpectation<decltype(equals_matcher)>(std::move(retval), std::move(equals_matcher)));
    has_expectations_ = true;
    return *this;
  }

  template <typename M>
  MockFunction& ExpectCallWithMatcher(R retval, M matcher) {
    expectations_.emplace_back(MakeExpectation<M>(std::move(retval), std::move(matcher)));
    has_expectations_ = true;
    return *this;
  }

  MockFunction& ExpectNoCall() {
    has_expectations_ = true;
    return *this;
  }

  template <typename... As>
  R Call(As&&... args) {
    std::unique_ptr<Expectation> exp;
    CallHelper(&exp);
    exp->Match(std::forward<As>(args)...);
    return std::move(exp->retval);
  }

  bool HasExpectations() const { return has_expectations_; }

  void VerifyAndClear() {
    EXPECT_EQ(expectation_index_, expectations_.size());
    expectations_.clear();
    expectation_index_ = 0;
  }

 private:
  struct Expectation {
    explicit Expectation(R retval) : retval(std::move(retval)) {}
    virtual ~Expectation() = default;
    virtual void Match(Ts... actual_args) = 0;

    R retval;
  };

  template <typename M>
  std::unique_ptr<Expectation> MakeExpectation(R retval, M matcher) {
    struct ExpectationWithMatcher : public Expectation {
      explicit ExpectationWithMatcher(R retval, M matcher)
          : Expectation(std::move(retval)), matcher(std::move(matcher)) {}
      ~ExpectationWithMatcher() override = default;
      void Match(Ts... actual_args) override { matcher(std::move(actual_args)...); }

      M matcher;
    };

    return std::make_unique<ExpectationWithMatcher>(std::move(retval), std::move(matcher));
  }

  void CallHelper(std::unique_ptr<Expectation>* exp) {
    ASSERT_LT(expectation_index_, expectations_.size());
    *exp = std::move(expectations_[expectation_index_++]);
  }

  bool has_expectations_ = false;
  std::vector<std::unique_ptr<Expectation>> expectations_;
  size_t expectation_index_ = 0;
};

template <typename... Ts>
class MockFunction<void, Ts...> {
 public:
  template <typename... As>
  MockFunction& ExpectCall(As&&... args) {
    static_assert(sizeof...(As) == sizeof...(Ts), "wrong number of arguments to ExpectCall");
    using ArgsTuple = std::tuple<typename std::decay<Ts>::type...>;
    auto equals_matcher = [expected_args =
                               ArgsTuple(std::forward<As>(args)...)](Ts... actual_args) {
      EXPECT_EQ(expected_args, std::tie(actual_args...));
    };
    expectations_.emplace_back(
        MakeExpectation<decltype(equals_matcher)>(std::move(equals_matcher)));
    has_expectations_ = true;
    return *this;
  }

  template <typename M>
  MockFunction& ExpectCallWithMatcher(M matcher) {
    expectations_.emplace_back(MakeExpectation<M>(std::move(matcher)));
    has_expectations_ = true;
    return *this;
  }

  MockFunction& ExpectNoCall() {
    has_expectations_ = true;
    return *this;
  }

  template <typename... As>
  void Call(As&&... args) {
    std::unique_ptr<Expectation> exp;
    CallHelper(&exp);
    exp->Match(std::forward<As>(args)...);
  }

  bool HasExpectations() const { return has_expectations_; }

  void VerifyAndClear() {
    ASSERT_EQ(expectation_index_, expectations_.size());
    expectations_.clear();
    expectation_index_ = 0;
  }

 private:
  struct Expectation {
    virtual ~Expectation() = default;
    virtual void Match(Ts... actual_args) = 0;
  };

  template <typename M>
  std::unique_ptr<Expectation> MakeExpectation(M matcher) {
    struct ExpectationWithMatcher : public Expectation {
      explicit ExpectationWithMatcher(M matcher) : matcher(std::move(matcher)) {}
      ~ExpectationWithMatcher() override = default;
      void Match(Ts... actual_args) override { matcher(std::move(actual_args)...); }

      M matcher;
    };

    return std::make_unique<ExpectationWithMatcher>(std::move(matcher));
  }

  void CallHelper(std::unique_ptr<Expectation>* exp) {
    ASSERT_LT(expectation_index_, expectations_.size());
    *exp = std::move(expectations_[expectation_index_++]);
  }

  bool has_expectations_ = false;
  std::vector<std::unique_ptr<Expectation>> expectations_;
  size_t expectation_index_ = 0;
};

}  // namespace mock_function
