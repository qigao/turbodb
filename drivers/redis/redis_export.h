#ifndef REDIS_EXPORT_H
#define REDIS_EXPORT_H

#if defined(REDIS_STATIC)
  #define REDIS_API
#elif defined(_WIN32)
  #if defined(REDIS_BUILD)
    #define REDIS_API __declspec(dllexport)
  #else
    #define REDIS_API __declspec(dllimport)
  #endif
#elif defined(REDIS_BUILD) && (defined(__GNUC__) || defined(__clang__))
  #define REDIS_API __attribute__((visibility("default")))
#else
  #define REDIS_API
#endif

#endif /* REDIS_EXPORT_H */
