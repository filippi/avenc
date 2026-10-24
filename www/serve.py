#!/usr/bin/env python3
"""Small static development server with HTTP byte ranges. No PHP/backend decoding."""
import argparse
import functools
import os
import re
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

class RangeHandler(SimpleHTTPRequestHandler):
    def send_head(self):
        path=self.translate_path(self.path)
        if os.path.isdir(path) or not self.headers.get('Range'):
            return super().send_head()
        try:
            f=open(path,'rb')
        except OSError:
            self.send_error(404);return None
        size=os.fstat(f.fileno()).st_size
        match=re.fullmatch(r'bytes=(\d*)-(\d*)',self.headers['Range'])
        if not match or not any(match.groups()):
            f.close();self.send_error(416);return None
        a,b=match.groups()
        start=int(a) if a else max(0,size-int(b))
        end=min(int(b),size-1) if a and b else size-1
        if start>end or start>=size:
            f.close();self.send_response(416);self.send_header('Content-Range',f'bytes */{size}');self.send_header('Content-Length','0');self.end_headers();return None
        self.send_response(206);self.send_header('Content-Type',self.guess_type(path))
        self.send_header('Content-Length',str(end-start+1));self.send_header('Content-Range',f'bytes {start}-{end}/{size}')
        self.send_header('Accept-Ranges','bytes');self.send_header('Last-Modified',self.date_time_string(os.fstat(f.fileno()).st_mtime));self.end_headers()
        f.seek(start);self._remaining=end-start+1;return f

    def copyfile(self,source,outputfile):
        remaining=getattr(self,'_remaining',None)
        try:
            if remaining is None:return super().copyfile(source,outputfile)
            while remaining:
                b=source.read(min(256*1024,remaining))
                if not b:break
                outputfile.write(b);remaining-=len(b)
        except (BrokenPipeError,ConnectionResetError):pass
        finally:self._remaining=None

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',type=int,default=8000);p.add_argument('--bind',default='127.0.0.1')
    p.add_argument('--directory',default=str(Path(__file__).resolve().parent));a=p.parse_args()
    server=ThreadingHTTPServer((a.bind,a.port),functools.partial(RangeHandler,directory=a.directory))
    print(f'ECFlix weather map: http://{a.bind}:{a.port}/index.html',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()
