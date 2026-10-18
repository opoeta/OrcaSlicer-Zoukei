"""Tests of the Zoukei project / model operation tools (G2) against a fake Remote API."""
import unittest

from fakeapi import REPLIES, SEEN, ToolTestCase


class ProjectToolsTest(ToolTestCase):
    MODULES = ("tools_project",)

    def test_tool_list(self):
        self.assertEqual(sorted(self.mcp.tools), sorted([
            "project_info", "project_new", "project_open", "project_save", "project_save_as", "project_import",
            "project_export", "project_undo", "project_redo", "select_objects", "object_rename", "object_volumes",
            "volume_add", "volume_set", "volume_remove", "object_split", "object_copy", "object_mirror",
            "object_scale_to_fit", "object_drop_to_bed", "object_cut"]))

    def test_info(self):
        self.run_tool("project_info")
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/project"))

    def test_new_and_open_default_to_not_discarding(self):
        self.run_tool("project_new")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/new", {"discard_changes": False}))
        self.run_tool("project_open", path="C:/p/a.3mf")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/open", {"path": "C:/p/a.3mf", "discard_changes": False}))
        self.run_tool("project_open", path="C:/p/a.3mf", discard_changes=True)
        self.assertTrue(self.last()[2]["discard_changes"])

    def test_unsaved_changes_refusal_is_returned(self):
        REPLIES[("POST", "/api/v1/project/new")] = (409, {"error": "unsaved_changes"})
        self.assertIn("error", self.run_tool("project_new"))

    def test_save_and_save_as_never_overwrite_by_default(self):
        self.run_tool("project_save")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/save", {"overwrite": False}))
        self.run_tool("project_save_as", path="C:/p/new.3mf")
        self.assertEqual(self.last()[2], {"path": "C:/p/new.3mf", "overwrite": False})
        self.run_tool("project_save_as", path="C:/p/new.3mf", overwrite=True)
        self.assertTrue(self.last()[2]["overwrite"])

    def test_import_and_export(self):
        self.run_tool("project_import", paths=["C:/a.stl", "C:/b.obj"])
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/import", {"paths": ["C:/a.stl", "C:/b.obj"]}))
        self.run_tool("project_export", format="stl", path="C:/out.stl", objects=[4, 5], plate=0)
        self.assertEqual(self.last()[2], {"format": "stl", "path": "C:/out.stl", "overwrite": False,
                                          "objects": [4, 5], "plate": 0})
        self.run_tool("project_export", format="3mf", path="C:/out.3mf", overwrite=True)
        self.assertEqual(self.last()[2], {"format": "3mf", "path": "C:/out.3mf", "overwrite": True})

    def test_undo_redo(self):
        self.run_tool("project_undo", steps=3)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/undo", {"steps": 3}))
        self.run_tool("project_redo")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/project/redo", {"steps": 1}))

    def test_select(self):
        self.run_tool("select_objects", objects=[4], volume=1)
        self.assertEqual(self.last()[:3], ("PUT", "/api/v1/selection", {"objects": [4], "volume": 1}))
        self.run_tool("select_objects", objects=[])
        self.assertEqual(self.last()[2], {"objects": []})

    def test_rename_and_volumes(self):
        self.run_tool("object_rename", object_id=4, name="Lid")
        self.assertEqual(self.last()[:3], ("PUT", "/api/v1/objects/4", {"name": "Lid"}))
        self.run_tool("object_volumes", object_id=4)
        self.assertEqual(self.last()[:2], ("GET", "/api/v1/objects/4/volumes"))

    def test_volume_add_modifier_box(self):
        self.run_tool("volume_add", object_id=4, type="modifier", shape="box", size=[5, 5, 5])
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/volumes",
                                           {"type": "modifier", "shape": "box", "size": [5, 5, 5]}))

    def test_volume_add_negative_from_file(self):
        self.run_tool("volume_add", object_id=4, type="negative", file="C:/hole.stl", scale=2.0, position=[1, 2, 3])
        body = self.last()[2]
        self.assertEqual((body["type"], body["file"], body["scale"], body["position"]), ("negative", "C:/hole.stl", 2.0, [1, 2, 3]))

    def test_volume_set_and_remove(self):
        self.run_tool("volume_set", object_id=4, volume=2, name="X", type="part", config={"wall_loops": 4, "infill_direction": None})
        self.assertEqual(self.last()[:2], ("PUT", "/api/v1/objects/4/volumes/2"))
        self.assertEqual(self.last()[2], {"name": "X", "type": "part", "config": {"wall_loops": 4, "infill_direction": None}})
        self.run_tool("volume_remove", object_id=4, volume=2)
        self.assertEqual(self.last()[:2], ("DELETE", "/api/v1/objects/4/volumes/2"))

    def test_volume_set_refuses_sensitive_keys_without_calling(self):
        out = self.run_tool("volume_set", object_id=4, volume=0, config={"machine_start_gcode": "M104 S300"})
        self.assertIn("error", out)
        self.assertEqual(SEEN, [])

    def test_split_copy_mirror(self):
        self.run_tool("object_split", object_id=4)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/split", {"mode": "objects"}))
        self.run_tool("object_split", object_id=4, mode="parts", volume=1)
        self.assertEqual(self.last()[2], {"mode": "parts", "volume": 1})
        self.run_tool("object_copy", object_id=4, copies=2)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/copy", {"copies": 2}))
        self.run_tool("object_mirror", object_id=4, axis="x")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/mirror", {"axis": "x"}))

    def test_scale_drop_cut(self):
        self.run_tool("object_scale_to_fit", object_id=4, margin=5.0)
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/scale_to_fit",
                                           {"margin": 5.0, "allow_upscale": False, "center": False}))
        self.run_tool("object_drop_to_bed", object_id=4)
        self.assertEqual(self.last()[:2], ("POST", "/api/v1/objects/4/drop"))
        self.run_tool("object_cut", object_id=4, z=12.5, keep="upper")
        self.assertEqual(self.last()[:3], ("POST", "/api/v1/objects/4/cut",
                                           {"z": 12.5, "keep": "upper", "place_on_cut_upper": True}))


if __name__ == "__main__":
    unittest.main()
