#include "orm_module_loader.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

static orm_status_t loader_result(orm_error_t *error, orm_status_t status,
                                  const char *message) {
  orm_error_set(error, status, message);
  return status;
}

#if defined(_WIN32)
static int loader_windows_absolute(const wchar_t *path) {
  if (path == NULL || path[0] == L'\0') return 0;
  if (((path[0] >= L'A' && path[0] <= L'Z') ||
       (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
    return 1;
  return path[0] == L'\\' && path[1] == L'\\';
}
#endif

orm_status_t orm_module_open_absolute(const char *utf8_path,
                                      orm_module_handle *out,
                                      orm_error_t *error) {
  if (out != NULL) out->native = NULL;
  if (utf8_path == NULL || out == NULL)
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid driver module path");
#if defined(_WIN32)
  const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        utf8_path, -1, NULL, 0);
  if (chars <= 0)
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "driver module path is not valid UTF-8");
  wchar_t *wide = (wchar_t *)calloc((size_t)chars, sizeof(*wide));
  if (wide == NULL)
    return loader_result(error, ORM_STATUS_OUT_OF_MEMORY,
                         "allocate driver module path");
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path, -1,
                          wide, chars) != chars) {
    free(wide);
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "driver module path conversion failed");
  }
  if (!loader_windows_absolute(wide)) {
    free(wide);
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "driver module path must be absolute");
  }
  const DWORD attributes = GetFileAttributesW(wide);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
    free(wide);
    return loader_result(error, ORM_STATUS_DRIVER_MODULE_NOT_FOUND,
                         "driver module file not found");
  }
  HMODULE module = LoadLibraryExW(
      wide, NULL,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  free(wide);
  if (module == NULL)
    return loader_result(error, ORM_STATUS_DRIVER_LOAD_ERROR,
                         "driver module load or dependency resolution failed");
  out->native = (void *)module;
#else
  if (utf8_path[0] != '/')
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "driver module path must be absolute");
  struct stat attributes;
  if (stat(utf8_path, &attributes) != 0 || !S_ISREG(attributes.st_mode))
    return loader_result(error, ORM_STATUS_DRIVER_MODULE_NOT_FOUND,
                         "driver module file not found");
  void *module = dlopen(utf8_path, RTLD_NOW | RTLD_LOCAL);
  if (module == NULL)
    return loader_result(error, ORM_STATUS_DRIVER_LOAD_ERROR,
                         "driver module load or dependency resolution failed");
  out->native = module;
#endif
  return loader_result(error, ORM_STATUS_OK, NULL);
}

orm_status_t orm_module_symbol(const orm_module_handle *module,
                               const char *name,
                               void *out_function, size_t function_bytes,
                               orm_error_t *error) {
  if (out_function != NULL && function_bytes != 0u)
    memset(out_function, 0, function_bytes);
  if (module == NULL || module->native == NULL || name == NULL ||
      out_function == NULL || function_bytes == 0u)
    return loader_result(error, ORM_STATUS_INVALID_ARGUMENT,
                         "invalid driver symbol lookup");
#if defined(_WIN32)
  FARPROC symbol = GetProcAddress((HMODULE)module->native, name);
  if (symbol == NULL)
    return loader_result(error, ORM_STATUS_DRIVER_ENTRY_MISSING,
                         "driver bootstrap entry is missing");
  if (sizeof(symbol) > function_bytes)
    return loader_result(error, ORM_STATUS_INTERNAL_ERROR,
                         "driver function pointer representation is unsupported");
  memcpy(out_function, &symbol, sizeof(symbol));
#else
  (void)dlerror();
  void *symbol = dlsym(module->native, name);
  const char *lookup_error = dlerror();
  if (lookup_error != NULL || symbol == NULL)
    return loader_result(error, ORM_STATUS_DRIVER_ENTRY_MISSING,
                         "driver bootstrap entry is missing");
  if (sizeof(symbol) > function_bytes)
    return loader_result(error, ORM_STATUS_INTERNAL_ERROR,
                         "driver function pointer representation is unsupported");
  memcpy(out_function, &symbol, sizeof(symbol));
#endif
  return loader_result(error, ORM_STATUS_OK, NULL);
}

void orm_module_close(orm_module_handle *module) {
  if (module == NULL || module->native == NULL) return;
#if defined(_WIN32)
  (void)FreeLibrary((HMODULE)module->native);
#else
  (void)dlclose(module->native);
#endif
  module->native = NULL;
}
