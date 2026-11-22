"""Tests for interior (MLO) placement (no game data).

Covers world_index.py --interiors-out (interior archetypes with their entity lists and entity sets reached
through pool references, interior instances with their default sets) and world_build.py --interiors (the
instance rotation is stored as is, interior entities store the inverse like map entities, entity-set selection,
counts in world.json). Every resource, entity and model is synthetic.

    python -I tests/test_world_interiors.py
"""
import contextlib
import io
import json
import math
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.dirname(os.path.abspath(__file__))
for p in (os.path.join(ROOT, "tools"), TESTS):
    if p not in sys.path:
        sys.path.insert(0, p)

import test_world_index as twi  # noqa: E402  (0x7a member/struct encoders, RPF stream writer)
import test_world_place as twp  # noqa: E402  (box model assets, entity rows, rotations)
import world_build  # noqa: E402
import world_names  # noqa: E402

WI = twi.WI
ARRAY_BLOCK = 0x25771B45     # any struct hash: the block holding a pointer or hash array

ENTITY_MEMBERS = [
    twi.member(WI.FIELD["archetypeName"], 0x44, 0x4A),
    twi.member(WI.FIELD["position"], 0x38, 0x32),
    twi.member(WI.FIELD["rotation"], 0x10, 0x34),
    twi.member(WI.FIELD["scaleXY"], 0x54, 0x21),
    twi.member(WI.FIELD["scaleZ"], 0x58, 0x21),
    twi.member(WI.FIELD["lodLevel"], 0x7E, 0x60),
    twi.member(WI.FIELD["lodDist"], 0x60, 0x21),
    twi.member(WI.ARCHETYPE_KEY, 0x20, 0x4F),
]
MLO_MEMBERS = [
    twi.member(WI.MLO_FIELD["name"], 0x78, 0x4A),
    twi.member(WI.MLO_FIELD["bbMin"], 0x40, 0x32),
    twi.member(WI.MLO_FIELD["bbMax"], 0x4C, 0x32),
    twi.member(WI.MLO_FIELD["entities"], 0xC0, 0x52),
    twi.member(WI.MLO_FIELD["entitySets"], 0xF0, 0x52),
]
SET_MEMBERS = [
    twi.member(WI.MLO_FIELD["name"], 0x00, 0x4A),
    twi.member(WI.MLO_FIELD["entities"], 0x18, 0x52),
]
INSTANCE_MEMBERS = ENTITY_MEMBERS + [
    twi.member(WI.MLO_FIELD["groupId"], 0x90, 0x15),
    twi.member(WI.MLO_FIELD["floorId"], 0x94, 0x15),
    twi.member(WI.MLO_FIELD["defaultEntitySets"], 0xA0, 0x52),
]


def pool(block_number, offset=0):
    """A pool reference: 1-based block number in bits 0..11, byte offset in the block from bit 20."""
    return block_number | (offset << 20)


def resource(structs, blocks):
    """A 0x7a resource. structs: [(hash, size, [member bytes])]; blocks: [(hash, bytes)], numbered from 1."""
    b = bytearray(0x100)
    b[0x10:0x14] = b"0DRP"
    b[0x14] = 0x7A
    struct.pack_into("<I", b, 0x20, 1)
    st = len(b)
    b.extend(bytes(32 * len(structs)))
    bt = len(b)
    b.extend(bytes(16 * len(blocks)))
    for i, (h, size, members) in enumerate(structs):
        at = len(b)
        for m in members:
            b.extend(m)
        b[st + 32 * i:st + 32 * (i + 1)] = twi.struct_def(h, size, members, 0, at)
    for i, (h, data) in enumerate(blocks):
        b.extend(bytes((-len(b)) % 16))
        at = len(b)
        b.extend(data)
        struct.pack_into("<IIQ", b, bt + 16 * i, h, len(data), twi.ptr(at))
    struct.pack_into("<Q", b, 0x28, twi.ptr(st))
    struct.pack_into("<Q", b, 0x38, twi.ptr(bt))
    struct.pack_into("<HHHH", b, 0x48, len(structs), 0, len(blocks), 0)
    return bytes(b)


