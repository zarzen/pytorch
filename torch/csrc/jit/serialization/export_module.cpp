#include <torch/csrc/jit/serialization/export.h>

#include <c10/util/Exception.h>
#include <torch/csrc/jit/api/function_impl.h>
#include <torch/csrc/jit/backends/backend_debug_handler.h>
#include <torch/csrc/jit/backends/backend_debug_info.h>
#include <torch/csrc/jit/frontend/source_range.h>
#include <torch/csrc/jit/ir/attributes.h>
#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/ir/type_hashing.h>
#include <torch/csrc/jit/mobile/function.h>
#include <torch/csrc/jit/mobile/interpreter.h>
#include <torch/csrc/jit/mobile/method.h>
#include <torch/csrc/jit/mobile/module.h>
#include <torch/csrc/jit/passes/inliner.h>
#include <torch/csrc/jit/runtime/instruction.h>
#include <torch/csrc/jit/serialization/callstack_debug_info_serialization.h>
#include <torch/csrc/jit/serialization/export_bytecode.h>
#include <torch/csrc/jit/serialization/import_export_constants.h>
#include <torch/csrc/jit/serialization/import_export_functions.h>
#include <torch/csrc/jit/serialization/import_export_helpers.h>
#include <torch/csrc/jit/serialization/pickle.h>
#include <torch/csrc/jit/serialization/python_print.h>
#include <torch/csrc/jit/serialization/source_range_serialization.h>
#include <torch/csrc/jit/serialization/type_name_uniquer.h>

#include <caffe2/serialize/inline_container.h>

#include <ATen/ATen.h>

