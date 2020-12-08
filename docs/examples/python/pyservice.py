#!/usr/bin/env python

"""
One dummy infinite loop
"""

import time

def main():
    print("Python pyservice.py started", flush=True)

    while True:
        time.sleep(5)
        now = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime())
        print("Running : ", now, flush=True)

if __name__ == '__main__':
    main()
