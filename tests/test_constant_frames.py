"""Constant frames in varying scientific streams: exact bounds, values and masks."""
import json
import base64
import zlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import eccodes as ec
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
BIN=Path(os.environ.get('AVENC_BIN_DIR',ROOT/'bin'))

class ConstantFrames(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='avenc-constant-')
        self.work=Path(self.temp.name)
    def tearDown(self):self.temp.cleanup()
    def call(self,*args,**kw):
        p=subprocess.run(list(map(str,args)),capture_output=True,**kw)
        self.assertEqual(p.returncode,0,p.stderr.decode(errors='replace'))
        return p.stdout
    def source(self,mask='none',all_constant=False):
        path=self.work/('source-'+mask+str(all_constant)+'.grib2'); expected=[]
        with path.open('wb') as f:
            for frame in range(4):
                h=ec.codes_grib_new_from_samples('regular_ll_sfc_grib2')
                for key,value in {'Ni':32,'Nj':17,'latitudeOfFirstGridPointInDegrees':60.,
                    'longitudeOfFirstGridPointInDegrees':0.,'latitudeOfLastGridPointInDegrees':44.,
                    'longitudeOfLastGridPointInDegrees':31.,'iDirectionIncrementInDegrees':1.,
                    'jDirectionIncrementInDegrees':1.,'dataDate':20261019,'dataTime':0,
                    'step':frame*3,'shortName':'10u','bitsPerValue':24}.items():ec.codes_set(h,key,value)
                values=np.full(32*17,-2.25 if all_constant else [0,0,7.5,-2.25][frame],dtype=float)
                if frame==1 and not all_constant:values=np.linspace(-1000,1000,32*17)
                if mask!='none':
                    ec.codes_set(h,'bitmapPresent',1);ec.codes_set(h,'missingValue',9999.)
                    values[[1,5] if mask=='shared' else [frame+1]]=9999.
                ec.codes_set_values(h,values)
                truth=ec.codes_get_values(h).reshape(17,32)
                truth[truth==9999.]=np.nan
                expected.append(truth.copy());ec.codes_write(h,f);ec.codes_release(h)
        return path,np.array(expected)
    def encode(self,source,name,bits=16,quant='frameMinMax',transform='linear',version=2):
        video=self.work/(name+'.mp4')
        self.call(BIN/'avpack','--codec','ffv1','--encoder','ffv1','--quality','lossless',
            '--bits',bits,'--chroma','400','--quantization',quant,'--transform',transform,
            '--metadata-version',version,'--threads',1,'--fast-path','--out',video,source)
        return video
    def check(self,video,truth,nonlinear=False):
        subset=np.frombuffer(self.call(BIN/'avenc2grib','--stream-id',0,video),dtype='<f4').reshape(truth.shape)
        grib=self.work/(video.stem+'.grib2');self.call(BIN/'avenc2grib',video,grib)
        reconstructed=[]
        with grib.open('rb') as f:
            while True:
                h=ec.codes_grib_new_from_file(f)
                if h is None:break
                a=ec.codes_get_values(h).reshape(17,32);a[a==9999.]=np.nan
                reconstructed.append(a);ec.codes_release(h)
        reconstructed=np.array(reconstructed)
        np.testing.assert_array_equal(np.isnan(subset),np.isnan(truth))
        np.testing.assert_array_equal(np.isnan(reconstructed),np.isnan(truth))
        constants=[i for i,a in enumerate(truth) if np.nanmin(a)==np.nanmax(a)]
        for i in constants:
            finite=np.isfinite(truth[i])
            if nonlinear:
                # Inverse shifted-log can introduce tiny cancellation at zero.
                np.testing.assert_allclose(subset[i][finite],truth[i][finite].astype('f4'),rtol=0,atol=1e-10)
            else:np.testing.assert_array_equal(subset[i][finite],truth[i][finite].astype('f4'))
            if nonlinear:np.testing.assert_allclose(reconstructed[i][finite],truth[i][finite],rtol=0,atol=1e-5)
            else:np.testing.assert_array_equal(reconstructed[i][finite],truth[i][finite])
        head=self.call(BIN/'avenc2grib','--head',video).decode()
        records=[line.split('\t') for line in head.splitlines() if line.startswith('RECORD\t')]
        if records:
            for i in constants:self.assertEqual(float(records[i][15]),float(records[i][16]))
        else:
            probe=json.loads(self.call('ffprobe','-v','error','-show_entries','format_tags=comment','-of','json',video))
            comment=probe['format']['tags']['comment']
            meta=json.loads(zlib.decompress(base64.b64decode(comment.split('|',1)[1])))
            rows=meta['groups'][0]['records']
            for i in constants:self.assertEqual(rows[i]['quantization_min'],rows[i]['quantization_max'])
        return subset
    def test_linear_ranges_depths_and_bitmaps(self):
        for mask in ['none','shared','changing']:
            source,truth=self.source(mask)
            for bits in [8,16]:
                for quant in ['frameMinMax','groupMinMax','auto']:
                    with self.subTest(mask=mask,bits=bits,quant=quant):
                        self.check(self.encode(source,'%s-%s-%s'%(mask,bits,quant),bits,quant),truth)
    def test_nonlinear_and_metadata_v1_constants(self):
        source,truth=self.source('shared')
        for mode in ['asinh','shifted-log']:
            self.check(self.encode(source,mode,transform=mode),truth,nonlinear=True)
        self.check(self.encode(source,'v1',version=1),truth)
    def test_entire_stream_constant(self):
        source,truth=self.source('shared',all_constant=True)
        self.check(self.encode(source,'allconstant',quant='groupMinMax'),truth)
    def test_python_xarray_packed_reader(self):
        try:
            import test_scaled_metadata as helpers
        except ImportError as exc:self.skipTest('Optional Python extension runtime unavailable: '+str(exc))
        _,backend,saved=helpers.load_current_modules()
        try:
            source,truth=self.source('shared');video=self.encode(source,'python')
            with backend.AvencBackendEntrypoint().open_dataset(video,avenc2grib_bin=str(BIN/'avenc2grib')) as ds:
                variable=ds[next(iter(ds.data_vars))]
                for i in [0,2,3]:
                    np.testing.assert_array_equal(variable.isel({variable.dims[0]:i}).values,truth[i].astype('f4'))
        finally:
            import sys
            for name,module in saved.items():
                if module is None:sys.modules.pop(name,None)
                else:sys.modules[name]=module
    def test_arbitrary_constant_pixels_and_zarr_exports(self):
        source,truth=self.source();video=self.encode(source,'original')
        probe=json.loads(self.call('ffprobe','-v','error','-show_entries','format_tags=comment','-of','json',video))
        raw=self.call('ffmpeg','-v','error','-i',video,'-f','rawvideo','-pix_fmt','gray16le','pipe:1')
        pixels=np.frombuffer(raw,dtype='<u2').copy().reshape(4,18,32)
        # Simulate codec perturbations: numerical constants must ignore all codes.
        for i in [0,2,3]:pixels[i]=np.arange(18*32,dtype=np.uint16).reshape(18,32)*97
        modified=self.work/'perturbed.mp4'
        self.call('ffmpeg','-v','error','-f','rawvideo','-pix_fmt','gray16le','-s','32x18','-r','1','-i','pipe:0',
            '-c:v','ffv1','-level','3','-threads','1','-metadata','comment='+probe['format']['tags']['comment'],modified,input=pixels.tobytes())
        self.check(modified,truth)
        for subset in [False,True]:
            zar=self.work/('subset.zarr' if subset else 'full.zarr')
            if subset:self.call(BIN/'avenc2grib','--stream-id',0,'--zarr-out',zar,modified)
            else:self.call(BIN/'avenc2grib',modified,zar)
            fields=[p.parent for p in zar.rglob('.zarray') if json.loads(p.read_text()).get('dtype')=='<f4']
            self.assertEqual(len(fields),1)
            for i in [0,2,3]:
                a=np.fromfile(fields[0]/('%d.0.0'%i),dtype='<f4').reshape(17,32)
                np.testing.assert_array_equal(a,truth[i].astype('f4'))

if __name__=='__main__':unittest.main()
