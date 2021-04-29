from io import BytesIO

import torch
from torch.fx import Graph, GraphModule, Tracer, symbolic_trace
from torch.package import (
    ObjMismatchError,
    PackageExporter,
    PackageImporter,
    sys_importer,
)
from torch.testing._internal.common_utils import run_tests

try:
    from .common import PackageTestCase
except ImportError:
    # Support the case where we run this file directly.
    from common import PackageTestCase


class CustomTracer(Tracer):
    def is_leaf_module(self, m, module_qualified_name,) -> bool:
        return super().is_leaf_module(m, module_qualified_name) or module_qualified_name == "MyModule"


class MyModule(torch.nn.Module):
    def forward(self, x):
        return torch.relu(x)


class SimpleTest(torch.nn.Module):
    def forward(self, x):
        return torch.relu(x + 3.0)


class TestPackageFX(PackageTestCase):
    """Tests for compatibility with FX."""

    def test_package_fx_simple(self):
        st = SimpleTest()
        traced = symbolic_trace(st)

        f = BytesIO()
        with PackageExporter(f, verbose=False) as pe:
            pe.extern(["io", "sys"])
            pe.save_pickle("model", "model.pkl", traced)

        f.seek(0)
        pi = PackageImporter(f)
        loaded_traced = pi.load_pickle("model", "model.pkl")
        input = torch.rand(2, 3)
        self.assertTrue(torch.allclose(loaded_traced(input), traced(input)))

    def test_package_then_fx(self):
        from package_a.test_module import SimpleTest

        model = SimpleTest()
        f = BytesIO()
        with PackageExporter(f, verbose=False) as pe:
            pe.save_pickle("model", "model.pkl", model)

        f.seek(0)
        pi = PackageImporter(f)
        loaded = pi.load_pickle("model", "model.pkl")
        traced = symbolic_trace(loaded)
        input = torch.rand(2, 3)
        self.assertTrue(torch.allclose(loaded(input), traced(input)))

    def test_package_fx_package(self):
        from package_a.test_module import SimpleTest

        model = SimpleTest()
        f = BytesIO()
        with PackageExporter(f, verbose=False) as pe:
            pe.save_pickle("model", "model.pkl", model)

        f.seek(0)
        pi = PackageImporter(f)
        loaded = pi.load_pickle("model", "model.pkl")
        traced = symbolic_trace(loaded)

        # re-save the package exporter
        f2 = BytesIO()
        # This should fail, because we are referencing some globals that are
        # only in the package.
        with self.assertRaises(ObjMismatchError):
            with PackageExporter(f2, verbose=False) as pe:
                pe.save_pickle("model", "model.pkl", traced)

        f2.seek(0)
        with PackageExporter(f2, importer=(pi, sys_importer), verbose=False) as pe:
            # Make the package available to the exporter's environment.
            pe.save_pickle("model", "model.pkl", traced)
        f2.seek(0)
        pi2 = PackageImporter(f2)
        loaded2 = pi2.load_pickle("model", "model.pkl")

        input = torch.rand(2, 3)
        self.assertTrue(torch.allclose(loaded(input), loaded2(input)))

    def test_package_fx_with_imports(self):
        import package_a.subpackage

        # Manually construct a graph that invokes a leaf function
        graph = Graph()
        a = graph.placeholder("x")
        b = graph.placeholder("y")
        c = graph.call_function(package_a.subpackage.leaf_function, (a, b))
        d = graph.call_function(torch.sin, (c,))
        graph.output(d)
        gm = GraphModule(torch.nn.Module(), graph)

        f = BytesIO()
        with PackageExporter(f, verbose=False) as pe:
            pe.save_pickle("model", "model.pkl", gm)
        f.seek(0)

        pi = PackageImporter(f)
        loaded_gm = pi.load_pickle("model", "model.pkl")
        input_x = torch.rand(2, 3)
        input_y = torch.rand(2, 3)

        self.assertTrue(
            torch.allclose(loaded_gm(input_x, input_y), gm(input_x, input_y))
        )

        # Check that the packaged version of the leaf_function dependency is
        # not the same as in the outer env.
        packaged_dependency = pi.import_module("package_a.subpackage")
        self.assertTrue(packaged_dependency is not package_a.subpackage)

    def test_package_fx_custom_tracer(self):
        """
        Tests that if a GraphModule containing a Graph
        produced using a custom Tracer is packaged, is it unpackaged
        using the same custom Tracer.
        """
        my_mod = MyModule()
        tracer = CustomTracer()
        graph = tracer.trace(my_mod)

        self.assertIs(graph.tracer, tracer)

        gm = GraphModule(my_mod, graph)

        f = BytesIO()
        with PackageExporter(f, verbose=False) as pe:
            pe.extern(["io", "sys"])
            pe.save_pickle("model", "model.pkl", gm)

        f.seek(0)
        pi = PackageImporter(f)
        loaded = pi.load_pickle("model", "model.pkl")

        # TODO: How to check that loaded uses CustomTracer (rather, packaged
        # CustomTracer)?
        # self.assertIsInstance(loaded.graph.tracer, CustomTracer)


if __name__ == "__main__":
    run_tests()
