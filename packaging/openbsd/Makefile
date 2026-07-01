COMMENT =	WeeChat plugin for XMPP/Jabber protocol

V =		0.10.2
GH_ACCOUNT =	ekollof
GH_PROJECT =	xepher
GH_TAGNAME =	v${V}

CATEGORIES =	net

HOMEPAGE =	https://github.com/ekollof/xepher

MAINTAINER =	Emiel Kollof <emiel@kollof.nl>

# MPL-2.0
PERMIT_PACKAGE =	Yes

WANTLIB += c crypto curl fmt gcrypt gpgme lmdb m omemo-c pthread
WANTLIB += signal-protocol-c ssl strophe xml2 z

# C++23 (project makefile sets -std=c++23)
COMPILER =	base-clang ports-gcc

BUILD_DEPENDS =	devel/bison \
		devel/cmake \
		devel/flex \
		devel/ninja \
		net/weechat

LIB_DEPENDS =	databases/lmdb \
		devel/fmt \
		net/curl \
		net/libomemo-c \
		net/libsignal-protocol-c \
		net/libstrophe \
		security/gpgme \
		security/libgcrypt \
		textproc/libxml

RUN_DEPENDS =	net/weechat

NO_CONFIGURE =	Yes
USE_GMAKE =	Yes

MAKE_FLAGS =	PACKAGE_BUILD=1

pre-build:
	@echo "Building vendored libdiff"
	@if [ ! -f ${WRKSRC}/deps/diff/libdiff.a ]; then \
		cd ${WRKSRC}/deps/diff && \
		echo "HAVE___PROGNAME=1" > configure.local && \
		env -u MAKEFLAGS ${SH} ./configure && \
		${MAKE} ${MAKE_FLAGS} CFLAGS="${CFLAGS} -fPIC"; \
	fi

do-build:
	@cd ${WRKSRC} && ${SETENV} ${MAKE_ENV} ${MAKE_PROGRAM} ${MAKE_FLAGS} weechat-xmpp

do-install:
	${INSTALL_DATA_DIR} ${PREFIX}/lib/weechat/plugins
	${INSTALL_PROGRAM} ${WRKSRC}/xmpp.so ${PREFIX}/lib/weechat/plugins/xmpp.so
	${INSTALL_DATA_DIR} ${PREFIX}/share/doc/xepher
	${INSTALL_DATA} ${WRKSRC}/README.md ${PREFIX}/share/doc/xepher/
	${INSTALL_DATA} ${WRKSRC}/LICENSE ${PREFIX}/share/doc/xepher/

.include <bsd.port.mk>