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

  dnl -fvisibility=hidden + -DCIVETWEB_API=: civetweb.c exports ~200
  dnl non-static C functions (mg_start, mg_read, ...) with default (public)
  dnl visibility, explicitly re-asserted by civetweb.h's own CIVETWEB_API
  dnl macro regardless of -fvisibility. PHP extension bundles link with
  dnl -flat_namespace on this platform (confirmed in the actual link
  dnl command), so when 2+ extensions that each vendor their own copy of
  dnl civetweb.c are loaded into the same process (e.g. this extension
  dnl alongside gateway or socket, which also embed civetweb), the dynamic
  dnl linker can resolve a call in ONE extension's object code to the
  dnl OTHER extension's same-named symbol - silently running the wrong
  dnl compiled civetweb (confirmed empirically this session while
  dnl debugging kislayphp/socket). Pre-defining CIVETWEB_API as empty
  dnl defers to -fvisibility=hidden instead, without touching the vendored
  dnl header/source; get_module() (PHP's own dlopen()-based loader only
  dnl needs that one symbol) stays exported via ZEND_GET_MODULE's own
  dnl ZEND_DLEXPORT, independent of this flag.
  CFLAGS="$CFLAGS -DOPENSSL_API_3_0 -fvisibility=hidden -DCIVETWEB_API="
  CXXFLAGS="$CXXFLAGS -DOPENSSL_API_3_0 -std=c++17 -fvisibility=hidden -DCIVETWEB_API="

  PHP_ADD_INCLUDE($srcdir/include)
  PHP_NEW_EXTENSION(kislayphp_extension, kislay_extension.cpp src/runtime/event_loop.cpp src/runtime/uv_server.cpp src/runtime/worker_pool.cpp src/runtime/async_bridge.cpp src/runtime/php_runtime.cpp third_party/civetweb/src/civetweb.c third_party/llhttp/src/api.c third_party/llhttp/src/http.c third_party/llhttp/src/llhttp.c, $ext_shared)
fi
