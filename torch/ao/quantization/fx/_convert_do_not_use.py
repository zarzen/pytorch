from typing import Any, Dict, List
import torch
from torch.fx import (
    GraphModule,
)
from torch.fx.graph import (
    Graph,
    Node,
)
from ..qconfig import QConfigAny
from ..quantization_mappings import get_static_quant_module_class
from ..utils import (
    weight_is_statically_quantized,
    get_qparam_dict,
)

from .match_utils import (
    find_matches,
)
from .graph_module import (
    QuantizedGraphModule,
)
from ._equalize import update_obs_for_equalization, convert_eq_obs
from .utils import (
    get_custom_module_class_keys,
    get_quantize_node_info,
    create_getattr_from_value,
    _parent_name,
)

from torch.ao.quantization.quantize import (
    _remove_qconfig,
    is_activation_post_process,
)

from .convert import restore_state

def _convert_do_not_use(
        model: GraphModule, is_reference: bool = False,
        convert_custom_config_dict: Dict[str, Any] = None,
        is_standalone_module: bool = False,
        _remove_qconfig_flag: bool = True) -> QuantizedGraphModule:
    """
    We will convert an observed model (a module with observer calls) to a reference
    quantized model, the rule is simple:
    1. for each observer module call in the graph, we'll convert it to calls to
       quantize and dequantize functions based on the observer instance
    2. for weighted operations like linear/conv, we need to convert them to reference
       quantized module, this requires us to know whether the dtype configured for the
       weight is supported in the backend, this is done in prepare step and the result
       is stored in observed_node_names, we can decide whether we need to swap the
       module based on this set

    standalone_module means it a submodule that is not inlined in
    parent module, and will be quantized separately as one unit.

    Returns a quantized standalone module, whether input/output is quantized is
    specified by prepare_custom_config_dict, with
    input_quantized_idxs, output_quantized_idxs, please
    see docs for prepare_fx for details
    """
    if convert_custom_config_dict is None:
        convert_custom_config_dict = {}
    patterns, node_name_to_scope, prepare_custom_config_dict, observed_node_names = restore_state(model)
    qconfig_map: Dict[str, QConfigAny] = model._qconfig_map  # type: ignore[assignment]

    assert is_reference, "convert2 only supports reference option"

    # mapping from fully qualified module name to module instance
    # for example,
    # {
    #   '': Model(...),
    #   'linear': Linear(...),
    #   'linear.weight_fake_quant': PerChannelMinMaxObserver(...),
    # }
    # We use remove_duplicate=False here because torch.cat uses
    # the same activation_post_process module instance but different names
    modules = dict(model.named_modules(remove_duplicate=False))

    custom_module_classes = get_custom_module_class_keys(
        convert_custom_config_dict,
        "observed_to_quantized_custom_module_class")
    matches = find_matches(
        model.graph, modules, patterns,
        qconfig_map,
        custom_module_classes=custom_module_classes)

    if model._equalization_qconfig_map is not None:
        # If we want to do equalization then do the following:
        # Calculate the equalization scale, update the observers with the scaled
        # inputs, and scale the weight
        weight_eq_obs_dict = update_obs_for_equalization(model, modules)
        convert_eq_obs(model, modules, weight_eq_obs_dict)

    graph_inputs: List[str] = []
    for node in model.graph.nodes:
        if node.op == 'placeholder':
            graph_inputs.append(node.name)

    def replace_observer_with_quantize_dequantize_node(graph: Graph, node: Node, modules: Dict[str, torch.nn.Module]) -> None:
        """ Replace activation_post_process module call node with quantize and
        dequantize node

        Before:
        ... -> observer_0(x) -> ...
        After:
        ... -> torch.quantize_per_tensor(x, ...) -> x.dequantize() -> ...
        """
        assert modules is not None
        assert isinstance(node.target, str)
        observer_module = modules[node.target]
        root_module = modules[""]
        if observer_module.dtype == torch.float32:
            # remove the node for now
            # TODO: support dynamic quant
            with graph.inserting_before(node):
                node.replace_all_uses_with(node.args[0])
                graph.erase_node(node)
        elif observer_module.dtype in [torch.quint8, torch.qint8, torch.float16]:
            node_type, quantize_op, qparams = get_quantize_node_info(observer_module)
            # replace observer node with quant - dequant node
            with graph.inserting_before(node):
                input_node = node.args[0]
                inputs = [input_node]
                for key, value in qparams.items():
                    if key in ['_scale_', '_zero_point_']:
                        # For scale and zero_point values we register them as buffers in the root module.
                        # TODO: maybe need more complex attr name here
                        qparam_node = create_getattr_from_value(root_module, graph, key, value)
                        inputs.append(qparam_node)
                    else:
                        # for qparams that are not scale/zero_point (like axis, dtype) we store them as literals in the graph.
                        inputs.append(value)

                quantized_node = graph.create_node(node_type, quantize_op, tuple(inputs), {})
                dequantized_node = graph.call_method("dequantize", args=(quantized_node,))
                node.replace_all_uses_with(dequantized_node)
                graph.erase_node(node)


    # additional state to override inputs to be quantized, if specified
    # by the user
    placeholder_node_seen_cnt = 0
    output_node_seen_cnt = 0
    input_quantized_idxs: List[int] = prepare_custom_config_dict.get(
        "input_quantized_idxs", [])
    output_quantized_idxs: List[int] = prepare_custom_config_dict.get(
        "output_quantized_idxs", [])

    for node in list(model.graph.nodes):
        if node.op == 'placeholder':
            cur_placeholder_node_idx = placeholder_node_seen_cnt
            placeholder_node_seen_cnt += 1
            if cur_placeholder_node_idx in input_quantized_idxs:
                # Inputs are assumed to be quantized if the user specifid the
                # input_quantized_idxs override.
                # TODO: remove the quantize node for the placeholder
                raise Exception("input_quantized_idxs is not supported yet")
        elif node.op == "output":
            cur_output_node_idx = output_node_seen_cnt
            output_node_seen_cnt += 1
            if cur_output_node_idx in output_quantized_idxs:
                # Result are kept quantized if the user specified the
                # output_quantized_idxs override.
                # TODO: remove dequantize node if any
                raise Exception("output_quantized_idxs is not supported yet")
        elif node.op == "call_module":
            if is_activation_post_process(modules[node.target]):
                replace_observer_with_quantize_dequantize_node(model.graph, node, modules)
            elif type(modules[node.target]) in [
                    torch.nn.Linear,
                    torch.nn.Conv1d,
                    torch.nn.Conv2d,
                    torch.nn.Conv3d]:
                fmodule = modules[node.target]
                qconfig = fmodule.qconfig

                is_observed = node.name in observed_node_names
                is_weight_quantized = weight_is_statically_quantized(qconfig)
                # TODO: rename weight_is_statically_quantized to weight_is_int8_quantized
                if qconfig is not None and is_observed and is_weight_quantized:
                    weight_post_process = qconfig.weight()
                    # run weight observer
                    weight_post_process(fmodule.weight)  # type: ignore[operator]
                    weight_qparams = get_qparam_dict(weight_post_process)
                    ref_qmodule_cls = get_static_quant_module_class(type(fmodule), is_reference=True)
                    ref_qmodule = ref_qmodule_cls.from_float(fmodule, weight_qparams)
                    parent_name, name = _parent_name(node.target)
                    setattr(modules[parent_name], name, ref_qmodule)

    # removes qconfig and activation_post_process modules
    if _remove_qconfig_flag:
        _remove_qconfig(model)
    preserved_attributes = set(convert_custom_config_dict.get("preserved_attributes", []))
    model = QuantizedGraphModule(model, model.graph, preserved_attributes)
    return model
