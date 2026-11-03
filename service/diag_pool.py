"""Isolate the 65x slowdown: same merge, inside a ProcessPoolExecutor."""
import glob
import os
import time

from concurrent.futures import ProcessPoolExecutor

from service.merge import merge_task

objs = sorted(glob.glob("service/data/objects/*.bin"))[:3]

# Windows spawns workers by re-importing __main__, so everything that creates a
# pool has to sit behind this guard or the child starts a pool of its own.
if __name__ == "__main__":
    print("inputs:", [os.path.basename(p) for p in objs],
          "%.2f MB" % (sum(os.path.getsize(p) for p in objs) / 1048576.0))

    os.makedirs("work/_bench", exist_ok=True)

    t = time.perf_counter()
    merge_task(objs, "work/_bench/direct.pdf")
    print("direct call          %.3f s" % (time.perf_counter() - t))

    t = time.perf_counter()
    merge_task(objs, "work/_bench/direct2.pdf")
    print("direct call again    %.3f s" % (time.perf_counter() - t))

    N = 60
    for workers in (1, 7):
        pool = ProcessPoolExecutor(max_workers=workers)
        t0 = time.perf_counter()
        futs = [pool.submit(merge_task, objs,
                            "work/_bench/pool_%d_%d.pdf" % (workers, i))
                for i in range(N)]
        t_sub = time.perf_counter() - t0
        per = []
        for f in futs:
            t1 = time.perf_counter()
            f.result()
            per.append(time.perf_counter() - t1)
        wall = time.perf_counter() - t0
        per.sort()
        print("pool workers=%d  submit %.2f s  wall %.2f s  "
              "%.1f jobs/s  first-result wait median %.2f s"
              % (workers, t_sub, wall, N / wall, per[len(per) // 2]))
        pool.shutdown()
