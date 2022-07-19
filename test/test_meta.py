# Owner(s): ["module: primTorch"]

import torch
import os
from enum import Enum
from torch.overrides import resolve_name
from torch.utils._pytree import tree_map, tree_flatten
import torch.utils._python_dispatch
from torch._prims.utils import is_complex_dtype, corresponding_real_dtype
from torch.testing._internal.common_utils import (
    TestCase,
    skipIfCrossRef,
    suppress_warnings,
    TEST_WITH_ASAN,
    run_tests,
)
from torch.testing._internal.common_device_type import (
    ops,
    instantiate_device_type_tests,
    onlyCUDA,
)
from torch.testing._internal.logging_tensor import no_dispatch
from torch.testing._internal.common_methods_invocations import op_db
import torch._prims as prims

import atexit
import re
from collections import defaultdict
import unittest
import warnings

bf16 = torch.bfloat16
f64 = torch.float64
f32 = torch.float32
f16 = torch.float16
c32 = torch.complex32
c64 = torch.complex64
c128 = torch.complex128
i8 = torch.int8
i16 = torch.int16
i32 = torch.int32
i64 = torch.int64
b8 = torch.bool
u8 = torch.uint8

dtype_abbrs = {
    torch.bfloat16: 'bf16',
    torch.float64: 'f64',
    torch.float32: 'f32',
    torch.float16: 'f16',
    torch.complex32: 'c32',
    torch.complex64: 'c64',
    torch.complex128: 'c128',
    torch.int8: 'i8',
    torch.int16: 'i16',
    torch.int32: 'i32',
    torch.int64: 'i64',
    torch.bool: 'b8',
    torch.uint8: 'u8',
}

def safe_is_leaf(t):
    try:
        return t.is_leaf
    except RuntimeError:
        # inference mode can trigger this
        return False


# This is a class for converting multiple tensors into meta tensors which
# share the same view/storage structure.  The operation model is you allocate
# one of these, and then call it repeatedly on all the tensors you want to
# convert.  It's important to use the same object for tensors you want to
# share storage because this is how we correlate shared storages to the same
# meta storages; similarly, it's important NOT to use the same object for
# unrelated groups of tensors because this class will remember all the
# tensors/storages its seen and therefore leak memory.
class MetaConverter:
    def __init__(self):
        self.storage_memo = {}
        self.tensor_memo = {}
        self.hit = 0
        self.miss = 0

    def successful(self):
        return self.hit > 0 and self.miss == 0

    # NB: doesn't actually return a storage, because meta storage is
    # not supported
    def meta_storage(self, s):
        # NB: TypedStorage is freshly allocated and cannot be used as hash
        # key index.
        if s._cdata not in self.storage_memo:
            self.storage_memo[s._cdata] = torch.empty(s.size(), dtype=s.dtype, device='meta')
        return self.storage_memo[s._cdata]

    # This function assumes that it's possible to do the conversion
    def meta_tensor(self, t):
        if t not in self.tensor_memo:
            with torch.inference_mode(t.is_inference()):
                if t._is_view():
                    # Construct views in two steps: recursively meta-fy their
                    # base, and then create the view off that.  NB: doing it
                    # directly from storage is WRONG because this won't cause
                    # version counters to get shared.
                    assert t._is_view()
                    base = self.meta_tensor(t._base)

                    def is_c_of_r(complex_dtype, real_dtype):
                        return is_complex_dtype(complex_dtype) and \
                            corresponding_real_dtype(complex_dtype) == real_dtype

                    if base.dtype == t.dtype:
                        pass
                    elif is_c_of_r(base.dtype, t.dtype):
                        base = torch.view_as_real(base)
                    elif is_c_of_r(t.dtype, base.dtype):
                        base = torch.view_as_complex(base)
                    else:
                        # This is not guaranteed to succeed.  If it fails, it
                        # means there is another dtype-converting view function
                        # that hasn't been handled here
                        base = base.view(t.dtype)

                    with torch.enable_grad():
                        r = base.as_strided(t.size(), t.stride(), t.storage_offset())
                else:
                    is_leaf = safe_is_leaf(t)
                    # Fake up some autograd history.
                    if t.requires_grad:
                        r = torch.empty((0,), dtype=t.dtype, device='meta', requires_grad=True)
                        if not is_leaf:
                            with torch.enable_grad():
                                # The backward function here will be wrong, but
                                # that's OK; our goal is just to get the metadata
                                # looking as close as possible; we're not going to
                                # actually try to backward() on these produced
                                # metas.  TODO: would be safer to install some
                                # sort of unsupported grad_fn here
                                r = r.clone()
                    else:
                        r = torch.empty((0,), dtype=t.dtype, device='meta')
                    # As long as meta storage is not supported, need to prevent
                    # redispatching on set_(Storage, ...) which will choke with
                    # meta storage
                    s = self.meta_storage(t.storage())
                    with no_dispatch():
                        with torch.no_grad():
                            r.set_(s, t.storage_offset(), t.size(), t.stride())

                torch._C._set_conj(r, t.is_conj())
                torch._C._set_neg(r, t.is_neg())
            self.tensor_memo[t] = r

        return self.tensor_memo[t]

    def __call__(self, t):
        # TODO: zero tensors?  We appear to have eliminated them by
        # excluding complex for now
        if type(t) is torch.Tensor or type(t) is torch.nn.Parameter:
            if any([
                t.is_sparse_csr, t.is_sparse, t.is_mkldnn, t.is_quantized,
                t.is_nested, torch._is_functional_tensor(t),
                # these are supported in meta conversion but the fallbacks
                # don't work
                t.is_neg(), t.is_conj(),
                # conjugate fallback does not support meta tensors
                t.dtype in (torch.complex128, torch.complex64),
            ]):
                # TODO: sparse should support meta
                # NB technically to('meta') does work but our logging
                # instrumentation will see the meta conversions and the
                # tests all break so we just exclude this.  In any case
                # the to conversion isn't really right anyhow.
                self.miss += 1
                return t
            elif any([
                t.device.type in ("lazy", "meta"), t.is_complex(),
                # We need a way to test if a tensor is batched but there
                # is no official APi to do it
                # torch._C._is_batched(t),
            ]):
                # TODO: this stuff should support storage
                # (well, maybe not batched)
                self.hit += 1
                return t.to("meta")
            else:
                self.hit += 1
                r = self.meta_tensor(t)
                if type(t) is torch.nn.Parameter:
                    r = torch.nn.Parameter(r, requires_grad=r.requires_grad)
                return r
        elif torch.overrides.is_tensor_like(t):
            # Blindly converting tensor subclasses to meta can cause
            # unpredictable problems; e.g., FX tests will trace meta
            # tensors into their trace / some subclasses don't correctly
            # support meta.  Trying to YOLO this is more trouble than it's
            # worth.
            self.miss += 1
            return t
        else:
            # non-Tensor types don't count as hit or miss
            return t


