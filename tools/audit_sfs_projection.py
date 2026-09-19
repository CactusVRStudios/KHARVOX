from pathlib import Path
import subprocess,struct,json,concurrent.futures,collections,argparse
parser=argparse.ArgumentParser(description='Compile a captured SFS corpus and inventory projection. Successful compilation is not visual correctness; pipeline roles require separate review.')
parser.add_argument('--originals',type=Path,required=True)
parser.add_argument('--profile',type=Path,required=True)
parser.add_argument('--compiler',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
dest=args.output;dest.mkdir(parents=True,exist_ok=True)
exe=args.compiler.resolve()
ui={'2047418e3f6ad5a','5d8a0b69a38eb2a0','bfe07c0adb6207d7','c757868ee21edb47','d7790e0979cc584f'}
def stage(p):
 w=struct.unpack('<%dI'%(p.stat().st_size//4),p.read_bytes());i=5
 while i<len(w):
  n=w[i]>>16
  if w[i]&65535==15:return w[i+1]
  if not n:raise ValueError(p)
  i+=n
def run(job):
 p,mode=job; out=dest/(p.name+'.'+mode+'.spv')
 args=[str(exe),str(p),str(out),mode]
 if p.stem.split('_')[0] in ui:args+=['ui']
 r=subprocess.run(args,capture_output=True,text=True)
 s=Path(str(out)+'.glsl').read_text() if r.returncode==0 else ''
 return dict(file=str(p),mode=mode,stage=stage(p),code=r.returncode,error=r.stderr[:1500],projection='gl_Position = khSfsProjection.clipFromCenter' in s,ui='gl_Position.w > 8.0)' in s,stereo_residual=[x.strip() for x in s.splitlines() if '.stereo.' in x],position=[x.strip() for x in s.splitlines() if 'gl_Position' in x],source=str(out)+'.glsl')
jobs=[]
for p in args.originals.glob('*.spv'):
 jobs.append((p,'generic'))
 if stage(p) in (0,4):jobs.append((p,'mono'))
for p in args.profile.glob('*.spv'):
 jobs.append((p,'profile'))
 if stage(p)==0:jobs.append((p,'profile-shadow'))
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:rows=list(pool.map(run,jobs))
if not rows:raise SystemExit('No shaders found')
(dest/'results.json').write_text(json.dumps(rows,indent=2))
print('jobs',len(rows),'failed',sum(bool(x['code']) for x in rows),'stages',dict(collections.Counter(x['stage'] for x in rows if x['mode'] in ('generic','profile'))))
for x in rows:
 if x['code']:print('FAILED',x['file'],x['mode'],x['error'])
print('unprojected vertices:')
for x in rows:
 if x['stage']==0 and x['mode'] in ('generic','profile') and not x['projection']:print(Path(x['file']).name,x['mode'],' | '.join(x['position'])[:350])
raise SystemExit(any(x['code'] for x in rows))
