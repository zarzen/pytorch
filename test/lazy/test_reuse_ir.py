# Owner(s): ["oncall: jit"]

import torch
import torch._lazy
import torch._lazy.config
import torch._lazy.ir_cache
import torch._lazy.ts_backend
import torch._lazy.metrics as metrics
from torch.testing._internal.common_utils import IS_WINDOWS, run_tests, TestCase
import os
import unittest

torch._lazy.ts_backend.init()
torch._lazy.config.set_reuse_ir(True)

def get_test_device():
    return 'cuda' if 'LTC_TS_CUDA' in os.environ else 'cpu'

@unittest.skipIf(IS_WINDOWS, "To be fixed")
class TestLazyReuseIr(TestCase):
    def testAdd(self):
        device = get_test_device()
        x = torch.randn(2, 3, 4, device=device)
        y = torch.randn(2, 3, 4, device=device)
        z = torch.zeros(2, 3, 4, device=device)

        device = 'lazy'
        x_lazy = x.detach().clone().to(device=device)
        y_lazy = y.detach().clone().to(device=device)
        z_lazy = z.detach().clone().to(device=device)

        for i in range(10):
            z += (x + y)

        for i in range(10):
            z_lazy += (x_lazy + y_lazy)
            torch._lazy.mark_step()

        torch.testing.assert_close(z.cpu(), z_lazy.cpu())
        assert metrics.counter_value("IrNodeReused_torch::lazy::AddTensor") >= 14
        metrics.reset()
        torch._lazy.ir_cache.reset()

    def testAddSub(self):
        device = get_test_device()
        x = torch.randn(2, 3, 4, device=device)
        y = torch.randn(2, 3, 4, device=device)
        z = torch.zeros(2, 3, 4, device=device)

        device = 'lazy'
        x_lazy = x.detach().clone().to(device=device)
        y_lazy = y.detach().clone().to(device=device)
        z_lazy = z.detach().clone().to(device=device)

        for i in range(10):
            if i < 5:
                z += (x + y)
            else:
                z += (x - y)

        for i in range(10):
            if i < 5:
                z_lazy += (x_lazy + y_lazy)
            else:
                z_lazy += (x_lazy - y_lazy)
            torch._lazy.mark_step()

        torch.testing.assert_close(z.cpu(), z_lazy.cpu())
        assert metrics.counter_value("IrNodeReused_torch::lazy::AddTensor") >= 8
        metrics.reset()
        torch._lazy.ir_cache.reset()

    def testAddSubFallback(self):
        torch._lazy.config.set_force_fallback("aten::sub")
        device = get_test_device()
        x = torch.randn(2, 3, 4, device=device)
        y = torch.randn(2, 3, 4, device=device)
        z = torch.zeros(2, 3, 4, device=device)

        device = 'lazy'
        x_lazy = x.detach().clone().to(device=device)
        y_lazy = y.detach().clone().to(device=device)
        z_lazy = z.detach().clone().to(device=device)

        for i in range(10):
            if i < 5:
                z += (x + y)
            else:
                z += (x - y)

        for i in range(10):
            if i < 5:
                z_lazy += (x_lazy + y_lazy)
            else:
                z_lazy += (x_lazy - y_lazy)
            torch._lazy.mark_step()

        torch.testing.assert_close(z.cpu(), z_lazy.cpu())
        assert metrics.counter_value("IrNodeReused_torch::lazy::AddTensor") >= 8
        metrics.reset()
        torch._lazy.ir_cache.reset()
        torch._lazy.config.set_force_fallback("")

if __name__ == '__main__':
    run_tests()
