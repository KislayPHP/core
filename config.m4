PHP_ARG_ENABLE(kislayphp_extension, whether to enable kislayphp_extension,
[  --enable-kislayphp_extension   Enable kislayphp_extension support])

PHP_ARG_WITH([civetweb],
  [for civetweb support],
  [AS_HELP_STRING([--with-civetweb[=DIR]], [Path to civetweb install prefix])],
  [yes],
  [no])

PHP_ARG_WITH([curl],
  [for curl support],
  [AS_HELP_STRING([--with-curl[=DIR]], [Path to curl install prefix])],
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

  if test "$PHP_CURL" != "no"; then
    if test "$PHP_CURL" != "yes"; then
      CURL_DIR=$PHP_CURL
      PHP_ADD_INCLUDE($CURL_DIR/include)
      PHP_ADD_LIBRARY_WITH_PATH(curl, $CURL_DIR/lib, KISLAYPHP_EXTENSION_SHARED_LIBADD)
    else
      PHP_CHECK_LIBRARY(curl, curl_easy_init,
      [
        PHP_ADD_LIBRARY(curl,, KISLAYPHP_EXTENSION_SHARED_LIBADD)
      ],[
        AC_MSG_ERROR([curl library not found])
      ])
    fi
  fi

  PKG_CHECK_MODULES([OPENSSL], [openssl])
  PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
  PHP_EVAL_LIBLINE($OPENSSL_LIBS, KISLAYPHP_EXTENSION_SHARED_LIBADD)

  PKG_CHECK_MODULES([LIBUV], [libuv])
  PHP_EVAL_INCLINE($LIBUV_CFLAGS)
  PHP_EVAL_LIBLINE($LIBUV_LIBS, KISLAYPHP_EXTENSION_SHARED_LIBADD)

  LLHTTP_INCLUDE_DIR=`pwd`/third_party/llhttp/include
  PHP_ADD_INCLUDE($LLHTTP_INCLUDE_DIR)

  PHP_ADD_LIBRARY(stdc++,, KISLAYPHP_EXTENSION_SHARED_LIBADD)
  PHP_SUBST(KISLAYPHP_EXTENSION_SHARED_LIBADD)

  PKG_CHECK_MODULES([NGHTTP2], [libnghttp2],
  [
    PHP_EVAL_INCLINE($NGHTTP2_CFLAGS)
    PHP_EVAL_LIBLINE($NGHTTP2_LIBS, KISLAYPHP_EXTENSION_SHARED_LIBADD)
    CFLAGS="$CFLAGS -DUSE_HTTP2"
  ],[
    AC_MSG_WARN([libnghttp2 not found — building without HTTP/2 support])
  ])

  CFLAGS="$CFLAGS -DOPENSSL_API_3_0"
  CXXFLAGS="$CXXFLAGS -DOPENSSL_API_3_0 -std=c++17"

  PHP_ADD_INCLUDE($srcdir/include)
  PHP_NEW_EXTENSION(kislayphp_extension, kislay_extension.cpp src/runtime/event_loop.cpp src/runtime/uv_server.cpp src/runtime/worker_pool.cpp src/runtime/async_bridge.cpp src/runtime/php_runtime.cpp third_party/civetweb/src/civetweb.c third_party/llhttp/src/api.c third_party/llhttp/src/http.c third_party/llhttp/src/llhttp.c, $ext_shared)
fi
