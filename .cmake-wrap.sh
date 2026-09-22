#!/bin/bash
# Wrapper: run cmake with a clean argv[0] (sandbox spoofs argv0 and breaks cmake self-location)
exec python3 -c 'import os,sys; os.execv("/usr/bin/cmake", ["cmake"]+sys.argv[1:])' "$@"