class TestMetaConverter(TestCase):
    def assertSameVersionCounter(self, m1, m2):
        # Cannot easily test m1 and m2 have same storage due to
        # lack of Storage bindings.  Use version counter.
        vc = m1._version
        self.assertEqual(m2._version, vc)
        # Doing it this way ensures that we get VC bump even with leaves
        with torch.no_grad():
            m1._base.add_(3)
        self.assertNotEqual(m1._version, vc)
        self.assertEqual(m2._version, m1._version)

    def test_view_of_non_leaf(self):
        x = torch.randn(4, requires_grad=True)
        y = x.neg()
        z1 = y[:]
        z2 = y[:]
        to_meta = MetaConverter()
        m1 = to_meta(z1)
        m2 = to_meta(z2)
        self.assertEqual(m1.shape, z1.shape)
        self.assertTrue(m1._is_view())
        self.assertFalse(m1._base.is_leaf)
        self.assertSameVersionCounter(m1, m2)

    def test_view_of_leaf(self):
        x = torch.randn(4, requires_grad=True)
        z1 = x[:]
        z2 = x[:]
        to_meta = MetaConverter()
        m1 = to_meta(z1)
        m2 = to_meta(z2)
        self.assertEqual(m1.shape, z1.shape)
        self.assertTrue(m1._is_view())
        self.assertTrue(m1._base.is_leaf)
        self.assertSameVersionCounter(m1, m2)

    def test_leaf(self):
        x = torch.randn(4, requires_grad=True)
        to_meta = MetaConverter()
        m = to_meta(x)
        self.assertEqual(m.shape, x.shape)
        self.assertTrue(m.is_leaf)
        self.assertTrue(m.requires_grad)

    def test_non_leaf(self):
        x = torch.randn(4, requires_grad=True)
        y = x.neg()
        to_meta = MetaConverter()
        m = to_meta(y)
        self.assertEqual(m.shape, y.shape)
        self.assertFalse(m.is_leaf)
        self.assertTrue(m.requires_grad)

    def test_requires_grad_false(self):
        x = torch.randn(4, requires_grad=False)
        to_meta = MetaConverter()
        m = to_meta(x)
        self.assertEqual(m.shape, x.shape)
        self.assertFalse(m.requires_grad)

    def test_view_as_real(self):
        x = torch.randn(4, dtype=torch.complex64)
        y = torch.view_as_real(x)
        m = MetaConverter()(y)
        self.assertEqual(m.shape, y.shape)
        self.assertEqual(m.dtype, y.dtype)

    def test_view_as_complex(self):
        x = torch.randn((4, 2), dtype=torch.float32)
        y = torch.view_as_complex(x)
        m = MetaConverter()(y)
        self.assertEqual(m.shape, y.shape)
        self.assertEqual(m.dtype, y.dtype)

    def test_view_dtype(self):
        x = torch.randn(4, dtype=torch.float32)
        y = x.view(dtype=torch.int32)
        m = MetaConverter()(y)
        self.assertEqual(m.shape, y.shape)
        self.assertEqual(m.dtype, y.dtype)

    def test_imag(self):
        x = torch.randn(4, dtype=torch.complex64)
        y = x.imag
        m = MetaConverter()(y)
        self.assertEqual(m.shape, y.shape)
        self.assertEqual(m.dtype, y.dtype)
        self.assertEqual(m.stride(), y.stride())
        self.assertEqual(m.storage_offset(), y.storage_offset())


def assert_ref_meta_equal(test_case, meta_rs, rs, msg_callable):
    flat_meta_rs, _ = tree_flatten(meta_rs)
    flat_rs, _ = tree_flatten(rs)
    test_case.assertEqual(len(flat_meta_rs), len(flat_rs))
    for i, meta_r, r in zip(range(len(flat_rs)), flat_meta_rs, flat_rs):
        def test_assert(cond, msg):
            if not cond:
                raise RuntimeError(f"output {i}: {msg_callable(msg)}")
        if not isinstance(r, torch.Tensor):
            continue
        test_assert(isinstance(meta_r, torch.Tensor), f"but real {i}th result is Tensor")
        test_assert(meta_r.dtype == r.dtype, f"but real dtype was {r.dtype}")
        test_assert(meta_r.shape == r.shape, f"but real shape was {r.shape}")
        # NOTE: this helper is used instead of a direct stride comparison
        # because strides of tensors with no elements and dimensions of
        # length 1 are not computed consistently
        same_strides, _ = prims.utils.check_significant_strides(meta_r, r)
        test_assert(same_strides, f"but real stride was {r.stride()}")
        test_assert(
            meta_r.storage_offset() == r.storage_offset(),
            f"but real storage_offset was {r.storage_offset()}")
        test_assert(meta_r.requires_grad == r.requires_grad, f"but real requires_grad was {r.requires_grad}")
        test_assert(meta_r.is_conj() == r.is_conj(), f"but real is_conj was {r.is_conj()}")
        test_assert(meta_r.is_neg() == r.is_neg(), f"but real is_neg was {r.is_neg()}")


# This environment variable controls whether or not we print expected failure
# lists at the end of a test suite run.  The intended usage looks like this:
#
# 1. Run `PYTORCH_COLLECT_EXPECT=1 python test/test_meta.py` on a CUDA build
#    of PyTorch that has LAPACK/MAGMA installed.  You can filter `-k test_meta`
#    or `-k test_dispatch_meta` to only focus on one or another list
# 2. Given the printed skip/xfail list, add them to the corresponding lists;
#    torch.* entries go in meta_function and aten.* entries go in meta_dispatch.
#    If there are preexisting entries, you need to merge in the entries.
#
# This is somewhat manual but typically you shouldn't need to do this, unless
# you've made a major change (e.g., added a new dtype to PyTorch) and need to
# refresh the lists.  If you want to do it from scratch, just clear out the
# preexisting lists before running.
#
# WARNING: Python dict literals will silently ignore duplicate keys
COLLECT_EXPECT = os.getenv('PYTORCH_COLLECT_EXPECT', '0') == '1'

