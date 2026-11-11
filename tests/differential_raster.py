"""Independent exact-rational geometry/coverage oracle for ReferenceV1.

The fixture runs VS/FS machine code and all five commands. This script never
calls C++ clipping/setup: it derives six-plane clipping, 1/256 RNE positions,
top-left coverage, and expected D32 bytes from rational input independently.
Only constant per-triangle depth is used here; varying/perspective/Late-Z side
effects, scissor and pause/resume have separate C++ tests.
"""
from fractions import Fraction as F
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

BLACK, RED = 0xff000000, 0xff0000ff


def clip(vertices):
    planes = [lambda p:p[0]+p[3],lambda p:p[3]-p[0],lambda p:p[1]+p[3],
              lambda p:p[3]-p[1],lambda p:p[2],lambda p:p[3]-p[2]]
    polygon = vertices
    for plane in planes:
        result = []
        for a,b in zip(polygon,polygon[1:]+polygon[:1]):
            da,db = plane(a),plane(b)
            if da >= 0:
                result.append(a)
            if (da >= 0) != (db >= 0):
                t = da/(da-db)
                result.append(tuple(x+t*(y-x) for x,y in zip(a,b)))
        polygon = result
    return [(polygon[0],polygon[i],polygon[i+1]) for i in range(1,len(polygon)-1)]


def edge(a,b,p):
    return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])


def covered(triangle,flags):
    points = [(round((p[0]/p[3]+1)*4*256),round((p[1]/p[3]+1)*4*256)) for p in triangle]
    area = edge(points[0],points[1],points[2])
    if area == 0 or flags & 4 and area > 0:
        return set()
    if area < 0:
        points[1],points[2] = points[2],points[1]
    output = set()
    for y in range(8):
        for x in range(8):
            inside = True
            for a,b in zip(points,points[1:]+points[:1]):
                value = edge(a,b,(x*256+128,y*256+128))
                # Interior-positive, top-left predicate derived in Y-down space.
                tie = b[1]<a[1] or b[1]==a[1] and b[0]>a[0]
                if value < 0 or value == 0 and not tie:
                    inside = False
            if inside:
                output.add(y*8+x)
    return output


def reference(vertices,flags):
    colors,depth = [BLACK]*64,[F(1)]*64
    fragments = 0
    for i in range(0,len(vertices),3):
        for triangle in clip(vertices[i:i+3]):
            # Constant clip z/w remains constant after clipping.
            z = triangle[0][2]/triangle[0][3]
            assert all(p[2]/p[3] == z for p in triangle)
            for pixel in covered(triangle,flags):
                fragments += 1
                if flags & 1 and not z < depth[pixel]:
                    continue
                colors[pixel] = RED
                if flags & 2:
                    depth[pixel] = z
    depth_bits = [struct.unpack('<I',struct.pack('<f',float(value)))[0] for value in depth]
    return colors,depth_bits,fragments


def main():
    executable = Path(sys.argv[1] if len(sys.argv)>1 else 'build/animus_submission_demo.exe').resolve()
    cases = [[],[(-.5,-.5,.5,1),(.5,-.5,.5,1),(-.5,.5,.5,1)],
             [(-2,-.5,.5,1),(.5,-.5,.5,1),(-.5,.5,.5,1)],
             [(-.5,-.5,.5,1),(2,-.5,.5,1),(-.5,2,.5,1)],
             [(-.5,-.5,2,1),(.5,-.5,2,1),(-.5,.5,2,1)],
             [(0,0,.5,1),(0,0,.5,1),(0,0,.5,1)],
             [(-.5,-.5,.5,1),(.5,-.5,.5,1),(-.5,.5,.5,1),
              (.5,-.5,.5,1),(.5,.5,.5,1),(-.5,.5,.5,1)]]
    # Near/far-plane crossings have nonconstant z/w, so their exact clipping is
    # tested separately in C++ and not hidden behind a wrong constant-depth oracle.
    rng = random.Random(20261107)
    while len(cases)<40:
        triangles=[]
        for _ in range(rng.randint(1,3)):
            w = rng.choice([.5,1,2])
            z = rng.choice([0,.25,.5,.75,1])
            triangles += [(rng.randint(-12,12)/8*w,rng.randint(-12,12)/8*w,z*w,w) for _ in range(3)]
        cases.append(triangles)
    directory=Path('build'); directory.mkdir(exist_ok=True)
    count=0
    for index,vertices in enumerate(cases):
        raw=b''.join(struct.pack('<4f',*p) for p in vertices)
        # Reference the actual FP32 bytes supplied, not wider original Python values.
        exact=[tuple(F(v) for v in p) for p in struct.iter_unpack('<4f',raw)]
        (directory/'raster_oracle_vertices.bin').write_bytes(raw)
        for flags in (0,3,7):
            result=subprocess.run([str(executable),'--vertices',str(directory/'raster_oracle_vertices.bin'),
                '--output',str(directory/'raster_oracle.bin'),'--flags',str(flags)],capture_output=True,text=True,check=True)
            summary=json.loads(result.stdout)
            colors,depth,fragments=reference(exact,flags)
            actual=list(struct.unpack('<64I',(directory/'raster_oracle.bin').read_bytes()))
            actual_depth=list(struct.unpack('<64I',(directory/'raster_oracle.bin.depth.bin').read_bytes()))
            assert actual==colors,('color',index,flags,actual,colors)
            assert actual_depth==depth,('depth',index,flags,actual_depth,depth)
            assert summary['status']==2 and summary['error_code']==0 and summary['commands']==5
            assert summary['fence']==0x12345678 and summary['fragments']==fragments
            count+=1
    print(f'PASS raster Fraction oracle: {count} submissions, exact color/depth/coverage and completion')


if __name__=='__main__':
    main()
