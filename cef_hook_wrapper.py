import os
import sys
import subprocess

os.chdir('src/cef')
args = sys.argv
args[0] = sys.executable
exit(subprocess.call(args))