#include <ATen/core/jit_type.h>
#include <ATen/core/qualified_name.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace torch {
namespace jit {

IValue to_tuple(std::vector<IValue> ivalues) {
  return c10::ivalue::Tuple::create(std::move(ivalues));
}

IValue Table(const std::vector<std::pair<std::string, IValue>>& entries) {
  std::vector<IValue> ivalue_entries;
  ivalue_entries.reserve(entries.size());
  for (const auto& e : entries) {
    ivalue_entries.push_back(to_tuple({e.first, e.second}));
  }
  return to_tuple(std::move(ivalue_entries));
}

namespace {

ExportModuleExtraFilesHook& GetExtraFilesHook() {
  static ExportModuleExtraFilesHook func = nullptr;
  return func;
}

std::pair<IValue, IValue> getFunctionTuple(
    const Module& module,
    const Function& func,
    std::unique_ptr<Graph> optimizedGraph,
    BackendDebugInfoRecorder& debug_info_recorder,
    const std::string& qn,
    TypeNameUniquer& type_name_uniquer_) {
  TORCH_INTERNAL_ASSERT(optimizedGraph);
  std::shared_ptr<MobileCode> code;
  code = std::make_shared<MobileCode>(
      std::move(optimizedGraph), func.name(), BytecodeEmitMode::is_default_value_for_unspecified_arg_enabled() /* emit_default_input_instructions */, BytecodeEmitMode::is_default_args_before_out_args_enabled() /* enable_defaults_args_with_out_args */);
  auto instructions_copy = code->instructions();

  // operator names
  std::vector<c10::OperatorName> opnames;
  std::vector<std::string> method_names;
  std::vector<int64_t> op_debug_handles;
  int next_new_op_index = 0;
  for (size_t i = 0; i < instructions_copy.size(); ++i) {
    Instruction ins = instructions_copy[i];
    if ((ins.op == OP || ins.op == OPN) && ins.X == next_new_op_index) {
      // Found a new op (assumes new operators ordered by ascending ins.X)
      auto node = code->instructions_source()[i];
      opnames.emplace_back(node->schema().operator_name());
      next_new_op_index++;
    }
    // CALL nodes at this point represent built-in (i.e. non-Graph)
    // functions that were not inlined. Here we convert the CALL
    // instructions for these functions into INTERFACE_CALL instructions
    // s.t. at runtime, we will look up the Function* on the Type of the
    // 0th argument in the stack and call that directly.
    if (ins.op == CALL) {
      auto node = code->instructions_source()[i];
      if (node->kind() == prim::CallMethod) {
        // NB: replacing instruction
        auto method_name_idx =
            code->constant_table().size() + method_names.size();
        method_names.emplace_back(node->s(attr::name));
        Instruction new_instr{
            INTERFACE_CALL,
            static_cast<int32_t>(method_name_idx),
            static_cast<uint16_t>(node->inputs().size())};
        instructions_copy[i] = new_instr;
      } else {
        TORCH_INTERNAL_ASSERT(
            false, "Unsupported node kind on CALL opcode for mobile");
      }
    } else if (ins.op == RET) {
      auto node = code->instructions_source()[i];
      for (const auto& input : node->inputs()) {
        const auto& input_type = input->type();
        if (input_type->kind() == TypeKind::ListType ||
            input_type->kind() == TypeKind::DictType) {
          for (const TypePtr& element_type : input_type->containedTypes()) {
            TORCH_CHECK(
                element_type->kind() != TypeKind::ClassType,
                "Returining a list or dictionary with pytorch class type ",
                "is not supported in mobile module "
                "(List[Foo] or Dict[int, Foo] for class Foo(torch.nn.Module)). "
                "Workaround: instead of using pytorch class as their element type, ",
                "use a combination of list, dictionary, and single types.");
          }
        }
      }
    } else {
      TORCH_CHECK(
          isOpSupportedInMobile(ins.op),
          toString(ins.op),
          " is not supported in mobile module.");
    }
    auto node = code->instructions_source()[i];
    int64_t debug_handle = debug_info_recorder.getNextDebugHandle(node);
    // Note 1-to-1 correspondence between instructions and debug handles
    op_debug_handles.emplace_back(debug_handle);
  }

  // instructions
  std::vector<IValue> instructions;
  instructions.reserve(instructions_copy.size());
  for (Instruction ins : instructions_copy) {
    instructions.emplace_back(to_tuple({toString(ins.op), ins.X, ins.N}));
  }

  // operators
  std::vector<IValue> operators;
  auto op_to_specified_args = code->op_to_num_specified_args();
  operators.reserve(opnames.size());
  for (const auto& opname : opnames) {
    auto unique_name = c10::toString(opname);
    // For operator with vararg, adding default arguments would be confusing and
    // is not allowed. For an operator with num_args = -1, it means the number
    // of arguments is not available for this operator, we don't do any backward
    // compatibility adaptation at runtime.
    int num_args = -1;
    auto it = op_to_specified_args.find(unique_name);
    if (it != op_to_specified_args.end()) {
      num_args = it->second;
    }
    if (BytecodeEmitMode::is_default_value_for_unspecified_arg_enabled()) {
      operators.emplace_back(to_tuple({opname.name, opname.overload_name}));
    } else {
      operators.emplace_back(
          to_tuple({opname.name, opname.overload_name, num_args}));
    }
  }

  // constants
  //
  // Make a copy of the constants and append the method names
  // that we emitted for the converted INTERFACE_CALL nodes above.
  auto constants = code->constant_table();
  for (auto& method_name : method_names) {
    constants.emplace_back(std::move(method_name));
  }

  // types
  std::vector<IValue> types;
  types.reserve(code->type_table().size());
  static const std::string torch_prefix("__torch__");
  static const std::string class_prefix("__torch__.torch.classes");
  std::shared_ptr<torch::jit::CompilationUnit> cu =
      module._ivalue()->compilation_unit();

  for (const TypePtr& t : code->type_table()) {
    std::string type_str = t->annotation_str();
    if (t->kind() == TypeKind::TupleType) {
      TORCH_CHECK(
          cu->get_named_tuple(t->str()),
          "Can't find definition for the qualified name: ",
          t->str(),
          "(TypeKind::TupleType)  in compilation unit.",
          "Please report a bug to PyTorch.");
      auto named_tuple_type = cu->get_named_tuple(t->str());
      if (named_tuple_type != nullptr) {
        std::string named_tuple_str = t->str();
        named_tuple_str.append("[NamedTuple, [");
        std::vector<IValue> name_type_pairs;

        // Get the field name and field type for the NamedTuple
        for (auto it = named_tuple_type->schema()->arguments().begin();
             it != named_tuple_type->schema()->arguments().end();
             it++) {
          name_type_pairs.emplace_back(
              c10::ivalue::Tuple::create({it->name(), it->type()->repr_str()}));

          // When it->type() is Tensor type, in Python, if it's inferred type,
          // str() return "Tensor" and repr_str() return "Tensor (inferred)". If
          // it's not inferred type, str() return "Tensor[]" and repr_str()
          // return "Tensor". In cpp, repr_str() will always return "Tensor"
          // regardless inferred type. When exporing custom type in bytecode,
          // "Tensor" is the preferred way to deserialize Tensor type
          type_str = it->is_inferred_type() ? it->type()->str()
                                            : it->type()->repr_str();
          named_tuple_str.append("[" + it->name() + ", " + type_str + "]");
          if (it != named_tuple_type->schema()->arguments().end() - 1) {
            named_tuple_str.append(",");
          }
        }
        named_tuple_str.append("]]");
        // Create a named_tuple type with following structure
        // "qualified_named[
        //   NamedTuple, [
        //       [filed_name_1, field_type_1],
        //       [filed_name_2, field_type_2]
        //   ]
        // ]"
        //  Example NamedTuple type:
        //  "__torch__.base_models.sparse_nn.pytorch_preproc_types.PreprocOutputType[
        //     NamedTuple, [
        //         [float_features, Tensor],
        //         [id_list_features, List[Tensor]],
        //         [label,  Tensor],
        //         [weight, Tensor],
        //         ]
        //     ]"
        types.emplace_back(named_tuple_str);
        continue;
      }
    } else if (type_str.find(torch_prefix) == 0) {
      TORCH_CHECK(
          type_str.find(class_prefix) == 0,
          "__torch__ types other than torchbind (__torch__.torch.classes)"
          "are not supported in lite interpreter. ",
          "Workaround: instead of using arbitrary class type (class Foo()), ",
          "define a pytorch class (class Foo(torch.nn.Module)).");
    }
    types.emplace_back(type_str);
  }

  // since the register location is embedded into the bytecode, pass the
  // register size
  auto register_size = static_cast<int>(code->register_size());

  auto codeTable = Table(
      {{"instructions", to_tuple(instructions)},
       {"operators", to_tuple(operators)},
       {"constants", to_tuple(constants)},
       {"types", to_tuple(types)},
       {"register_size", register_size}});

  // schema
  const auto& schema = func.getSchema();
  auto type_printer =
      [&](const c10::ConstTypePtr& t) -> c10::optional<std::string> {
    auto namedType = t->cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return type_name_uniquer_.getUniqueName(namedType).qualifiedName();
    }
    return c10::nullopt;
  };
  TORCH_CHECK(
      schema.overload_name().empty(), // @TODO: is this check correct?
      "Overloads are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_vararg(), "Python *args are not supported in mobile modules.");
  TORCH_CHECK(
      !schema.is_varret(),
      "A variable number of return values is not supported in mobile modules.");
  auto makeArgTuple = [&](const std::vector<Argument>& args) {
    std::vector<IValue> argTables;
    for (auto&& arg : args) {
      TORCH_CHECK(
          !arg.N(),
          "Arguments with known list lengths are not supported in mobile modules.");
      TORCH_CHECK(
          !arg.kwarg_only(),
          "Keyword-only arguments are not supported in mobile modules.");
      /*
        This part adds the argument's name, type and default_value in
        `bytecode.pkl` This has to be consistent with the `code/` directory
        which has annotated py code of the entire module. `type_printer` uses
        `TypeNameUniquer` to get the managled name of the argument. This helps
        in having the right object reference when a class method is called using
        the `self` argument.

        arg.type()->annotation_str(type_printer) => mangled unique name of the
        module/submodule
      */
      argTables.emplace_back(Table({
          {"name", arg.name()},
          {"type", arg.type()->annotation_str(type_printer)},
          {"default_value", arg.default_value()},
      }));
    }
    return to_tuple(argTables);
  };
  auto schemaTable = Table({
      {"arguments", makeArgTuple(schema.arguments())},
      {"returns", makeArgTuple(schema.returns())},
  });

  // function tuple
  auto bytecode_vals = to_tuple({qn, codeTable, schemaTable});

  c10::optional<IValue> debug_info_vals;
  // module debug info
  // This is just a set of debug handles.
  // We always save debug handles.
  // debug handles generated by debug_handle_manager
  // will correspond to {source_range, inlinedCallStackPtr} which we will
  // serialize separately.
  IValue module_debug_tuple = c10::ivalue::Tuple::create(op_debug_handles);
  auto function_debug_info =
      Table({{"function_debug_handles", module_debug_tuple}});
  debug_info_vals = to_tuple({qn, function_debug_info});
  return std::make_pair(bytecode_vals, debug_info_vals);
}

void pushFunctionToIValues(
    BytecodeExportSet exportSet,
    std::vector<c10::IValue>& elements,
    std::vector<c10::IValue>& debugInfoElements,
    BackendDebugInfoRecorder& recorder,
    TypeNameUniquer& uniquer) {
  exportSet.visit(
      [&](const c10::QualifiedName& qn, ExportedFunction& exported) {
        auto tuple = getFunctionTuple(
            exported.mod,
            exported.function,
            std::move(exported.optimizedGraph),
            recorder,
            qn.qualifiedName(),
            uniquer);
        elements.push_back(std::move(tuple.first));
        debugInfoElements.push_back(std::move(tuple.second));
      });
}

void pushFunctionToIValues(
    BytecodeExportSet exportSet,
    std::vector<c10::IValue>& elements,
    BackendDebugInfoRecorder& recorder,
    TypeNameUniquer& uniquer) {
  std::vector<c10::IValue> debugInfoElements;
  pushFunctionToIValues(
      std::move(exportSet), elements, debugInfoElements, recorder, uniquer);
}

std::unordered_set<const FunctionSchema*> getInterfaceCalls(Graph& graph) {
  std::unordered_set<const FunctionSchema*> ret;
  auto nodes = findAllNodes(graph, c10::prim::CallMethod, true);
  for (Node* node : nodes) {
    if (auto iface = node->input(0)->type()->castRaw<InterfaceType>()) {
      ret.insert(iface->getMethod(node->s(attr::name)));
    }
  }
  return ret;
}

struct ModuleMethod {
  ModuleMethod(const Module& m, const GraphFunction& f, c10::QualifiedName n)
      : module(m), function(f), exportName(std::move(n)) {}
  Module module;
  const GraphFunction& function;
  c10::QualifiedName exportName;
};

std::vector<ModuleMethod> getModuleInterfaceExports(
    const Module& module,
    const std::unordered_set<const FunctionSchema*>& schemas) {
  if (schemas.size() == 0) {
    return {};
  }
  std::unordered_set<std::string> names;
  for (auto schema : schemas) {
    names.insert(schema->name());
  }
  std::vector<ModuleMethod> ret;
  for (const auto& submodule : module.modules()) {
    for (const auto& method : submodule.get_methods()) {
      const auto& f = toGraphFunction(method.function());
      if (names.find(f.qualname().name()) != names.end()) {
        ret.emplace_back(submodule, f, f.qualname());
      }
    }
  }
  return ret;
}

void exportFunction(
    BytecodeExportSet& exportSet,
    const ModuleMethod& method,
    bool toplevel = false) {
  const auto& func = method.function;
  const auto& qn = method.exportName;
  if (exportSet.contains(qn)) {
    if (toplevel) {
      exportSet.update(qn, toplevel);
    }
    return;
  }
  auto graph = func.graph()->copyUnique();
  Inline(*graph);
  auto interfaceCalls = getInterfaceCalls(*graph);
  exportSet.add(
      qn, ExportedFunction{method.module, func, std::move(graph), toplevel});

  if (!getMobileInterfaceCallExport()) {
    return;
  }

  auto interfaces = getModuleInterfaceExports(method.module, interfaceCalls);
  for (const auto& interface : interfaces) {
    exportFunction(exportSet, interface);
  }
}

void setstateTuple(
    BytecodeExportSet& exportSet,
    const Module& module,
    const IValue& ivalue,
    TypeNameUniquer& type_name_uniquer_,
    bool toplevel = false) {
  if (!ivalue.isObject())
    return;
  auto obj = ivalue.toObject();
  auto type = obj->type();
  if (checkHasValidSetGetState(type)) {
    Function& setstate = type->getMethod("__setstate__");
    auto qn = type_name_uniquer_.getUniqueName(obj->type()).qualifiedName() +
        "." + setstate.name();
    if (exportSet.contains(qn)) {
      return;
    }
    if (auto f = tryToGraphFunction(setstate)) {
      exportFunction(exportSet, ModuleMethod{module, *f, qn}, toplevel);
    }
  } else {
    for (size_t i = 0, n = type->numAttributes(); i < n; ++i) {
      setstateTuple(exportSet, module, obj->getSlot(i), type_name_uniquer_);
    }
  }
}

bool isLoweredModule(const Module& m) {
  c10::QualifiedName type_name;
  if (m.type()->name()) {
    type_name = m.type()->name().value();
  }
  bool isLoweredModule = false;
  for (const auto& atom : type_name.atoms()) {
    if (atom == "LoweredModule") {
      isLoweredModule = true;
      break;
    }
  }
  return isLoweredModule;
}

// Check if the global static map of backend debug info
// contains debug info for this module and any of its children.
// If so combine all the maps together and return one.
void getBackendDebugInfoMap(
    const Module& m,
    BackendDebugInfoMapType& debug_map) {
  if (isLoweredModule(m)) {
    auto backend_debug_info =
        m.attr("__backend_debug_info").toCustomClass<PyTorchBackendDebugInfo>();
    const auto& map = backend_debug_info->getDebugInfoMap();
    if (map) {
      debug_map.insert(map.value().begin(), map.value().end());
    }
  }
  for (const auto& c : m.children()) {
    getBackendDebugInfoMap(c, debug_map);
  }
}

SourceRangeRecords getBackendSourceRanges(const Module& m) {
  SourceRangeRecords sr_records;
  if (isLoweredModule(m)) {
    constexpr size_t kSourceRange = 1;
    auto backend_debug_info =
        m.attr("__backend_debug_info").toCustomClass<PyTorchBackendDebugInfo>();
    const auto& map = backend_debug_info->getDebugInfoMap();
    if (map) {
      const auto& map_val = map.value();
      // This map is map of debug handle-to-DebugInfoTuple
      // DebugInfoTuple= <source range, op name, inlined_cs_ptr>
      for (const auto& it : map_val) {
        auto& source_range =
            std::get<kDebugInfoTupleSourceRangeIndex>(it.second);
        sr_records.emplace_back(
            std::numeric_limits<size_t>::max(), source_range);
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        auto cs_ptr = std::get<kDebugInfoTupleInlinedCSIndex>(it.second);
        if (cs_ptr) {
          for (const auto& e : cs_ptr->vec()) {
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const auto sr = std::get<kSourceRange>(e);
            sr_records.emplace_back(std::numeric_limits<size_t>::max(), sr);
          }
        }
      }
    }
  }
  for (const auto& c : m.children()) {
    const auto& child_sr_records = getBackendSourceRanges(c);
    sr_records.reserve(sr_records.size() + child_sr_records.size());
    std::move(
        child_sr_records.begin(),
        child_sr_records.end(),
        std::back_inserter(sr_records));
  }
  return sr_records;
}

auto& mobileInterfaceCallExport() {
  static std::atomic<bool> flag{false};
  return flag;
}

} // namespace

TORCH_API void enableMobileInterfaceCallExport() {
  mobileInterfaceCallExport().store(true, std::memory_order_relaxed);
}
bool getMobileInterfaceCallExport() {
  return mobileInterfaceCallExport().load(std::memory_order_relaxed);
}

BytecodeExportSet moduleMethodsTuple(
    const Module& module,
    TypeNameUniquer& type_name_uniquer_) {
  BytecodeExportSet exportSet;
  auto methods = module.get_methods();
  // top level methods
  for (const auto& method : methods) {
    const auto& f = toGraphFunction(method.function());
    exportFunction(
        exportSet, ModuleMethod{module, f, f.qualname()}, /* toplevel */ true);
  }

  // __setstate__ of all components
  setstateTuple(exportSet, module, module._ivalue(), type_name_uniquer_, true);

  return exportSet;
}

void SetExportModuleExtraFilesHook(ExportModuleExtraFilesHook hook) {
  GetExtraFilesHook() = std::move(hook);
}

void ScriptModuleSerializer::serialize(
    const Module& module,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info) {
  C10_LOG_API_USAGE_ONCE("torch.script.save");
  writeExtraFiles(module, extra_files);
  // Serialize the model object
  writeArchive(
      module._ivalue(),
      /*archive_name=*/"data",
      /*archive_dir=*/"",
      /*tensor_dir=*/"data/");
  // Then we serialize all code info.
  convertTypes(module.type());
  writeFiles("code/");
  // The tensor constants from the code are written to a separate archive
  // so loading the code does not depend on loading the data
  std::vector<IValue> ivalue_constants(
      constant_table_.begin(), constant_table_.end());
  if (bytecode_format) {
    writeArchive(
        c10::ivalue::Tuple::create(ivalue_constants),
        /*archive_name=*/"constants",
        /*archive_dir=*/"",
        /*tensor_dir=*/"constants/",
        /*use_storage_context=*/true);

    writeByteCode(module, save_mobile_debug_info);
  } else {
    writeArchive(
        c10::ivalue::Tuple::create(ivalue_constants),
        /*archive_name=*/"constants",
        /*archive_dir=*/"",
        /*tensor_dir=*/"constants/");
  }
  // Acquires and sets minimum (dynamic) version
  for (auto& item : file_streams_) {
    writer_.setMinVersion(item.value().minVersion());
  }
}

void ScriptModuleSerializer::writeArchive(
    const IValue& value,
    const std::string& archive_name,
    const std::string& archive_dir,
    const std::string& tensor_dir,
    bool use_storage_context) {
  std::vector<char> data;
  // Vector to capture the run-time class types during pickling the IValues
  std::vector<c10::ClassTypePtr> memoizedClassTypes;
  std::vector<std::string> tensor_names;
  // tensors that are already serialized in use_storage_context
  std::unordered_set<std::string> serialized_tensors;
  Pickler data_pickle(
      [&](const char* buf, size_t size) {
        data.insert(data.end(), buf, buf + size);
      },
      nullptr,
      [&](const c10::ClassTypePtr& t) {
        return type_name_uniquer_.getUniqueName(t);
      },
      &memoizedClassTypes,
      [&](const at::Tensor& tensor) {
        // returns a string to use in picker.cpp as storage obj key
        if (use_storage_context) {
          bool already_serialized =
              storage_context_.hasStorage(tensor.storage());
          std::string tensor_name =
              std::to_string(
                  storage_context_.getOrAddStorage(tensor.storage())) +
              ".storage";
          if (already_serialized) {
            // this case is hit when storage has been serialized already
            // from a torch.package context
            serialized_tensors.insert(tensor_name);
          }
          tensor_names.push_back(tensor_name);
        } else {
          tensor_names.push_back(std::to_string(tensor_names.size()));
        }
        return tensor_names.back();
      });
  data_pickle.protocol();
  data_pickle.pushIValue(value);
  data_pickle.stop();
  // write out tensor data
  size_t i = 0;
  std::string prefix = archive_name + "/";

  TORCH_INTERNAL_ASSERT(tensor_names.size() == data_pickle.tensorData().size());

  for (const auto& td : data_pickle.tensorData()) {
    WriteableTensorData writable_td = getWriteableTensorData(td);
    std::string tensor_name = tensor_names[i++];
    if (use_storage_context && serialized_tensors.count(tensor_name)) {
      // storage has been serialzed already, skip
      continue;
    }
    writer_.writeRecord(
        tensor_dir + tensor_name,
        writable_td.data(),
        writable_td.sizeInBytes());
  }

  std::string fname = archive_dir + archive_name + ".pkl";
  writer_.writeRecord(fname, data.data(), data.size());

  // serialize all the captured run-time class types
  for (const c10::ClassTypePtr& wroteType : memoizedClassTypes) {
    convertNamedType(wroteType);
  }
}

void ScriptModuleSerializer::writeExtraFiles(
    const Module& module,
    const ExtraFilesMap& extra_files) {
  // Write out extra files.
  for (const auto& kv : extra_files) {
    const std::string key = "extra/" + kv.first;
    writer_.writeRecord(key, kv.second.data(), kv.second.size());
  }
  auto hook = GetExtraFilesHook();
  if (hook) {
    ExtraFilesMap hook_files = hook(module);
    for (const auto& kv : hook_files) {
      // Checks if the hooked file is already written in extra files,
      //   if so, skips it and warns
      if (extra_files.find(kv.first) != extra_files.end()) {
        TORCH_WARN_ONCE(
            "An extra files hook attempted to write ",
            kv.first,
            " but ",
            "this is already written in extra files and so will be skipped. ",
            "This warning will only appear once per process.");
        continue;
      }
      const std::string key = "extra/" + kv.first;
      writer_.writeRecord(key, kv.second.data(), kv.second.size());
    }
  }
}

void ScriptModuleSerializer::updateSourceRangeTags(
    const SourceRangeRecords& ranges) {
  for (const auto& range : ranges) {
    if (source_range_tags_.find(range.range) == source_range_tags_.end()) {
      source_range_tags_[range.range] = current_source_range_tag_;
      current_source_range_tag_++;
    }
  }
}

void ScriptModuleSerializer::convertTypes(const at::NamedTypePtr& root_type) {
  class_deps_.add(root_type);
  for (size_t i = 0; i < class_deps_.size(); ++i) {
    // note: convertNameType may extend class_deps_, so re-checking .size() is
    // necessary
    convertNamedType(class_deps_[i]);
  }
}

void ScriptModuleSerializer::writeFiles(const std::string& code_dir) {
  current_source_range_tag_ = 0;
  // Mapping of filename => src. We need this because multiple classes may go
  // in the same file (e.g. foo.bar.Baz and foo.bar.Qux)
  for (auto& item : file_streams_) {
    const std::string filename = qualifierToArchivePath(item.key(), code_dir);

    std::string src = item.value().str();

    // Only compress these records if they're not tiny.
    // The cpu cost of generating zip datastructs and compressing isn't
    // well-spent for very small records.
    static constexpr size_t kMinToCompress = 200;

    writer_.writeRecord(
        filename,
        src.c_str(),
        src.size(),
        src.size() > kMinToCompress /*compress*/);

    // Write out the debug information
    std::string debugFilename = filename + ".debug_pkl";
    SourceRangePickler source_range_pickler;
    updateSourceRangeTags(item.value().ranges());
    auto range_data =
        source_range_pickler.pickle(item.value().ranges(), source_range_tags_);
    writer_.writeRecord(
        debugFilename,
        range_data.data(),
        range_data.size(),
        range_data.size() > kMinToCompress /*compress*/);
  }
}

void ScriptModuleSerializer::writeByteCode(
    const Module& module,
    const bool save_mobile_debug_info) {
  std::vector<c10::IValue> elements;
  BackendDebugInfoRecorder debug_info_recorder;
  int64_t version_to_write = caffe2::serialize::kProducedBytecodeVersion;

  elements.emplace_back(static_cast<int64_t>(version_to_write));
  std::vector<c10::IValue> debug_info_elements;
  // Always save debug handles
  debug_info_elements.emplace_back(static_cast<int64_t>(version_to_write));

  BytecodeExportSet exportSet = moduleMethodsTuple(module, type_name_uniquer_);
  pushFunctionToIValues(
      std::move(exportSet),
      elements,
      debug_info_elements,
      debug_info_recorder,
      type_name_uniquer_);

  auto telements = to_tuple(std::move(elements));
  writeArchive(
      telements,
      /*archive_name=*/"bytecode",
      /*archive_dir=*/"",
      /*tensor_dir=*/"constants/",
      /*use_storage_context=*/true);

  auto debug_info_telements = to_tuple(std::move(debug_info_elements));

  // At the moment keeping this feature experimental
  // since we have not evaluated how this affect model size
  // and we have not build any utility to strip off debug info
  // when desired
  // TODO: Build utility to strip off debug map. It should also do the
  // same for debug_pkl files
  if (save_mobile_debug_info) {
    // Note that stripping off debug map will not strip off
    // debug handles.
    // The reason we save debug handles conditionally is so that
    // we dont end up with a model that has debug handles but has not
    // debug map to correlate debug handels with.
    // Once we have a model with both handles and debug map, we can
    // strip off debug map and have a lean model served to production.
    // If exception ocurrs we have a model with debug map that can be
    // used to symbolicate debug handles
    writeArchive(
        debug_info_telements,
        /*archive_name=*/"mobile_debug_handles",
        /*archive_dir=*/"",
        /*tensor_dir=*/"mobile_debug_handles/");
    static constexpr size_t kMinToCompress = 200;
    // For delegated backends get source ranges that are in the debug info
    // map. Since delegated backend replace original module with lowered
    // module we will not serialize original module's code which is what would
    // have contained source range. Since we dont have that anymore, extract
    // source ranges out of delegated module and store in a separate archive.
    // Note that we must do this first because in order to serialize inlined
    // CS appropriate source_range_tags must have been generated.
    auto backend_source_range_records = getBackendSourceRanges(module);
    SourceRangePickler source_range_pickler;
    updateSourceRangeTags(backend_source_range_records);
    auto range_data = source_range_pickler.pickle(
        backend_source_range_records, source_range_tags_);
    std::string debugFilename = "delegated_backends.debug_pkl";
    writer_.writeRecord(
        debugFilename,
        range_data.data(),
        range_data.size(),
        range_data.size() > kMinToCompress /*compress*/);

    // For delegated backends get debug_info_map
    // This is merged with other debug_info_map of other modules
    // which were not delegated.
    BackendDebugInfoMapType backend_debug_info_map;
    getBackendDebugInfoMap(module, backend_debug_info_map);
    // Now get the debug-handles-to-inlined-cs-ptr-map
    // And serialize that in a separate archive
    auto debug_handle_cs_ptr_map = debug_info_recorder.stopRecording();
    debug_handle_cs_ptr_map.insert(
        backend_debug_info_map.begin(), backend_debug_info_map.end());
    CallStackDebugInfoPickler cs_debug_info_pickler;
    auto cs_data = cs_debug_info_pickler.pickle(
        debug_handle_cs_ptr_map, source_range_tags_);
    // Write out map: [debug-handle, {source range, InlinedCallStack}]
    std::string filename = "callstack_debug_map.pkl";
    writer_.writeRecord(
        filename,
        cs_data.data(),
        cs_data.size(),
        cs_data.size() > kMinToCompress /*compress*/);
  }
}

void ScriptModuleSerializer::convertNamedType(
    const c10::NamedTypePtr& class_type) {
  if (converted_types_.count(class_type)) {
    return;
  }
  converted_types_.insert(class_type);
  auto qualname = type_name_uniquer_.getUniqueName(class_type);
  std::string qualifier = qualname.prefix();
  PythonPrint* pp = file_streams_.find(qualifier);

  auto type_printer =
      [&](const c10::ConstTypePtr& t) -> c10::optional<std::string> {
    auto namedType = t->cast<c10::NamedType>();
    if (namedType && namedType->name()) {
      return type_name_uniquer_.getUniqueName(namedType).qualifiedName();
    }
    return c10::nullopt;
  };
  if (!pp) {
    pp = &file_streams_.insert(
        std::move(qualifier),
        PythonPrint(
            constant_table_,
            class_deps_,
            type_printer,
            /*enforce_importable=*/true));
  }
  pp->printNamedType(class_type);
}

void ScriptModuleSerializer::serialize_unified_format(
    Module& module,
    uint64_t script_module_id) {
  const std::string archive_dir =
      ".data/ts_code/" + std::to_string(script_module_id) + "/";

  // Serialize the model object
  writeArchive(
      module._ivalue(),
      "data",
      archive_dir,
      /*tensor_dir=*/".data/",
      /*use_storage_context=*/true);
  // Then we serialize all code info.
  convertTypes(module.type());
  // The tensor constants from the code are written to a separate archive
  // so loading the code does not depend on loading the data
  std::vector<IValue> ivalue_constants(
      constant_table_.begin(), constant_table_.end());
  writeArchive(
      c10::ivalue::Tuple::create(ivalue_constants),
      "constants",
      archive_dir,
      /*tensor_dir=*/".data/",
      /*use_storage_context=*/true);

  // Note: writeFiles() call needs to be made in addition to calling this
  // function to have the code actually saved (tensors are saved)
}

SerializationStorageContext& ScriptModuleSerializer::storage_context() {
  return storage_context_;
}

void ExportModule(
    const Module& module,
    std::ostream& out,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info) {
  caffe2::serialize::PyTorchStreamWriter writer(
      [&](const void* buf, size_t nbytes) -> size_t {
        out.write(static_cast<const char*>(buf), nbytes);
        return !out ? 0 : nbytes;
      });
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info);
}

