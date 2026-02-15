# https tests

Run with the core extension loaded:

```sh
PHP_EXTS="-d extension=https/modules/kislayphp_extension.so"
php $PHP_EXTS https/tests/empty_query_test.php
php $PHP_EXTS https/tests/lifecycle_async_test.php
php -n https/run-tests.php -n $PHP_EXTS https/tests/lifecycle_async_test.phpt
php -n https/run-tests.php -n $PHP_EXTS https/tests/lifecycle_wait_invalid_test.phpt
php -n https/run-tests.php -n $PHP_EXTS https/tests/lifecycle_double_listen_test.phpt
php -n https/run-tests.php -n $PHP_EXTS https/tests/express_alignment_test.phpt
```
