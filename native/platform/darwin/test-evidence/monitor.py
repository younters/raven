import os,sys,subprocess,time,signal,json,shutil,threading
label=sys.argv[1]
base=shutil.disk_usage('.').free
p=subprocess.Popen(sys.argv[2:],start_new_session=True)
low=base
def files():
 with open('/tmp/raven-impl-'+label+'-files.log','w') as out:
  while p.poll() is None:
   out.write(str(time.time())+'\n');out.flush()
   subprocess.run(['lsof','-nP','+L1'],stdout=out,stderr=subprocess.DEVNULL)
   time.sleep(1)
threading.Thread(target=files,daemon=True).start()
with open('/tmp/raven-impl-'+label+'-disk.jsonl','w') as log:
 while p.poll() is None:
  free=shutil.disk_usage('.').free; low=min(low,free)
  log.write(json.dumps({'time':time.time(),'free':free,'extra':base-free})+'\n');log.flush()
  if free<10*1024**3:
   os.killpg(p.pid,signal.SIGTERM)
   print('Stopped at 10 GiB reserve',flush=True);break
  time.sleep(.1)
print(json.dumps({'baseline':base,'minimum_free':low,'peak_extra':base-low,'exit':p.wait()}),flush=True)
sys.exit(p.returncode)
