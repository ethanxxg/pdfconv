"""Have each worker report its own execution time, so scheduling overhead and
actual work can be separated."""
import glob
import os
import time

from concurrent.futures import ProcessPoolExecutor

from service.merge import merge_task

objs = sorted(glob.glob("service/data/objects/*.bin"))[:3]


def timed(args):
    paths, out = args
    t = time.perf_counter()
    n = merge_task(paths, out)
    return time.perf_counter() - t, n, os.getpid()


if __name__ == "__main__":
    os.makedirs("work/_bench", exist_ok=True)
    N = 60
    for workers in (1, 4, 7):
        pool = ProcessPoolExecutor(max_workers=workers)
        t0 = time.perf_counter()
        futs = [pool.submit(timed, (objs, "work/_bench/t_%d_%d.pdf" % (workers, i)))
                for i in range(N)]
        res = [f.result() for f in futs]
        wall = time.perf_counter() - t0
        durs = sorted(r[0] for r in res)
        pids = len({r[2] for r in res})
        print("workers=%d  wall %.2f s  %.1f jobs/s | in-worker median %.3f s "
              "max %.3f s | processes used %d | sum(work)=%.2f s"
              % (workers, wall, N / wall, durs[len(durs) // 2], durs[-1],
                 pids, sum(durs)))
        pool.shutdown()
