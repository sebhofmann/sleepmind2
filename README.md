# SleepMind 2

## Release binaries

Pushing a tag matching `v*` builds and publishes self-contained binaries for
Linux, macOS, and Windows. The workflow can also be started manually to produce
downloadable build artifacts without creating a GitHub release.

Choose the binary for your CPU:

* `arm64` is for Apple Silicon or 64-bit ARM Linux.
* `x64` is the portable choice for any 64-bit Intel/AMD processor.
* `x64-avx2` is faster on processors with AVX2 (roughly Intel Haswell/AMD
  Excavator or newer).
* `x64-avx512` uses AVX-512F and AVX-512BW and only starts on processors that
  support both extensions.

SleepMind selects its NNUE SIMD implementation at compile time, not at runtime.
Running an AVX build on an unsupported processor can terminate with an illegal
instruction; use the portable `x64` archive when unsure.

## Issue workflow

Every issue is implemented and evaluated separately:

1. Start from the unchanged parent commit and create a dedicated issue branch.
2. Run a clean build of both binaries:

   ```sh
   make clean && make both
   ```

3. Before changing code, snapshot the baseline build:

   ```sh
   ./variants.sh create <issue>_base
   ```

4. Implement only the scoped issue, then run the clean build and all tests named
   in the issue.
5. Snapshot the tested implementation:

   ```sh
   ./variants.sh create <issue>_new
   ```

6. Run the mandatory LTC SPRT with the repository defaults:

   ```sh
   TC=60+0.6 ./tournament.sh sprt <issue>_new <issue>_base 2>&1 \
     | tee <issue>_sprt.log
   ```

   The test uses `/home/paschty/Downloads/2moves_v2.pgn`, paired colors, and
   `-repeat`. Unless an issue explicitly says otherwise, use `H0=0`, `H1=+5`,
   and `alpha=beta=0.05`. Always start the SPRT with output logging as shown
   above and keep the complete log file.

7. Push the branch and create a pull request. Record the final W/L/D, Elo
   estimate and error, LLR decision, time control, base commit, and test commit
   in the PR, and attach the SPRT log file to it.
8. Merge only after a passing SPRT. If the SPRT fails, still create the PR with
   the implementation and complete result, then close it without merging. An
   inconclusive SPRT must not be reported as an accepted Elo gain.
