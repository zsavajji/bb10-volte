#ifndef ZIP_WRITER_H
#define ZIP_WRITER_H

#include <stddef.h>

/* Writes every regular file in dir into a store-only (uncompressed) .zip at zip_path.
 * The phone has no reliable zip binary, and the debug bundle is text, so deflate would only
 * add code. Returns 0 on success, -1 on any error. zip_path is replaced atomically. */
int zip_directory(const char *dir, const char *zip_path);

#endif /* ZIP_WRITER_H */
