#ifndef PHP_KISLAYPHP_EXTENSION_H
#define PHP_KISLAYPHP_EXTENSION_H

extern "C" {
    #include "php.h"
}

#define PHP_KISLAYPHP_EXTENSION_VERSION "1.0.1"
#define PHP_KISLAYPHP_EXTENSION_EXTNAME "kislayphp_extension"

extern zend_module_entry kislayphp_extension_module_entry;
#define phpext_kislayphp_extension_ptr &kislayphp_extension_module_entry

#endif
