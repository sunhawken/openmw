from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent
subprocess.run(['g++','-std=c++17','-O2','-shared','-fPIC','-I'+str(root),'-I'+str(root/'includes'),str(root/'bridge.cpp'),'-o',str(root/'final.so')],check=True)
