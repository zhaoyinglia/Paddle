/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <string>
#include <vector>

#include "paddle/fluid/framework/op_desc.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/operators/elementwise/elementwise_op_function.h"
#include "paddle/phi/kernels/funcs/compound_functors.h"
#include "paddle/phi/kernels/funcs/elementwise_functor.h"
#include "paddle/phi/kernels/funcs/functors.h"

namespace paddle {
namespace operators {

/**
 * Whether the compound function is Unary(Binary(X, Y)).
 * For Unary(Binary(X, Y)), the intermediate_out's shape is the same the final
 * out.
 */
bool IsUnaryCompound(const std::vector<std::string> &functor_list);

/**
 *  For the in-place unary functor, the inputs of op_desc only have Out and
 *  Out@Grad.
 */
bool HasInPlaceUnary(const std::vector<std::string> &functor_list);

/**
 * Whether the Input(X) could be absent.
 */
bool InputXCanBeAbsent(const std::vector<std::string> &functor_list);

template <typename DeviceContext,
          typename T,
          typename BinaryFunctor,
          typename UnaryFunctor>
static void RunBinaryCompoundFunctor(const framework::ExecutionContext &ctx,
                                     const BinaryFunctor &binary_functor,
                                     const UnaryFunctor &unary_functor,
                                     const phi::DenseTensor &in_x,
                                     const phi::DenseTensor &in_y,
                                     std::vector<phi::DenseTensor *> *outputs) {
  // Z = Binary(X, Unary(Y))
  // intermediate_out = Unary(Y)
  // out = Binary(X, Unary(Y))
  // In this case, the shape of intermediate_out and out are different.
  phi::funcs::BinaryCompoundFunctor<T, BinaryFunctor, UnaryFunctor>
      compound_func(binary_functor, unary_functor);
  int axis = ctx.Attr<int>("axis");
  if (ctx.Attr<bool>("save_intermediate_out")) {
    FusedElemwiseAndActComputeEx<
        DeviceContext,
        T,
        phi::funcs::BinaryCompoundFunctor<T, BinaryFunctor, UnaryFunctor>,
        true /*KeepIntermediateValue*/,
        false /*SameShapeOfIntermediateOutAndOut*/>(
        ctx, in_x, in_y, axis, compound_func, (*outputs)[0], (*outputs)[1]);
  } else {
    FusedElemwiseAndActComputeEx<
        DeviceContext,
        T,
        phi::funcs::BinaryCompoundFunctor<T, BinaryFunctor, UnaryFunctor>,
        false /*KeepIntermediateValue*/,
        false /*SameShapeOfIntermediateOutAndOut*/>(
        ctx, in_x, in_y, axis, compound_func, (*outputs)[0], (*outputs)[1]);
  }
}

template <typename DeviceContext,
          typename T,
          typename UnaryFunctor,
          typename BinaryFunctor>
static void RunUnaryCompoundFunctors(const framework::ExecutionContext &ctx,
                                     const UnaryFunctor &unary_functor,
                                     const BinaryFunctor &binary_functor,
                                     const phi::DenseTensor &in_x,
                                     const phi::DenseTensor &in_y,
                                     std::vector<phi::DenseTensor *> *outputs) {
  // Z = Unary(Binary(X, Y))
  // intermediate_out = Binary(X, Y)
  // out = Unary(Binary(X, Y))
  // In this case, the shape of intermediate_out and out are the same.
  int axis = ctx.Attr<int>("axis");

  phi::funcs::UnaryCompoundFunctor<T, UnaryFunctor, BinaryFunctor>
      compound_func(unary_functor, binary_functor);

  if (ctx.Attr<bool>("save_intermediate_out")) {
    FusedElemwiseAndActComputeEx<
        DeviceContext,
        T,
        phi::funcs::UnaryCompoundFunctor<T, UnaryFunctor, BinaryFunctor>,
        true /*KeepIntermediateValue*/,
        true /*SameShapeOfIntermediateOutAndOut*/>(
        ctx, in_x, in_y, axis, compound_func, (*outputs)[0], (*outputs)[1]);
  } else {
    FusedElemwiseAndActComputeEx<
        DeviceContext,
        T,
        phi::funcs::UnaryCompoundFunctor<T, UnaryFunctor, BinaryFunctor>,
        false /*KeepIntermediateValue*/,
        true /*SameShapeOfIntermediateOutAndOut*/>(
        ctx, in_x, in_y, axis, compound_func, (*outputs)[0], (*outputs)[1]);
  }
}

template <typename DeviceContext,
          typename T,
          typename BinaryGradFunctor,
          typename UnaryFunctor,
          typename UnaryGradFunctor,
          bool InPlace>
static void RunBinaryCompoundGradFunctors(
    const framework::ExecutionContext &ctx,
    const BinaryGradFunctor &binary_grad_functor,
    const UnaryFunctor &unary_functor,
    const UnaryGradFunctor &unary_grad_functor,
    const phi::DenseTensor *in_x,
    const phi::DenseTensor *in_y,
    const phi::DenseTensor *in_out,
    const phi::DenseTensor *in_intermediate_out,
    const phi::DenseTensor *in_out_grad,
    phi::DenseTensor *x_grad,
    phi::DenseTensor *y_grad,
    phi::DenseTensor *d_intermediate_out) {
  // Z = Binary(X, Unary(Y))
  int axis = ctx.Attr<int>("axis");

  using BinaryCompoundDxFunctor = phi::funcs::
      BinaryCompoundGradDxFunctor<T, BinaryGradFunctor, UnaryFunctor>;
  using BinaryCompoundDyFunctor =
      phi::funcs::BinaryCompoundGradDyFunctor<T,
                                              BinaryGradFunctor,
                                              UnaryFunctor,
                                              UnaryGradFunctor,
                                              InPlace>;
  using BinaryCompoundDIntermedaiteOutFunctor =
      phi::funcs::BinaryCompoundGradDIntermedaiteOutFunctor<T,
                                                            BinaryGradFunctor,
                                                            UnaryFunctor>;

  if (in_intermediate_out) {
    FusedElemwiseAndActGradComputeEx<
        DeviceContext,
        T,
        BinaryCompoundDxFunctor,
        BinaryCompoundDyFunctor,
        BinaryCompoundDIntermedaiteOutFunctor,
        true /*UseIntermediateOut*/,
        false /*SameShapeOfIntermediateOutAndOut*/>(
        ctx,
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        axis,
        x_grad,
        y_grad,
        d_intermediate_out,
        BinaryCompoundDxFunctor(binary_grad_functor, unary_functor),
        BinaryCompoundDyFunctor(
            binary_grad_functor, unary_functor, unary_grad_functor),
        BinaryCompoundDIntermedaiteOutFunctor(binary_grad_functor,
                                              unary_functor));
  } else {
    FusedElemwiseAndActGradComputeEx<
        DeviceContext,
        T,
        BinaryCompoundDxFunctor,
        BinaryCompoundDyFunctor,
        BinaryCompoundDIntermedaiteOutFunctor,
        false /*UseIntermediateOut*/,
        false /*SameShapeOfIntermediateOutAndOut*/>(
        ctx,
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        axis,
        x_grad,
        y_grad,
        d_intermediate_out,
        BinaryCompoundDxFunctor(binary_grad_functor, unary_functor),
        BinaryCompoundDyFunctor(
            binary_grad_functor, unary_functor, unary_grad_functor),
        BinaryCompoundDIntermedaiteOutFunctor(binary_grad_functor,
                                              unary_functor));
  }
}

template <typename DeviceContext,
          typename T,
          typename UnaryGradFunctor,
          typename BinaryFunctor,
          typename BinaryGradFunctor,
          bool InPlace>
static void RunUnaryCompoundGradFunctors(
    const framework::ExecutionContext &ctx,
    const UnaryGradFunctor &unary_grad_functor,
    const BinaryFunctor &binary_functor,
    const BinaryGradFunctor &binary_grad_functor,
    const phi::DenseTensor *in_x,
    const phi::DenseTensor *in_y,
    const phi::DenseTensor *in_out,
    const phi::DenseTensor *in_intermediate_out,
    const phi::DenseTensor *in_out_grad,
    phi::DenseTensor *x_grad,
    phi::DenseTensor *y_grad,
    phi::DenseTensor *d_intermediate_out) {
  // Z = Unary(Binary(X, Y))
  int axis = ctx.Attr<int>("axis");

  using UnaryCompoundDxFunctor =
      phi::funcs::UnaryCompoundGradDxFunctor<T,
                                             UnaryGradFunctor,
                                             BinaryFunctor,
                                             BinaryGradFunctor,
                                             InPlace>;
  using UnaryCompoundDyFunctor =
      phi::funcs::UnaryCompoundGradDyFunctor<T,
                                             UnaryGradFunctor,
                                             BinaryFunctor,
                                             BinaryGradFunctor,
                                             InPlace>;
  using UnaryCompoundDIntermediateFunctor =
      phi::funcs::UnaryCompoundGradDIntermediateFunctor<T,
                                                        UnaryGradFunctor,
                                                        BinaryFunctor,
                                                        InPlace>;

  if (in_intermediate_out) {
    FusedElemwiseAndActGradComputeEx<DeviceContext,
                                     T,
                                     UnaryCompoundDxFunctor,
                                     UnaryCompoundDyFunctor,
                                     UnaryCompoundDIntermediateFunctor,
                                     true /*UseIntermediateOut*/,
                                     true /*SameShapeOfIntermediateOutAndOut*/>(
        ctx,
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        axis,
        x_grad,
        y_grad,
        d_intermediate_out,
        UnaryCompoundDxFunctor(
            unary_grad_functor, binary_functor, binary_grad_functor),
        UnaryCompoundDyFunctor(
            unary_grad_functor, binary_functor, binary_grad_functor),
        UnaryCompoundDIntermediateFunctor(unary_grad_functor, binary_functor));
  } else {
    FusedElemwiseAndActGradComputeEx<DeviceContext,
                                     T,
                                     UnaryCompoundDxFunctor,
                                     UnaryCompoundDyFunctor,
                                     UnaryCompoundDIntermediateFunctor,
                                     false /*UseIntermediateOut*/,
                                     true /*SameShapeOfIntermediateOutAndOut*/>(
        ctx,
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        axis,
        x_grad,
        y_grad,
        d_intermediate_out,
        UnaryCompoundDxFunctor(
            unary_grad_functor, binary_functor, binary_grad_functor),
        UnaryCompoundDyFunctor(
            unary_grad_functor, binary_functor, binary_grad_functor),
        UnaryCompoundDIntermediateFunctor(unary_grad_functor, binary_functor));
  }
}

template <typename DeviceContext, typename T>
static void RunFunctors(const framework::ExecutionContext &ctx,
                        const phi::DenseTensor &in_x,
                        const phi::DenseTensor &in_y,
                        std::vector<phi::DenseTensor *> *outputs) {
  auto &functors = ctx.Attr<std::vector<std::string>>("functor_list");

  // TODO(zcd): The following code can be refined.
  auto funcs_str = functors[0] + "," + functors[1];
  if (funcs_str == "elementwise_add,scale") {
    // Z = Binary(X, Unary(Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunBinaryCompoundFunctor<DeviceContext,
                             T,
                             phi::funcs::AddFunctor<T>,
                             phi::funcs::ScaleFunctor<T>>(
        ctx,
        phi::funcs::AddFunctor<T>(),
        phi::funcs::ScaleFunctor<T>(scale),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "scale,elementwise_add") {
    // Z = Unary(Binary(X, Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunUnaryCompoundFunctors<DeviceContext,
                             T,
                             phi::funcs::ScaleFunctor<T>,
                             phi::funcs::AddFunctor<T>>(
        ctx,
        phi::funcs::ScaleFunctor<T>(scale),
        phi::funcs::AddFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "elementwise_add,relu") {
    // Z = Binary(X, Unary(Y))
    RunBinaryCompoundFunctor<DeviceContext,
                             T,
                             phi::funcs::AddFunctor<T>,
                             phi::funcs::ReluFunctor<T>>(
        ctx,
        phi::funcs::AddFunctor<T>(),
        phi::funcs::ReluFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "relu,elementwise_add") {
    // Z = Unary(Binary(X, Y))
    RunUnaryCompoundFunctors<DeviceContext,
                             T,
                             phi::funcs::ReluFunctor<T>,
                             phi::funcs::AddFunctor<T>>(
        ctx,
        phi::funcs::ReluFunctor<T>(),
        phi::funcs::AddFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "elementwise_mul,scale") {
    // Z = Binary(X, Unary(Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunBinaryCompoundFunctor<DeviceContext,
                             T,
                             phi::funcs::MultiplyFunctor<T>,
                             phi::funcs::ScaleFunctor<T>>(
        ctx,
        phi::funcs::MultiplyFunctor<T>(),
        phi::funcs::ScaleFunctor<T>(scale),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "tanh,elementwise_add") {
    // Z = Unary(Binary(X, Y))
    RunUnaryCompoundFunctors<DeviceContext,
                             T,
                             phi::funcs::TanhFunctor<T>,
                             phi::funcs::AddFunctor<T>>(
        ctx,
        phi::funcs::TanhFunctor<T>(),
        phi::funcs::AddFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "elementwise_mul,tanh") {
    // Z = Binary(X, Unary(Y))
    RunBinaryCompoundFunctor<DeviceContext,
                             T,
                             phi::funcs::MultiplyFunctor<T>,
                             phi::funcs::TanhFunctor<T>>(
        ctx,
        phi::funcs::MultiplyFunctor<T>(),
        phi::funcs::TanhFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "elementwise_mul,sigmoid") {
    // Z = Binary(X, Unary(Y))
    RunBinaryCompoundFunctor<DeviceContext,
                             T,
                             phi::funcs::MultiplyFunctor<T>,
                             phi::funcs::SigmoidFunctor<T>>(
        ctx,
        phi::funcs::MultiplyFunctor<T>(),
        phi::funcs::SigmoidFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else if (funcs_str == "gelu,elementwise_add") {
    // Z = Unary(Binary(X, Y))
    RunUnaryCompoundFunctors<DeviceContext,
                             T,
                             phi::funcs::GeluFunctor<T>,
                             phi::funcs::AddFunctor<T>>(
        ctx,
        phi::funcs::GeluFunctor<T>(),
        phi::funcs::AddFunctor<T>(),
        in_x,
        in_y,
        outputs);
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "%s has not been implemented.", funcs_str));
  }
}

template <typename DeviceContext, typename T, bool InPlace>
static void RunGradFunctors(const framework::ExecutionContext &ctx,
                            const phi::DenseTensor *in_x,
                            const phi::DenseTensor *in_y,
                            const phi::DenseTensor *in_out,
                            const phi::DenseTensor *in_intermediate_out,
                            const phi::DenseTensor *in_out_grad,
                            phi::DenseTensor *x_grad,
                            phi::DenseTensor *y_grad,
                            phi::DenseTensor *d_intermediate_out) {
  auto &functors = ctx.Attr<std::vector<std::string>>("functor_list");
  auto funcs_str = functors[0] + "," + functors[1];

  if (funcs_str == "elementwise_add_grad,scale_grad") {
    // The backward of Z = Binary(X, Unary(Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunBinaryCompoundGradFunctors<DeviceContext,
                                  T,
                                  phi::funcs::AddGradFunctor<T>,
                                  phi::funcs::ScaleFunctor<T>,
                                  phi::funcs::ScaleGradFunctor<T>,
                                  InPlace>(
        ctx,
        phi::funcs::AddGradFunctor<T>(),
        phi::funcs::ScaleFunctor<T>(scale),
        phi::funcs::ScaleGradFunctor<T>(scale),
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        x_grad,
        y_grad,
        d_intermediate_out);
  } else if (funcs_str == "scale_grad,elementwise_add_grad") {
    // The backward of Z = Unary(Binary(X, Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunUnaryCompoundGradFunctors<DeviceContext,
                                 T,
                                 phi::funcs::ScaleGradFunctor<T>,
                                 phi::funcs::AddFunctor<T>,
                                 phi::funcs::AddGradFunctor<T>,
                                 InPlace>(
        ctx,
        phi::funcs::ScaleGradFunctor<T>(scale),
        phi::funcs::AddFunctor<T>(),
        phi::funcs::AddGradFunctor<T>(),
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        x_grad,
        y_grad,
        d_intermediate_out);
  } else if (funcs_str == "elementwise_add_grad,relu_grad") {
    // The backward of Z = Binary(X, Unary(Y))
    RunBinaryCompoundGradFunctors<DeviceContext,
                                  T,
                                  phi::funcs::AddGradFunctor<T>,
                                  phi::funcs::ReluFunctor<T>,
                                  phi::funcs::ReluGradFunctor<T>,
                                  InPlace>(ctx,
                                           phi::funcs::AddGradFunctor<T>(),
                                           phi::funcs::ReluFunctor<T>(),
                                           phi::funcs::ReluGradFunctor<T>(),
                                           in_x,
                                           in_y,
                                           in_out,
                                           in_intermediate_out,
                                           in_out_grad,
                                           x_grad,
                                           y_grad,
                                           d_intermediate_out);
  } else if (funcs_str == "relu_grad,elementwise_add_grad") {
    // The backward of Z = Unary(Binary(X, Y))
    RunUnaryCompoundGradFunctors<DeviceContext,
                                 T,
                                 phi::funcs::ReluGradFunctor<T>,
                                 phi::funcs::AddFunctor<T>,
                                 phi::funcs::AddGradFunctor<T>,
                                 InPlace>(ctx,
                                          phi::funcs::ReluGradFunctor<T>(),
                                          phi::funcs::AddFunctor<T>(),
                                          phi::funcs::AddGradFunctor<T>(),
                                          in_x,
                                          in_y,
                                          in_out,
                                          in_intermediate_out,
                                          in_out_grad,
                                          x_grad,
                                          y_grad,
                                          d_intermediate_out);
  } else if (funcs_str == "elementwise_mul_grad,scale_grad") {
    // The backward of Z = Binary(X, Unary(Y))
    T scale = static_cast<T>(ctx.Attr<float>("scale"));
    RunBinaryCompoundGradFunctors<DeviceContext,
                                  T,
                                  phi::funcs::MulGradFunctor<T>,
                                  phi::funcs::ScaleFunctor<T>,
                                  phi::funcs::ScaleGradFunctor<T>,
                                  InPlace>(
        ctx,
        phi::funcs::MulGradFunctor<T>(),
        phi::funcs::ScaleFunctor<T>(scale),
        phi::funcs::ScaleGradFunctor<T>(scale),
        in_x,
        in_y,
        in_out,
        in_intermediate_out,
        in_out_grad,
        x_grad,
        y_grad,
        d_intermediate_out);
  } else if (funcs_str == "tanh_grad,elementwise_add_grad") {
    // The backward of Z = Unary(Binary(X, Y))
    RunUnaryCompoundGradFunctors<DeviceContext,
                                 T,
                                 phi::funcs::TanhGradFunctor<T>,
                                 phi::funcs::AddFunctor<T>,
                                 phi::funcs::AddGradFunctor<T>,
                                 InPlace>(ctx,
                                          phi::funcs::TanhGradFunctor<T>(),
                                          phi::funcs::AddFunctor<T>(),
                                          phi::funcs::AddGradFunctor<T>(),
                                          in_x,
                                          in_y,
                                          in_out,
                                          in_intermediate_out,
                                          in_out_grad,
                                          x_grad,
                                          y_grad,
                                          d_intermediate_out);
  } else if (funcs_str == "elementwise_mul_grad,tanh_grad") {
    // The backward of Z = Binary(X, Unary(Y))
    RunBinaryCompoundGradFunctors<DeviceContext,
                                  T,
                                  phi::funcs::MulGradFunctor<T>,
                                  phi::funcs::TanhFunctor<T>,
                                  phi::funcs::TanhGradFunctor<T>,
                                  InPlace>(ctx,
                                           phi::funcs::MulGradFunctor<T>(),
                                           phi::funcs::TanhFunctor<T>(),
                                           phi::funcs::TanhGradFunctor<T>(),
                                           in_x,
                                           in_y,
                                           in_out,
                                           in_intermediate_out,
                                           in_out_grad,
                                           x_grad,
                                           y_grad,
                                           d_intermediate_out);
  } else if (funcs_str == "elementwise_mul_grad,sigmoid_grad") {
    // The backward of Z = Binary(X, Unary(Y))
    RunBinaryCompoundGradFunctors<DeviceContext,
                                  T,
                                  phi::funcs::MulGradFunctor<T>,
                                  phi::funcs::SigmoidFunctor<T>,
                                  phi::funcs::SigmoidGradFunctor<T>,
                                  InPlace>(ctx,
                                           phi::funcs::MulGradFunctor<T>(),
                                           phi::funcs::SigmoidFunctor<T>(),
                                           phi::funcs::SigmoidGradFunctor<T>(),
                                           in_x,
                                           in_y,
                                           in_out,
                                           in_intermediate_out,
                                           in_out_grad,
                                           x_grad,
                                           y_grad,
                                           d_intermediate_out);
  } else if (funcs_str == "gelu_grad,elementwise_add_grad") {
    // The backward of Z = Unary(Binary(X, Y))
    RunUnaryCompoundGradFunctors<DeviceContext,
                                 T,
                                 phi::funcs::GeluGradFunctor<T>,
                                 phi::funcs::AddFunctor<T>,
                                 phi::funcs::AddGradFunctor<T>,
                                 InPlace>(ctx,
                                          phi::funcs::GeluGradFunctor<T>(),
                                          phi::funcs::AddFunctor<T>(),
                                          phi::funcs::AddGradFunctor<T>(),
                                          in_x,
                                          in_y,
                                          in_out,
                                          in_intermediate_out,
                                          in_out_grad,
                                          x_grad,
                                          y_grad,
                                          d_intermediate_out);
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "%s has not been implemented.", funcs_str));
  }
}

template <typename T, typename DeviceContext>
class FusedElemwiseActivationKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto &in_x = GET_DATA_SAFELY(ctx.Input<phi::DenseTensor>("X"),
                                 "Input",
                                 "X",
                                 "FusedElemwiseActivation");
    auto &in_y = GET_DATA_SAFELY(ctx.Input<phi::DenseTensor>("Y"),
                                 "Input",
                                 "Y",
                                 "FusedElemwiseActivation");

    PADDLE_ENFORCE_EQ(ctx.HasOutput("Out"),
                      true,
                      platform::errors::InvalidArgument(
                          "The output(Out) should not be empty"));
    auto output = ctx.Output<phi::DenseTensor>("Out");

    std::vector<phi::DenseTensor *> outputs;
    outputs.emplace_back(output);

    if (ctx.Attr<bool>("save_intermediate_out")) {
      PADDLE_ENFORCE_EQ(ctx.HasOutput("IntermediateOut"),
                        true,
                        platform::errors::InvalidArgument(
                            "The save_intermediate_out is enable, so the "
                            "IntermediateOut should not be empty."));

      auto intermediate_out = ctx.Output<phi::DenseTensor>("IntermediateOut");
      outputs.emplace_back(intermediate_out);
    } else {
      outputs.emplace_back(nullptr);
    }

    RunFunctors<DeviceContext, T>(ctx, in_x, in_y, &outputs);
  }
};

template <typename T, typename DeviceContext>
class FusedElemwiseActivationGradKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext &ctx) const override {
    auto in_y = ctx.Input<phi::DenseTensor>("Y");
    PADDLE_ENFORCE_NE(
        in_y,
        nullptr,
        platform::errors::InvalidArgument("Input(Y) should not be nullptr."));
    phi::DenseTensor *in_out =
        const_cast<phi::DenseTensor *>(ctx.Input<phi::DenseTensor>("Out"));

    auto in_out_grad =
        ctx.Input<phi::DenseTensor>(framework::GradVarName("Out"));
    PADDLE_ENFORCE_NE(in_out_grad,
                      nullptr,
                      platform::errors::InvalidArgument(
                          "Input(Out@Grad) should not be nullptr."));

    phi::DenseTensor *in_x =
        const_cast<phi::DenseTensor *>(ctx.Input<phi::DenseTensor>("X"));
    phi::DenseTensor *x_grad =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("X"));
    phi::DenseTensor *y_grad =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("Y"));
    phi::DenseTensor *d_intermediate_out =
        ctx.Output<phi::DenseTensor>(framework::GradVarName("IntermediateOut"));

    auto functor_list = ctx.Attr<std::vector<std::string>>("functor_list");

    // Get intermediate_out
    phi::DenseTensor *in_intermediate_out = nullptr;
    if (ctx.Attr<bool>("save_intermediate_out")) {
      // if save_intermediate_out is true, for Unary(Binary(x, y)) and
      // Binary(x, Unary(y)), the Binary(x, y) and Unary(y) not need to
      // recompute.
      in_intermediate_out = const_cast<phi::DenseTensor *>(
          ctx.Input<phi::DenseTensor>("IntermediateOut"));
      PADDLE_ENFORCE_NE(in_intermediate_out,
                        nullptr,
                        platform::errors::InvalidArgument(
                            "The option of 'save_intermediate_out' is opened,"
                            " so the number of 'Out' should be two."));
    } else {
      if (!InputXCanBeAbsent(functor_list)) {
        PADDLE_ENFORCE_NE(
            in_x,
            nullptr,
            platform::errors::InvalidArgument("Input(X) should not be null."));
      }
    }

    // Get in_x
    if (ctx.HasInput("X")) {
      PADDLE_ENFORCE_NE(
          in_x,
          nullptr,
          platform::errors::InvalidArgument("Input(X) should not be null."));
    } else {
      // If functor_list contains elementwise_add, the backward doesn't use
      // in_x, in_y and in_out.
      PADDLE_ENFORCE_EQ(InputXCanBeAbsent(functor_list),
                        true,
                        platform::errors::InvalidArgument(
                            "Only when the compoundfunctor contains "
                            "elementwise_add_grad, the 'X' could be absent."));
      in_x = const_cast<phi::DenseTensor *>(in_out_grad);
    }

    // Get in_Out
    if (ctx.HasInput("Out")) {
      PADDLE_ENFORCE_NE(
          in_out,
          nullptr,
          platform::errors::InvalidArgument("Input(X) should not be null."));
    } else {
      // If functor_list contains elementwise_add, the backward doesn't use
      // in_x, in_y and in_out.
      PADDLE_ENFORCE_EQ(InputXCanBeAbsent(functor_list),
                        true,
                        platform::errors::InvalidArgument(
                            "Only when the compoundfunctor contains "
                            "elementwise_add_grad, the 'X' could be absent."));
      in_out = const_cast<phi::DenseTensor *>(in_out_grad);
    }

    bool has_in_place = HasInPlaceUnary(functor_list);
    if (has_in_place) {
      RunGradFunctors<DeviceContext, T, true /*InPlace*/>(ctx,
                                                          in_x,
                                                          in_y,
                                                          in_out,
                                                          in_intermediate_out,
                                                          in_out_grad,
                                                          x_grad,
                                                          y_grad,
                                                          d_intermediate_out);
    } else {
      RunGradFunctors<DeviceContext, T, false /*InPlace*/>(ctx,
                                                           in_x,
                                                           in_y,
                                                           in_out,
                                                           in_intermediate_out,
                                                           in_out_grad,
                                                           x_grad,
                                                           y_grad,
                                                           d_intermediate_out);
    }
  }
};

template <typename T, typename DeviceContext>
class FusedElemwiseAddActivationKernel
    : public FusedElemwiseActivationKernel<T, DeviceContext> {};

template <typename T, typename DeviceContext>
class FusedElemwiseAddActivationGradKernel
    : public FusedElemwiseActivationGradKernel<T, DeviceContext> {};

}  // namespace operators
}  // namespace paddle
