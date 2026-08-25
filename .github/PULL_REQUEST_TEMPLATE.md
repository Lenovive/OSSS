<!--
Thanks for contributing. Nothing here is a gate you have to pass to open a PR —
it is a checklist so reviewers know what was actually verified and what was not.
Saying "I could not run this, no GPU" is genuinely fine and much better than
leaving a box ambiguous.
-->

## What this changes

<!-- One or two sentences. What behaviour is different afterwards? -->

## Why

<!-- The problem, or a link to the issue. -->

## Checks

- [ ] The platform Release build succeeds with **zero new warnings**
- [ ] The platform Release test suite is green (16 Windows or 13 portable tests)
- [ ] The matching `osss --self-test` passes
      <!-- Required for any change to a shader, the renderer, the overlay, or
           the output clock. Windows shaders compile at runtime, so ctest cannot
           catch an HLSL error and only --self-test will. -->
- [ ] Touches the presentation loop or swap chain, so `--self-test` also passes
      with `--pacing queued` and `--pacing unpaced` — or: not applicable
- [ ] Documentation updated per the ownership table in CONTRIBUTING.md
- [ ] A new or changed CLI flag appears in **both** `--help` and `README.md`,
      and the launcher can produce it — or: no flags changed
- [ ] Frozen fixtures untouched (see CONTRIBUTING.md)

**Could not run:**
<!-- List anything you could not verify, and why. Environment-scoped failures
     are not regressions; just say so. -->

## Measurements

<!--
If this touches interpolation quality, pacing, or performance, put the numbers
here. `osss_interpolation_quality_tests --report` before and after is the
usual evidence; the test-animation harness covers pacing. Name the GPU and the
resolution you measured at.

Compare the fastest call rather than the mean — timing means here are skewed by
background work.

Delete this section if the change cannot affect either.
-->

## Notes for the reviewer

<!-- Anything surprising, any dead end you ruled out, anything you are unsure
     about. Uncertainty flagged here saves a review round trip. -->
