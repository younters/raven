# Fork branches

`main` follows `mirkobrombin/raven`'s `main`. Keep macOS port changes on `macos`, the default branch of `younters/raven`.

The **Sync upstream main** GitHub Actions workflow lives on `macos`. It fast-forwards the fork's `main` every six hours, or on a manual run from the Actions page. It stops if `main` contains commits that upstream does not have; it never force-pushes or merges port changes into `main`.

Bring upstream changes into the port separately, resolve platform conflicts, and run the relevant macOS checks before pushing `macos`. Open upstream PRs can be tested on `macos` without adding their unmerged commits to `main`.

This checkout names the upstream remote `origin` and the fork remote `fork`:

```sh
git fetch origin
git switch macos
git merge origin/main
git push fork macos
```

To trigger the mirror manually:

```sh
gh workflow run sync-upstream-main.yml --repo younters/raven --ref macos
```

GitHub can disable scheduled workflows in inactive public repositories. Re-enable the workflow from Actions if that happens.