def entity_bytes(name, position, rotation=(0.0, 0.0, 0.0, 1.0), lod=5, size=0x90):
    e = bytearray(size)
    struct.pack_into("<4f", e, 0x10, *rotation)
    struct.pack_into("<Q", e, 0x20, world_names.hash64(name))
    struct.pack_into("<3f", e, 0x38, *position)
    struct.pack_into("<I", e, 0x44, world_names.joaat(name))
    struct.pack_into("<2f", e, 0x54, 1.0, 1.0)
    struct.pack_into("<f", e, 0x60, -1.0)
    e[0x7E] = lod
    return e


def interior_resource(name="int_test", bad_pointer=False):
    """One interior: entity list [chair, table] and two sets, 'set_a' [lamp] and 'set_b' [rug].

    Blocks: 1 entities (4 CEntityDef), 2 the archetype, 3 the two sets, 4 the pointer arrays."""
    ents = b"".join(entity_bytes(n, p) for n, p in (("chair", (1, 0, 0)), ("table", (0, 2, 0)),
                                                        ("lamp", (0, 0, 3)), ("rug", (-1, 0, 0))))
    arrays = struct.pack("<4Q", pool(1, 0x00), pool(1, 0x90), pool(1, 0x120), pool(1, 0x1B0))
    if bad_pointer:
        arrays = struct.pack("<Q", pool(9, 0)) + arrays[8:]
    mlo = bytearray(0x220)
    struct.pack_into("<3f3f", mlo, 0x40, -5, -5, -1, 5, 5, 4)
    struct.pack_into("<I", mlo, 0x78, world_names.joaat(name))
    struct.pack_into("<QH", mlo, 0xC0, pool(4, 0), 2)
    struct.pack_into("<QH", mlo, 0xF0, pool(3, 0), 2)
    sets = bytearray(0x50)
    struct.pack_into("<I", sets, 0x00, world_names.joaat("set_a"))
    struct.pack_into("<QH", sets, 0x18, pool(4, 0x10), 1)
    struct.pack_into("<I", sets, 0x28, world_names.joaat("set_b"))
    struct.pack_into("<QH", sets, 0x28 + 0x18, pool(4, 0x18), 1)
    return resource(
        [(WI.ENTITY, 0x90, ENTITY_MEMBERS), (WI.MLO_ARCHETYPE, 0x220, MLO_MEMBERS),
         (WI.MLO_ENTITY_SET, 0x28, SET_MEMBERS)],
        [(WI.ENTITY, ents), (WI.MLO_ARCHETYPE, bytes(mlo)), (WI.MLO_ENTITY_SET, bytes(sets)), (ARRAY_BLOCK, arrays)])


def instance_resource(name="int_test", position=(100.0, 200.0, 10.0), rotation=(0.0, 0.0, 0.0, 1.0),
                      defaults=("set_a",)):
    inst = entity_bytes(name, position, rotation, lod=0, size=0x250)
    struct.pack_into("<II", inst, 0x90, 7, 2)
    struct.pack_into("<QH", inst, 0xA0, pool(2, 0), len(defaults))
    hashes = struct.pack(f"<{len(defaults)}I", *(world_names.joaat(d) for d in defaults)) or bytes(4)
    return resource([(WI.MLO_INSTANCE, 0x250, INSTANCE_MEMBERS)],
                    [(WI.MLO_INSTANCE, bytes(inst)), (ARRAY_BLOCK, hashes)])


def records(*resources):
    out = io.StringIO()
    sink = WI.InteriorRows(out)
    for r in resources:
        WI.parse_resource(r, sink)
    return [json.loads(line) for line in out.getvalue().splitlines()], sink.counts


