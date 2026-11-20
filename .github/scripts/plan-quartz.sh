#!/usr/bin/env bash
# Prints the Quartz release Qiln builds against (also to $GITHUB_OUTPUT): its tag, or main before the first release.
set -euo pipefail

tag="$(gh release view --repo QuartzProject/Quartz --json tagName --jq .tagName 2>/dev/null || true)"
if [ -n "$tag" ]; then
    ref="$tag"
    version="${tag#v}"
else
    ref=main
    version=0.0
fi

echo "Quartz $version ($ref)"
[ -z "${GITHUB_OUTPUT:-}" ] || { echo "quartz-ref=$ref"; echo "quartz-version=$version"; } >>"$GITHUB_OUTPUT"
