// Copyright (c) 2006, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// simple_symbol_supplier.cc: A simple SymbolSupplier implementation
//
// See simple_symbol_supplier.h for documentation.
//
// Author: Mark Mentovai

#include "processor/simple_symbol_supplier.h"

#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <algorithm>
#include <iostream>
#include <fstream>

#include "common/using_std_string.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/system_info.h"
#include "processor/logging.h"
#include "processor/pathname_stripper.h"
#include <curl/curl.h>
#ifdef __linux__
#include <unistd.h>
#endif

#ifndef __MINGW32__
#define MKDIRARGS ,0777
#else
#define MKDIRARGS
#endif

struct buffer
{
    char *m_buffer;
    int m_buf_size;
};

static size_t buffer_write_cb(void *ptr, size_t size, size_t nmemb, void *stream)
{
    struct buffer *b = (struct buffer *)stream;
    int buf_pos = b->m_buf_size;
    b->m_buf_size += size*nmemb;
    b->m_buffer = (char*)realloc(b->m_buffer, b->m_buf_size);
    memcpy(b->m_buffer + buf_pos, ptr, size*nmemb);
    return size*nmemb;
}

static char *load_url(const char *url, int *size)
{
    struct buffer b = { 0 };
    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();
    if (!curl)
        return 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buffer_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Microsoft-Symbol-Server/6.2.9200.16384");
    long enable = 1;
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, enable);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        if (b.m_buffer)
            free(b.m_buffer);
        printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        return 0;
    }
fail:
    curl_easy_cleanup(curl);
    *size = b.m_buf_size;
    //printf("%d readed\n", *size);
    return b.m_buffer;
}

static int mkpath(const char *path)
{
    char *p = 0;
    int len = (int)strlen(path);
    if (len <= 0)
        return 0;

    char *buffer = (char*)malloc(len + 1);
    if (!buffer)
        goto fail;
    strcpy(buffer, path);

    if (buffer[len - 1] == '/')
    {
        buffer[len - 1] = '\0';
        if (!mkdir(buffer MKDIRARGS))
        {
            free(buffer);
            return 1;
        }
        buffer[len - 1] = '/';
    }

    p = buffer + 1;
    while (1)
    {
        while (*p && *p != '\\' && *p != '/')
            p++;
        if (!*p)
            break;
        char sav = *p;
        *p = 0;
        if ((mkdir(buffer MKDIRARGS) == -1) && (errno == ENOENT))
            goto fail;
        *p++ = sav;
    }
    free(buffer);
    return 1;
fail:
    printf("error: creating %s failed", path);
    if (buffer)
        free(buffer);
    return 0;
}