def rotation_about_z(degrees):
    """A world rotation of `degrees` about +Z (what CMloInstanceDef stores)."""
    h = math.radians(degrees) / 2
    return [0.0, 0.0, math.sin(h), math.cos(h)]


def matrix(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w)
    x, y, z, w = x / n, y / n, z / n, w / n
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


class InteriorsOutTest(unittest.TestCase):
    def test_interior_record(self):
        recs, counts = records(interior_resource())
        self.assertEqual(len(recs), 1)
        rec = recs[0]
        self.assertEqual(rec["interior"], f"{world_names.joaat('int_test'):08x}")
        self.assertEqual((rec["bb_min"], rec["bb_max"]), ([-5, -5, -1], [5, 5, 4]))
        self.assertEqual([e["archetype"] for e in rec["entities"]],
                         [f"{world_names.joaat(n):08x}" for n in ("chair", "table")])
        self.assertEqual(rec["entities"][1]["position"], [0, 2, 0])
        self.assertEqual(rec["entities"][0]["key"], f"{world_names.hash64('chair'):016x}")
        self.assertEqual(rec["entities"][0]["lod_level"], 5)
        self.assertEqual([(s["name"], [e["archetype"] for e in s["entities"]]) for s in rec["sets"]],
                         [(f"{world_names.joaat('set_a'):08x}", [f"{world_names.joaat('lamp'):08x}"]),
                          (f"{world_names.joaat('set_b'):08x}", [f"{world_names.joaat('rug'):08x}"])])
        self.assertEqual(counts, {"interiors": 1, "interior_entities": 2, "entity_sets": 2, "set_entities": 2,
                                  "instances": 0, "unresolved_entities": 0})

    def test_instance_record(self):
        recs, counts = records(instance_resource(rotation=rotation_about_z(90), defaults=("set_a", "set_b")))
        self.assertEqual(len(recs), 1)
        rec = recs[0]
        self.assertEqual(rec["instance"], f"{world_names.joaat('int_test'):08x}")
        self.assertEqual(rec["position"], [100, 200, 10])
        self.assertTrue(twp.close(rec["rotation"], rotation_about_z(90), 1e-6))
        self.assertEqual(rec["default_sets"], [f"{world_names.joaat(n):08x}" for n in ("set_a", "set_b")])
        self.assertEqual((rec["group_id"], rec["floor_id"]), (7, 2))
        self.assertEqual(counts["instances"], 1)

    def test_unresolved_entity_reference_is_counted(self):
        recs, counts = records(interior_resource(bad_pointer=True))
        self.assertEqual([e["archetype"] for e in recs[0]["entities"]], [f"{world_names.joaat('table'):08x}"])
        self.assertEqual(counts["unresolved_entities"], 1)

    def test_map_and_meta_resources_write_nothing(self):
        recs, counts = records(twi.make_resource(), twi.make_resource(map_file=False))
        self.assertEqual(recs, [])
        self.assertEqual(sum(counts.values()), 0)

    def test_cli_writes_interiors_and_keeps_them_out_of_entities(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = os.path.join(tmp, "a.rpf")
            with open(archive, "wb") as f:
                f.write(twi.make_rpf_streams([twi.make_resource(), interior_resource(), instance_resource()]))
            ents, ints = os.path.join(tmp, "e.jsonl"), os.path.join(tmp, "i.jsonl")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = WI.main([archive, "--out", os.path.join(tmp, "s.json"), "--entities-out", ents,
                              "--interiors-out", ints])
            self.assertEqual(rc, 0)
            summary = json.loads(out.getvalue())
            self.assertEqual(summary["entity_rows"]["written"], 1)
            self.assertEqual((summary["interior_rows"]["interiors"], summary["interior_rows"]["instances"]), (1, 1))
            with open(ints, encoding="utf-8") as f:
                kinds = sorted("interior" if "interior" in json.loads(line) else "instance" for line in f)
            self.assertEqual(kinds, ["instance", "interior"])
            with open(ents, encoding="utf-8") as f:
                self.assertEqual(len(f.readlines()), 1)

    def test_cli_refuses_to_overwrite_the_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            archive = os.path.join(tmp, "a.rpf")
            data = twi.make_rpf_streams([interior_resource()])
            with open(archive, "wb") as f:
                f.write(data)
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit):
                WI.main([archive, "--out", os.path.join(tmp, "s.json"), "--interiors-out", archive])
            self.assertIn("--interiors-out would overwrite the input", err.getvalue())
            with open(archive, "rb") as f:
                self.assertEqual(f.read(), data)


