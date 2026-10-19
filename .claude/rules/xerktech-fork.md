# xerktech/wolf fork

This checkout is xerktech's fork of games-on-whales/wolf. It builds the Wolf image fenrir's session
pods run (`ghcr.io/xerktech/wolf`, XERK-1944).

## Branches

- `xerktech` is the fork's long-lived branch: upstream `stable` plus our patches, one commit each.
  - Base work and PRs on `origin/xerktech`, never on `beta` (upstream's default branch, which the
    fork's default still points at) — `gh pr create --base xerktech`.
  - PRs into `xerktech` are squash-merged, so keep one patch per PR: the squash commit is the
    patch that a rebase carries forward.
- Upstream every generally useful patch as a PR to games-on-whales/wolf (against `beta`).
  - Fork-only commits carry `(xerktech)` in their subject, e.g. `ci(xerktech): …`; they are never
    upstreamed.

## Image

- `.github/workflows/docker-build.yml` (fork-only) builds and pushes only on a push to `xerktech`
  (or a manual `workflow_dispatch` on a branch), tagging `:<branch>` and `:<branch>-<short sha>`;
  the run summary prints the digest.
  - Never trigger it from PR branches or pull_request: PR CI must not publish images.
  - fenrir pins the image by digest in `charts/direwolf-operator/values.yaml` (`WOLF_IMAGE`).
- `docker/wolf.Dockerfile` builds gst-wayland-display from xerktech/gst-wayland-display, pinned
  to a commit on a `xerk-*` tag (tags keep the commit fetchable whatever its branch does).
  - That commit must sit on the upstream gst-wayland-display commit `stable` pinned; when a
    rebase moves the upstream pin, rebase the gst-wayland-display fix onto it and cut a new tag.
- `mirror-repo.yml` only runs in games-on-whales/wolf; it must never push from the fork.

## Rebasing onto a newer upstream stable

1. `git remote add upstream https://github.com/games-on-whales/wolf` (once), then
   `git fetch upstream origin`.
2. Branch from `origin/xerktech` and `git rebase upstream/stable`. Drop any patch upstream
   has since merged (the rebase usually empties it).
3. If upstream moved `docker/wolf.Dockerfile`'s gst-wayland-display pin or its GStreamer
   `BASE_IMAGE`, update the fork's equivalents (see Image) in the commits that own them.
4. Also refresh the `stable` branch on the fork: `git push origin upstream/stable:stable`.
5. Push the branch, run Docker build on it by hand (`gh workflow run docker-build.yml --ref
   <branch>`), and test that image in fenrir (a talos04
   session streams and survives repeated `/resume`, the XERK-1740 check).
6. Only then move `xerktech`: `git push --force-with-lease=xerktech:<old sha> origin
   <branch>:xerktech`. Tag the old tip first (`xerktech-<yyyy-mm-dd>`) so fenrir's pinned image
   stays traceable to source.
