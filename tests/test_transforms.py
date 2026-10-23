"""Small scientific transform integration tests; no external datasets required."""
import base64
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

import eccodes as ec
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get('AVENC_BIN_DIR', ROOT/'bin'))


class TransformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='avenc-transform-')
        cls.work = Path(cls.temp.name)
        cls.references = {}
        for name in ['signed', 'positive', 'precip_name']:
            path = cls.work/(name+'.grib2'); fields = []
            with path.open('wb') as f:
                for frame in range(3):
                    h = ec.codes_grib_new_from_samples('regular_ll_sfc_grib2')
                    for key, value in {'Ni':128,'Nj':64,'latitudeOfFirstGridPointInDegrees':60.,
                        'longitudeOfFirstGridPointInDegrees':0.,'latitudeOfLastGridPointInDegrees':-3.,
                        'longitudeOfLastGridPointInDegrees':127.,'iDirectionIncrementInDegrees':1.,
                        'jDirectionIncrementInDegrees':1.,'dataDate':20261019,'dataTime':0,
                        'step':frame*3,'shortName':'ro' if name=='positive' else '10u',
                        'bitmapPresent':1,'missingValue':9999.,'bitsPerValue':24}.items():
                        ec.codes_set(h,key,value)
                    y,x=np.mgrid[:64,:128]
                    values=(np.sin(x/5)*np.cos(y/7)*(frame+1)*3).ravel()
                    if name=='positive': values=np.abs(values)/10
                    values[0]=0.;values[7]=9999.
                    ec.codes_set_values(h,values)
                    if name=='precip_name': ec.codes_set(h,'shortName','rdp')
                    fields.append(ec.codes_get_values(h).reshape(64,128))
                    ec.codes_write(h,f);ec.codes_release(h)
            cls.references[name]=(path,np.array(fields))
        cls.videos={}

    @classmethod
    def tearDownClass(cls):
        if os.environ.get('AVENC_TRANSFORM_FIXTURE_DIR'):
            import shutil
            folder=Path(os.environ['AVENC_TRANSFORM_FIXTURE_DIR']);folder.mkdir(exist_ok=True)
            for key,path in cls.videos.items():
                if key[1] in ('asinh','log') and key[2]==2 and key[3]=='frameMinMax' and not key[4]:
                    shutil.copyfile(path,folder/(key[1]+'.mp4'))
                    shutil.copyfile(cls.references[key[0]][0],folder/(key[1]+'.grib2'))
        cls.temp.cleanup()

    def call(self,*args,ok=True):
        p=subprocess.run(list(map(str,args)),capture_output=True)
        if ok:self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'))
        else:self.assertNotEqual(p.returncode,0)
        return p

    def video(self,kind,transform,version=2,quantization='frameMinMax',fast=False):
        key=(kind,transform,version,quantization,fast)
        if key not in self.videos:
            path=self.work/('_'.join(map(str,key))+'.mp4')
            command=[BIN/'grib2avenc','--lossless','--threads','1',
                '--metadata-version',version,'--quantization',quantization]
            if transform:command += ['--'+transform]
            if fast:command += ['--fast-path']
            self.call(*command,self.references[kind][0],path)
            self.videos[key]=path
        return self.videos[key]

    def metadata(self,path):
        p=self.call('ffprobe','-v','error','-show_entries','format_tags=comment','-of','json',path)
        comment=json.loads(p.stdout)['format']['tags']['comment']
        raw=zlib.decompress(base64.b64decode(comment.split('|',1)[1]))
        if comment.startswith('AVENCv2|'):
            return struct.unpack_from('<I',raw,8)[0],struct.unpack_from('<I',raw,68)[0]
        return 1,None

    def decoded(self,path):
        p=self.call(BIN/'avenc2grib','--stream-id',0,path)
        return np.frombuffer(p.stdout,dtype='<f4').reshape(3,64,128)

    def test_default_is_linear_even_for_historical_log_name(self):
        for kind in ['signed','precip_name']:
            path=self.video(kind,None)
            self.assertEqual(self.metadata(path),(2,0))
            explicit=self.video(kind,'linear')
            np.testing.assert_array_equal(self.decoded(path),self.decoded(explicit))

    def test_log_and_asinh_roundtrip_and_full_comparison(self):
        for kind,transform,revision,mode in [('positive','log',2,1),('signed','asinh',3,2)]:
            path=self.video(kind,transform)
            self.assertEqual(self.metadata(path),(revision,mode))
            values=self.decoded(path);expected=self.references[kind][1]
            finite=expected!=9999.
            np.testing.assert_array_equal(np.isnan(values),~finite)
            self.assertLess(np.max(np.abs(values[finite]-expected[finite])),0.05)
            if kind=='signed':
                self.assertLess(values.min(where=np.isfinite(values),initial=0),-1)
                self.assertGreater(np.nanmax(values),1)
            report=self.work/(kind+'.json')
            self.call(BIN/'avenc-compare','--full',self.references[kind][0],path,'--json',report)
            result=json.loads(report.read_text())
            self.assertEqual(result['matching']['matched'],3)
            self.assertEqual(result['groups'][0]['missing_mismatch'],0)
            # GRIB, Float32 subset, and extracted MP4 must apply the same inverse.
            extracted=self.work/(kind+'.subset.mp4')
            self.call(BIN/'avenc2grib','--group','stream:0',path,extracted)
            self.assertEqual(self.metadata(extracted),(revision,mode))
            np.testing.assert_array_equal(self.decoded(extracted),values)
            grib=self.work/(kind+'.decoded.grib2')
            self.call(BIN/'avenc2grib',path,grib)
            with grib.open('rb') as f:
                h=ec.codes_grib_new_from_file(f)
                g=ec.codes_get_values(h).reshape(64,128);ec.codes_release(h)
            np.testing.assert_allclose(g[finite[0]],values[0][finite[0]],atol=2e-5)
            # Zarr uses the CLI's Float32 reconstruction too.
            zar=self.work/(kind+'.zarr')
            self.call(BIN/'avenc2grib','--stream-id',0,'--frame-count',1,'--zarr-out',zar,path)
            self.assertTrue((zar/'zarr.json').exists() or (zar/'.zgroup').exists())

    def test_asinh_with_auto_group_and_fast_path(self):
        expected=self.references['signed'][1];finite=expected!=9999.
        for quantization in ['auto','groupMinMax']:
            path=self.video('signed','asinh',quantization=quantization,fast=True)
            values=self.decoded(path)
            self.assertLess(np.max(np.abs(values[finite]-expected[finite])),0.05)
            np.testing.assert_array_equal(np.isnan(values),~finite)

    def test_legacy_log_v1_and_new_asinh_rejections(self):
        path=self.video('positive','log',version=1)
        values=self.decoded(path);expected=self.references['positive'][1];finite=expected!=9999.
        self.assertLess(np.max(np.abs(values[finite]-expected[finite])),0.003)
        for args in [ ['--log'],['--asinh','--metadata-v1'],['--transform','wrong'] ]:
            p=self.call(BIN/'grib2avenc',*args,self.references['signed'][0],self.work/'invalid.mp4',ok=False)
            self.assertRegex(p.stderr.decode(), 'nonnegative|binary metadata|transform must')
        self.assertFalse((self.work/'invalid.mp4').exists())


    def test_scaled_asinh_and_shifted_log_all_reconstruction_paths(self):
        tiny=self.work/'tiny-signed.grib2';truth=[]
        with self.references['signed'][0].open('rb') as source,tiny.open('wb') as output:
            for _ in range(3):
                h=ec.codes_grib_new_from_file(source)
                values=ec.codes_get_values(h);valid=ec.codes_get_array(h,'bitmap').astype(bool)
                values[valid]*=1e-6;ec.codes_set_values(h,values)
                truth.append(ec.codes_get_values(h).reshape(64,128));ec.codes_write(h,output);ec.codes_release(h)
        expected=np.array(truth);finite=expected!=9999.
        for mode,extra,code in [('scaled_asinh',['--asinh','--asinh-scale','1e-6'],3),
                                ('shifted_log',['--shifted-log','--log-alpha','99'],4)]:
            for quant in ['frameMinMax','groupMinMax']:
                path=self.work/(mode+'_'+quant+'.mp4')
                self.call(BIN/'avpack','--codec','ffv1','--quality','lossless','--chroma','400',
                    '--bits',16,'--threads',1,'--quantization',quant,'--fast-path',*extra,'--out',path,tiny)
                self.assertEqual(self.metadata(path),(4,code))
                values=self.decoded(path)
                np.testing.assert_array_equal(np.isnan(values),~finite)
                self.assertLess(np.max(np.abs(values[finite]-expected[finite])),5e-9)
                self.assertLess(np.nanmin(values),-1e-6);self.assertGreater(np.nanmax(values),1e-6)
                report=self.work/(mode+quant+'.json')
                self.call(BIN/'avenc-compare','--full',tiny,path,'--json',report)
                result=json.loads(report.read_text());self.assertEqual(result['matching']['matched'],3)
                self.assertEqual(result['groups'][0]['missing_mismatch'],0)
                extracted=self.work/(mode+quant+'.subset.mp4')
                self.call(BIN/'avenc2grib','--group','stream:0',path,extracted)
                self.assertEqual(self.metadata(extracted),(4,code))
                np.testing.assert_array_equal(self.decoded(extracted),values)
                grib=self.work/(mode+quant+'.grib2');self.call(BIN/'avenc2grib',path,grib)
                with grib.open('rb') as source:
                    for frame in range(3):
                        h=ec.codes_grib_new_from_file(source);g=ec.codes_get_values(h).reshape(64,128);ec.codes_release(h)
                        np.testing.assert_allclose(g[finite[frame]],values[frame][finite[frame]],rtol=0,atol=1e-11)
                zar=self.work/(mode+quant+'.zarr')
                self.call(BIN/'avenc2grib','--stream-id',0,'--frame-count',1,'--zarr-out',zar,path)
                self.assertTrue((zar/'zarr.json').exists() or (zar/'.zgroup').exists())
                if os.environ.get('AVENC_SCALED_FIXTURE_DIR'):
                    import shutil
                    folder=Path(os.environ['AVENC_SCALED_FIXTURE_DIR']);folder.mkdir(exist_ok=True)
                    shutil.copyfile(path,folder/(mode+'_'+quant+'.mp4'))

    def test_scaled_transform_invalid_parameters(self):
        cases=[['--asinh','--asinh-scale','0'],['--asinh','--asinh-scale','nan'],
               ['--asinh','--asinh-scale','-1'],['--shifted-log','--log-alpha','inf'],
               ['--shifted-log','--log-alpha','0'],['--asinh-scale','1e-6'],
               ['--log-alpha','99'],['--shifted-log','--metadata-v1']]
        for i,args in enumerate(cases):
            p=self.call(BIN/'grib2avenc',*args,self.references['signed'][0],self.work/f'bad-{i}.mp4',ok=False)
            self.assertRegex(p.stderr.decode(),'transform|scale|requires|binary')
        # Verify the wrapper forwards both numerical options to the packer.
        for i,args in enumerate([['--asinh','--asinh-scale','.25'],['--shifted-log','--log-alpha','9']]):
            output=self.work/f'wrapper-{i}.mp4'
            self.call(BIN/'grib2avenc','--lossless','--threads',1,*args,self.references['signed'][0],output)
            self.assertEqual(self.metadata(output)[0],4)
            values=self.decoded(output);self.assertTrue(np.isfinite(values[:,1:,:]).all())

if __name__=='__main__': unittest.main()
