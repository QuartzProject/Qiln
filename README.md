<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/QuartzProject/.github/main/.github/assets/quartz-logo-rose.svg">
    <img src="https://raw.githubusercontent.com/QuartzProject/.github/main/.github/assets/quartz-logo-ink-rose.svg" alt="Quartz" width="360">
  </picture>
</p>

<h2 align="center">Qiln</h2>

<p align="center">
  <strong>The build tool and package manager of Quartz, configured in Quartz.</strong>
</p>

<p align="center">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white">
  <img alt="Repositories" src="https://img.shields.io/badge/repositories-Maven_layout-C71A36?logo=apachemaven&logoColor=white">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green">
</p>

## About

Qiln builds, tests, runs and publishes [Quartz](https://github.com/QuartzProject/Quartz) packages.

- **Builds in Quartz.** Each project has a `build.qz` and a workspace has a `settings.qz`, much like Gradle's Kotlin
  DSL, type checked so a mistake is a compile error with a line number.
- **Sandboxed.** Build scripts and plugins run in the Quartz sandbox: no files, network or processes beyond what the
  build API allows.
- **Plugins in Quartz.** A build plugin is a Quartz package that adds its own settings and tasks.
- **Maven repositories.** Packages use the Maven layout, so any Maven host can serve them. Every version is signed
  with Ed25519 and pinned by hash in `qiln.lock`.

## Getting started

```console
$ qiln init hello && cd hello
$ qiln test
$ qiln run
Hello, Quartz!
```

```quartz
import org.quartzlang.qiln.*

fn build(p: Project) {
    p.group = "org.example"
    p.version = "0.1.0"
    p.kind = Kind.Application
}
```

Dependencies, workspaces, plugins and publishing are covered in the [Qiln chapter](https://quartzlang.org/docs/qiln.html)
of the Quartz book.

## Building

```bash
cmake --preset release
cmake --build build/release
```

Needs Clang with C++23, CMake 3.28+, Ninja and a checkout of [Quartz](https://github.com/QuartzProject/Quartz) next to
this one (or `-DQILN_QUARTZ_SOURCE=<path>`).

## License

[MIT](LICENSE).
