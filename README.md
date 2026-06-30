> **This fork ([oovets/gittyup](https://github.com/oovets/gittyup))** extends Gittyup with an
> AI-powered development assistant (code review, chat, commit-message generation,
> codebase-aware context), a built-in **terminal emulator**, a merged repo/changes
> sidebar, and a live status bar. See [AI & Productivity Features](#ai--productivity-features)
> below.

Table of contents
=================
<!--ts-->
   * [AI & Productivity Features](#ai--productivity-features)
   * [Features](#features)
   * [How to Get Help](#how-to-get-help)
   * [Build Environment](#build-environment)
   * [Dependencies](#dependencies)
   * [How to Build](#how-to-build)
   * [How to Install](#how-to-install)
      * [Flatpak from terminal](#flatpak-from-terminal)
   * [How to Contribute](#how-to-contribute)
   * [License](#license)
<!--te-->

AI & Productivity Features
--------------------------

This fork adds a full AI-powered development assistant backed by either
[Ollama](https://ollama.com) (local / LAN) or the Anthropic Claude API, plus a
real integrated terminal and several UI improvements. See the
[CHANGELOG](CHANGELOG.md) for the complete list.

**AI assistant**

- **Code review** — review staged/unstaged diffs or any commit. Findings
  **stream in progressively** with a Stop button and show severity
  (CRITICAL / HIGH / MEDIUM / LOW), file/line references, and suggested fixes. A
  3-step cache (exact diff hash → semantic embedding similarity → full LLM call)
  avoids redundant requests, and the full content of changed files is included
  as context for fewer false positives.
- **Commit message generation** from the current diff (conventional commits).
- **Per-hunk explanations** rendered inline in the diff view.
- **Chat panel** docked in the repo view (`Ctrl+Shift+C`).
- **Knowledge base & codebase RAG** — structured findings, embeddings, and
  fix recipes stored locally in SQLite; the most relevant code chunks are
  injected as context for deeper reviews.
- **Background analysis** of recently-opened repositories on HEAD changes.

The AI engine is built for local/LAN Ollama: **batched embeddings**, parallel
indexing, request **timeouts**, **retry with backoff**, **cancellation**, and a
**task dispatcher** that all AI work (reviews, chat, embeddings, commit
messages) flows through — with live model/queue/token/cache status in the
status bar. See the [CHANGELOG](CHANGELOG.md#ai-engine-improvements) for details.

**Integrated terminal**

A built-in terminal emulator powered by [libvterm](https://www.leonerd.org.uk/code/libvterm/)
(MIT) with a custom Qt renderer — 256-color/true-color, full ANSI rendering,
scrollback, working tab-completion, and resize handling. Select to highlight,
copy/paste (`Cmd+C`/`Cmd+V` or the right-click menu), `Cmd`/`Ctrl`+click to open
URLs, and **"Send selection to Chat"** to ask the AI about terminal output.
Toggle it from **View → Show Terminal** (Ctrl + `` ` ``).

**Merged sidebar & status bar**

The sidebar shows each open repository's changed files as expandable children
under the **OPEN** section (with a change count and per-file status), so changes
live in the same list as the repos themselves. SSH repositories appear with a
distinct icon. A live status bar at the bottom shows branch/tracking state, the
active AI model, Ollama/GPU availability, and the task queue.

How to Get Help
---------------

Ask questions about building or using Gittyup on
[Stack Overflow](http://stackoverflow.com/questions/tagged/gittyup) by
including the `gittyup` tag. Remember to search for existing questions
before creating a new one.

Report bugs in Gittyup by opening an issue in the
[issue tracker](https://github.com/Murmele/gittyup/issues).
Remember to search for existing issues before creating a new one.

If you still need help, check out our Matrix channel
[Gittyup:matrix.org](https://matrix.to/#/#Gittyup:matrix.org).

Build Environment
-----------------

* C++11 compiler
  * Windows - MSVC >= 2017 recommended
  * Linux - GCC >= 6.2 recommended
  * macOS - Xcode >= 10.1 recommended
* CMake >= 3.19
* Ninja (optional)

Dependencies
------------

External dependencies can be satisfied by system libraries or installed
separately. Included dependencies are submodules of this repository. Some
submodules are optional or may also be satisfied by system libraries.

**External Dependencies**

* Qt (required >= 6.6)

**Included Dependencies**

* libgit2 (required)
* cmark (required)
* git (only needed for the credential helpers)
* libssh2 (needed by `libgit2` for SSH support)
* openssl (needed by `libssh2` and `libgit2` on some platforms)

Note that building `OpenSSL` on Windows requires `Perl` and `NASM`.

How to Build
------------

**Initialize Submodules**

    git submodule init
    git submodule update --depth 1

**Build OpenSSL**

    # Start from root of gittyup repo.
    cd dep/openssl/openssl

Windows:

    perl Configure VC-WIN64A
    nmake

macOS (Intel):

    ./Configure darwin64-x86_64-cc no-shared
    make
    
macOS (Apple Silicon)

    ./Configure darwin64-arm64-cc no-shared
    make
    
Linux:

    ./config -fPIC
    make

**Configure Build**

    # Start from root of gittyup repo.
    mkdir -p build/release
    cd build/release
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ../..

If you have Qt installed in a non-standard location, you may have to
specify the path to Qt by passing `-DCMAKE_PREFIX_PATH=<path-to-qt>`
where `<path-to-qt>` points to the Qt install directory that contains
`bin`, `lib`, etc.

**Build**
```
    ninja
```
    
### A Convenient Shell Script for Ubuntu is available [here](https://raw.githubusercontent.com/Murmele/Gittyup/master/pack/buildUbuntu.sh), and will install all the necessary prerequisites, and build a release version for immediate use.

How to Install
-----------------
### Linux

The easiest way to install Gittyup is by using [Flatpak](https://flathub.org/apps/details/com.github.Murmele.Gittyup).

**Arch Linux**

Install the `gittyup` package from the Arch User Repository.

	git clone https://aur.archlinux.org/gittyup.git
	cd gittyup
	makepkg -si

Or use an AUR helper.
Install `gittyup-git` for the VCS build.

### Mac OS

**Homebrew**

Install the `gittyup` cask from [Homebrew](https://formulae.brew.sh/cask/gittyup).

    brew install gittyup

### Flatpak from terminal

If you want a more pure console use, this script run flatpak version disowning the process and silence the output pushing it to /dev/null.
Just save the script somewhere in your path, for example `/usr/bin` (or `~/.local/bin` if you have exported it), give execution permissions `chmod +x`, and run `gittyup` from your terminal.

```bash
#!/bin/bash
DIR=$(dirname "${BASH_SOURCE[0]}")
function run_disown() {
    "$@" & disown
}
function run_disown_silence(){
    run_disown "$@" 1>/dev/null 2>/dev/null
}
run_disown_silence flatpak run com.github.Murmele.Gittyup
```

How to Contribute
-----------------

We welcome contributions of all kinds, including bug fixes, new features,
documentation and translations. By contributing, you agree to release
your contributions under the terms of the license.

Contribute by following the typical
[GitHub workflow](https://docs.github.com/en/get-started/quickstart/github-flow)
for pull requests. Fork the repository and make changes on a new named
branch. Create pull requests against the `master` branch. Follow the
[seven guidelines](https://chris.beams.io/posts/git-commit/) to writing a
great commit message.

Prior to committing a change, please use `cl-fmt.sh` to ensure your code
adheres to the formatting conventions for this project. You can also use the
`setup-env.sh` script to install a pre-commit hook which will automatically
run `clang-format` against all modified files.

Prior to pushing a change, please ensure you run the unit tests to avoid any
regressions. These are run using `ctest` in `<build-dir>`.

License
-------

Gittyup and its predecessor GitAhead are licensed under the MIT license. See LICENSE.md for details.