seen_succeeded = {}
seen_failed = {}
failed_reasons = defaultdict(set)
def print_seen():
    expected_failures = []
    skips = []

    def fmt_dtypes(dtypes):
        r = ', '.join(sorted(dtype_abbrs[d] for d in dtypes))
        return '{' + r + '}'

    for op, failed_dtypes in seen_failed.items():
        ops = resolve_name(op)
        succeeded_dtypes = seen_succeeded.get(op, set())
        expected_failures_dtypes = failed_dtypes - succeeded_dtypes
        skips_dtypes = failed_dtypes & succeeded_dtypes
        reasons = ""
        if failed_reasons[op]:
            reasons = "  # " + ", ".join(sorted(failed_reasons[op]))
        if expected_failures_dtypes:
            expected_failures.append(f"    {ops}: {fmt_dtypes(expected_failures_dtypes)},{reasons}")
        if skips_dtypes:
            skips.append(f"    {ops}: {fmt_dtypes(skips_dtypes)},")
    expected_failures.sort()
    skips.sort()
    nl = '\n'
    print(f"""\
expected_failures = {{
{nl.join(expected_failures)}
}}

skips = {{
{nl.join(skips)}
}}
""")
if COLLECT_EXPECT:
    atexit.register(print_seen)

# Success forces pass; failure forces fail; skip unconditionally skips testing
TestExpect = Enum("TestExpect", ("SUCCESS", "XFAILURE", "SKIP"))

# unlike print produce strides
def verbose_print(e):
    class Lit:
        def __init__(self, s):
            self.s = s

        def __repr__(self):
            return self.s

    def go(t):
        if isinstance(t, torch.Tensor):
            return Lit(f"{t} stride={t.stride()}")
        else:
            return t

    return repr(tree_map(go, e))

def run_meta_crossref(
    test_case,
    test_expect,
    func,
    args,
    kwargs,
    *,
    dtype,
    device_type,
):
    to_meta = MetaConverter()
    do_meta = test_expect is not TestExpect.SKIP

    if do_meta:
        try:
            meta_args = tree_map(to_meta, args)
            meta_kwargs = tree_map(to_meta, kwargs)
        except Exception as e:
            raise RuntimeError(
                f"failed to convert args to meta; "
                f"originally (*{args}, **{kwargs})") from e

    rs = func(*args, **kwargs)

    # TODO: also handle cases where func raise an exception

    # For now, only attempt if we managed to convert all tensor types
    # (if any of them failed, we're in a mixed device situation and
    # this isn't well supported)
    if do_meta and to_meta.successful():
        try:
            # Suppress warnings, this doesn't matter for test_meta.py
            # but it does matter if you want to use this decorator
            # for cross-ref testing, as some tests may be looking at
            # errors
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                meta_rs = func(*meta_args, **meta_kwargs)
        except Exception as e:
            if test_expect is TestExpect.XFAILURE:
                return rs
            seen_failed.setdefault(func, set()).add(dtype)
            if isinstance(e, NotImplementedError):
                m = RE_NOT_IMPLEMENTED_MSG.search(e.args[0])
                if m:
                    failed_reasons[func].add(m.group(1))
            if COLLECT_EXPECT:
                return rs
            raise RuntimeError(f"""\
failed to run: {resolve_name(func)}(
*{verbose_print(meta_args)},
**{verbose_print(meta_kwargs)}
)""") from e
        else:
            try:
                delim = ',\n  '
                assert_ref_meta_equal(test_case, meta_rs, rs, lambda msg: f"""\
meta disagrees with real impl:
{resolve_name(func)}(
  {delim.join(map(verbose_print, meta_args))},
  {delim.join(k + ": " + verbose_print(v) for k, v in meta_kwargs.items())}
) = (
  {verbose_print(meta_rs)}
)
{msg}
""")
            except Exception:
                if test_expect is TestExpect.XFAILURE:
                    return rs
                seen_failed.setdefault(func, set()).add(dtype)
                if COLLECT_EXPECT:
                    return rs
                raise
            else:
                seen_succeeded.setdefault(func, set()).add(dtype)
                if test_expect is TestExpect.XFAILURE and not COLLECT_EXPECT:
                    raise RuntimeError(f"unexpected success {resolve_name(func)}")

    return rs



RE_NOT_IMPLEMENTED_MSG = re.compile(r"Could not run '([^']+)' with arguments ")