void ExportModule(
    const Module& module,
    const std::string& filename,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info) {
  caffe2::serialize::PyTorchStreamWriter writer(filename);
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info);
}

void ExportModule(
    const Module& module,
    const std::function<size_t(const void*, size_t)>& writer_func,
    const ExtraFilesMap& extra_files,
    bool bytecode_format,
    bool save_mobile_debug_info) {
  caffe2::serialize::PyTorchStreamWriter writer(writer_func);
  ScriptModuleSerializer serializer(writer);
  serializer.serialize(
      module, extra_files, bytecode_format, save_mobile_debug_info);
}

namespace {
void export_opnames(const script::Module& m, std::set<std::string>& opnames) {
  std::vector<c10::IValue> elements;
  BackendDebugInfoRecorder dummy;
  TypeNameUniquer dummy_uniquer = TypeNameUniquer();
  BytecodeExportSet exportSet = moduleMethodsTuple(m, dummy_uniquer);
  pushFunctionToIValues(std::move(exportSet), elements, dummy, dummy_uniquer);
  for (const auto& element : elements) {
    auto table = element.toTuple()->elements()[1];
    auto row =
        table.toTuple()->elements().at(BYTECODE_INDEX_OPERATOR).toTuple();
    TORCH_INTERNAL_ASSERT(
        row->elements().at(0).toStringRef() == "operators",
        "Expected operators but found ",
        row->elements().at(0).toStringRef());
    const auto& ops_list = row->elements().at(1).toTuple()->elements();
    for (const auto& op : ops_list) {
      const auto& op_item = op.toTuple()->elements();
      TORCH_CHECK(
          op_item.size() >= 2,
          "There should be either two parts (name and overload name), ",
          "or three parts (name, overload name and number of specified args) ",
          "for an operator.");
      auto opname = op_item[0].toString()->string();
      auto overload = op_item[1].toString()->string();
      // NOLINTNEXTLINE(performance-inefficient-string-concatenation)
      opnames.emplace(overload.empty() ? opname : opname + "." + overload);
    }
  }
}
} // namespace

std::vector<std::string> export_opnames(const script::Module& m) {
  std::set<std::string> names;
  export_opnames(m, names);
  return std::vector<std::string>(names.begin(), names.end());
}

// Thread local flag (only happens in export, i.e. on server side)
// to control if instructions for bytecode default inputs are emitted
// or not. It's the major difference between bytecode v5 and v6.
thread_local bool emitBytecodeDefaultInputs =
    caffe2::serialize::kProducedBytecodeVersion <= 5 ? true : false;
bool BytecodeEmitMode::is_default_value_for_unspecified_arg_enabled() {
  return emitBytecodeDefaultInputs;
}
void BytecodeEmitMode::set_default_value_for_unspecified_arg_enabled(
    bool enabled) {
  emitBytecodeDefaultInputs = enabled;
}

thread_local bool emitDefautlArgsWithOutArgs =
    caffe2::serialize::kProducedBytecodeVersion <= 6 ? false : true;
bool BytecodeEmitMode::is_default_args_before_out_args_enabled() {
  return emitDefautlArgsWithOutArgs;
}
void BytecodeEmitMode::set_default_args_before_out_args_enabled(bool enabled) {
  emitDefautlArgsWithOutArgs = enabled;
}

} // namespace jit
} // namespace torch
