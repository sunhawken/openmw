from pathlib import Path
import subprocess,argparse
root=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--output',default='final.so');args=parser.parse_args()
subprocess.run(['g++','-std=c++17','-O2','-shared','-fPIC','-I'+str(root),'-I'+str(root/'includes'),str(root/'bridge.cpp'),'-o',str(root/args.output)],check=True)
