# Releasing hyper-linux

Release publication is deliberately split from preparation. Never use
`git push --tags`; historical local and remote tag objects are not all
identical. Push only the reviewed branch and the explicit new tag.

## Prepare

1. Curate `CHANGELOG.md` under `## [VERSION] - YYYY-MM-DD`.
2. Reconcile `README.md`, `hl.1`, `flake.nix`, the site, installer and Formula.
3. Run the bounded qualification gates:

   ```sh
   timeout 900 nix develop -c make clean hl test-host-units
   timeout 1800 nix develop -c make test-both-modes
   timeout 1800 nix develop -c make test-x64-both-modes
   timeout 1800 nix develop -c make test-matrix
   timeout 900 nix develop .#ci -c make lint shellcheck lint-actions
   timeout 300 nix develop .#ci -c make test-multi-vcpu test-rwx
   timeout 900 nix build
   ```

   The matrix permits only the mode-specific bounded outcomes declared in
   `test/matrix-xfails.sh`: two Rosetta signal outcomes in each x64 mode,
   plus the `test-clock-gettime-efault` exit 139 outcome in `lima-x64`.
   Any other failure or timeout, or an XPASS that makes the policy stale,
   fails the gate. Current full-matrix pass totals must come from the release
   run; no current total is asserted in this document.

4. Push the reviewed commit without a release tag and require green public CI.

## Build a draft

Run `make release-interactive` from a clean `master`. The script understands
stable and `-rcN` versions, selects exact artifact paths, commits metadata,
builds before tagging, and optionally pushes the explicit tag plus a **draft**
GitHub release. A failed build leaves no release tag.

The tag workflow also creates drafts only. No tag path may publish a release
before CI succeeds.

## Publish

After the exact tag CI succeeds, run:

```sh
sh dist/publish-release.sh vVERSION
```

The publisher verifies the local/remote tag commit, successful tag CI, draft
state, Formula URL and SHA-256, and byte-for-byte identity of the local and
draft ZIP, PKG and `SHA256SUMS`. A CI fallback draft contains a separate build;
replace those assets with the locally signed release artifacts before publishing
if the byte check rejects them:

```sh
gh release upload vVERSION dist/out/hl-vVERSION.zip \
  dist/out/hl-vVERSION.pkg dist/out/SHA256SUMS --clobber
```

RC tags remain GitHub prereleases.

After confirmation the publisher updates the Homebrew tap first and only then
makes the GitHub release public, so release-triggered install tests cannot race
a stale tap. If publication fails or the publisher is interrupted after the tap
push, its exit trap reverts and pushes the tap update. Resolve any reported
rollback failure manually before leaving the release draft pending.

For a final 0.3 release, record the exact `hyper-linux-x11` and
`hyper-linux-examples` qualification commits in the release notes.
