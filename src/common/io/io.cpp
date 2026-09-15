/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2010
//

#include <dirent.h>
#include <iostream>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/math/regex.h"

/**
 * @file io.cpp
 * @brief 文件与目录工具实现
 * @details 文件末尾的 writen/readn/preadn/pwriten 是页 I/O 的可靠传输原语，
 * 共同点是都把「一次调用未必传完」这件事封装掉，用循环保证整块数据传完。
 */
namespace common {

int readFromFile(const string &fileName, char *&outputData, size_t &fileSize)
{
  FILE *file = fopen(fileName.c_str(), "rb");
  if (file == NULL) {
    cerr << "Failed to open file " << fileName << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
    return -1;
  }

  // fseek( file, 0, SEEK_END );
  // size_t fsSize = ftell( file );
  // fseek( file, 0, SEEK_SET );

  char   buffer[4 * ONE_KILO];
  size_t readSize = 0;
  size_t oneRead  = 0;

  char *data = NULL;
  do {
    memset(buffer, 0, sizeof(buffer));
    oneRead = fread(buffer, 1, sizeof(buffer), file);
    if (ferror(file)) {
      cerr << "Failed to read data" << fileName << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      fclose(file);
      if (data != NULL) {
        free(data);
        data = NULL;
      }
      return -1;
    }

    data = (char *)realloc(data, readSize + oneRead);
    if (data == NULL) {
      cerr << "Failed to alloc memory for " << fileName << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      free(data);
      fclose(file);
      return -1;
    } else {
      memcpy(data + readSize, buffer, oneRead);
      readSize += oneRead;
    }

  } while (feof(file) == 0);

  fclose(file);

  data           = (char *)realloc(data, readSize + 1);
  data[readSize] = '\0';
  outputData     = data;
  fileSize       = readSize;
  return 0;
}

int writeToFile(const string &fileName, const char *data, uint32_t dataSize, const char *openMode)
{
  FILE *file = fopen(fileName.c_str(), openMode);
  if (file == NULL) {
    cerr << "Failed to open file " << fileName << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
    return -1;
  }

  uint32_t    leftSize = dataSize;
  const char *buffer   = data;
  while (leftSize > 0) {
    int writeCount = fwrite(buffer, 1, leftSize, file);
    if (writeCount <= 0) {
      cerr << "Failed to open file " << fileName << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      fclose(file);
      return -1;
    } else {
      leftSize -= writeCount;
      buffer += writeCount;
    }
  }

  fclose(file);

  return 0;
}

int getFileLines(const string &fileName, uint64_t &lineNum)
{
  lineNum = 0;

  char line[4 * ONE_KILO] = {0};

  ifstream ifs(fileName.c_str());
  if (!ifs) {
    return -1;
  }

  while (ifs.good()) {
    line[0] = 0;
    ifs.getline(line, sizeof(line));
    char *lineStrip = strip(line);
    if (strlen(lineStrip)) {
      lineNum++;
    }
  }

  ifs.close();
  return 0;
}

int getFileNum(int64_t &fileNum, const string &path, const string &pattern, bool recursive)
{
  try {
    DIR *dirp = NULL;
    dirp      = opendir(path.c_str());
    if (dirp == NULL) {
      cerr << "Failed to opendir " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      return -1;
    }

    string    fullPath;
    struct dirent *entry = NULL;
    struct stat    fs;
    while ((entry = readdir(dirp)) != NULL) {
      // don't care ".", "..", ".****" hidden files
      if (!strncmp(entry->d_name, ".", 1)) {
        continue;
      }

      fullPath = path;
      if (path[path.size() - 1] != FILE_PATH_SPLIT) {
        fullPath += FILE_PATH_SPLIT;
      }
      fullPath += entry->d_name;
      memset(&fs, 0, sizeof(fs));
      if (stat(fullPath.c_str(), &fs) < 0) {
        cout << "Failed to stat " << fullPath << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
        continue;
      }

      if (fs.st_mode & S_IFDIR) {
        if (recursive == 0) {
          continue;
        }

        if (getFileNum(fileNum, fullPath, pattern, recursive) < 0) {
          closedir(dirp);
          return -1;
        }
      }

      if (!(fs.st_mode & S_IFREG)) {
        // not regular files
        continue;
      }

      if (pattern.empty() == false && regex_match(entry->d_name, pattern.c_str())) {
        // Don't match
        continue;
      }

      fileNum++;
    }

    closedir(dirp);

    return 0;
  } catch (...) {
    cerr << "Failed to get file num " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
  }
  return -1;
}

int getFileList(vector<string> &fileList, const string &path, const string &pattern, bool recursive)
{
  try {
    DIR *dirp = NULL;
    dirp      = opendir(path.c_str());
    if (dirp == NULL) {
      cerr << "Failed to opendir " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      return -1;
    }

    string    fullPath;
    struct dirent *entry = NULL;
    struct stat    fs;
    while ((entry = readdir(dirp)) != NULL) {
      // don't care ".", "..", ".****" hidden files
      if (!strncmp(entry->d_name, ".", 1)) {
        continue;
      }

      fullPath = path;
      if (path[path.size() - 1] != FILE_PATH_SPLIT) {
        fullPath += FILE_PATH_SPLIT;
      }
      fullPath += entry->d_name;
      memset(&fs, 0, sizeof(fs));
      if (stat(fullPath.c_str(), &fs) < 0) {
        cout << "Failed to stat " << fullPath << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
        continue;
      }

      if (fs.st_mode & S_IFDIR) {
        if (recursive == 0) {
          continue;
        }

        if (getFileList(fileList, fullPath, pattern, recursive) < 0) {
          closedir(dirp);
          return -1;
        }
      }

      if (!(fs.st_mode & S_IFREG)) {
        // regular files
        continue;
      }

      if (pattern.empty() == false && regex_match(entry->d_name, pattern.c_str())) {
        // Don't match
        continue;
      }

      fileList.push_back(fullPath);
    }

    closedir(dirp);
    return 0;
  } catch (...) {
    cerr << "Failed to get file list " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
  }
  return -1;
}

int getDirList(vector<string> &dirList, const string &path, const string &pattern)
{
  try {
    DIR *dirp = NULL;
    dirp      = opendir(path.c_str());
    if (dirp == NULL) {
      cerr << "Failed to opendir " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
      return -1;
    }

    string    fullPath;
    struct dirent *entry = NULL;
    struct stat    fs;
    while ((entry = readdir(dirp)) != NULL) {
      // don't care ".", "..", ".****" hidden files
      if (!strncmp(entry->d_name, ".", 1)) {
        continue;
      }

      fullPath = path;
      if (path[path.size() - 1] != FILE_PATH_SPLIT) {
        fullPath += FILE_PATH_SPLIT;
      }
      fullPath += entry->d_name;
      memset(&fs, 0, sizeof(fs));
      if (stat(fullPath.c_str(), &fs) < 0) {
        cout << "Failed to stat " << fullPath << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
        continue;
      }

      if ((fs.st_mode & S_IFDIR) == 0) {
        continue;
      }

      if (pattern.empty() == false && regex_match(entry->d_name, pattern.c_str())) {
        // Don't match
        continue;
      }

      dirList.push_back(fullPath);
    }

    closedir(dirp);
    return 0;
  } catch (...) {
    cerr << "Failed to get file list " << path << SYS_OUTPUT_FILE_POS << SYS_OUTPUT_ERROR << endl;
  }
  return -1;
}

int touch(const string &path)
{
  // CWE367: A check occurs on a file's attributes before
  // the file is used in a privileged operation, but things
  // may have changed

  // struct stat fs;

  // memset(&fs, 0, sizeof(fs));
  // if (stat(path.c_str(), &fs) == 0) {
  //   return 0;
  // }

  // create the file
  FILE *file = fopen(path.c_str(), "a");
  if (file == NULL) {
    return -1;
  }
  fclose(file);
  return 0;
}

int getFileSize(const char *filePath, int64_t &fileLen)
{
  if (filePath == NULL || *filePath == '\0') {
    cerr << "invalid filepath" << endl;
    return -EINVAL;
  }
  struct stat statBuf;
  memset(&statBuf, 0, sizeof(statBuf));

  int rc = stat(filePath, &statBuf);
  if (rc) {
    cerr << "Failed to get stat of " << filePath << "," << errno << ":" << strerror(errno) << endl;
    return rc;
  }

  if (S_ISDIR(statBuf.st_mode)) {
    cerr << filePath << " is directory " << endl;
    return -EINVAL;
  }

  fileLen = statBuf.st_size;
  return 0;
}

/**
 * @brief 一次性写完指定长度的数据
 * @details 循环调用 write 直到剩余长度减到 0。短写是正常现象，每轮都要推进缓冲区指针
 * 与剩余长度；遇到 EINTR 与 EAGAIN 直接重试，其余错误返回 errno。
 * @return 0 表示全部写完；其他值为 errno
 */
int writen(int fd, const void *buf, int size)
{
  const char *tmp = (const char *)buf;
  while (size > 0) {
    const ssize_t ret = ::write(fd, tmp, size);
    if (ret >= 0) {
      tmp += ret;
      size -= ret;
      continue;
    }
    const int err = errno;
    if (EAGAIN != err && EINTR != err)
      return err;
  }
  return 0;
}

/**
 * @brief 一次性读满指定长度的数据
 * @details 循环调用 read 直到读满。读到 0 字节说明遇到文件尾，返回 -1 与普通错误区分开；
 * EINTR 与 EAGAIN 重试。
 * @return 0 表示读满；-1 表示提前遇到文件尾；其他值为 errno
 */
int readn(int fd, void *buf, int size)
{
  char *tmp = (char *)buf;
  while (size > 0) {
    const ssize_t ret = ::read(fd, tmp, size);
    if (ret > 0) {
      tmp += ret;
      size -= ret;
      continue;
    }
    if (0 == ret)
      return -1;  // end of file

    const int err = errno;
    if (EAGAIN != err && EINTR != err)
      return err;
  }
  return 0;
}

/**
 * @brief 用 pread 可靠读满一页，不改变文件描述符的当前位置
 * @details 每一轮都要显式推进 offset：pread 不会自己移动文件位置，
 * 忘了推进就会在同一个位置反复读而陷入死循环。
 * @return 0 表示读满；-1 表示提前遇到文件尾；其他值为 errno
 */
int preadn(int fd, void *buf, int size, int64_t offset)
{
  char *tmp = static_cast<char *>(buf);
  while (size > 0) {
    const ssize_t ret = ::pread(fd, tmp, size, offset);
    if (ret > 0) {
      tmp += ret;
      size -= static_cast<int>(ret);
      offset += ret;
      continue;
    }
    if (ret == 0) {
      return -1;
    }

    const int err = errno;
    if (err != EAGAIN && err != EINTR) {
      return err;
    }
  }
  return 0;
}

/**
 * @brief 用 pwrite 可靠写满一页，不改变文件描述符的当前位置
 * @details 与 preadn 一样每轮显式推进 offset。写出 0 字节被当作 I/O 错误返回 EIO，
 * 避免在写不下去的情况下空转。
 * @return 0 表示写满；其他值为 errno
 */
int pwriten(int fd, const void *buf, int size, int64_t offset)
{
  const char *tmp = static_cast<const char *>(buf);
  while (size > 0) {
    const ssize_t ret = ::pwrite(fd, tmp, size, offset);
    if (ret > 0) {
      tmp += ret;
      size -= static_cast<int>(ret);
      offset += ret;
      continue;
    }
    if (ret == 0) {
      return EIO;
    }

    const int err = errno;
    if (err != EAGAIN && err != EINTR) {
      return err;
    }
  }
  return 0;
}
}  // namespace common
