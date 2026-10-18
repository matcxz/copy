# copy

**An independent, lightweight alternative to `cp`.**

`copy` is a C command-line file copier with recursive copying, metadata
preservation, no-clobber mode, aggregate progress, and resumable, verified
regular-file copies.

## Project goals

`copy` aims to be an independent and separately distributed alternative to
`cp`, focused on a small set of useful features rather than reproducing every
option in GNU coreutils. It is a standalone implementation and does not depend
on `cp` to perform copies.

## Recommended projects

- [bat](https://github.com/sharkdp/bat) is an alternative to `cat` with
  features such as syntax highlighting and line numbers.
- [eza](https://github.com/eza-community/eza) is an alternative to `ls` with
  enhanced listings, including colors, icons, and Git information.
- [zoxide](https://github.com/ajeetdsouza/zoxide) is an alternative to `cd`
  that learns frequently used directories and helps you navigate to them.

## License

This project is licensed under the [MIT License](LICENSE).

## Build

```sh
make
```

## Usage

```sh
./copy [OPTIONS] SOURCE... DESTINATION
```

Examples:

```sh
./copy report.txt backup.txt
./copy -rp project/ project-backup/
./copy -nv *.jpg images/
./copy -c large-archive.tar archive-copy.tar
./copy -g video.mp4 /mnt/backup/
./copy --resume -r ./data /media/backup/data
./copy -rpnv project/ project-backup/
./copy -rpcvg project/ project-backup/
```

Short options can be combined in any order: for example, `-rpnv` is equivalent
to `-r -p -n -v`, and `-rpcvg` enables recursive copying, metadata preservation,
no-clobber, verbose output, verification, and progress. Help (`-h`) and version
(`-V`) are standalone options. Multiple sources require an existing destination
directory. Symbolic links are copied as links, and directories require `-r`.
With `--resume`, interrupted regular-file copies can be continued from a hidden
partial file next to the destination. The completed file is verified before it
replaces the destination; mismatched partial data is discarded and recopied.
The destination must be a regular file (not a symbolic link) when resuming.

## Install

Install to `/usr/local/bin` by default:

```sh
sudo make install
```

Choose a different prefix with `make PREFIX="$HOME/.local" install`.
To remove the installed executable, run `sudo make uninstall` with the same
prefix.

## Tests

```sh
make test
```
