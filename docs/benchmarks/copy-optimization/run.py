import subprocess,os,csv,pathlib,json,hashlib,time,argparse
p=argparse.ArgumentParser()
p.add_argument("--before",type=pathlib.Path,required=True)
p.add_argument("--after",type=pathlib.Path,required=True)
p.add_argument("--ankerl",type=pathlib.Path,required=True)
p.add_argument("--output",type=pathlib.Path,required=True)
p.add_argument("--cpu",type=int,default=0)
p.add_argument("--regression-repeats",type=int,default=11)
a=p.parse_args()
root=a.output.resolve();root.mkdir(parents=True,exist_ok=True)
bins={k:getattr(a,k).resolve() for k in ['before','after','ankerl']}
fields=['map','hash','test','measurement_id','measurement','actual_result','runtime_s','legacy_peak_delta_mib','rss_current_mib','rss_minus_input_baseline_mib','rss_peak_mib','rss_input_baseline_mib','rss_peak_minus_input_baseline_mib']
rows=[]
def run(label,case,n,repeat):
 env=dict(os.environ,MAP_BENCH_EXTRA_MEMORY='1',MAP_BENCH_N=str(n),MAP_BENCH_SEED='42')
 p=subprocess.run(['taskset','-c',str(a.cpu),str(bins[label]),case],env=env,text=True,capture_output=True,timeout=180)
 (root/f'{case}-{n}-{label}-{repeat}.stdout').write_text(p.stdout)
 (root/f'{case}-{n}-{label}-{repeat}.stderr').write_text(p.stderr)
 if p.returncode: raise RuntimeError((label,case,p.returncode,p.stderr))
 for values in csv.reader(p.stdout.splitlines(),delimiter=';',skipinitialspace=True):
  if len(values)!=13:raise RuntimeError(values)
  row=dict(zip(fields,values));row.update(variant=label,n=n,repeat=repeat);rows.append(row)
 print(label,case,n,repeat,[(r['measurement'],r['runtime_s']) for r in rows[-(2 if case=='Copy' else 3 if case=='FlatMap64Reserved' else 1):]],flush=True)
 with (root/'measurements.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=['variant','n','repeat']+fields);w.writeheader();w.writerows(rows)
for rep in range(5):
 order=['before','after','ankerl'] if rep%2==0 else ['ankerl','after','before']
 for label in order:run(label,'Copy',1000000,rep)
for n in [1000000,10000000]:
 for rep in range(a.regression_repeats):
  for case in ['FlatMap64Reserved','FlatMap64Grow']:
   for label in (['before','after'] if rep%2==0 else ['after','before']):run(label,case,n,rep)
(root/'binary-sha256.json').write_text(json.dumps({k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in bins.items()},indent=2))
