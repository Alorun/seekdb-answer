#!/usr/bin/env python3
"""Compile bundled construction support without installing or replacing system libraries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler',required=True,type=Path)
    parser.add_argument('--archive',required=True,type=Path)
    parser.add_argument('--include',dest='includes',action='append',default=[])
    args=parser.parse_args()
    root=Path(__file__).resolve().parent
    archive=args.archive.resolve()
    archive.parent.mkdir(parents=True,exist_ok=True)
    manifest=archive.with_suffix('.json')
    stamp=hashlib.sha256()
    for path in sorted(root.rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            stamp.update(str(path.relative_to(root)).encode())
            stamp.update(path.read_bytes())
    stamp.update(str(args.compiler.resolve()).encode())
    stamp.update(json.dumps(args.includes).encode())
    digest=stamp.hexdigest()
    if archive.is_file() and manifest.is_file():
        meta=json.loads(manifest.read_text())
        if meta.get('source_sha256')==digest and meta.get('archive_sha256')==hashlib.sha256(archive.read_bytes()).hexdigest():
            print('OFFLINE_SUPPORT_CACHED='+str(archive),flush=True)
            return
    objects=[]
    for source in sorted((root/'src').glob('*.cpp')):
        obj=archive.parent/(source.stem+'.o')
        print('COMPILING_OFFLINE_SUPPORT='+source.name,flush=True)
        command=[str(args.compiler),'-std=c++17','-O2','-fPIC','-fvisibility=hidden','-pthread',
                 '--offload-arch=gfx1100','-D__HIP_PLATFORM_AMD__','-DLIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE',
                 '-I'+str(root/'include'),*['-I'+p for p in args.includes],'-c',str(source),'-o',str(obj)]
        subprocess.run(command,check=True)
        objects.append(str(obj))
    if len(objects)!=7:
        raise RuntimeError('Expected the six upstream RMM implementation files and one logger source')
    archiver=shutil.which('ar')
    if not archiver:raise RuntimeError('Missing system ar tool')
    temporary=archive.with_suffix('.new.a')
    temporary.unlink(missing_ok=True)
    subprocess.run([archiver,'rcs',str(temporary),*objects],check=True)
    temporary.replace(archive)
    manifest.write_text(json.dumps(dict(source_sha256=digest,
        archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest()),indent=2)+'\n')
    print('OFFLINE_SUPPORT_READY='+str(archive),flush=True)


if __name__=='__main__':main()
