# Development Tools Guide

This document provides a quick reference for the essential command-line tools available in this development environment.

## Version Control: `git`

`git` is used for tracking changes in the source code, allowing you to revisit previous versions and collaborate with others.

- **Check status:** See which files have been modified.
  ```bash
  git status
  ```

- **Stage changes:** Add a file's changes to be included in the next commit.
  ```bash
  git add src/MyFile.cpp
  ```

- **Commit changes:** Save the staged changes with a descriptive message.
  ```bash
  git commit -m "Feat: Implement the new trading algorithm"
  ```

- **View history:** See a log of all previous commits.
  ```bash
  git log
  ```

## Build System: `ninja`

`ninja` is a fast, small build system. You don't typically invoke it directly. Instead, it's used by `cmake` to compile your C++ project after the build files have been generated.

- **Run a build:** From your build directory (e.g., `build-windows`), simply run:
  ```bash
  ninja
  ```
  Ninja will automatically find the necessary files and compile the project, only rebuilding what has changed.

## Fast Code Search: `ripgrep` (`rg`)

`ripgrep` is a line-oriented search tool that recursively searches your current directory for a regex pattern. It is extremely fast and automatically respects your `.gitignore`.

- **Search for text:** Find all occurrences of "MyFunction".
  ```bash
  rg "MyFunction"
  ```

- **Search in a specific file type:** Search for "MyVariable" only in `.cpp` files.
  ```bash
  rg "MyVariable" -g "*.cpp"
  ```

- **Search with context:** Show 3 lines before and after each match.
  ```bash
  rg "some_error_code" -C 3
  ```

## Fast File Finder: `fd`

`fd` is a simple, fast, and user-friendly alternative to `find`. It's great for quickly finding files by name.

- **Find a file by name:**
  ```bash
  fd TripleScreenStudy.cpp
  ```

- **Find files with a specific extension:**
  ```bash
  fd -e h
  ```

- **Find files based on a pattern:** Find all files that start with "Triple".
  ```bash
  fd "^Triple"
  ```
