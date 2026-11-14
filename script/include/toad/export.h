#ifndef TOAD_SCRIPT_EXPORT_H
#define TOAD_SCRIPT_EXPORT_H

#ifndef TOAD_SCRIPT_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TOAD_SCRIPT_BUILD_SHARED)
#define TOAD_SCRIPT_API __declspec(dllexport)
#elif defined(TOAD_SCRIPT_USE_SHARED)
#define TOAD_SCRIPT_API __declspec(dllimport)
#else
#define TOAD_SCRIPT_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TOAD_SCRIPT_API __attribute__((visibility("default")))
#else
#define TOAD_SCRIPT_API
#endif
#endif

#ifdef __cplusplus
#define TOAD_SCRIPT_C_API extern "C" TOAD_SCRIPT_API
#else
#define TOAD_SCRIPT_C_API TOAD_SCRIPT_API
#endif

#endif /* TOAD_SCRIPT_EXPORT_H */
