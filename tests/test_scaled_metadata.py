"""Check the current Python metadata reader using native revision-4 fixtures.

The source reader is loaded explicitly so an older editable installation cannot
silently substitute another checkout. Requires the existing AVENC Python runtime.
Run after tests/test_transforms.py with AVENC_SCALED_FIXTURE_DIR set.
"""
import base64
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import zlib
import numpy as np
import avenc_xarray  # existing optional extension/dependency runtime

ROOT=Path(__file__).resolve().parents[1]
def load_current_modules():
    saved={name:sys.modules.get(name) for name in ['avenc_xarray.codec','avenc_xarray.grib_dataset','avenc_xarray.backend']}
    spec=importlib.util.spec_from_file_location('avenc_xarray._scaled_metadata_check',ROOT/'python/avenc/avenc_xarray/codec.py')
    codec=importlib.util.module_from_spec(spec);sys.modules[spec.name]=codec;spec.loader.exec_module(codec)

    sys.modules['avenc_xarray.codec']=codec
    for name in ['grib_dataset','backend']:
        module_spec=importlib.util.spec_from_file_location('avenc_xarray.'+name,ROOT/'python/avenc/avenc_xarray'/f'{name}.py')
        module=importlib.util.module_from_spec(module_spec);sys.modules[module_spec.name]=module;module_spec.loader.exec_module(module)
    backend=module
    return codec,backend,saved

FIXTURES=Path(os.environ['AVENC_SCALED_FIXTURE_DIR']) if os.environ.get('AVENC_SCALED_FIXTURE_DIR') else (Path(sys.argv[1]) if __name__=='__main__' and len(sys.argv)>1 else None)

@unittest.skipUnless(FIXTURES and FIXTURES.is_dir(),'Generate revision-4 fixtures with test_transforms.py and provide their directory')
class MetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.codec,cls.backend,cls.saved=load_current_modules()

    @classmethod
    def tearDownClass(cls):
        for name,module in cls.saved.items():
            if module is None:sys.modules.pop(name,None)
            else:sys.modules[name]=module

    def test_native_revision4_fixtures(self):
        files=list(FIXTURES.glob('*.mp4'))
        self.assertEqual(len(files),4,'generate fixtures using test_transforms.py first')
        for path in files:
            probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_entries','format_tags=comment','-of','json',str(path)]))
            comment=probe['format']['tags']['comment']
            meta=self.codec._decode_metadata_comment(comment)
            self.assertEqual(meta['binary_format_version'],4)
            opts=meta['groups'][0]['options']
            if path.name.startswith('scaled_asinh'):
                self.assertTrue(opts['apply_asinh']);self.assertEqual(opts['transform_scale'],1e-6)
            else:
                self.assertTrue(opts['apply_shifted_log']);self.assertEqual(opts['transform_scale'],99)
                self.assertLess(opts['transform_origin'],0);self.assertGreater(opts['transform_span'],0)
            raw=bytearray(zlib.decompress(base64.b64decode(comment.split('|',1)[1])))
            # Revision-4 parameters begin at stream byte offset 80.
            raw[80:88]=bytes(8)
            bad='AVENCv2|'+base64.b64encode(zlib.compress(raw)).decode()
            with self.assertRaisesRegex(ValueError,'Invalid AVENC transform'):
                self.codec._decode_metadata_comment(bad)

    def test_lazy_xarray_uses_updated_native_decoder(self):
        folder=FIXTURES
        for path in folder.glob('*.mp4'):
            engine=self.backend.AvencBackendEntrypoint()
            with engine.open_dataset(path,avenc2grib_bin=str(ROOT/'bin/avenc2grib')) as ds:
                variable=ds[next(iter(ds.data_vars))]
                values=variable.isel({variable.dims[0]:1,variable.dims[1]:slice(10,13),variable.dims[2]:slice(20,27)}).values
                raw=subprocess.check_output([str(ROOT/'bin/avenc2grib'),'--stream-id','0','--frame-start','1','--frame-count','1','--ni-start','20','--nj-start','10','--width','7','--height','3',str(path)])
                np.testing.assert_array_equal(values,np.frombuffer(raw,dtype='<f4').reshape(3,7))
            with self.assertRaisesRegex(ValueError,'require use_cli_subset'):
                engine.open_dataset(path,avenc2grib_bin=str(ROOT/'bin/avenc2grib'),use_cli_subset_for_packed=False)

if __name__=='__main__':unittest.main(argv=[sys.argv[0]])
