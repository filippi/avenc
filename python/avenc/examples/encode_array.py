"""Encode a small synthetic DataArray; no external input dataset required."""
import numpy as np
import xarray as xr
from avenc_xarray import compress_dataarray, decompress_to_dataarray

y, x = np.mgrid[:32, :48]
values = np.stack([np.sin(x / 8 + t / 4) + np.cos(y / 7) for t in range(6)])
da = xr.DataArray(values.astype('float32'), dims=('time', 'y', 'x'), name='wave')
compress_dataarray(da, 'example.avenc', codec='avc', quality='high', bits=8,
                   bitmask_enabled=False)
restored = decompress_to_dataarray('example.avenc')
print(restored)
print('RMSE:', float(np.sqrt(((restored.values - da.values) ** 2).mean())))