meta_function_expected_failures = {
    torch.Tensor.item: {b8, bf16, c128, c64, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::_local_scalar_dense
    torch.Tensor.to_sparse: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::to_sparse, aten::to_sparse.sparse_dim
    torch.addbmm: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::addbmm, aten::addbmm.out
    torch.allclose: {bf16, f16, f32, f64},  # aten::_local_scalar_dense
    torch.angle: {c32, b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::angle, aten::angle.out
    torch.argwhere: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::nonzero
    torch.bincount: {i16, i32, i64, i8, u8},  # aten::bincount
    torch.bucketize: {bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::bucketize.Tensor, aten::bucketize.Tensor_out
    torch.combinations: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::masked_select
    torch.complex: {f16, f32, f64},  # aten::complex.out
    torch.conj_physical: {c32},  # aten::conj_physical.out
    torch.corrcoef: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::_local_scalar_dense
    torch.count_nonzero: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::count_nonzero.dim_IntList
    torch.cov: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::_local_scalar_dense
    torch.diag: {bf16, b8, f32, f64, i16, i32, i64, i8, u8},  # aten::diag.out
    torch.diagflat: {bf16, b8, f32, f64, i16, i32, i64, i8, u8},  # aten::diag.out
    torch.fft.fft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.fft: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.fftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.fftshift: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::roll
    torch.fft.hfft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.hfft: {b8, f32, f64, i16, i32, i64, i8, u8},
    torch.fft.hfftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.ifft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.ifft: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.ifftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2c
    torch.fft.ifftshift: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::roll
    torch.fft.ihfft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.ihfft: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.ihfftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.irfft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.irfft: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.irfftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.rfft2: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.rfft: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.fft.rfftn: {b8, f32, f64, i16, i32, i64, i8, u8},  # aten::_fft_r2c
    torch.floor_divide: {bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::floor_divide, aten::floor_divide.out
    torch.frexp: {bf16, f16, f32, f64},  # aten::frexp.Tensor_out
    torch.functional.istft: {f32, f64},  # aten::view_as_complex
    torch.functional.stft: {f32, f64},  # aten::_fft_r2c
    torch.functional.unique: {b8, bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::_unique2, aten::unique_dim
    torch.functional.unique_consecutive: {b8, bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::unique_consecutive
    torch.histc: {bf16, f32, f64},  # aten::histc, aten::histc.out
    torch.histogram: {f32, f64},  # aten::histogram.bin_ct, aten::histogram.bins_tensor
    torch.histogramdd: {f32, f64},  # aten::_histogramdd_bin_edges, aten::_histogramdd_from_bin_tensors
    torch.kthvalue: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::kthvalue.values
    torch.linalg.qr: {f32, f64},  # aten::_linalg_qr_helper
    torch.logcumsumexp: {bf16, f32, f64},  # aten::_logcumsumexp, aten::_logcumsumexp.out
    torch.masked_select: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::masked_select, aten::masked_select.out
    torch.matrix_exp: {bf16, f32, f64},  # aten::linalg_matrix_exp
    torch.median: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::median, aten::median.dim_values
    torch.mode: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::mode
    torch.multinomial: {bf16, f32, f64},  # aten::multinomial, aten::multinomial.out
    torch.mvlgamma: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::_local_scalar_dense, aten::mvlgamma.out
    torch.nan_to_num: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::nan_to_num.out
    torch.nanmean: {bf16, f16, f32, f64},
    torch.nanmedian: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::nanmedian, aten::nanmedian.dim_values
    torch.nanquantile: {f32, f64},
    torch.nansum: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::nansum, aten::nansum.out
    torch.nn.functional.conv1d: {bf16, f32, f64, i64},
    torch.nn.functional.conv2d: {bf16, f32, f64, i64},
    torch.nn.functional.conv_transpose1d: {f32, f64, i64},
    torch.nn.functional.conv_transpose2d: {f32, f64, i64},
    torch.nn.functional.conv_transpose3d: {f32, f64, i64},
    torch.nn.functional.ctc_loss: {f32, f64},
    torch.nn.functional.embedding_bag: {f16, f32, f64},  # aten::_embedding_bag_forward_only
    torch.nn.functional.gaussian_nll_loss: {bf16, f32, f64},  # aten::_local_scalar_dense
    torch.nn.functional.grid_sample: {f32, f64},  # aten::grid_sampler_2d, aten::grid_sampler_3d
    torch.nn.functional.layer_norm: {bf16, f32, f64},
    torch.nn.functional.max_pool3d: {f32, f64},  # aten::max_pool3d_with_indices
    torch.nn.functional.max_pool3d_with_indices: {f32, f64},  # aten::max_pool3d_with_indices
    torch.nn.functional.max_unpool1d: {f32, f64},  # aten::max_unpool2d
    torch.nn.functional.max_unpool2d: {f32, f64},  # aten::max_unpool2d
    torch.nn.functional.max_unpool3d: {f32, f64},  # aten::max_unpool3d
    torch.nn.functional.multi_margin_loss: {f32, f64},  # aten::multi_margin_loss
    torch.nn.functional.multilabel_margin_loss: {f32, f64},  # aten::multilabel_margin_loss_forward
    torch.nn.functional.one_hot: {i64},  # aten::_local_scalar_dense
    torch.nn.functional.pdist: {f32, f64},  # aten::_pdist_forward
    torch.nn.functional.prelu: {bf16, f32, f64},  # aten::prelu
    torch.nn.functional.relu: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::relu
    torch.nn.functional.rrelu: {bf16, f32, f64},  # aten::rrelu_with_noise
    torch.nn.functional.unfold: {bf16, f16, f32, f64},  # aten::im2col
    torch.nonzero: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::nonzero, aten::nonzero.out
    torch.polar: {f32, f64},  # aten::polar.out
    torch.repeat_interleave: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::repeat_interleave.Tensor
    torch.roll: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::roll
    torch.searchsorted: {bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::searchsorted.Tensor, aten::searchsorted.Tensor_out
    torch.symeig: {f32, f64},
    torch.std_mean: {bf16, f16, f32, f64},  # aten::std_mean.correction
    torch.take: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},  # aten::take, aten::take.out
    torch.trace: {f32, f64, i16, i32, i64, i8, u8},  # aten::trace
    torch.vdot: {bf16, f32, f64, i16, i32, i64, i8, u8},  # aten::vdot
    torch.qr: {f32, f64},
    torch.ormqr: {f32, f64},
    torch.lu_solve: {f32, f64},
    torch.cholesky: {f32, f64},  # aten::cholesky, aten::cholesky.out
    torch.cholesky_inverse: {f32, f64},  # aten::cholesky_inverse, aten::cholesky_inverse.out
    torch.cholesky_solve: {f32, f64},  # aten::_cholesky_solve_helper
    torch.eig: {f32, f64},  # aten::_local_scalar_dense
    torch.geqrf: {f32, f64},  # aten::geqrf
    torch.linalg.cholesky: {f32, f64},  # aten::linalg_cholesky_ex, aten::linalg_cholesky_ex.L
    torch.linalg.cholesky_ex: {f32, f64},  # aten::linalg_cholesky_ex
    torch.linalg.det: {f32, f64},  # aten::_det_lu_based_helper
    torch.linalg.eig: {f32, f64},  # aten::linalg_eig
    torch.linalg.eigh: {f32, f64},
    torch.linalg.eigvals: {f32, f64},
    torch.linalg.eigvalsh: {f32, f64},  # aten::linalg_eigvalsh.out
    torch.linalg.householder_product: {f32, f64},  # aten::linalg_householder_product
    torch.linalg.inv: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.ldl_factor: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.lstsq: {f32, f64},  # aten::linalg_lstsq.out
    torch.linalg.lu_factor: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.slogdet: {f32, f64},  # aten::linalg_slogdet
    torch.linalg.solve: {f32, f64},  # aten::linalg_solve, aten::linalg_solve.out
    torch.linalg.solve_triangular: {f32, f64},  # aten::linalg_solve_triangular
    torch.linalg.tensorinv: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.tensorsolve: {f32, f64},  # aten::linalg_solve
    torch.logdet: {f32, f64},  # aten::_local_scalar_dense, aten::nonzero
}

"""
# This is some sample code for how we could dump these dicts into YAML
# file for easier reading/writing
import yaml
print(yaml.dump(
  {resolve_name(k): [dtype_abbrs[d] for d in v]
   for k, v in meta_function_expected_failures.items()}, default_flow_style=None))
import sys
sys.exit()
"""

meta_function_skips = {
    torch.Tensor.__getitem__: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8, c32},
    torch.addr: {b8},
    torch.aminmax: {b8, f32, f64, i16, i32, i64, i8, u8},
    torch.conj_physical: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},
    torch.cummax: {b8, bf16, f32, f64, i16, i32, i64, i8, u8},
    torch.cummin: {b8, bf16, f32, f64, i16, i32, i64, i8, u8},
    torch.diff: {b8},
    torch.functional.cdist: {f32, f64},
    torch.functional.tensordot: {bf16, f32, f64, i16, i32, i64, i8, u8},
    torch.index_add: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},
    torch.inner: {bf16, f32, f64, i16, i32, i64, i8, u8},
    torch.logical_not: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},
    torch.logical_xor: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},
    torch.logit: {b8, bf16, f32, f64, i16, i32, i64, i8, u8},
    torch.nn.functional.cross_entropy: {bf16, f32, f64},
    torch.nn.functional.interpolate: {bf16, f32, f64, u8},
    # BEGIN TODO
    torch.nn.functional.nll_loss: {bf16, f32, f64},
    torch.prod: {b8, f32, f64, i16, i32, i64, i8, u8},
    torch.tensor_split: {b8, bf16, f16, f32, f64, i16, i32, i64, i8, u8},
    # END TODO
    torch.inverse: {f32, f64},
    torch.linalg.matrix_power: {f32, f64},
    torch.linalg.matrix_rank: {f32, f64},
    torch.linalg.pinv: {f32, f64},
    torch.empty: {b8, bf16, c128, c64, c32, f16, f32, f64, i16, i32, i64, i8, u8},
}

meta_function_device_expected_failures = defaultdict(dict)
meta_function_device_skips = defaultdict(dict)

meta_function_device_expected_failures['cpu'] = {
}

meta_function_device_expected_failures['cuda'] = {
    torch.addbmm: {f16},  # aten::addbmm, aten::addbmm.out
    torch.corrcoef: {bf16, f16},  # aten::_local_scalar_dense
    torch.cov: {f16},  # aten::_local_scalar_dense
    torch.diag: {bf16, f16},  # aten::diag.out
    torch.diagflat: {bf16, f16},  # aten::diag.out
    torch.fft.fft2: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.fft: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.fftn: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.hfft2: {c32, f16},  # aten::_fft_c2c
    torch.fft.hfft: {c32, f16},
    torch.fft.hfftn: {c32, f16},  # aten::_fft_c2c
    torch.fft.ifft2: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.ifft: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.ifftn: {c32, f16},  # aten::_fft_c2c, aten::_fft_c2c.out
    torch.fft.ihfft2: {f16},
    torch.fft.ihfft: {f16},
    torch.fft.ihfftn: {f16},
    torch.fft.irfft2: {c32, f16},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.irfft: {c32, f16},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.irfftn: {c32, f16},  # aten::_fft_c2r, aten::_fft_c2r.out
    torch.fft.rfft2: {f16},
    torch.fft.rfft: {f16},
    torch.fft.rfftn: {f16},
    torch.functional.unique: {f16},  # aten::_unique2, aten::unique_dim
    torch.functional.unique_consecutive: {f16},  # aten::unique_consecutive
    torch.geqrf: {f32, f64},  # aten::geqrf
    torch.histc: {i16, i32, i64, i8},  # aten::histc, aten::histc.out
    torch.kthvalue: {f16},  # aten::kthvalue.values
    torch.linalg.cholesky: {f32, f64},  # aten::linalg_cholesky_ex, aten::linalg_cholesky_ex.L
    torch.linalg.cholesky_ex: {f32, f64},  # aten::linalg_cholesky_ex
    torch.linalg.householder_product: {f32, f64},  # aten::linalg_householder_product, aten::linalg_householder_product.out
    torch.linalg.inv: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.ldl_factor: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.lu_factor: {f32, f64},  # aten::_local_scalar_dense
    torch.linalg.solve_triangular: {f32, f64},  # aten::linalg_solve_triangular, aten::linalg_solve_triangular.out
    torch.linalg.tensorinv: {f32, f64},  # aten::_local_scalar_dense
    torch.logcumsumexp: {bf16, f16},  # aten::_logcumsumexp, aten::_logcumsumexp.out
    torch.matrix_exp: {f16},  # aten::linalg_matrix_exp
    torch.median: {f16},  # aten::median, aten::median.dim_values
    torch.multinomial: {f16},  # aten::multinomial, aten::multinomial.out
    torch.mvlgamma: {f16},  # aten::_local_scalar_dense, aten::mvlgamma.out
    torch.nanmedian: {f16},  # aten::nanmedian, aten::nanmedian.dim_values
    torch.nn.functional.conv1d: {f16},
    torch.nn.functional.conv2d: {f16},
    torch.nn.functional.conv_transpose1d: {bf16, f16},
    torch.nn.functional.conv_transpose2d: {bf16, f16},
    torch.nn.functional.conv_transpose3d: {bf16, f16},
    torch.nn.functional.embedding_bag: {bf16},  # aten::_embedding_bag_forward_only
    torch.nn.functional.gaussian_nll_loss: {f16},  # aten::_local_scalar_dense
    torch.nn.functional.grid_sample: {f16},  # aten::grid_sampler_2d, aten::grid_sampler_3d
    torch.nn.functional.layer_norm: {f16},
    torch.nn.functional.max_pool3d: {bf16, f16},  # aten::max_pool3d_with_indices
    torch.nn.functional.max_pool3d_with_indices: {bf16, f16},  # aten::max_pool3d_with_indices
    torch.nn.functional.max_unpool1d: {f16},  # aten::max_unpool2d
    torch.nn.functional.max_unpool2d: {f16},  # aten::max_unpool2d
    torch.nn.functional.max_unpool3d: {f16},  # aten::max_unpool3d
    torch.nn.functional.multi_margin_loss: {bf16, f16},  # aten::multi_margin_loss
    torch.nn.functional.multilabel_margin_loss: {bf16, f16},  # aten::multilabel_margin_loss_forward
    torch.nn.functional.prelu: {f16},  # aten::prelu
    torch.nn.functional.relu: {f16},  # aten::relu
    torch.nn.functional.rrelu: {f16},  # aten::rrelu_with_noise
    torch.ormqr: {f32, f64},  # aten::ormqr, aten::ormqr.out
    torch.qr: {f32, f64},  # aten::_linalg_qr_helper
    torch.trace: {b8, bf16, f16},  # aten::diag.out
    torch.vdot: {f16},  # aten::vdot
}

meta_function_device_skips['cuda'] = {
    torch.Tensor.__getitem__: {c32},
    torch.cummax: {f16},
    torch.cummin: {f16},
    torch.functional.tensordot: {f16},
    torch.inner: {f16},
    torch.inverse: {f32, f64},
    torch.linalg.matrix_power: {f32, f64},
    torch.linalg.matrix_rank: {f32, f64},
    torch.linalg.svd: {f32, f64},
    torch.logit: {f16},
    torch.nn.functional.cross_entropy: {f16},
    torch.nn.functional.interpolate: {f16},
    torch.nn.functional.nll_loss: {f16},
    torch.prod: {bf16, c32, f16},
    torch.svd: {f32, f64},
}

# This is a __torch_function__ mode that, when enabled, interposes every
# Torch API call and runs the operator as normal, and then reruns it
# with meta inputs, and then checks that everything about the output agrees.
# Most of the logic deals with faithfully replicating the original tensor
# as a meta tensor, which is nontrivial because there are a lot of subsystems
# that may potentially be exercised.
#
# That being said, this class is a little overkill for what it is doing in
# this test file (since I could have just inlined __torch_function__ on the
# OpInfo call, and OpInfos generally have very regular inputs), but it will be
# useful for more comprehensive testing e.g., as seen in
# https://github.com/pytorch/pytorch/pull/75994  The big benefit is it is
# A LOT more efficient that torch dispatch mode (at the cost of less coverage)
class MetaCrossRefFunctionMode(torch.overrides.TorchFunctionMode):
    test_case: TestCase
    device_type: str
    dtype: torch.dtype

    def __init__(self, test_case, *, device, dtype):
        self.test_case = test_case
        self.device_type = torch.device(device).type
        self.dtype = dtype

    def __torch_function__(self, func, types, args=(), kwargs=None):
        kwargs = kwargs or {}

        if torch.jit.is_tracing() or isinstance(func, torch.ScriptMethod):
            return func(*args, **kwargs)

        if self.dtype in meta_function_skips.get(func, set()):
            test_expect = TestExpect.SKIP
        elif self.dtype in meta_function_device_skips[self.device_type].get(func, set()):
            test_expect = TestExpect.SKIP
        elif self.dtype in meta_function_expected_failures.get(func, set()):
            test_expect = TestExpect.XFAILURE
        elif self.dtype in meta_function_device_expected_failures[self.device_type].get(func, set()):
            test_expect = TestExpect.XFAILURE
        else:
            test_expect = TestExpect.SUCCESS

        return run_meta_crossref(
            self.test_case, test_expect, func, args,
            kwargs, dtype=self.dtype, device_type=self.device_type
        )

aten = torch.ops.aten

# these always fail
meta_dispatch_expected_failures = {
    aten._cdist_forward.default: {f64, f32},
    aten._conj_physical.default: {c32},
    aten._convolution.default: {c64, i64, f64, c128, bf16, f32},
    aten._ctc_loss.default: {f64, f32},
    aten._embedding_bag_forward_only.default: {f16, f64, f32},
    aten._fft_r2c.default: {i64, u8, b8, f32, i8, f64, i16, i32},
    aten._histogramdd_bin_edges.default: {f64, f32},
    aten._histogramdd_from_bin_cts.default: {f64, f32},
    aten._histogramdd_from_bin_tensors.default: {f64, f32},
    aten._local_scalar_dense.default: {c64, i64, c128, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten._pdist_forward.default: {f64, f32},
    aten._unique2.default: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.addbmm.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.addbmm.out: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.angle.default: {c32, i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.angle.out: {c32, i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.bincount.default: {i8, i64, i16, u8, i32},
    aten.bucketize.Tensor: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.bucketize.Tensor_out: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.col2im.default: {c64, f32, f64, c128},
    aten.complex.default: {c64, f64, c128, f16, f32},
    aten.complex.out: {f16},
    aten.conj_physical.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, c32, i32},
    aten.convolution.default: {c64, i64, f64, c128, bf16, f32},
    aten.count_nonzero.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.count_nonzero.dim_IntList: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.diag.default: {i64, u8, b8, f32, i8, f64, i16, i32, bf16},
    aten.diag.out: {bf16, i64, u8, b8, f32, i8, f64, i16, i32},
    aten.floor_divide.default: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.floor_divide.out: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.frexp.Tensor: {bf16, f16, f64, f32},
    aten.grid_sampler_2d.default: {f64, f32},
    aten.grid_sampler_3d.default: {f64, f32},
    aten.histc.default: {bf16, f64, f32},
    aten.histc.out: {bf16, f64, f32},
    aten.histogram.bin_ct: {f64, f32},
    aten.histogram.bins_tensor: {f64, f32},
    aten.im2col.default: {bf16, f16, f64, f32},
    aten.index.Tensor: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32, c32},
    aten.kthvalue.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.linalg_matrix_exp.default: {bf16, f64, f32},
    aten.log_sigmoid_forward.output: {bf16, f64, f32},
    aten.logcumsumexp.default: {bf16, f64, f32},
    aten.logcumsumexp.out: {bf16, f64, f32},
    aten.logical_not.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.logical_not_.default: {bf16, f16, f64, f32},
    aten.logical_xor.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.logit.out: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.masked_select.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.masked_select.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.max_pool3d_with_indices.default: {f64, f32},
    aten.max_unpool2d.default: {f64, f32},
    aten.max_unpool3d.default: {f64, f32},
    aten.median.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.median.dim: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.mode.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.multi_margin_loss.default: {f64, f32},
    aten.multilabel_margin_loss_forward.default: {f64, f32},
    aten.multinomial.default: {bf16, f64, f32},
    aten.multinomial.out: {bf16, f64, f32},
    aten.mvlgamma.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.mvlgamma.out: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.nan_to_num.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.nan_to_num.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.nanmedian.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.nanmedian.dim: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.nansum.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.nansum.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.nll_loss2d_forward.default: {bf16, f64, f32},
    aten.nonzero.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.nonzero.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.polar.default: {f64, f32},
    aten.prelu.default: {bf16, f64, f32},
    aten.prod.default: {i64, u8, b8, f32, i8, f64, i16, i32},
    aten.relu.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.repeat_interleave.Tensor: {c64, i64, c128, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.roll.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.rrelu_with_noise.default: {bf16, f64, f32},
    aten.searchsorted.Tensor: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.searchsorted.Tensor_out: {i64, bf16, f16, u8, f32, i8, f64, i16, i32},
    aten.std_mean.correction: {bf16, f16, f64, f32},
    aten.take.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.take.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.tensordot.out: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.to_sparse.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.to_sparse.sparse_dim: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.trace.default: {i8, i64, f64, i16, u8, i32, f32},
    aten.unique_consecutive.default: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.unique_dim.default: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.upsample_nearest3d.vec: {bf16, u8, f64, f32},
    aten.vdot.default: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten.vdot.out: {i64, bf16, u8, f32, i8, f64, i16, i32},
    aten._det_lu_based_helper.default: {f32, f64},  # aten::_det_lu_based_helper
    aten._linalg_check_errors.default: {c128, c64, f32, f64},  # aten::_local_scalar_dense
    aten.cholesky.default: {f32, f64},  # aten::cholesky
    aten.cholesky.out: {f32, f64},  # aten::cholesky.out
    aten.cholesky_inverse.default: {f32, f64},  # aten::cholesky_inverse
    aten.cholesky_inverse.out: {f32, f64},  # aten::cholesky_inverse.out
    aten.cholesky_solve.default: {f32, f64},  # aten::_cholesky_solve_helper
    aten.cholesky_solve.out: {f32, f64},  # aten::_cholesky_solve_helper
    aten.eig.default: {f32, f64},  # aten::_local_scalar_dense
    aten.geqrf.default: {f32, f64},  # aten::geqrf
    aten.inverse.out: {f32, f64},  # aten::_local_scalar_dense
    aten.linalg_cholesky_ex.L: {f32, f64},  # aten::linalg_cholesky_ex.L
    aten.linalg_cholesky_ex.default: {f32, f64},  # aten::linalg_cholesky_ex
    aten.linalg_eig.default: {f32, f64},  # aten::linalg_eig
    aten.linalg_eigh.default: {f32, f64},
    aten.linalg_eigvalsh.out: {f32, f64},  # aten::linalg_eigvalsh.out
    aten.linalg_householder_product.default: {f32, f64},  # aten::linalg_householder_product
    aten.linalg_householder_product.out: {f32, f64},  # aten::linalg_householder_product.out
    aten.linalg_lstsq.default: {f32, f64},  # aten::linalg_lstsq.out
    aten.linalg_qr.default: {f32, f64},  # aten::_linalg_qr_helper
    aten.linalg_slogdet.default: {f32, f64},  # aten::linalg_slogdet
    aten.linalg_solve.default: {f32, f64},  # aten::linalg_solve
    aten.linalg_solve.out: {f32, f64},  # aten::linalg_solve.out
    aten.linalg_solve_triangular.default: {f32, f64},  # aten::linalg_solve_triangular
    aten.linalg_solve_triangular.out: {f32, f64},  # aten::linalg_solve_triangular.out
    aten.logdet.default: {f32, f64},  # aten::_local_scalar_dense, aten::nonzero
    aten.lu_solve.default: {f32, f64},  # aten::lu_solve
    aten.lu_solve.out: {f32, f64},  # aten::lu_solve.out
    aten.ormqr.default: {f32, f64},  # aten::ormqr
    aten.ormqr.out: {f32, f64},  # aten::ormqr.out
    aten.symeig.default: {f32, f64},  # aten::_symeig_helper
}

# these sometimes pass and sometimes fail
meta_dispatch_skips = {
    aten._to_copy.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},
    aten.addr.default: {b8},
    aten.addr.out: {b8},
    aten.aminmax.default: {i64, u8, b8, f32, i8, f64, i16, i32},
    aten.copy_.default: {c32},
    aten.cummax.default: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.cummin.default: {i64, bf16, u8, b8, f32, i8, f64, i16, i32},
    aten.index_add.default: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},  # TODO
    aten.index_add.out: {i64, bf16, f16, u8, b8, f32, i8, f64, i16, i32},  # TODO
    aten.isnan.default: {f64, f32},
    aten.native_batch_norm.default: {f64, f32},  # waiting on https://github.com/pytorch/pytorch/pull/77407
    aten.native_layer_norm.default: {bf16, f64, f32},
    aten.mul.Scalar: {i64, bf16, f16, f32, i8, f64, i16, i32},  # test_dispatch_meta_gradient_cpu_bfloat16
    aten.slice.Tensor: {c32},  # TODO
    aten.linalg_pinv.atol_rtol_tensor: {f32, f64},
    aten.linalg_pinv.atol_rtol_tensor_out: {f32, f64},
    aten.empty.memory_format: {b8, bf16, c128, c64, c32, f16, f32, f64, i16, i32, i64, i8, u8},
}

meta_dispatch_device_expected_failures = defaultdict(dict)
meta_dispatch_device_skips = defaultdict(dict)

meta_dispatch_device_expected_failures['cuda'] = {
    aten._conj_physical.default: {f16},  # aten::conj_physical.out
    aten._convolution.default: {f16},
    aten._embedding_bag_forward_only.default: {bf16},  # aten::_embedding_bag_forward_only
    aten._fft_c2c.default: {c32, f16},  # aten::_fft_c2c
    aten._fft_c2c.out: {c32, f16},  # aten::_fft_c2c.out
    aten._fft_c2r.default: {c32, f16},  # aten::_fft_c2r
    aten._fft_c2r.out: {c32, f16},  # aten::_fft_c2r.out
    aten._fft_r2c.default: {f16},  # aten::_fft_r2c
    aten._fft_r2c.out: {f16},  # aten::_fft_r2c.out
    aten._linalg_check_errors.default: {c128, c64, f32, f64},  # aten::_local_scalar_dense
    aten._unique2.default: {f16},  # aten::_unique2
    aten._use_cudnn_ctc_loss.default: {f32, f64},  # aten::_use_cudnn_ctc_loss
    aten.addbmm.default: {f16},  # aten::addbmm
    aten.addbmm.out: {f16},  # aten::addbmm.out
    aten.convolution.default: {f16},
    aten.cudnn_grid_sampler.default: {f16, f32, f64},  # aten::cudnn_grid_sampler
    aten.diag.default: {f16},  # aten::diag.out
    aten.diag.out: {bf16, f16},  # aten::diag.out
    aten.geqrf.default: {f32, f64},  # aten::geqrf
    aten.grid_sampler_2d.default: {f16},  # aten::grid_sampler_2d
    aten.grid_sampler_3d.default: {f16},  # aten::grid_sampler_3d
    aten.histc.default: {i16, i32, i64, i8},  # aten::histc
    aten.histc.out: {i16, i32, i64, i8},  # aten::histc.out
    aten.index.Tensor: {c32},  # aten::index.Tensor
    aten.inverse.out: {f32, f64},  # aten::_local_scalar_dense
    aten.kthvalue.default: {f16},  # aten::kthvalue.values
    aten.linalg_cholesky_ex.L: {f32, f64},  # aten::linalg_cholesky_ex.L
    aten.linalg_cholesky_ex.default: {f32, f64},  # aten::linalg_cholesky_ex
    aten.linalg_eigvalsh.out: {f32, f64},  # aten::linalg_eigvalsh.out
    aten.linalg_householder_product.default: {f32, f64},  # aten::linalg_householder_product
    aten.linalg_householder_product.out: {f32, f64},  # aten::linalg_householder_product.out
    aten.linalg_matrix_exp.default: {f16},  # aten::linalg_matrix_exp
    aten.linalg_qr.default: {f32, f64},  # aten::_linalg_qr_helper
    aten.linalg_solve_triangular.default: {f32, f64},  # aten::linalg_solve_triangular
    aten.linalg_solve_triangular.out: {f32, f64},  # aten::linalg_solve_triangular.out
    aten.log_sigmoid_forward.default: {bf16, f16, f64, f32},
    aten.log_sigmoid_forward.output: {f16},  # aten::log_sigmoid_forward.output
    aten.logcumsumexp.default: {bf16, f16},  # aten::_logcumsumexp
    aten.logcumsumexp.out: {bf16, f16},  # aten::_logcumsumexp.out
    aten.logit.out: {f16},
    aten.max_pool3d_with_indices.default: {bf16, f16},  # aten::max_pool3d_with_indices
    aten.max_unpool2d.default: {f16},  # aten::max_unpool2d
    aten.max_unpool3d.default: {f16},  # aten::max_unpool3d
    aten.median.default: {f16},  # aten::median
    aten.median.dim: {f16},  # aten::median.dim_values
    aten.multi_margin_loss.default: {bf16, f16},  # aten::multi_margin_loss
    aten.multilabel_margin_loss_forward.default: {bf16, f16},  # aten::multilabel_margin_loss_forward
    aten.multinomial.default: {f16},  # aten::multinomial
    aten.multinomial.out: {f16},  # aten::multinomial.out
    aten.mvlgamma.default: {f16},  # aten::_local_scalar_dense
    aten.mvlgamma.out: {f16},  # aten::mvlgamma.out
    aten.nanmedian.default: {f16},  # aten::nanmedian
    aten.nanmedian.dim: {f16},  # aten::nanmedian.dim_values
    aten.native_batch_norm.default: {bf16, f16},  # waiting on https://github.com/pytorch/pytorch/pull/77407
    aten.native_dropout.default: {bf16, f16, f32, f64},
    aten.native_layer_norm.default: {f16},  # aten::var_mean.correction
    aten.nll_loss2d_forward.default: {f16},  # aten::nll_loss2d_forward
    aten.ormqr.default: {f32, f64},  # aten::ormqr
    aten.ormqr.out: {f32, f64},  # aten::ormqr.out
    aten.prelu.default: {f16},  # aten::prelu
    aten.prod.default: {bf16, c32, f16},  # aten::prod
    aten.relu.default: {f16},  # aten::relu
    aten.rrelu_with_noise.default: {f16},  # aten::rrelu_with_noise
    aten.tensordot.out: {f16},  # aten::tensordot.out
    aten.trace.default: {b8, bf16, f16},  # aten::diag.out
    aten.unique_consecutive.default: {f16},  # aten::unique_consecutive
    aten.unique_dim.default: {f16},  # aten::unique_dim
    aten.upsample_nearest3d.vec: {f16},  # aten::upsample_nearest3d.vec
    aten.vdot.default: {f16},  # aten::vdot
    aten.vdot.out: {f16},  # aten::vdot
}

meta_dispatch_device_skips['cuda'] = {
    aten._conj.default: {c32, f16},
    aten._linalg_svd.default: {f32, f64},
    aten.cudnn_batch_norm.default: {f32, f64},
    aten.cummax.default: {f16},
    aten.cummin.default: {f16},
    aten.inverse.default: {f32, f64},
    aten.slice.Tensor: {f16},
    # ROCm stuff; technically this should be expected failure but it's
    # not worth it; these should get unified anyway
    aten.miopen_batch_norm.default: {f32},
}

class MetaCrossRefDispatchMode(torch.utils._python_dispatch.TorchDispatchMode):
    test_case: TestCase
    device: torch.device
    dtype: torch.dtype

    def __init__(self, test_case, *, device, dtype):
        self.test_case = test_case
        # save TLS
        self.precision = test_case.precision
        self.rel_tol = test_case.rel_tol
        self.device_type = torch.device(device).type
        self.dtype = dtype

    def __torch_dispatch__(self, func, types, args=(), kwargs=None):
        kwargs = kwargs or {}

        self.test_case.precision = self.precision
        self.test_case.rel_tol = self.rel_tol

        if self.dtype in meta_dispatch_skips.get(func, set()):
            test_expect = TestExpect.SKIP
        elif self.dtype in meta_dispatch_device_skips[self.device_type].get(func, set()):
            test_expect = TestExpect.SKIP
        elif self.dtype in meta_dispatch_expected_failures.get(func, set()):
            test_expect = TestExpect.XFAILURE
        elif self.dtype in meta_dispatch_device_expected_failures[self.device_type].get(func, set()):
            test_expect = TestExpect.XFAILURE
        else:
            test_expect = TestExpect.SUCCESS

        return run_meta_crossref(
            self.test_case,
            test_expect,
            func,
            args,
            kwargs,
            dtype=self.dtype,
            device_type=self.device_type,
        )


# NB: we're running these tests only on CUDA because there are some
# inconsistencies between CUDA and CPU, and running on CUDA makes it easier
# to ignore the CPU case when inconsistencies arise.  Ideally we deal
# with the inconsistencies but this takes time.
class TestMeta(TestCase):
    @unittest.skipIf(TEST_WITH_ASAN, "Skipped under ASAN")
    @onlyCUDA
    @skipIfCrossRef
    @suppress_warnings
    @ops(op_db)
    def test_meta(self, device, dtype, op):
        # run the OpInfo sample inputs, cross-referencing them with the
        # meta implementation and check the results are the same.  All
        # the heavy lifting happens in MetaCrossRefFunctionMode
        func = op.get_op()
        samples = op.sample_inputs(device, dtype, requires_grad=False)
        for sample_input in samples:
            args = [sample_input.input] + list(sample_input.args)
            kwargs = sample_input.kwargs
            with MetaCrossRefFunctionMode.push(self, dtype=dtype, device=device):
                expected = func(*args, **kwargs)
                if isinstance(expected, torch.Tensor) and op.supports_out:
                    func(*args, **kwargs, out=expected)

    @unittest.skipIf(TEST_WITH_ASAN, "Skipped under ASAN")
    @onlyCUDA
    @skipIfCrossRef
    @suppress_warnings
    @ops(op_db)
    def test_dispatch_meta(self, device, dtype, op):
        func = op.get_op()
        samples = op.sample_inputs(device, dtype, requires_grad=False)
        for sample_input in samples:
            args = [sample_input.input] + list(sample_input.args)
            kwargs = sample_input.kwargs
            with MetaCrossRefDispatchMode.push(self, dtype=dtype, device=device):
                expected = func(*args, **kwargs)
                if isinstance(expected, torch.Tensor) and op.supports_out:
                    func(*args, **kwargs, out=expected)

instantiate_device_type_tests(TestMeta, globals())

if __name__ == "__main__":
    run_tests()
