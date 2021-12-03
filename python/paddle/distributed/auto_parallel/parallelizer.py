#   Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import sys
import json
import shlex
import copy
import pathlib
import subprocess
import logging
import pickle
import time
import paddle
from paddle.distributed.utils import get_logger
from paddle.distributed.fleet import cloud_utils
import paddle.fluid.core as core
from paddle.fluid import program_guard
from .dist_context import DistributedContext
from .dist_context import get_default_distributed_context
from .dist_context import set_default_distributed_context
from .completion import complete_annotation, complete_backward_annotation, complete_update_annotation
from .partitioner import Partitioner
from .process_group import get_all_process_groups
from .process_group import get_world_process_groups
from .process_group import _g_process_group_map, ProcessGroup
from .utils import make_data_unshard
from .utils import set_grad_var_shape
from .reshard import reshard, HAS_SENT, HAS_RECV, HAS_ALLGATHER
from .cluster import Cluster
from .mapper import mapping
from .auto_search import auto_search
from .dist_op import DistributedOperator
from .dist_tensor import DistributedTensor

_logger = get_logger(logging.INFO)


class AutoParallelizer:
    """
    AutoParallelizer is the main controller class to do the auto parallel process.
    And the auto parallel process will be triggered in the wrapped parallelize function.
    To facilitate the auto parallelization, it will contain information about program, cluster and the
    related context. In this basic version, the program information will be retrevied from 
    Fleet object, and the cluster information can be retrevied in the new created Cluster object,
    and the context information can be retrevied in the new created DistributedContext. 
    """

    def __init__(self, fleet):
        self._fleet = fleet
        self._optimizer = self._fleet.user_defined_optimizer
        self._dist_strategy = self._fleet._user_defined_strategy
        self._dist_context = DistributedContext()
        self._cluster = None
        self._cluster_topo_path = os.getenv("PADDLE_CLUSTER_TOPO_PATH", None)
        if self._cluster_topo_path is not None:
            self._cluster = Cluster()
            self._cluster.build_from_file(self._cluster_topo_path)
        # Prepare information for auto mapping
        self._rank_mapping_path = os.getenv("PADDLE_RANK_MAPPING_PATH", None)
        enable_auto_mapping_env = os.getenv("PADDLE_ENABLE_AUTO_MAPPING", None)
        if enable_auto_mapping_env is None:
            self._enable_auto_mapping = False
        else:
            self._enable_auto_mapping = True
        # TODO enable pass
        # self._pass_context = PassContext()
        self._pass_context = None

    def _remove_distributed_attrs(self, main_program):
        suffix = core.kAutoParallelSuffix()
        # distributed attributes for variable have been removed
        # in previous process.
        for block in main_program.blocks:
            for op in block.ops:
                for attr_name in op.attr_names:
                    if suffix in attr_name:
                        op._remove_attr(attr_name)

    def _apply_serial_forward_pass(self, main_program, startup_program):

        # apply amp forward pass
        if self._dist_strategy.amp:
            auto_parallel_amp_pass = new_pass("auto_parallel_amp_pass",
                                              self._dist_strategy.amp_configs)
            auto_parallel_amp_pass.apply_forward(main_program, startup_program,
                                                 self._pass_context)

        # apply recompute forward pass
        if self._dist_strategy.recompute:
            auto_parallel_recompute_pass = new_pass(
                "auto_parallel_recompute_pass",
                self._dist_strategy.recompute_configs)
            auto_parallel_recompute_pass.apply_forward(
                main_program, startup_program, self._pass_context)

    def _generate_backward(self, main_program, startup_program, loss,
                           parameter_list, no_grad_set, callbacks):

        # apply recompute backward pass
        if self._dist_strategy.recompute:
            assert auto_parallel_recompute_pass
            auto_parallel_recompute_pass.apply_forward(
                main_program, startup_program, parameter_list, no_grad_set,
                self._pass_context)
        else:
            from paddle.fluid.backward import append_backward
            with program_guard(main_program, startup_program):
                params_grads = append_backward(
                    loss,
                    parameter_list,
                    no_grad_set,
                    callbacks,
                    distop_context=self._dist_context.dist_op_context)
            complete_backward_annotation(
                main_program, dist_context=self._dist_context)

        # apply amp forward pass
        if self._dist_strategy.amp:
            assert auto_parallel_amp_pass
            auto_parallel_amp_pass.apply_backward(main_program, startup_program,
                                                  self._pass_context)

        return params_grads

    def _apply_optimize(self, main_program, startup_program, params_grads):

        if self._dist_strategy.sharding:
            auto_parallel_sharding_pass = new_pass(
                "auto_parallel_sharding_pass", self._dist_strategy)
            params_grads = auto_parallel_sharding_pass.apply(
                main_program, startup_program, params_grads, self._pass_context)

        if self._dist_strategy.gradient_merge:
            auto_parallel_gradient_merge_pass = new_pass(
                "auto_parallel_gradient_merge_pass",
                self._dist_strategy.gradient_merge_configs)
            auto_parallel_gradient_merge_pass.apply(
                main_program, startup_program, params_grads, self._pass_context)

        else:
            with program_guard(main_program, startup_program):
                optimize_ops = self._optimizer.apply_gradients(params_grads)

        # update completion 
        complete_update_annotation(
            main_program, dist_context=self._dist_context)

        return optimize_ops

    def _get_dist_program(self, rank, dist_context=None, relaunch_phase=False):
        completed_main_program = None

        # generating serial 
        if dist_context is None:
            # Annotation completion
            self._dist_context = DistributedContext()
            _logger.info("Start annotation dist attr.")
            completed_main_program = complete_annotation(self._main_program,
                                                         self._dist_context)
        else:
            completed_main_program = self._main_program
            self._dist_context = copy.deepcopy(dist_context)

        # serial forward pass
        self._apply_serial_forward_pass(completed_main_program, startup_program)

        # serial backward pass
        params_grads = self._generate_backward(
            completed_main_program, startup_program, loss, self._parameter_list,
            self._no_grad_set, self._callbacks)

        # Logical partition 
        rank = paddle.distributed.get_rank()
        partitioner = Partitioner(self._dist_context, rank)
        dist_main_prog, dist_startup_prog, dist_params_grads = partitioner.partition(
            completed_main_program, startup_program, params_grads)

        # TODO refactor the placement of optimizer
        # generate optimize program
        dist_optimize_ops = self._apply_optimize(
            dist_main_prog, dist_startup_prog, dist_params_grads)

        set_grad_var_shape(dist_main_prog, self._dist_context)

        make_data_unshard(dist_main_prog, dist_startup_prog, self._dist_context)

        reshard(dist_main_prog, dist_startup_prog, rank, self._dist_context)

        g_process_group_map = None
        if not relaunch_phase:
            g_process_group_map = copy.deepcopy(_g_process_group_map)
            HAS_SENT.clear()
            HAS_RECV.clear()
            HAS_ALLGATHER.clear()
            _g_process_group_map.clear()
            _g_process_group_map[0] = ProcessGroup(0, [])
        return dist_optimize_ops, dist_params_grads, dist_startup_prog, dist_main_prog, g_process_group_map

    def parallelize(self,
                    loss,
                    startup_program,
                    parameter_list=None,
                    no_grad_set=None,
                    callbacks=None):
        assert startup_program is not None
        self._loss = loss
        self._startup_program = startup_program
        self._main_program = loss.block.program
        self._parameter_list = parameter_list
        self._no_grad_set = no_grad_set
        self._callbacks = callbacks

        if self._enable_auto_mapping and self._rank_mapping_path is None:
            # Do the mapping pass before parallelization
            assert self._cluster is not None, \
                "The cluster must not be none when using auto mapping."
            dist_programs = {}
            world_process_group = get_world_process_groups()
            dist_context = None
            # auto_search
            if self._dist_strategy.auto_search:
                _logger.info("Start search dist attr.")
                dist_context, _ = auto_search(
                    self._main_program,
                    self._startup_program,
                    loss,
                    self._optimizer,
                    cluster=self._cluster)
                _logger.info("End search dist attr.")

            # serialize the dist_context by planner
            if dist_context is not None:
                _logger.info("Start serialize searched dist attr")
                cwd = pathlib.Path().resolve()
                searched_dist_context_path = os.path.join(
                    cwd, f"searched_dist_context_{time.time()}.pkl")
                saved_dist_context = {}
                ops_dist_attr = {}
                tensors_dist_attr = {}
                for key, dist_op in dist_context._dist_ops_for_program.items():
                    ops_dist_attr[key] = dist_op.dist_attr
                for key, dist_tensor in dist_context._dist_tensors_for_program.items(
                ):
                    tensors_dist_attr[key] = dist_tensor.dist_attr
                saved_dist_context["ops_dist_attr"] = ops_dist_attr
                saved_dist_context["tensors_dist_attr"] = tensors_dist_attr
                saved_dist_context[
                    "process_meshes"] = dist_context._process_meshes
                with open(searched_dist_context_path,
                          "wb") as dist_context_file:
                    pickle.dump(saved_dist_context, dist_context_file)
                    os.environ[
                        'PADDLE_SEARCHED_DIST_CONTEXT_PATH'] = searched_dist_context_path
                    _logger.info(
                        f"End serialize searched dist attr to {searched_dist_context_path}"
                    )

            for rank in world_process_group.ranks:
                dist_optimize_ops, dist_params_grads, dist_startup_prog, dist_main_prog, g_process_group_map = self._get_dist_program(
                    rank, dist_context)
                dist_programs[rank] = [dist_main_prog, g_process_group_map]

            # Do the mapping between the distributed program graph and the cluster graph
            rank_mapping_dict = mapping(dist_programs, self._cluster)
            rank_mapping = list(rank_mapping_dict.values())

            # Relaunch the training by using the rank mapping file
            cwd = pathlib.Path().resolve()
            rank_mapping_path = os.path.join(cwd,
                                             "auto_parallel_rank_mapping.json")
            with open(rank_mapping_path, "w") as rank_mapping_file:
                json.dump(rank_mapping, rank_mapping_file)

            original_cmd_args = os.getenv("PADDLE_ORIGINAL_CMD_ARGS")
            rank_mapping_args = " ".join(
                ["--rank_mapping_path", rank_mapping_path])
            new_cmd_args = "-u -m paddle.distributed.fleet.launch" + " " + rank_mapping_args + " " + original_cmd_args
            new_cmd = [sys.executable] + shlex.split(new_cmd_args)
            print(new_cmd)
            new_process = subprocess.Popen(new_cmd)
            new_process.wait()
            assert new_process.returncode == 0, \
                "Launch failed with rank mapping"
            print("Successfully do the second launch for auto mapping!")
            sys.exit(0)
        else:
            # Parallelization after the mapping pass
            rank = paddle.distributed.get_rank()
            dist_context = None
            searched_dist_context_path = os.getenv(
                "PADDLE_SEARCHED_DIST_CONTEXT_PATH", None)
            if searched_dist_context_path is not None:
                with open(searched_dist_context_path,
                          "rb") as dist_context_file:
                    saved_dist_context = pickle.load(dist_context_file)
                    dist_context = DistributedContext()
                    for op in self._main_program.global_block().ops:
                        dist_attr = saved_dist_context["ops_dist_attr"][
                            op.desc.id()]
                        dist_op = DistributedOperator(op, dist_attr)
                        dist_context.add_dist_op_for_program(dist_op)

                    vars = self._main_program.global_block().vars
                    for var in vars.values():
                        dist_attr = saved_dist_context["tensors_dist_attr"][
                            var.desc.id()]
                        dist_tensor = DistributedTensor(var, dist_attr)
                        dist_context.add_dist_tensor_for_program(dist_tensor)

                    dist_context._process_meshes = saved_dist_context[
                        "process_meshes"]

            else:
                if self._dist_strategy.auto_search:
                    _logger.info("Start search dist attr.")
                    dist_context, _ = auto_search(
                        self._main_program,
                        self._startup_program,
                        loss,
                        self._optimizer,
                        cluster=self._cluster)
                    _logger.info("End search dist attr.")
            dist_optimize_ops, dist_params_grads, dist_startup_prog, dist_main_prog, _ = self._get_dist_program(
                rank, dist_context, relaunch_phase=True)

            # Traverse different rank programs and traverse each op of them,
            # instantiate communication by process_mapping.
            all_process_groups = get_all_process_groups()
            for process_group in all_process_groups:
                if rank not in process_group.ranks:
                    continue
                process_group.instantiate()

            # Copy distributed info to the default context
            set_default_distributed_context(self._dist_context)

            # The last step: remove all distributed attributes to be compatible
            # with inference.
            self._remove_distributed_attrs(dist_main_prog)

            return dist_optimize_ops, dist_params_grads, dist_startup_prog, dist_main_prog
