// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "paddle/fluid/operators/collective/rpc_token_call_op.h"

#include "paddle/fluid/framework/op_proto_maker.h"
#include "paddle/fluid/framework/op_registry.h"

namespace paddle {
namespace operators {

class RpcTokenCallOp : public framework::OperatorWithKernel {
 public:
  using framework::OperatorWithKernel::OperatorWithKernel;

  void InferShape(framework::InferShapeContext* ctx) const override {}

 protected:
  framework::OpKernelType GetExpectedKernelType(
      const framework::ExecutionContext& ctx) const override {
    auto dtype = OperatorWithKernel::IndicateVarDataType(ctx, "X");
    return framework::OpKernelType(dtype, ctx.GetPlace());
  }
};

class RpcTokenCallOpMaker : public framework::OpProtoAndCheckerMaker {
 public:
  void Make() {
    AddInput("X", "(Tensor) Src words' ids.");
    AddInput("url_id", "(Tensor) Service URL's id.");
    AddOutput("Out", "(Tensor) Request id.");
    AddAttr<std::vector<std::string>>("url_list", "URL list.").SetDefault({});
    AddAttr<std::string>("vocab_path", "Vocab's absolute path.").SetDefault("");
    AddAttr<bool>("use_ids", "If true, use ids directly.").SetDefault(true);
    AddComment(R"DOC(
Rpc Token Call Operator

)DOC");
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;

REGISTER_OP_WITHOUT_GRADIENT(rpc_token_call,
                             ops::RpcTokenCallOp,
                             ops::RpcTokenCallOpMaker);

REGISTER_OP_CPU_KERNEL(rpc_token_call,
                       ops::RpcTokenCallOpKernel<int>,
                       ops::RpcTokenCallOpKernel<int64_t>);

REGISTER_OP_CUDA_KERNEL(rpc_token_call,
                        ops::RpcTokenCallOpKernel<int>,
                        ops::RpcTokenCallOpKernel<int64_t>);
