/* Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
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
#include "paddle/fluid/framework/ir/graph.h"

namespace paddle {
namespace framework {
void save_runtime_cinn_graph(const ir::Graph& graph,
                             std::string clusters_ops,
                             std::string clusters_inputs,
                             std::string cluster_outputs,
                             std::string cluster_intervals,
                             std::string saved_path);

}  // namespace framework
}  // namespace paddle
