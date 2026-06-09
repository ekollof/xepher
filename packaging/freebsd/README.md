# FreeBSD port skeleton

Reference port for submitting **xepher** to the FreeBSD ports tree
(`net-im/xepher`).

## Dependencies

All runtime and build dependencies are available as current FreeBSD packages:

| Role | Port |
|------|------|
| WeeChat | `irc/weechat` |
| libstrophe | `net-im/libstrophe` |
| libsignal-protocol-c | `net/libsignal-protocol-c` |
| libomemo-c | `security/libomemo-c` |
| libfmt | `devel/libfmt` |
| LMDB | `databases/lmdb` |
| GPGME | `security/gpgme` |
| libgcrypt | `security/libgcrypt` |
| libxml2 | `textproc/libxml2` |
| curl | `ftp/curl` |
| bison / flex | `devel/bison`, `devel/flex` |

## Local test build

Copy the port into a ports tree (or use `PORTSDIR`):

```sh
sudo mkdir -p /usr/ports/net-im/xepher
sudo cp packaging/freebsd/* /usr/ports/net-im/xepher/
cd /usr/ports/net-im/xepher
make makesum
make install clean
```

Or build a package without installing:

```sh
make package
```

## Version bump

1. Update `PORTVERSION` in `Makefile`.
2. Run `make makesum` to refresh `distinfo`.
3. Test `make package`.

## Notes

- `PACKAGE_BUILD=1` skips the dev-only `.source` ELF section (see `AGENTS.md`).
- Vendored `deps/diff` is built in `pre-build` (GitHub release tarballs have no `.git`).