#!/bin/bash
ulimit -n 200000
./server -l 1 -m 2 -t 3
