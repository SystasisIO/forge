import sys,os,subprocess,pathlib,time,json
name=sys.argv[1];cmd=sys.argv[2:]
if pathlib.Path(cmd[0]).name == 'cmake' and '-S' in cmd and not any(x.startswith('-DOPENSSL_ROOT_DIR=') for x in cmd):
 cmd += ['-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3','-DOPENSSL_INCLUDE_DIR=/opt/homebrew/opt/openssl@3/include','-DOPENSSL_SSL_LIBRARY=/opt/homebrew/opt/openssl@3/lib/libssl.dylib','-DOPENSSL_CRYPTO_LIBRARY=/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib']
out=pathlib.Path(__file__).resolve().parent
started=time.time();env=os.environ.copy()
env['OPENSSL_ROOT_DIR']='/opt/homebrew/opt/openssl@3'
for key in ['CPATH','CPLUS_INCLUDE_PATH','C_INCLUDE_PATH','OBJC_INCLUDE_PATH']:env.pop(key,None)
with (out/(name+'.log')).open('w') as f:
 p=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,env=env)
record={'name':name,'command':cmd,'cwd':os.getcwd(),'exit_code':p.returncode,'elapsed_seconds':round(time.time()-started,3),'started_unix':started,'environment':{'ambient_include_paths':'removed in child only'}}
(out/(name+'.json')).write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record));print('\n'.join((out/(name+'.log')).read_text(errors='replace').splitlines()[-8:]))
sys.exit(p.returncode)
