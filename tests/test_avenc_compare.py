"""Focused native integration checks. Run with the existing AVENC Python environment."""
from __future__ import annotations
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import eccodes as ec
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BIN_DIR = Path(os.environ.get("AVENC_BIN_DIR", ROOT / "bin"))
BIN = BIN_DIR / "avenc-compare"


class CompareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="avenc-compare-")
        cls.work = Path(cls.tmp.name)
        cls.records = []
        cls.shifted = []
        cls.original_values = {}
        for name in ("10u", "2t"):
            for member in (0, 1):
                for step in (0, 3, 6):
                    h = ec.codes_grib_new_from_samples("regular_ll_sfc_grib2")
                    for key, value in {
                        "Ni": 32, "Nj": 24,
                        "latitudeOfFirstGridPointInDegrees": 60.,
                        "longitudeOfFirstGridPointInDegrees": 0.,
                        "latitudeOfLastGridPointInDegrees": 37.,
                        "longitudeOfLastGridPointInDegrees": 31.,
                        "iDirectionIncrementInDegrees": 1.,
                        "jDirectionIncrementInDegrees": 1.,
                        "dataDate": 20260930, "dataTime": 0,
                        "productDefinitionTemplateNumber": 1,
                        "perturbationNumber": member,
                        "numberOfForecastsInEnsemble": 2,
                        "typeOfEnsembleForecast": 0 if member == 0 else 1,
                        "step": step, "shortName": name,
                        "bitmapPresent": 1, "missingValue": 9999.,
                        "bitsPerValue": 24,
                    }.items():
                        ec.codes_set(h, key, value)
                    y, x = np.mgrid[:24, :32]
                    values = (np.sin(x / 6) + np.cos(y / 4) + step / 10 + member / 5
                              + (280 if name == "2t" else 0)).ravel()
                    values[7] = 9999.
                    ec.codes_set_values(h, values)
                    cls.records.append(ec.codes_get_message(h))
                    cls.original_values[(name, member, step)] = ec.codes_get_values(h)
                    shifted = ec.codes_clone(h)
                    actual = ec.codes_get_values(h)
                    actual[actual != 9999.] += 1.
                    ec.codes_set_values(shifted, actual)
                    cls.shifted.append(ec.codes_get_message(shifted))
                    ec.codes_release(shifted)
                    ec.codes_release(h)
        cls.ref = cls.work / "reference.grib"
        cls.ref.write_bytes(b"".join(cls.records))
        cls.reverse = cls.work / "reversed.grib"
        cls.reverse.write_bytes(b"".join(reversed(cls.records)))
        cls.changed = cls.work / "shifted.grib"
        cls.changed.write_bytes(b"".join(reversed(cls.shifted)))
        cls.mp4s = []
        for version in (1, 2):
            mp4 = cls.work / f"packed_v{version}.mp4"
            subprocess.run([
                str(BIN_DIR / "avpack"), "--codec", "avc", "--quality", "high",
                "--bits", "8", "--threads", "1", "--metadata-version", str(version),
                "--out", str(mp4), str(cls.ref),
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            cls.mp4s.append(mp4)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def compare(self, candidate, *args, reference=None, ok=True):
        output = self.work / "metrics.json"
        csv = self.work / "metrics.csv"
        fields_csv = self.work / "fields.csv"
        output.unlink(missing_ok=True)
        p = subprocess.run([
            str(BIN), str(reference or self.ref), str(candidate), "--json", str(output),
            "--csv", str(csv), "--messages-csv", str(fields_csv), "--top", "1", *args,
        ], capture_output=True, text=True)
        if not ok:
            self.assertNotEqual(p.returncode, 0, p.stdout)
            self.assertFalse(output.exists())
            return p
        self.assertEqual(p.returncode, 0, p.stderr)
        result = json.loads(output.read_text())
        import csv as csv_module
        with csv.open() as f:
            self.assertEqual(len(list(csv_module.DictReader(f))), len(result["groups"]))
        return result

    def test_reordered_grib_and_hash(self):
        r = self.compare(self.reverse, "--full", "--quantile-cap", "0")
        self.assertEqual(r["matching"]["matched"], 12)
        self.assertEqual(len(r["groups"]), 2)
        self.assertEqual(r["reference_sources"][0]["sha256"], hashlib.sha256(self.ref.read_bytes()).hexdigest())
        for g in r["groups"]:
            self.assertEqual(g["n"], 6 * (768 - 1))
            self.assertEqual(g["missing_reference"], 6)
            self.assertEqual(g["missing_mismatch"], 0)
            self.assertEqual(g["metrics"]["rmse"], 0)
            self.assertTrue(g["quantiles_exact"])
            self.assertIsNone(g["metrics"]["snr_std_db"])

    def test_known_shift(self):
        r = self.compare(self.changed, "--full", "--quantile-cap", "0")
        for g in r["groups"]:
            m = g["metrics"]
            self.assertAlmostEqual(m["rmse"], 1, places=6)
            self.assertAlmostEqual(m["bias"], 1, places=6)
            self.assertAlmostEqual(m["std_ratio"], 1, places=6)
            self.assertAlmostEqual(m["q95_shift"], 1, places=6)
            self.assertAlmostEqual(m["correlation"], 1, places=10)

    def test_direct_avenc_v1_v2_against_independent_numpy(self):
        lib = ctypes.CDLL(str(next(p for p in (BIN_DIR / "libavenc_subset_decode.dylib", BIN_DIR / "libavenc_subset_decode.so") if p.exists())))
        decode = lib.avenc_decode_subset_to_float32
        decode.argtypes = [ctypes.c_char_p] + [ctypes.c_int] * 7 + [
            ctypes.POINTER(ctypes.POINTER(ctypes.c_float)),
            ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
        ]
        lib.avenc_free_float32.argtypes = [ctypes.POINTER(ctypes.c_float)]
        for mp4 in self.mp4s:
            r = self.compare(mp4, "--full", "--quantile-cap", "0", "--decode-batch", "3")
            self.assertEqual(r["matching"]["matched"], 12)
            for field in r["messages"]:
                pointer = ctypes.POINTER(ctypes.c_float)()
                w, h, n = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
                self.assertEqual(decode(
                    str(mp4).encode(), field["stream"], 0, 0, -1, -1, field["frame"], 1,
                    ctypes.byref(pointer), ctypes.byref(w), ctypes.byref(h), ctypes.byref(n),
                ), 0)
                try:
                    decoded = np.ctypeslib.as_array(pointer, shape=(w.value * h.value,)).astype(float)
                finally:
                    lib.avenc_free_float32(pointer)
                handle = ec.codes_new_from_message(self.records[field["reference_message"]])
                original = ec.codes_get_values(handle)
                ec.codes_release(handle)
                original[original == 9999.] = np.nan
                mask = np.isfinite(original) & np.isfinite(decoded)
                error = decoded[mask] - original[mask]
                self.assertAlmostEqual(field["metrics"]["rmse"], np.sqrt(np.mean(error**2)), places=10)
                self.assertEqual(field["missing_mismatch"], np.count_nonzero(np.isfinite(original) != np.isfinite(decoded)))

    def test_deterministic_sampling_and_member_groups(self):
        args = ("--frames-per-group", "3", "--points-per-frame", "17", "--seed", "42", "--quantile-cap", "20")
        a = self.compare(self.reverse, *args)
        b = self.compare(self.mp4s[1], *args)
        self.assertEqual({x["identity"] for x in a["messages"]}, {x["identity"] for x in b["messages"]})
        c = self.compare(self.reverse, *args)
        self.assertEqual(a["groups"], c["groups"])
        self.assertTrue(all(x["points_examined"] == 17 for x in a["messages"]))
        self.assertTrue(all(not x["quantiles_exact"] for x in a["groups"]))
        d = self.compare(self.reverse, "--group-by-member", "--frames-per-group", "1")
        self.assertEqual(len(d["groups"]), 4)

    def test_fast_sampling_coverage_and_overrides(self):
        a = self.compare(self.reverse, "--fast")
        self.assertEqual(a["configuration"]["mode"], "fast")
        self.assertEqual(a["configuration"]["frames_per_group"], 7)
        self.assertEqual(a["configuration"]["points_per_frame"], 8192)
        self.assertEqual(a["configuration"]["quantile_cap"], 16384)
        self.assertFalse(a["configuration"]["hashes_enabled"])
        self.assertIsNone(a["reference_sources"][0]["sha256"])
        for group in a["groups"]:
            fields = [f for f in a["messages"] if f["group"] == group["group"]]
            self.assertEqual(len(fields), 6)
            self.assertEqual({f["coordinates"]["endStep"] for f in fields}, {"0", "3", "6"})
            self.assertEqual(len({f["coordinates"]["number"] for f in fields}), 2)
        three = self.compare(self.reverse, "--fast", "--frames-per-group", "3")
        for group in three["groups"]:
            selected = [f for f in three["messages"] if f["group"] == group["group"]]
            self.assertEqual(len(selected), 3)
            self.assertEqual({f["coordinates"]["endStep"] for f in selected}, {"0", "3", "6"})
            self.assertEqual(len({f["coordinates"]["number"] for f in selected}), 2)
        args = ("--fast", "--points-per-frame", "64", "--seed", "42")
        a = self.compare(self.reverse, *args)
        b = self.compare(self.mp4s[1], *args)
        self.assertEqual({f["identity"]: f["points_examined"] for f in a["messages"]},
                         {f["identity"]: f["points_examined"] for f in b["messages"]})
        self.assertTrue(all(f["points_examined"] == 64 for f in b["messages"]))
        c = self.compare(self.reverse, "--points-per-frame", "64", "--seed", "42", "--fast")
        self.assertEqual(a["groups"], c["groups"])
        d = self.compare(self.reverse, "--fast", "--hash", "--frames-per-group", "1")
        self.assertTrue(d["configuration"]["hashes_enabled"])
        self.assertEqual(d["configuration"]["frames_per_group"], 1)
        self.assertEqual(d["reference_sources"][0]["sha256"], hashlib.sha256(self.ref.read_bytes()).hexdigest())
        self.assertIn("mutually exclusive", self.compare(self.reverse, "--fast", "--full", ok=False).stderr)

    def test_fast_coalesced_decode_matches_single_frame_reads(self):
        args = ("--fast", "--group-by-member", "--frames-per-group", "2",
                "--points-per-frame", "64", "--seed", "42")
        batched = self.compare(self.mp4s[1], *args, "--decode-batch", "8")
        single = self.compare(self.mp4s[1], *args, "--decode-batch", "1")
        self.assertEqual(batched["messages"], single["messages"])
        self.assertEqual(batched["groups"], single["groups"])
        self.assertGreater(batched["timing"]["candidate_points_decoded"],
                           single["timing"]["candidate_points_decoded"])

    def test_duplicate_and_missing_are_explicit(self):
        duplicate = self.work / "duplicate.grib"
        duplicate.write_bytes(self.ref.read_bytes() + self.records[0])
        self.assertIn("duplicate identity", self.compare(duplicate, ok=False).stderr)
        d = self.compare(duplicate, "--duplicate-policy", "first")
        self.assertEqual(d["matching"]["candidate_duplicates"], 1)
        missing = self.work / "missing.grib"
        missing.write_bytes(b"".join(self.records[1:]))
        self.assertIn("1 missing", self.compare(missing, ok=False).stderr)
        d = self.compare(missing, "--allow-unmatched")
        self.assertEqual(len(d["matching"]["missing"]), 1)

    def test_directory_input_and_wrong_grid_rejection(self):
        directory = self.work / "members"
        directory.mkdir(exist_ok=True)
        (directory / "a.grib").write_bytes(b"".join(self.records[:6]))
        (directory / "b.grib").write_bytes(b"".join(self.records[6:]))
        d = self.compare(self.reverse, reference=directory)
        self.assertEqual(d["matching"]["matched"], 12)
        self.assertEqual(len(d["reference_sources"]), 2)
        h = ec.codes_new_from_message(self.records[0])
        ec.codes_set(h, "longitudeOfFirstGridPointInDegrees", 1.)
        wrong = self.work / "wrong_grid.grib"
        wrong.write_bytes(ec.codes_get_message(h) + b"".join(self.records[1:]))
        ec.codes_release(h)
        self.assertIn("1 missing, 1 extra", self.compare(wrong, ok=False).stderr)

    def test_mask_mismatch_and_all_missing(self):
        h = ec.codes_new_from_message(self.records[0])
        values = ec.codes_get_values(h)
        values[8] = 9999.
        ec.codes_set_values(h, values)
        altered = self.work / "mask_mismatch.grib"
        altered.write_bytes(ec.codes_get_message(h) + b"".join(self.records[1:]))
        ec.codes_release(h)
        r = self.compare(altered, "--full")
        self.assertEqual(sum(g["missing_mismatch"] for g in r["groups"]), 1)
        self.assertEqual(sum(g["n"] for g in r["groups"]), 12 * 767 - 1)
        h = ec.codes_new_from_message(self.records[0])
        ec.codes_set_values(h, np.full(768, 9999.))
        empty = self.work / "all_missing.grib"
        empty.write_bytes(ec.codes_get_message(h))
        ec.codes_release(h)
        r = self.compare(empty, "--full", reference=empty)
        self.assertEqual(r["groups"][0]["n"], 0)
        self.assertIsNone(r["groups"][0]["metrics"]["rmse"])

    def test_literal_mp4_path(self):
        unusual = self.work / ("packed 'space $dollar " + chr(96) + "tick" + chr(96) + ".mp4")
        unusual.write_bytes(self.mp4s[1].read_bytes())
        r = self.compare(unusual, "--frames-per-group", "1", "--points-per-frame", "10")
        self.assertEqual(r["matching"]["matched"], 12)

    def test_output_cannot_overwrite_input(self):
        before = self.ref.read_bytes()
        p = subprocess.run([str(BIN), str(self.ref), str(self.reverse), "--json", str(self.ref)],
                           capture_output=True, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(before, self.ref.read_bytes())


if __name__ == "__main__":
    unittest.main()
