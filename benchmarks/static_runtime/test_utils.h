// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <string>
#include <vector>

#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/runtime/static/impl.h>

namespace c10 {
struct IValue;
}

namespace torch {
namespace jit {

struct Node;
class StaticModule;

namespace test {

// Given a model/function in jit or IR script, run the model/function
// with the jit interpreter and static runtime, and compare the results
void testStaticRuntime(
    const std::string& source,
    const std::vector<c10::IValue>& args,
    const std::vector<c10::IValue>& args2 = {},
    const bool use_allclose = false,
    const bool use_equalnan = false);

std::shared_ptr<Graph> getGraphFromIR(const std::string& ir);

bool hasProcessedNodeWithName(torch::jit::StaticModule& smodule, const char *name);

} // namespace test
} // namespace jit
} // namespace torch