class ComposeTest(unittest.TestCase):
    def test_instance_rotation_is_stored_as_is(self):
        # Instance at (100, 200, 10) turned +90 degrees; entity at (1, 0, 0) turned +90 degrees in interior space.
        pos, stored = world_build.compose_interior([100, 200, 10], rotation_about_z(90), [1, 0, 0],
                                                   twp.stored_rotation_about_z(90))
        self.assertTrue(twp.close(pos, [100, 201, 10], 1e-9))
        m, _, _ = world_build.entity_transform(stored, 1.0, 1.0)
        self.assertTrue(twp.close(twp.apply(m, (1, 0, 0)), (-1, 0, 0), 1e-9))   # 180 degrees in total
        self.assertTrue(twp.close(twp.apply(m, (0, 1, 0)), (0, -1, 0), 1e-9))

    def test_matches_the_matrix_product(self):
        rng = random.Random(1)
        for _ in range(50):
            q = [rng.uniform(-1, 1) for _ in range(4)]
            s = [rng.uniform(-1, 1) for _ in range(4)]
            p = [rng.uniform(-50, 50) for _ in range(3)]
            t = [rng.uniform(-1000, 1000) for _ in range(3)]
            pos, stored = world_build.compose_interior(t, q, p, s)
            r_inst = matrix(q)
            r_entity, _, _ = world_build.entity_transform(s, 1.0, 1.0)
            expect = [t[i] + sum(r_inst[i][j] * p[j] for j in range(3)) for i in range(3)]
            self.assertTrue(twp.close(pos, expect, 1e-6))
            got, _, _ = world_build.entity_transform(stored, 1.0, 1.0)
            want = matmul(r_inst, r_entity)
            self.assertTrue(all(twp.close(got[i], want[i], 1e-9) for i in range(3)))


def interior_rows(name="int_test", inst_position=(100.0, 200.0, 10.0), inst_rotation=(0.0, 0.0, 0.0, 1.0),
                  defaults=("set_a",)):
    """The --interiors-out lines for interior_resource() and one instance of it, via world_index."""
    recs, _ = records(interior_resource(name),
                      instance_resource(name, inst_position, inst_rotation, defaults))
    return recs


