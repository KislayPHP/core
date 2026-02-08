PHP_ARG_ENABLE(kislayphp_extension, whether to enable kislayphp_extension,
[  --enable-kislayphp_extension   Enable kislayphp_extension support])

PHP_ARG_WITH([civetweb],
  [for civetweb support],
  [AS_HELP_STRING([--with-civetweb[=DIR]], [Path to civetweb install prefix])],
  [yes],
  [no])

if test "$PHP_KISLAYPHP_EXTENSION" != "no"; then
  PHP_REQUIRE_CXX()

  if test "$PHP_CIVETWEB" != "no"; then
    if test "$PHP_CIVETWEB" != "yes"; then
      CIVETWEB_DIR=$PHP_CIVETWEB
      PHP_ADD_INCLUDE($CIVETWEB_DIR/include)
    else
      CIVETWEB_INCLUDE_DIR=`pwd`/third_party/civetweb/include
      PHP_ADD_INCLUDE($CIVETWEB_INCLUDE_DIR)
    fi
  fi

  PKG_CHECK_MODULES([OPENSSL], [openssl])
  PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
  PHP_EVAL_LIBLINE($OPENSSL_LIBS, KISLAYPHP_EXTENSION_SHARED_LIBADD)

  CFLAGS="$CFLAGS -DOPENSSL_API_3_0"
  CXXFLAGS="$CXXFLAGS -DOPENSSL_API_3_0"

  PHP_NEW_EXTENSION(kislayphp_extension, kislay_extension.cpp third_party/civetweb/src/civetweb.c, $ext_shared)
fi