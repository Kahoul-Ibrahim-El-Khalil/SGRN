// sgrn/scl/src/schema/xml_wrapper.h
#pragma once

#include <sgrn/utils/strings.hpp>
#include <cstdlib>
#include <cstring>

// Redefine allocation macros to include C++ explicit casts for compatibility.
// These must be defined before xml.h is included.
#ifdef XML_CALLOC_FUNC
#undef XML_CALLOC_FUNC
#endif
#define XML_CALLOC_FUNC(nm, sz) static_cast<void*>(calloc(nm, sz))

#ifdef XML_REALLOC_FUNC
#undef XML_REALLOC_FUNC
#endif
#define XML_REALLOC_FUNC(ptr, sz) static_cast<void*>(realloc(ptr, sz))

#ifdef XML_STRNDUP_FUNC
#undef XML_STRNDUP_FUNC
#endif
#define XML_STRNDUP_FUNC(s, n) sgrn::utils::strings::strndup(s, n)

#ifdef XML_FREE_FUNC
#undef XML_FREE_FUNC
#endif
#define XML_FREE_FUNC(ptr) free(ptr)

// Also define the casting helper that the library might use in C++ mode
#ifdef __cplusplus
#define XML_CAST(type, ptr) static_cast<type>(ptr)
#else
#define XML_CAST(type, ptr) ((type)(ptr))
#endif

#include <xml.h>
