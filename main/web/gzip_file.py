import gzip
import sys

with open(sys.argv[1], "rb") as f:
    data = f.read()
with open(sys.argv[2], "wb") as f:
    f.write(gzip.compress(data, compresslevel=9, mtime=0))
