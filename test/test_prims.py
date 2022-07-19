# Owner(s): ["module: primTorch"]

from functools import partial

import torch
from torch.testing import make_tensor
from torch.testing._internal.common_utils import run_tests, TestCase
from torch.testing._internal.common_device_type import (
    instantiate_device_type_tests,
    onlyCUDA,
    skipCUDAIfRocm,
    dtypes,
)
from torch.testing._internal.logging_tensor import LoggingTensor, capture_logs, log_input
import torch._prims as prims
from torch._prims.executor import make_traced


class TestPrims(TestCase):
    @onlyCUDA
    @skipCUDAIfRocm
    @dtypes(torch.float32)
    def test_broadcast_in_dim(self, device, dtype):
        # nvfuser is not currently capable of realizing a broadcasted tensor
        # when the broadcast is the only operation.  Another op is needed.
        def _wrapper(a, b, broadcast_dimensions):
            a_bc = prims.broadcast_in_dim(a, b.shape, broadcast_dimensions)
            return prims.add(a_bc, b)

        traced = make_traced(_wrapper)
        make_arg = partial(make_tensor, device=device, dtype=dtype)

        for executor in ('aten', 'nvfuser'):
            fn = partial(traced, executor=executor)
            # Same shape
            shape = (5, 5)
            a = make_arg(shape)
            b = make_arg(shape, low=0.0, high=0.0)
            result = fn(a, b, (0, 1))

            self.assertEqual(result.shape, a.shape)
            self.assertTrue(result.is_contiguous)
            self.assertEqual(a, result)

            # Error input: reordering dims
            with self.assertRaises(Exception):
                result = fn(a, b, (1, 0))

            # Adding outermost dimensions
            a = make_arg((5, 5))
            b = make_arg((3, 3, 5, 5), low=0.0, high=0.0)
            result = fn(a, b, (2, 3))

            self.assertEqual(result.shape, b.shape)
            self.assertEqual(a.broadcast_to(b.shape), result)

            # Expands
            a = make_arg((1, 5, 1))
            b = make_arg((3, 5, 7), low=0.0, high=0.0)
            result = fn(a, b, (0, 1, 2))

            self.assertEqual(result.shape, b.shape)
            self.assertEqual(a.expand_as(result), result)

            # Unsqueezes
            a = make_arg((1, 2, 3))
            b = make_arg((1, 2, 1, 3), low=0.0, high=0.0)
            result = fn(a, b, (0, 1, 3))

            self.assertEqual(result.shape, b.shape)
            self.assertEqual(a.unsqueeze(2), result)

            # FIXME: This test exposes an issue in nvfuser
            # Adds outermost, expands, and unsqueezes
            """
            a = make_arg((1, 2, 3))
            b = make_arg((4, 1, 7, 2, 3, 3), low=0.0, high=0.0)
            result = fn(a, b, (1, 3, 4))

            self.assertEqual(result.shape, b.shape)
            a.unsqueeze_(3)
            a.unsqueeze_(1)
            a.unsqueeze_(0)
            self.assertEqual(a.expand_as(result), result)
            """


class TestPrimsBasic(TestCase):
    def test_torch_ops(self):
        r = make_tensor((2,), device='cpu', dtype=torch.float)
        self.assertEqual(torch.ops.prims.sin(r), torch.sin(r))

        r = LoggingTensor(r)
        with capture_logs() as logs:
            log_input("input", r)
            prims.sin(r)
        self.assertExpectedInline('\n'.join(logs), """\
$0 = input('input')
$1 = torch._ops.prims.sin.default($0)""")

    def test_mul_complex(self):
        prims.mul(torch.randn(2), 1 + 1j)


instantiate_device_type_tests(TestPrims, globals())

if __name__ == "__main__":
    run_tests()