class InteriorPlacementTest(unittest.TestCase):
    def placements(self, recs, extra_lines=(), **kw):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "i.jsonl")
            twp.write_entities(path, recs, extra_lines)
            counts = world_build._placement_counts()
            rows = list(world_build.interior_placements([path], counts, **kw))
        return rows, counts

    def names(self, rows):
        by_hash = {world_names.joaat(n): n for n in ("chair", "table", "lamp", "rug")}
        return sorted(by_hash[r["_arch"]] for r in rows)

    def test_entity_sets(self):
        recs = interior_rows()
        self.assertEqual(self.names(self.placements(recs)[0]), ["chair", "lamp", "table"])
        self.assertEqual(self.names(self.placements(recs, sets="all")[0]), ["chair", "lamp", "rug", "table"])
        self.assertEqual(self.names(self.placements(recs, sets="none")[0]), ["chair", "table"])
        with self.assertRaises(ValueError):
            self.placements(recs, sets="some")

    def test_world_positions_and_counts(self):
        rows, counts = self.placements(interior_rows(inst_rotation=rotation_about_z(90)))
        by_name = dict(zip(self.names(rows), sorted(rows, key=lambda r: self.names([r])[0])))
        self.assertTrue(twp.close(by_name["chair"]["_pos"], [100, 201, 10], 1e-6))   # (1, 0, 0) turned +90
        self.assertTrue(twp.close(by_name["table"]["_pos"], [98, 200, 10], 1e-6))    # (0, 2, 0) turned +90
        self.assertTrue(twp.close(by_name["lamp"]["_pos"], [100, 200, 13], 1e-6))
        self.assertEqual((counts["interior_instances"], counts["interior_entities"], counts["selected"]), (1, 3, 3))

    def test_selection_and_unresolved_instances(self):
        recs = interior_rows()
        orphan = dict(recs[1], instance=f"{world_names.joaat('int_missing'):08x}")
        rows, counts = self.placements(recs + [orphan], extra_lines=["{bad", json.dumps({"other": 1})],
                                       lod_levels=(5,), region=(0, 0, 100.5, 1000))
        # chair lands at x 101 (outside), table at 100 and lamp at 100.
        self.assertEqual(self.names(rows), ["lamp", "table"])
        self.assertEqual((counts["interior_instances"], counts["interior_instances_unresolved"],
                          counts["outside_region"], counts["malformed"]), (2, 1, 1, 2))
        rows, counts = self.placements(recs, lod_levels=(0,))
        self.assertEqual((rows, counts["other_lod_level"]), ([], 3))


class InteriorBuildTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.assets = twp.write_assets(self.tmp)
        self.ents = os.path.join(self.tmp, "e.jsonl")
        twp.write_entities(self.ents, [twp.entity("test_box", (10, 10, 0))])
        self.ints = os.path.join(self.tmp, "i.jsonl")
        recs = interior_rows("int_box", inst_position=(50.0, 60.0, 5.0), inst_rotation=rotation_about_z(90))
        for rec in recs:
            for group in [rec.get("entities")] + [s["entities"] for s in rec.get("sets", ())]:
                for e in group or ():
                    e["archetype"] = f"{world_names.joaat('test_box'):08x}"
                    e["key"] = f"{world_names.hash64('test_box'):016x}"
        twp.write_entities(self.ints, recs)

    def test_build_places_interiors(self):
        for mode in ("gpu", "bake"):
            with self.subTest(mode=mode):
                doc = world_build.build_world(self.assets, os.path.join(self.tmp, "w_" + mode), textures=False,
                                              placements=self.ents, interiors=[self.ints], instancing=mode)
                pl = doc["stats"]["placements"]
                self.assertEqual((pl["rows"], pl["selected"], pl["placed"]), (3, 4, 4))
                self.assertEqual((pl["interior_instances"], pl["interior_entities"]), (1, 3))
        doc = world_build.build_world(self.assets, os.path.join(self.tmp, "w_none"), textures=False,
                                      placements=self.ents, interiors=[self.ints], interior_sets="none")
        self.assertEqual(doc["stats"]["placements"]["placed"], 3)

    def test_option_checks_and_cli(self):
        with self.assertRaisesRegex(ValueError, "--interiors needs --placements"):
            world_build.build_world(self.assets, os.path.join(self.tmp, "w"), interiors=[self.ints])
        with self.assertRaises(FileNotFoundError):
            world_build.build_world(self.assets, os.path.join(self.tmp, "w"), placements=self.ents,
                                    interiors=[os.path.join(self.tmp, "missing.jsonl")])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = world_build.main([self.assets, "--out", os.path.join(self.tmp, "w"), "--no-textures",
                                   "--placements", self.ents, "--interiors", self.ints, "--interior-sets", "all"])
        self.assertEqual(rc, 0)
        self.assertIn("interior_entities 4", out.getvalue())


def setUpModule():
    quiet = contextlib.redirect_stdout(io.StringIO())
    quiet.__enter__()
    unittest.addModuleCleanup(quiet.__exit__, None, None, None)


if __name__ == "__main__":
    unittest.main()
