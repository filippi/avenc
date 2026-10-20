"""Read one field/window from a local or HTTP AVENC file."""
import argparse
import xarray as xr

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('input', help='Local MP4 path or HTTP(S) URL')
args = parser.parse_args()
with xr.open_dataset(args.input, engine='avenc', cache=False) as ds:
    print(ds)
    name = next(iter(ds.data_vars))
    variable = ds[name]
    # Select one frame and at most 8 cells along each remaining dimension.
    index = {dim: (0 if i == 0 else slice(0, 8))
             for i, dim in enumerate(variable.dims)}
    print(variable.isel(index).load())
