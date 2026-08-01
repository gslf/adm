#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #include <windows.h>
#endif

#include "fileio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static int replace(const char *tmp, const char *dst){

  #ifdef _WIN32
    // rename() on Windows refuses to overwrite an existing file, so the
    // replace has to go through MoveFileEx and its explicit flag.
    if (MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING))
      return 0;
    return -1;

  #else
    return rename(tmp, dst);

  #endif
}

char *file_read(const char *path){
  FILE *fp = fopen(path, "rb");

  if (!fp) return NULL;

  // File lenght mesurement
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  if (size < 0){
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  // Allocate the buffer
  char *buf = malloc(size + 1);
  if (!buf){
    fclose(fp);
    return NULL;
  }

  // Read the file
  size_t n = fread(buf, 1, (size_t)size, fp);
  buf[n] = '\0';
  
  fclose(fp);
  return buf;
}


int file_write(const char *path, const char *data){

  // Create temporary swap file path
  size_t len = strlen(data);
  char tmp[4096];
  int name_len = snprintf(tmp, sizeof tmp, "%s.swp", path);
  if (name_len >= (int)sizeof tmp)
    return -1;

  FILE *fp = fopen(tmp, "wb");
  if (!fp) return -1;

  size_t write_len = fwrite(data, 1, len, fp);
  if (write_len != len){
    fclose(fp);
    remove(tmp);
    return -1;
  }

  if (fclose(fp) != 0){
    remove(tmp);
    return -1;
  }

  if (replace(tmp, path) != 0){
    remove(tmp);
    return -1;
  }

  return 0;

}
