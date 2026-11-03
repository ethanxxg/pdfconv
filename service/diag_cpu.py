"""How much real parallelism does this machine give us?

Matters because the capacity model in the README assumes throughput scales with
worker count; if it does not, the number of nodes -- not cores -- is the unit.
"""
import time
import multiprocessing as mp


def burn(n):
    x = 0
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
    return x


N = 3_000_000

if __name__ == "__main__":
    t = time.perf_counter()
    burn(N)
    one = time.perf_counter() - t
    print("1 process, 1 task : %.3f s" % one)
    for k in (2, 4, 8):
        pool = mp.Pool(k)
        t = time.perf_counter()
        pool.map(burn, [N] * k)
        w = time.perf_counter() - t
        pool.close()
        pool.join()
        print("%d parallel tasks : %.3f s   speedup %.2fx   efficiency %.0f%%"
              % (k, w, one * k / w, 100.0 * one * k / w / k))
