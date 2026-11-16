# Environment of the archived runs

The files in this directory are the unedited output of

```sh
xmake f -m release --cc=gcc-15 --cxx=g++-15
xmake
build/linux/x86_64/release/zipf_experiment                > results/zipf_experiment.md
build/linux/x86_64/release/zipf_experiment --topk-m=2000  > results/zipf_experiment_topk_m2000.md
```

built from the commit that added them.

| | |
|---|---|
| OS | Ubuntu 26.04.1 LTS, Linux 7.0.0-43-generic, x86-64 |
| CPU | Intel Core i9-13900K (24 cores / 32 threads, up to 5.8 GHz), `powersave` governor |
| Memory | 125 GiB |
| Compiler | g++-15 (Ubuntu 15.2.0-16ubuntu1) 15.2.0 |
| Build | xmake v3.1.2, release mode: `-std=c++26 -O3 -DNDEBUG` |
| Seed | 2005 (the default) |

Accuracy, the guarantee flags and memory are deterministic for a given seed and
compiler. Timings are not: every configuration runs once, single-threaded, in a
fixed order, on a desktop machine without CPU pinning. Treat them as indicative
(differences of a few percent between runs are normal), not as a benchmark.
