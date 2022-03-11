# Owner(s): ["oncall: jit"]

from typing import NamedTuple, Optional
import io
import os
import pathlib
import sys

from torch import Tensor
from torch.testing._internal.common_utils import TemporaryFileName
import torch

# Make the helper files in test/ importable
pytorch_test_dir = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.append(pytorch_test_dir)
from torch.testing._internal.jit_utils import (JitTestCase,
                                               clear_class_registry)

if __name__ == "__main__":
    raise RuntimeError(
        "This test file is not meant to be run directly, use:\n\n"
        "\tpython test/test_jit.py TESTNAME\n\n"
        "instead."
    )

class TestSaveLoad(JitTestCase):
    def test_different_modules(self):
        """
        Exercise the situation where we have the same qualified name
        in two different CompilationUnits on save/load.
        """
        class Foo(torch.nn.Module):
            def __init__(self):
                super(Foo, self).__init__()
                self.foo = torch.nn.Linear(2, 2)
                self.bar = torch.nn.Linear(2, 2)

            def forward(self, x):
                x = self.foo(x)
                x = self.bar(x)
                return x

        first_script_module = torch.jit.script(Foo())
        first_saved_module = io.BytesIO()
        torch.jit.save(first_script_module, first_saved_module)
        first_saved_module.seek(0)

        clear_class_registry()

        class Foo(torch.nn.Module):
            def __init__(self):
                super(Foo, self).__init__()
                self.foo = torch.nn.Linear(2, 2)

            def forward(self, x):
                x = self.foo(x)
                return x

        second_script_module = torch.jit.script(Foo())
        second_saved_module = io.BytesIO()
        torch.jit.save(torch.jit.script(Foo()), second_saved_module)
        second_saved_module.seek(0)

        clear_class_registry()

        self.assertEqual(
            first_script_module._c.qualified_name, second_script_module._c.qualified_name
        )

        class ContainsBoth(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.add_module("second", torch.jit.load(second_saved_module))
                self.add_module("first", torch.jit.load(first_saved_module))

            def forward(self, x):
                x = self.first(x)
                x = self.second(x)
                return x

        sm = torch.jit.script(ContainsBoth())
        contains_both = io.BytesIO()
        torch.jit.save(sm, contains_both)
        contains_both.seek(0)
        sm = torch.jit.load(contains_both)

    def test_different_functions(self):
        """
        Exercise the situation where we have the same qualified name
        in two different CompilationUnits on save/load.
        """
        def lol(x):
            return x

        class Foo(torch.nn.Module):
            def forward(self, x):
                return lol(x)

        first_script_module = torch.jit.script(Foo())
        first_saved_module = io.BytesIO()
        torch.jit.save(first_script_module, first_saved_module)
        first_saved_module.seek(0)

        clear_class_registry()

        def lol(x):  # noqa: F811
            return "hello"

        class Foo(torch.nn.Module):
            def forward(self, x):
                return lol(x)

        second_script_module = torch.jit.script(Foo())
        second_saved_module = io.BytesIO()
        torch.jit.save(torch.jit.script(Foo()), second_saved_module)
        second_saved_module.seek(0)

        clear_class_registry()

        self.assertEqual(
            first_script_module._c.qualified_name, second_script_module._c.qualified_name
        )

        class ContainsBoth(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.add_module("second", torch.jit.load(second_saved_module))
                self.add_module("first", torch.jit.load(first_saved_module))

            def forward(self, x):
                x = self.first(x)
                x = self.second(x)
                return x

        sm = torch.jit.script(ContainsBoth())
        contains_both = io.BytesIO()
        torch.jit.save(sm, contains_both)
        contains_both.seek(0)
        sm = torch.jit.load(contains_both)

    def test_different_interfaces(self):
        """
        Exercise the situation where we have the same qualified name
        in two different CompilationUnits on save/load.
        """
        @torch.jit.interface
        class MyInterface(object):
            def bar(self, x: Tensor) -> Tensor:
                pass

        @torch.jit.script
        class ImplementInterface(object):
            def __init__(self):
                pass

            def bar(self, x):
                return x

        class Foo(torch.nn.Module):
            __annotations__ = {"interface": MyInterface}

            def __init__(self):
                super().__init__()
                self.interface = ImplementInterface()

            def forward(self, x):
                return self.interface.bar(x)

        first_script_module = torch.jit.script(Foo())
        first_saved_module = io.BytesIO()
        torch.jit.save(first_script_module, first_saved_module)
        first_saved_module.seek(0)

        clear_class_registry()

        @torch.jit.interface
        class MyInterface(object):
            def not_bar(self, x: Tensor) -> Tensor:
                pass

        @torch.jit.script  # noqa: F811
        class ImplementInterface(object):  # noqa: F811
            def __init__(self):
                pass

            def not_bar(self, x):
                return x

        class Foo(torch.nn.Module):
            __annotations__ = {"interface": MyInterface}

            def __init__(self):
                super().__init__()
                self.interface = ImplementInterface()

            def forward(self, x):
                return self.interface.not_bar(x)

        second_script_module = torch.jit.script(Foo())
        second_saved_module = io.BytesIO()
        torch.jit.save(torch.jit.script(Foo()), second_saved_module)
        second_saved_module.seek(0)

        clear_class_registry()

        self.assertEqual(
            first_script_module._c.qualified_name, second_script_module._c.qualified_name
        )

        class ContainsBoth(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.add_module("second", torch.jit.load(second_saved_module))
                self.add_module("first", torch.jit.load(first_saved_module))

            def forward(self, x):
                x = self.first(x)
                x = self.second(x)
                return x

        sm = torch.jit.script(ContainsBoth())
        contains_both = io.BytesIO()
        torch.jit.save(sm, contains_both)
        contains_both.seek(0)
        sm = torch.jit.load(contains_both)

    def test_many_collisions(self):
        class MyCoolNamedTuple(NamedTuple):
            a: int

        @torch.jit.interface
        class MyInterface(object):
            def bar(self, x: Tensor) -> Tensor:
                pass

        @torch.jit.script
        class ImplementInterface(object):
            def __init__(self):
                pass

            def bar(self, x):
                return x

        def lol(x):
            return x

        class Foo(torch.nn.Module):
            interface: MyInterface

            def __init__(self):
                super().__init__()
                self.foo = torch.nn.Linear(2, 2)
                self.bar = torch.nn.Linear(2, 2)
                self.interface = ImplementInterface()

            def forward(self, x):
                x = self.foo(x)
                x = self.bar(x)
                x = lol(x)
                x = self.interface.bar(x)

                return x, MyCoolNamedTuple(a=5)


        first_script_module = torch.jit.script(Foo())
        first_saved_module = io.BytesIO()
        torch.jit.save(first_script_module, first_saved_module)
        first_saved_module.seek(0)

        clear_class_registry()

        @torch.jit.interface
        class MyInterface(object):
            def not_bar(self, x: Tensor) -> Tensor:
                pass

        @torch.jit.script  # noqa: F811
        class ImplementInterface(object):  # noqa: F811
            def __init__(self):
                pass

            def not_bar(self, x):
                return x

        def lol(x):  # noqa: F811
            return "asdofij"

        class MyCoolNamedTuple(NamedTuple):  # noqa: F811
            a: str

        class Foo(torch.nn.Module):
            interface: MyInterface

            def __init__(self):
                super().__init__()
                self.foo = torch.nn.Linear(2, 2)
                self.interface = ImplementInterface()

            def forward(self, x):
                x = self.foo(x)
                self.interface.not_bar(x)
                x = lol(x)
                return x, MyCoolNamedTuple(a="hello")

        second_script_module = torch.jit.script(Foo())
        second_saved_module = io.BytesIO()
        torch.jit.save(second_script_module, second_saved_module)
        second_saved_module.seek(0)

        clear_class_registry()

        self.assertEqual(
            first_script_module._c.qualified_name, second_script_module._c.qualified_name
        )

        class ContainsBoth(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.add_module("second", torch.jit.load(second_saved_module))
                self.add_module("first", torch.jit.load(first_saved_module))

            def forward(self, x):
                x, named_tuple_1 = self.first(x)
                x, named_tuple_2 = self.second(x)
                return len(x + named_tuple_2.a) + named_tuple_1.a

        sm = torch.jit.script(ContainsBoth())
        contains_both = io.BytesIO()
        torch.jit.save(sm, contains_both)
        contains_both.seek(0)
        sm = torch.jit.load(contains_both)

    def test_save_load_with_extra_files(self):
        class MyMod(torch.jit.ScriptModule):
            @torch.jit.script_method
            def forward(self, a):
                return a

        # specifically test binary data
        value = b"bar\x00\xffbaz"

        expected_extra_files = {}
        expected_extra_files['foo'] = value
        # verify that str to bytes conversion also works
        expected_extra_files['foo2'] = "bar"
        m = MyMod()

        # Save to file.
        with TemporaryFileName() as fname:
            m.save(fname, _extra_files=expected_extra_files)
            # values don't matter
            extra_files = {'foo': '', 'foo2': None}
            torch.jit.load(fname, _extra_files=extra_files)
            self.assertEqual(value, extra_files['foo'])
            # results come back always as bytes
            self.assertEqual(b"bar", extra_files['foo2'])

            # Use torch.jit API
            torch.jit.save(m, fname, _extra_files=expected_extra_files)
            extra_files['foo'] = ''
            torch.jit.load(fname, _extra_files=extra_files)
            self.assertEqual(value, extra_files['foo'])

        # Save to buffer.
        buffer = io.BytesIO(m.save_to_buffer(_extra_files=expected_extra_files))
        extra_files = {'foo': ''}
        torch.jit.load(buffer, _extra_files=extra_files)
        self.assertEqual(value, extra_files['foo'])

        # Use torch.jit API
        buffer = io.BytesIO()
        torch.jit.save(m, buffer, _extra_files=expected_extra_files)
        buffer.seek(0)
        extra_files = {'foo': ''}
        torch.jit.load(buffer, _extra_files=extra_files)
        self.assertEqual(value, extra_files['foo'])

        # Non-existent file 'bar'
        with self.assertRaises(RuntimeError):
            extra_files['bar'] = ''
            torch.jit.load(buffer, _extra_files=extra_files)

    def test_save_load_using_pathlib(self):
        class MyMod(torch.jit.ScriptModule):
            @torch.jit.script_method
            def forward(self, a):
                return 2 * a

        m = MyMod()

        # Save then load.
        with TemporaryFileName() as fname:
            path = pathlib.Path(fname)
            m.save(path)
            m2 = torch.jit.load(path)

        x = torch.tensor([1., 2., 3., 4.])
        self.assertTrue(torch.equal(m(x), m2(x)))

    def test_save_nonexit_file(self):
        class Foo(torch.nn.Module):
            def forward(self, x):
                return 2 * x

        script_module = torch.jit.script(Foo())
        with self.assertRaises(RuntimeError):
            script_module.save("NonExist/path/test.pt")

    def test_save_namedtuple_input_only(self):
        """
        Even if a NamedTuple is only used as an input argument, saving and
        loading should work correctly.
        """
        global FooTuple  # see [local resolution in python]

        class FooTuple(NamedTuple):
            a: int

        class MyModule(torch.nn.Module):
            def forward(self, x: FooTuple) -> torch.Tensor:
                return torch.tensor(3)

        m_loaded = self.getExportImportCopy(torch.jit.script(MyModule()))
        output = m_loaded(FooTuple(a=5))
        self.assertEqual(output, torch.tensor(3))

    def test_save_namedtuple_output_only(self):
        """
        Even if a NamedTuple is only used as an output argument, saving and
        loading should work correctly.
        """
        global FooTuple  # see [local resolution in python]

        class FooTuple(NamedTuple):
            a: int

        class MyModule(torch.nn.Module):
            def forward(self) -> Optional[FooTuple]:
                return None

        m_loaded = self.getExportImportCopy(torch.jit.script(MyModule()))
        output = m_loaded()
        self.assertEqual(output, None)

    def test_save_load_params_buffers_submodules(self):
        """
        Check that parameters, buffers, and submodules are the same after loading.
        """

        class Submodule(torch.nn.Module):
            def __init__(self):
                super().__init__()

        class TestModule(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.add_module("submodule_a", Submodule())
                self.register_parameter("parameter_a", torch.nn.Parameter(torch.randn(4)))
                self.register_buffer("buffer", torch.randn(4))
                self.t = torch.rand(4)  # not buffer

                self.parameter_b = torch.nn.Parameter(torch.randn(4))
                self.submodule_b = Submodule()

        m = TestModule()
        m_loaded = self.getExportImportCopy(torch.jit.script(m))

        # Check submodules.
        self.assertEqual(len(list(m.named_modules())), len(list(m_loaded.named_modules())))
        for m_s, loaded_s in zip(m.named_modules(), m_loaded.named_modules()):
            m_name, _ = m_s
            loaded_name, _ = loaded_s
            self.assertEqual(m_name, loaded_name)

        # Check parameters.
        self.assertEqual(len(list(m.parameters())), len(list(m_loaded.parameters())))
        for m_p, loaded_p in zip(m.parameters(), m_loaded.parameters()):
            self.assertEqual(m_p, loaded_p)

        # Check buffers.
        self.assertEqual(len(list(m.named_buffers())), len(list(m_loaded.named_buffers())))
        for m_b, loaded_b in zip(m.named_buffers(), m_loaded.named_buffers()):
            m_name, m_buffer = m_b
            loaded_name, loaded_buffer = loaded_b
            self.assertEqual(m_name, loaded_name)
            self.assertEqual(m_buffer, loaded_buffer)

    def test_save_load_meta_tensors(self):
        """
        Check that parameters, buffers, and submodules are the same after loading
        for a module with parameters and buffers that are meta tensors
        """
        class Foo(torch.nn.Module):
            def __init__(self):
                super(Foo, self).__init__()
                self.foo = torch.nn.Linear(2, 3, device="meta")
                self.bar = torch.nn.Linear(3, 4)
                self.register_buffer("buffer", torch.randn(4, device="meta"))

            def forward(self, x):
                x = self.foo(x)
                x = self.bar(x)
                return x

        m = Foo()
        m_loaded = self.getExportImportCopy(torch.jit.script(m))
        # Check submodules.
        self.assertEqual(len(list(m.named_modules())), len(list(m_loaded.named_modules())))
        self.assertEqual(set(name for name, _ in m.named_modules()), set(name for name, _ in m_loaded.named_modules()))
        # Check parameters.
        m_params = dict(m.named_parameters())
        m_loaded_params = dict(m_loaded.named_parameters())
        self.assertEqual(len(m_params), len(m_loaded_params))
        self.assertEqual(m_params, m_loaded_params)
        # Check buffers.
        m_buffers = dict(m.named_buffers())
        m_loaded_buffers = dict(m_loaded.named_buffers())
        self.assertEqual(len(m_buffers), len(m_loaded_buffers))
        self.assertEqual(m_buffers, m_loaded_buffers)
        # Check params and buffers that are/are not meta tensors
        self.assertTrue(m_params['foo.weight'].is_meta)
        self.assertTrue(m_loaded_params['foo.weight'].is_meta)
        self.assertTrue(m_params['foo.bias'].is_meta)
        self.assertTrue(m_loaded_params['foo.bias'].is_meta)
        self.assertFalse(m_params['bar.weight'].is_meta)
        self.assertFalse(m_loaded_params['bar.weight'].is_meta)
        self.assertFalse(m_params['bar.bias'].is_meta)
        self.assertFalse(m_loaded_params['bar.bias'].is_meta)
        self.assertTrue(m_buffers['buffer'].is_meta)
        self.assertTrue(m_loaded_buffers['buffer'].is_meta)
