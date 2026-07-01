PORTNAME=	xepher
PORTVERSION=	0.10.3
DISTVERSIONPREFIX=	v
CATEGORIES=	net-im

MAINTAINER=	Emiel Kollof <emiel@kollof.nl>
COMMENT=	WeeChat plugin for XMPP/Jabber protocol
WWW=		https://github.com/ekollof/xepher

LICENSE=	MPL20
LICENSE_FILE=	${WRKSRC}/LICENSE

LIB_DEPENDS=	libcurl.so:ftp/curl \
		libfmt.so:devel/libfmt \
		libgcrypt.so:security/libgcrypt \
		libgpgme.so:security/gpgme \
		liblmdb.so:databases/lmdb \
		libomemo-c.so:security/libomemo-c \
		libsignal-protocol-c.so:net/libsignal-protocol-c \
		libstrophe.so:net-im/libstrophe \
		libxml2.so:textproc/libxml2
RUN_DEPENDS=	weechat>=4.0:irc/weechat

BUILD_DEPENDS=	bison:devel/bison \
		cmake:devel/cmake \
		flex:devel/flex \
		ninja:devel/ninja \
		weechat:irc/weechat

USES=		compiler:c++23-lang gmake pkgconfig ssl

USE_GITHUB=	yes
GH_ACCOUNT=	ekollof

MAKE_ENV=	PACKAGE_BUILD=1
ALL_TARGET=	weechat-xmpp

pre-build:
	@${ECHO_MSG} "Building vendored libdiff"
	@if [ ! -f ${WRKSRC}/deps/diff/libdiff.a ]; then \
		cd ${WRKSRC}/deps/diff && \
		echo "HAVE___PROGNAME=1" > configure.local && \
		env -u MAKEFLAGS ${SH} ./configure && \
		${SETENV} ${MAKE_ENV} ${MAKE_CMD} CFLAGS="${CFLAGS} -fPIC"; \
	fi

do-install:
	${MKDIR} ${STAGEDIR}${PREFIX}/lib/weechat/plugins
	${INSTALL_PROGRAM} ${WRKSRC}/xmpp.so \
		${STAGEDIR}${PREFIX}/lib/weechat/plugins/xmpp.so
	${MKDIR} ${STAGEDIR}${PREFIX}/share/doc/${PORTNAME}
	${INSTALL_DATA} ${WRKSRC}/README.md \
		${STAGEDIR}${PREFIX}/share/doc/${PORTNAME}/
	${INSTALL_DATA} ${WRKSRC}/LICENSE \
		${STAGEDIR}${PREFIX}/share/licenses/${PORTNAME}

.include <bsd.port.mk>