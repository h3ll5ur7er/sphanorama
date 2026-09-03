"""Tests for the layer checker.

Each test builds a miniature repo in a temp dir rather than asserting against the real one, so a
rule can be exercised in isolation and the suite does not fail for unrelated reasons when the real
source tree grows.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import layer_check  # noqa: E402


class Repo:
    """A throwaway source tree. `write` takes a repo-relative path and file body."""

    def __init__(self, tmp: Path):
        self.root = tmp

    def write(self, rel: str, body: str = "") -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
        return p

    def contracts(self):
        """The contract headers every fixture needs to resolve includes against."""
        for name in ("capture_session_manager", "panorama_build_manager", "project_manager"):
            self.write(f"contracts/cpp/sphanorama/managers/{name}.h")
        for name in ("pose_engine", "registration_engine", "composition_engine"):
            self.write(f"contracts/cpp/sphanorama/engines/{name}.h")
        for name in ("camera_access", "compute_device_access", "frame_store_access",
                     "project_store_access"):
            self.write(f"contracts/cpp/sphanorama/resource_access/{name}.h")
        for name in ("logger", "arena", "clock"):
            self.write(f"contracts/cpp/sphanorama/utilities/{name}.h")
        self.write("contracts/cpp/sphanorama/types.h")
        return self


class LayerRuleTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name)).contracts()

    def tearDown(self):
        self._tmp.cleanup()

    def violations(self):
        return layer_check.check(self.repo.root)

    def assertClean(self):
        v = self.violations()
        self.assertEqual(v, [], f"expected no violations, got: {[str(x) for x in v]}")

    def assertViolation(self, *, substring):
        v = self.violations()
        self.assertTrue(v, "expected a violation, found none")
        self.assertTrue(any(substring in str(x) for x in v),
                        f"expected a violation mentioning {substring!r}, got: {[str(x) for x in v]}")

    # -- the rule that motivated splitting the contracts in the first place ------------------
    def test_manager_including_another_manager_is_a_violation(self):
        self.repo.write("core/src/managers/panorama_build_manager.cpp",
                        '#include "sphanorama/managers/capture_session_manager.h"\n')
        self.assertViolation(substring="capture_session_manager")

    def test_manager_including_its_own_contract_is_fine(self):
        self.repo.write("core/src/managers/panorama_build_manager.cpp",
                        '#include "sphanorama/managers/panorama_build_manager.h"\n')
        self.assertClean()

    def test_manager_may_call_engines_and_resource_access(self):
        self.repo.write("core/src/managers/panorama_build_manager.cpp",
                        '#include "sphanorama/engines/registration_engine.h"\n'
                        '#include "sphanorama/resource_access/project_store_access.h"\n')
        self.assertClean()

    # -- an implementation may include the contract it implements ------------------------------
    def test_an_implementation_directory_may_include_its_own_contract(self):
        # Implementations live in a directory named for the contract they implement, so several
        # implementations of one interface are one component. Without this the single legitimate
        # same-layer edge — implementing your own interface — would be indistinguishable from a
        # component reaching sideways.
        self.repo.write("core/src/engines/pose_engine/null_pose_engine.cpp",
                        '#include "sphanorama/engines/pose_engine.h"\n')
        self.assertClean()

    def test_two_implementations_of_one_contract_are_the_same_component(self):
        self.repo.write("core/src/engines/pose_engine/fused_pose_engine.h")
        self.repo.write("core/src/engines/pose_engine/null_pose_engine.cpp",
                        '#include "engines/pose_engine/fused_pose_engine.h"\n')
        self.assertClean()

    def test_an_implementation_still_may_not_include_a_sibling_contract(self):
        self.repo.write("core/src/engines/pose_engine/null_pose_engine.cpp",
                        '#include "sphanorama/engines/registration_engine.h"\n')
        self.assertViolation(substring="registration_engine")

    # -- engines are stateless and cannot reach upward ---------------------------------------
    def test_engine_including_a_manager_is_a_violation(self):
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "sphanorama/managers/capture_session_manager.h"\n')
        self.assertViolation(substring="capture_session_manager")

    def test_engine_including_another_engine_is_a_violation(self):
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "sphanorama/engines/registration_engine.h"\n')
        self.assertViolation(substring="registration_engine")

    # -- the two sanctioned exceptions (docs/03 §3.3 rule 5) ---------------------------------
    def test_engine_may_use_compute_and_frame_store(self):
        self.repo.write("core/src/engines/composition_engine.cpp",
                        '#include "sphanorama/resource_access/compute_device_access.h"\n'
                        '#include "sphanorama/resource_access/frame_store_access.h"\n')
        self.assertClean()

    def test_engine_using_any_other_resource_access_is_a_violation(self):
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "sphanorama/resource_access/camera_access.h"\n')
        self.assertViolation(substring="camera_access")

    # -- clients call managers only ----------------------------------------------------------
    def test_client_including_an_engine_is_a_violation(self):
        self.repo.write("bench/main.cpp", '#include "sphanorama/engines/pose_engine.h"\n')
        self.assertViolation(substring="pose_engine")

    def test_client_including_a_manager_is_fine(self):
        self.repo.write("bench/main.cpp",
                        '#include "sphanorama/managers/project_manager.h"\n')
        self.assertClean()

    def test_client_including_resource_access_is_a_violation(self):
        self.repo.write("bench/main.cpp",
                        '#include "sphanorama/resource_access/camera_access.h"\n')
        self.assertViolation(substring="camera_access")

    # -- the WASM bridge is a client -----------------------------------------------------------
    def test_the_bridge_may_call_managers(self):
        self.repo.write("bridge/facade.cpp",
                        '#include "sphanorama/managers/project_manager.h"\n')
        self.assertClean()

    def test_the_bridge_may_not_reach_past_managers(self):
        # The boundary is where it is most tempting to shortcut straight to an engine, and where
        # doing so would put business logic in the one tree that cannot be tested natively.
        self.repo.write("bridge/facade.cpp", '#include "sphanorama/engines/pose_engine.h"\n')
        self.assertViolation(substring="pose_engine")

    def test_includes_resolve_through_the_bridge_root_too(self):
        # bridge/ is on the include path for the same reason core/src is, so a port including its
        # own header has to resolve there rather than being reported as a missing file.
        self.repo.write("bridge/resource_access/project_store_access/browser_store.h")
        self.repo.write("bridge/resource_access/project_store_access/browser_store.cpp",
                        '#include "resource_access/project_store_access/browser_store.h"\n')
        self.assertClean()

    def test_the_composition_root_may_name_every_concrete_type(self):
        # Wiring is not a business dependency. A composition root exists precisely to know which
        # implementation each contract gets, and every layer rule is about who may *call* whom.
        # The exemption is one named file, not a directory, so it cannot quietly widen.
        self.repo.write("core/src/engines/pose_engine/null_pose_engine.h")
        self.repo.write("bridge/runtime.h",
                        '#include "engines/pose_engine/null_pose_engine.h"\n')
        self.assertClean()

    def test_the_exemption_does_not_extend_to_the_rest_of_the_bridge(self):
        self.repo.write("core/src/engines/pose_engine/null_pose_engine.h")
        self.repo.write("bridge/facade.generated.cpp",
                        '#include "engines/pose_engine/null_pose_engine.h"\n')
        self.assertViolation(substring="null_pose_engine")

    def test_the_bridge_may_hold_layered_subtrees(self):
        # Browser-backed resource accesses are resource-access *layer* code that needs Emscripten,
        # and only bridge/ may reference Emscripten. So bridge/ carries layer subdirectories: a
        # port under bridge/resource_access/ is judged as resource access, not as a client.
        self.repo.write("bridge/resource_access/project_store_access/browser_store.cpp",
                        '#include "sphanorama/resource_access/project_store_access.h"\n')
        self.assertClean()

    def test_a_port_may_not_reach_sideways_any_more_than_a_native_one(self):
        self.repo.write("bridge/resource_access/project_store_access/browser_store.cpp",
                        '#include "sphanorama/resource_access/camera_access.h"\n')
        self.assertViolation(substring="camera_access")

    def test_a_port_may_not_call_a_manager(self):
        self.repo.write("bridge/resource_access/project_store_access/browser_store.cpp",
                        '#include "sphanorama/managers/project_manager.h"\n')
        self.assertViolation(substring="project_manager")

    def test_bridge_tests_are_not_layer_checked(self):
        self.repo.write("bridge/test/facade_test.cpp",
                        '#include "sphanorama/engines/pose_engine.h"\n')
        self.assertClean()

    # -- resource access is a leaf, with one exception -----------------------------------------
    def test_resource_access_reaching_sideways_is_a_violation(self):
        self.repo.write("core/src/resource_access/frame_store_access.cpp",
                        '#include "sphanorama/resource_access/project_store_access.h"\n')
        self.assertViolation(substring="project_store_access")

    def test_a_port_that_produces_pixels_may_use_the_frame_store(self):
        # ICameraAccess::PeekPreviewFrame returns a FrameRef, which is a handle into the store —
        # so every implementation of it has to reach the store. Not a browser quirk: the fake
        # does the same. See ADR 0021.
        self.repo.write("core/src/resource_access/camera_access.cpp",
                        '#include "sphanorama/resource_access/frame_store_access.h"\n')
        self.assertClean()

    def test_the_frame_store_exception_does_not_open_the_layer_generally(self):
        self.repo.write("core/src/resource_access/camera_access.cpp",
                        '#include "sphanorama/resource_access/motion_sensor_access.h"\n')
        self.assertViolation(substring="motion_sensor_access")

    def test_utilities_may_not_reach_business_logic(self):
        self.repo.write("core/src/utilities/logger.cpp",
                        '#include "sphanorama/engines/pose_engine.h"\n')
        self.assertViolation(substring="pose_engine")

    # -- things everyone is allowed to do -----------------------------------------------------
    def test_everyone_may_include_types_and_utilities(self):
        for path in ("core/src/engines/pose_engine.cpp",
                     "core/src/managers/project_manager.cpp",
                     "core/src/resource_access/camera_access.cpp",
                     "bench/main.cpp"):
            self.repo.write(path, '#include "sphanorama/types.h"\n'
                                  '#include "sphanorama/utilities/logger.h"\n')
        self.assertClean()

    def test_standard_library_includes_are_ignored(self):
        self.repo.write("core/src/engines/pose_engine.cpp",
                        "#include <vector>\n#include <cmath>\n")
        self.assertClean()

    def test_component_private_header_in_the_components_own_subdir_is_fine(self):
        # Private headers live under a directory named for the component, so "same component"
        # stays decidable from the path alone.
        self.repo.write("core/src/engines/pose_engine/detail.h")
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "pose_engine/detail.h"\n')
        self.assertClean()

    def test_reaching_into_a_sibling_components_private_header_is_a_violation(self):
        # Without this, two managers sharing a directory could couple through a relative include
        # and never touch each other's contract header — invisible to a contract-level check.
        self.repo.write("core/src/managers/capture_session_manager/detail.h")
        self.repo.write("core/src/managers/panorama_build_manager.cpp",
                        '#include "capture_session_manager/detail.h"\n')
        self.assertViolation(substring="detail.h")

    def test_includes_resolve_through_the_core_source_root(self):
        # core/src is an include root, so "utilities/arena.h" from anywhere in the core means the
        # concrete utility, not a file beside the includer.
        self.repo.write("core/src/utilities/arena.h")
        self.repo.write("core/src/managers/project_manager.cpp",
                        '#include "utilities/arena.h"\n')
        self.assertClean()

    def test_a_root_relative_include_of_another_component_is_still_a_violation(self):
        # Without root-relative resolution this edge would be invisible: an engine could reach a
        # manager's concrete class without ever naming its contract header.
        self.repo.write("core/src/managers/capture_session_manager.h")
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "managers/capture_session_manager.h"\n')
        self.assertViolation(substring="capture_session_manager")

    def test_include_that_resolves_nowhere_is_reported(self):
        self.repo.write("core/src/engines/pose_engine.cpp",
                        '#include "sphanorama/engines/does_not_exist.h"\n')
        self.assertViolation(substring="does_not_exist")


class ContractHeaderTest(unittest.TestCase):
    """Contract headers are subject to the same rules as the code implementing them."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name)).contracts()

    def tearDown(self):
        self._tmp.cleanup()

    def test_an_engine_contract_may_not_depend_on_a_manager_contract(self):
        self.repo.write("contracts/cpp/sphanorama/engines/pose_engine.h",
                        '#include "sphanorama/managers/project_manager.h"\n')
        self.assertTrue(layer_check.check(self.repo.root))

    def test_types_header_may_not_depend_on_any_interface(self):
        self.repo.write("contracts/cpp/sphanorama/types.h",
                        '#include "sphanorama/utilities/logger.h"\n')
        self.assertTrue(layer_check.check(self.repo.root))


class CommandLineTest(unittest.TestCase):
    """CI depends on the exit code, so it is part of the contract."""

    def run_cli(self, root):
        return subprocess.run(
            [sys.executable, str(Path(__file__).parent / "layer_check.py"), str(root)],
            capture_output=True, text=True)

    def test_exit_zero_and_quiet_on_a_clean_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)).contracts()
            repo.write("core/src/managers/project_manager.cpp",
                       '#include "sphanorama/types.h"\n')
            r = self.run_cli(repo.root)
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_exit_nonzero_and_names_the_offending_edge(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp)).contracts()
            repo.write("core/src/engines/pose_engine.cpp",
                       '#include "sphanorama/managers/project_manager.h"\n')
            r = self.run_cli(repo.root)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("pose_engine.cpp", r.stdout + r.stderr)
            self.assertIn("project_manager", r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