namespace google_breakpad {

static bool file_exists(const string &file_name) {
  struct stat sb;
  return stat(file_name.c_str(), &sb) == 0;
}

SymbolSupplier::SymbolResult SimpleSymbolSupplier::GetSymbolFile(
    const CodeModule *module, const SystemInfo *system_info,
    string *symbol_file) {
  BPLOG_IF(ERROR, !symbol_file) << "SimpleSymbolSupplier::GetSymbolFile "
                                   "requires |symbol_file|";
  assert(symbol_file);
  symbol_file->clear();

  for (unsigned int path_index = 0; path_index < paths_.size(); ++path_index) {
    SymbolResult result;
    if ((result = GetSymbolFileAtPathFromRoot(module, system_info,
                                              paths_[path_index],
                                              symbol_file)) != NOT_FOUND) {
      return result;
    }
  }
  return NOT_FOUND;
}

SymbolSupplier::SymbolResult SimpleSymbolSupplier::GetSymbolFile(
    const CodeModule *module,
    const SystemInfo *system_info,
    string *symbol_file,
    string *symbol_data) {
  assert(symbol_data);
  symbol_data->clear();

  SymbolSupplier::SymbolResult s = GetSymbolFile(module, system_info,
                                                 symbol_file);
  if (s == FOUND) {
    std::ifstream in(symbol_file->c_str());
    std::getline(in, *symbol_data, string::traits_type::to_char_type(
                     string::traits_type::eof()));
    in.close();
  }
  return s;
}

SymbolSupplier::SymbolResult SimpleSymbolSupplier::GetCStringSymbolData(
    const CodeModule *module,
    const SystemInfo *system_info,
    string *symbol_file,
    char **symbol_data,
    size_t *symbol_data_size) {
  assert(symbol_data);
  assert(symbol_data_size);

  string symbol_data_string;
  SymbolSupplier::SymbolResult s =
      GetSymbolFile(module, system_info, symbol_file, &symbol_data_string);

  if (s == FOUND) {
    *symbol_data_size = symbol_data_string.size() + 1;
    *symbol_data = new char[*symbol_data_size];
    if (*symbol_data == NULL) {
      BPLOG(ERROR) << "Memory allocation for size " << *symbol_data_size
                   << " failed";
      return INTERRUPT;
    }
    memcpy(*symbol_data, symbol_data_string.c_str(), symbol_data_string.size());
    (*symbol_data)[symbol_data_string.size()] = '\0';
    memory_buffers_.insert(make_pair(module->code_file(), *symbol_data));
  }
  return s;
}

void SimpleSymbolSupplier::FreeSymbolData(const CodeModule *module) {
  if (!module) {
    BPLOG(INFO) << "Cannot free symbol data buffer for NULL module";
    return;
  }

  map<string, char *>::iterator it = memory_buffers_.find(module->code_file());
  if (it == memory_buffers_.end()) {
    BPLOG(INFO) << "Cannot find symbol data buffer for module "
                << module->code_file();
    return;
  }
  delete [] it->second;
  memory_buffers_.erase(it);
}

SymbolSupplier::SymbolResult SimpleSymbolSupplier::GetSymbolFileAtPathFromRoot(
    const CodeModule *module, const SystemInfo *system_info,
    const string &root_path, string *symbol_file) {
  BPLOG_IF(ERROR, !symbol_file) << "SimpleSymbolSupplier::GetSymbolFileAtPath "
                                   "requires |symbol_file|";
  assert(symbol_file);
  symbol_file->clear();

  if (!module)
    return NOT_FOUND;

  // Start with the base path.
  string path = root_path;

  // Append the debug (pdb) file name as a directory name.
  // FIXME: some dumps do not have debug_file and debug_identifier
  path.append("/");
  string debug_file_name = PathnameStripper::File(module->debug_file());
  string code_file = PathnameStripper::File(module->code_file());
  if (debug_file_name.empty() && code_file.length() > 3) {
    debug_file_name = code_file.substr(0, code_file.length() - 3) + "pdb";
    BPLOG(INFO) << "Assuming debug_file = " << debug_file_name;
  }
  if (debug_file_name.empty()) {
    BPLOG(ERROR) << "Can't construct symbol file path without debug_file "
                    "(code_file = " << code_file << ")";
    return NOT_FOUND;
  }
  path.append(debug_file_name);

  // Append the identifier as a directory name.
  string identifier = module->debug_identifier();
  string version = module->version();
  /*if (identifier.empty()) {
    BPLOG(ERROR) << "Can't construct symbol file path without debug_identifier "
                    "(code_file = " <<
                    PathnameStripper::File(module->code_file()) <<
                    ", debug_file = " << debug_file_name << ")";
    return NOT_FOUND;
  }*/
  if (!identifier.empty() || !version.empty()) {
    // if debug_identifier not found - use version instead
    path.append("/");
    if (!identifier.empty())
      path.append(identifier);
    else
      path.append(version);
  }

  // Transform the debug file name into one ending in .sym.  If the existing
  // name ends in .pdb, strip the .pdb.  Otherwise, add .sym to the non-.pdb
  // name.
  path.append("/");
  string debug_file_extension;
  if (debug_file_name.size() > 4)
    debug_file_extension = debug_file_name.substr(debug_file_name.size() - 4);
  std::transform(debug_file_extension.begin(), debug_file_extension.end(),
                 debug_file_extension.begin(), tolower);
  if (debug_file_extension == ".pdb") {
    path.append(debug_file_name.substr(0, debug_file_name.size() - 4));
  } else {
    path.append(debug_file_name);
  }
  string path_pdb = path + ".pdb";
  path.append(".sym");

  if (!file_exists(path)) {
      int buf_size;
      string url = string("http://msdl.microsoft.com/download/symbols") + (path_pdb.c_str() + root_path.length());
      char *data = load_url(url.c_str(), &buf_size);
      BPLOG(INFO) << "Downloaded: " << url << " ("  << buf_size << " bytes)";
      mkpath(path_pdb.c_str());
      FILE *f = fopen(path_pdb.c_str(), "wb");
      if (f) {
          fwrite(data, 1, buf_size, f);
          fclose(f);
      }
#ifdef __linux__
      string convert_cmd = string("wine dump_syms.exe ") + path_pdb + " >" + path;
#else
      string convert_cmd = string("dump_syms.exe ") + path_pdb + " >" + path;
#endif
      if (!system(convert_cmd.c_str())) {
          BPLOG(INFO) << "Converted: " << path;
          unlink(path_pdb.c_str());
      } else {
          BPLOG(ERROR) << "Convert fail: " << path;
      }
  }

  if (!file_exists(path)) {
    BPLOG(INFO) << "No symbol file at " << path;
    return NOT_FOUND;
  }

  *symbol_file = path;
  return FOUND;
}

}  // namespace google_breakpad
