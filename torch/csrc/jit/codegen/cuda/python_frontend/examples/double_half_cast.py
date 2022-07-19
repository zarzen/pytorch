import torch

from torch._C._nvfuser import Fusion, FusionDefinition, DataType

# Construct and Define Fusion
fusion = Fusion()

with FusionDefinition(fusion) as fd :
    t0 = fd.define_tensor(2, DataType.Double)
    t1 = fd.define_tensor(2, DataType.Double)

    fd.add_input(t0)
    fd.add_input(t1)

    t0h = fd.Ops.cast(DataType.Half, t0)
    t1h = fd.Ops.cast(DataType.Half, t1)
    t2 = fd.Ops.add(t0h, t1h)
    t3 = fd.Ops.relu(t2)

    fd.add_output(t3)

fusion.print_ir()

# Execute Fusion
input1 = torch.ones(2, 4, device='cuda', dtype=torch.float64)
input2 = torch.ones(2, 4, device='cuda', dtype=torch.float64)

# Kernel compilation should be cached for the 2nd iteration
# with input tensors of the same shape
for _ in range(5) :
    outputs = fusion.execute([input1, input2])

print(outputs[0])
