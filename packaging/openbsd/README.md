# OpenBSD port skeleton

Reference port for submitting **xepher** to OpenBSD ports (`net/xepher`).

## Dependencies

All runtime and build dependencies are available as current OpenBSD packages:

| Role | Port |
|------|------|
| WeeChat | `net/weechat` |
| libstrophe | `net/libstrophe` |
| libsignal-protocol-c | `net/libsignal-protocol-c` |
| libomemo-c | `net/libomemo-c` |
| fmt | `devel/fmt` |
| LMDB | `databases/lmdb` |
| GPGME | `security/gpgme` |
| libxml | `textproc/libxml` |
| curl | `net/curl` |
| bison / flex | `devel/bison`, `devel/flex` |

Install build dependencies from a live ports tree:

```sh
doas pkg_add gmake git bison flex \
    libstrophe libxml lmdb libsignal-protocol-c libomemo-c fmt gpgme curl weechat
```

## Local test build

Copy the port into your ports tree:

```sh
doas mkdir -p /usr/ports/net/xepher/pkg
doas cp packaging/openbsd/Makefile /usr/ports/net/xepher/
doas cp packaging/openbsd/pkg/* /usr/ports/net/xepher/pkg/
cd /usr/ports/net/xepher
make package
```

Install the resulting package:

```sh
doas pkg_add /usr/ports/packages/$(machine -a)/all/xepher-*
```

## Version bump

1. Update `V` and `GH_TAGNAME` in `Makefile`.
2. Rebuild and test `make package`.

## Notes

- `PACKAGE_BUILD=1` skips the dev-only `.source` ELF section (see `AGENTS.md`).
- Vendored `deps/diff` is built in `pre-build` (GitHub release tarballs have no `.git`).