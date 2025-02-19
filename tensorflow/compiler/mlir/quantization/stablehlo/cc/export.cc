/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/compiler/mlir/quantization/stablehlo/cc/export.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/quantization/tensorflow/exported_model.pb.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/passes/constants.h"
#include "tensorflow/compiler/mlir/quantization/tensorflow/passes/passes.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_saved_model.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/passes.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/protobuf/meta_graph.pb.h"
#include "tensorflow/core/protobuf/saver.pb.h"

namespace stablehlo::quantization {
namespace {

using ::mlir::quant::kTfFilePrefix;
using ::mlir::quant::kTfQuantSaveOpName;
using ::mlir::tf_saved_model::kTfSavedModelIndexPathAttr;
using ::mlir::tf_saved_model::kTfSavedModelInitializerRestoreType;
using ::tensorflow::AssetFileDef;
using ::tensorflow::FunctionLibraryDefinition;
using ::tensorflow::GraphDef;
using ::tensorflow::NodeDef;
using ::tensorflow::SaverDef;
using ::tensorflow::quantization::ExportedModel;

// Finds and returns the name of the node from a set of control output nodes.
// The name should contain the string `contains`. Returns an empty string if no
// node whose name contains `contains` is found. Assumes there is at most one
// such a node.
std::string GetNodeName(const std::vector<std::string>& control_ret_node_names,
                        const absl::string_view contains) {
  for (const std::string& node_name : control_ret_node_names) {
    if (absl::StrContains(node_name, contains)) {
      VLOG(1) << "Node found: " << node_name << ", contains: " << contains;
      return node_name;
    }
  }
  VLOG(1) << "Could not find node whose name conatins: " << contains;
  return "";
}

// Returns the file prefix tensor name. An empty string is returned if no such a
// tensor is found (when there are no variables to restore, it is expected that
// the file prefix tensor does not exist). The file prefix tensor is found among
// the "_Arg" nodes, as it is translated from the MLIR @main function's
// argument. It also must have the attribute `tf_saved_model.index_path =
// ["__tf_file_prefix"]`.
//
// See `MergeSaveFunctionOpsToMainPass` for details how the file prefix tensor
// ends up at the MLIR @main function's argument.
std::string FindFilePrefixTensorName(const GraphDef& graph_def) {
  for (const NodeDef& node_def : graph_def.node()) {
    if (node_def.op() == FunctionLibraryDefinition::kArgOp) {
      // Matches the `tf_saved_model.index_path = ["__tf_file_prefix"]`.
      const auto index_path_attr_itr =
          node_def.attr().find(kTfSavedModelIndexPathAttr.str());
      if (index_path_attr_itr != node_def.attr().end()) {
        const auto& index_paths = index_path_attr_itr->second.list().s();
        if (absl::c_find(index_paths, kTfFilePrefix.str()) !=
            index_paths.end()) {
          // ":0" appended to indicate that it is a tensor, not an Operation.
          return absl::StrCat(node_def.name(), ":0");
        }
      }
    }
  }
  return "";
}

}  // namespace

ExportedModel CreateExportedModel(
    GraphDef&& graph_def, const absl::string_view init_node_name,
    const absl::string_view checkpoint_dir,
    const std::optional<SaverDef> saver_def,
    const absl::flat_hash_map<std::string, std::string>& function_aliases,
    const std::vector<AssetFileDef>& asset_file_defs) {
  ExportedModel exported_model{};
  *exported_model.mutable_graph_def() = graph_def;
  exported_model.set_init_node_name(std::string(init_node_name));
  exported_model.set_checkpoint_dir(std::string(checkpoint_dir));

  exported_model.mutable_function_aliases()->insert(function_aliases.begin(),
                                                    function_aliases.end());

  for (const AssetFileDef& asset_file_def : asset_file_defs) {
    *exported_model.mutable_asset_file_defs()->Add() = asset_file_def;
  }

  if (saver_def != std::nullopt) {
    *exported_model.mutable_saver_def() = *std::move(saver_def);
  }

  return exported_model;
}

// TODO: b/315746734 - Test this function using a test-only pass.
void AddExportPasses(mlir::PassManager& pm,
                     const bool duplicate_shape_determining_constants) {
  if (duplicate_shape_determining_constants) {
    pm.addNestedPass<mlir::func::FuncOp>(
        mlir::quant::CreateDuplicateShapeDeterminingConstantsPass());
  }

  pm.addPass(mlir::quant::CreateInsertMainFunctionPass());
  pm.addPass(mlir::quant::CreateLiftHashTableOpsAsArgsPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::CreateFunctionalToExecutorDialectConversionPass());
  pm.addPass(mlir::CreateBreakUpIslandsPass());
  pm.addPass(mlir::quant::CreateMergeInitializerFunctionOpsToMainPass());
  pm.addPass(mlir::quant::CreateMergeSaveFunctionOpsToMainPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::quant::CreateMergeDuplicateResourceOpsPass());

  // Used to clean up the "tf._noinliner" attribute that is previously used to
  // prevent certain functions from being inlined (see
  // `MarkFunctionsNoinlinePass`). InlinerPass must not come after this pass.
  pm.addPass(mlir::TF::CreateStripNoinlineAttributePass());
}

absl::StatusOr<std::optional<SaverDef>> CreateSaverDef(
    const std::vector<std::string>& control_ret_node_names,
    const GraphDef& graph_def) {
  const std::string filename_tensor_name = FindFilePrefixTensorName(graph_def);
  const std::string restore_op_name =
      GetNodeName(control_ret_node_names, kTfSavedModelInitializerRestoreType);
  const std::string save_node_name =
      GetNodeName(control_ret_node_names, kTfQuantSaveOpName);

  const std::vector<absl::string_view> fields = {
      filename_tensor_name, restore_op_name, save_node_name};
  const auto is_empty_predicate = [](const absl::string_view s) {
    return s.empty();
  };

  if (absl::c_all_of(fields, is_empty_predicate)) {
    return std::nullopt;
  } else if (absl::c_none_of(fields, is_empty_predicate)) {
    SaverDef saver_def{};
    saver_def.set_version(SaverDef::V2);
    saver_def.set_filename_tensor_name(filename_tensor_name);
    saver_def.set_restore_op_name(restore_op_name);
    // :0 attached to indicate the first result tensor. This saves the model
    // checkpoint when fetched.
    saver_def.set_save_tensor_name(absl::StrCat(save_node_name, ":0"));
    return saver_def;
  } else {
    return absl::InternalError(
        absl::StrCat("Failed to create SaverDef. Fields should be either all "
                     "empty strings or all non-empty strings. Got fields: ",
                     absl::StrJoin(fields, ",")));
  }
}

}  // namespace stablehlo::quantization
