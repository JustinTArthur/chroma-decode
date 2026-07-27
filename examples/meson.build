# Examples are plain-C consumer programs, written to be read as much as run.
# They aren't installed, and are off by default.

# Per-frame timing for the neural decoders. Needs an NN backend to be useful,
# but builds either way: without one it reports the missing feature and exits.
executable('nn_benchmark', 'nn_benchmark.c',
           dependencies: [chromadec_dep],
           install: false)
