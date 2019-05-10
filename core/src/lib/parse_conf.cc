/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2018 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Master Configuration routines.
 *
 * This file contains the common parts of the BAREOS configuration routines.
 *
 * Note, the configuration file parser consists of four parts
 *
 * 1. The generic lexical scanner in lib/lex.c and lib/lex.h
 *
 * 2. The generic config scanner in lib/parse_conf.c and lib/parse_conf.h.
 *    These files contain the parser code, some utility routines,
 *
 * 3. The generic resource functions in lib/res.c
 *    Which form the common store routines (name, int, string, time,
 *    int64, size, ...).
 *
 * 4. The daemon specific file, which contains the Resource definitions
 *    as well as any specific store routines for the resource records.
 *
 * N.B. This is a two pass parser, so if you malloc() a string in a "store"
 * routine, you must ensure to do it during only one of the two passes, or to
 * free it between.
 *
 * Also, note that the resource record is malloced and saved in SaveResource()
 * during pass 1. Anything that you want saved after pass two (e.g. resource
 * pointers) must explicitly be done in SaveResource. Take a look at the Job
 * resource in src/dird/dird_conf.c to see how it is done.
 *
 * Kern Sibbald, January MM
 */

#include "include/bareos.h"
#include "include/jcr.h"
#include "lib/address_conf.h"
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "lib/qualified_resource_name_type_converter.h"
#include "lib/bstringlist.h"
#include "lib/ascii_control_characters.h"
#include "lib/messages_resource.h"
#include "lib/resource_item.h"
#include "lib/berrno.h"
#include "lib/util.h"

#include <algorithm>

#if defined(HAVE_WIN32)
#include "shlobj.h"
#else
#define MAX_PATH 1024
#endif

void PrintMessage(void* sock, const char* fmt, ...)
{
  va_list arg_ptr;

  va_start(arg_ptr, fmt);
  vfprintf(stdout, fmt, arg_ptr);
  va_end(arg_ptr);
}

ConfigurationParser::ConfigurationParser()
    : scan_error_(nullptr)
    , scan_warning_(nullptr)
    , init_res_(nullptr)
    , store_res_(nullptr)
    , print_res_(nullptr)
    , err_type_(0)
    , omit_defaults_(false)
    , r_first_(0)
    , r_last_(0)
    , r_own_(0)
    , resources_(0)
    , res_head_(nullptr)
    , SaveResourceCb_(nullptr)
    , DumpResourceCb_(nullptr)
    , FreeResourceCb_(nullptr)
    , use_config_include_dir_(false)
    , ParseConfigReadyCb_(nullptr)
    , parser_first_run_(true)
{
  return;
}

ConfigurationParser::ConfigurationParser(
    const char* cf,
    LEX_ERROR_HANDLER* ScanError,
    LEX_WARNING_HANDLER* scan_warning,
    INIT_RES_HANDLER* init_res,
    STORE_RES_HANDLER* StoreRes,
    PRINT_RES_HANDLER* print_res,
    int32_t err_type,
    int32_t r_first,
    int32_t r_last,
    ResourceTable* resources,
    BareosResource** res_head,
    const char* config_default_filename,
    const char* config_include_dir,
    void (*ParseConfigReadyCb)(ConfigurationParser&),
    SaveResourceCb_t SaveResourceCb,
    DumpResourceCb_t DumpResourceCb,
    FreeResourceCb_t FreeResourceCb)
    : ConfigurationParser()
{
  cf_ = cf == nullptr ? "" : cf;
  use_config_include_dir_ = false;
  config_include_naming_format_ = "%s/%s/%s.conf";
  scan_error_ = ScanError;
  scan_warning_ = scan_warning;
  init_res_ = init_res;
  store_res_ = StoreRes;
  print_res_ = print_res;
  err_type_ = err_type;
  r_first_ = r_first;
  r_last_ = r_last;
  resources_ = resources;
  res_head_ = res_head;
  config_default_filename_ =
      config_default_filename == nullptr ? "" : config_default_filename;
  config_include_dir_ = config_include_dir == nullptr ? "" : config_include_dir;
  ParseConfigReadyCb_ = ParseConfigReadyCb;
  ASSERT(SaveResourceCb);
  ASSERT(DumpResourceCb);
  ASSERT(FreeResourceCb);
  SaveResourceCb_ = SaveResourceCb;
  DumpResourceCb_ = DumpResourceCb;
  FreeResourceCb_ = FreeResourceCb;
}

ConfigurationParser::~ConfigurationParser()
{
  for (int i = r_first_; i <= r_last_; i++) {
    FreeResourceCb_(res_head_[i - r_first_], i);
    res_head_[i - r_first_] = nullptr;
  }
}

void ConfigurationParser::InitializeQualifiedResourceNameTypeConverter(
    const std::map<int, std::string>& map)
{
  qualified_resource_name_type_converter_.reset(
      new QualifiedResourceNameTypeConverter(map));
}

bool ConfigurationParser::ParseConfig()
{
  int errstat;
  PoolMem config_path;

  if (parser_first_run_ && (errstat = RwlInit(&res_lock_)) != 0) {
    BErrNo be;
    Jmsg1(nullptr, M_ABORT, 0,
          _("Unable to initialize resource lock. ERR=%s\n"),
          be.bstrerror(errstat));
  }
  parser_first_run_ = false;

  if (!FindConfigPath(config_path)) {
    Jmsg0(nullptr, M_ERROR_TERM, 0, _("Failed to find config filename.\n"));
  }
  used_config_path_ = config_path.c_str();
  Dmsg1(100, "config file = %s\n", used_config_path_.c_str());
  bool success =
      ParseConfigFile(config_path.c_str(), nullptr, scan_error_, scan_warning_);
  if (success && ParseConfigReadyCb_) { ParseConfigReadyCb_(*this); }
  return success;
}

void ConfigurationParser::lex_error(const char* cf,
                                    LEX_ERROR_HANDLER* ScanError,
                                    LEX_WARNING_HANDLER* scan_warning) const
{
  /*
   * We must create a lex packet to print the error
   */
  LEX* lexical_parser_ = (LEX*)malloc(sizeof(LEX));
  memset(lexical_parser_, 0, sizeof(LEX));

  if (ScanError) {
    lexical_parser_->ScanError = ScanError;
  } else {
    LexSetDefaultErrorHandler(lexical_parser_);
  }

  if (scan_warning) {
    lexical_parser_->scan_warning = scan_warning;
  } else {
    LexSetDefaultWarningHandler(lexical_parser_);
  }

  LexSetErrorHandlerErrorType(lexical_parser_, err_type_);
  BErrNo be;
  scan_err2(lexical_parser_, _("Cannot open config file \"%s\": %s\n"), cf,
            be.bstrerror());
  free(lexical_parser_);
}

enum
{
  GET_NEXT_TOKEN,
  ERROR,
  NEXT_STATE
};

struct ConfigParserStateMachine {
  ConfigParserStateMachine(ConfigurationParser& my_config)
      : my_config_(my_config){};
  ~ConfigParserStateMachine()
  {
    while (lexical_parser_) { lexical_parser_ = LexCloseFile(lexical_parser_); }
  }
  ConfigParserStateMachine(ConfigParserStateMachine&& ohter) = delete;
  ConfigParserStateMachine(ConfigParserStateMachine& ohter) = delete;
  ConfigParserStateMachine& operator=(ConfigParserStateMachine& ohter) = delete;
  bool InitParserPass(const char* cf,
                      void* caller_ctx,
                      LEX*& lexical_parser_,
                      LEX_ERROR_HANDLER* ScanError,
                      LEX_WARNING_HANDLER* scan_warning,
                      int pass);
  int ParserStateNone(int token);
  int ScanResource(int token);
  int ParseAllTokens();
  void DumpResourcesAfterSecondPass();
  LEX* lexical_parser_ = nullptr;
  ResourceTable* resource_table_ = nullptr;
  ResourceItem* resource_items_ = nullptr;
  ParseState state = ParseState::kInit;
  int rcode_ = 0;
  int parser_pass_number_ = 0;
  BareosResource* static_resource_ = nullptr;
  int config_level_ = 0;
  ConfigurationParser& my_config_;
};

int ConfigParserStateMachine::ParseAllTokens()
{
  int token;

  while ((token = LexGetToken(lexical_parser_, BCT_ALL)) != BCT_EOF) {
    Dmsg3(900, "parse state=%d parser_pass_number_=%d got token=%s\n", state,
          parser_pass_number_, lex_tok_to_str(token));
    switch (state) {
      case ParseState::kInit:
        switch (ParserStateNone(token)) {
          case GET_NEXT_TOKEN:
          case NEXT_STATE:
            continue;
          case ERROR:
            return ERROR;
          default:
            ASSERT(false);
        }
        break;
      case ParseState::kResource:
        switch (ScanResource(token)) {
          case GET_NEXT_TOKEN:
            continue;
          case ERROR:
            return ERROR;
          default:
            ASSERT(false);
        }
        break;
      default:
        scan_err1(lexical_parser_, _("Unknown parser state %d\n"), state);
        return ERROR;
    }
  }
  return NEXT_STATE;
}

int ConfigParserStateMachine::ScanResource(int token)
{
  switch (token) {
    case BCT_BOB:
      config_level_++;
      return GET_NEXT_TOKEN;
    case BCT_IDENTIFIER: {
      if (config_level_ != 1) {
        scan_err1(lexical_parser_, _("not in resource definition: %s"),
                  lexical_parser_->str);
        return ERROR;
      }

      int resource_item_index = my_config_.GetResourceItemIndex(
          resource_items_, lexical_parser_->str);

      if (resource_item_index >= 0) {
        ResourceItem* item = nullptr;
        item = &resource_items_[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lexical_parser_, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            scan_err1(lexical_parser_, _("expected an equals, got: %s"),
                      lexical_parser_->str);
            return ERROR;
          }
        }

        if (item->flags & CFG_ITEM_DEPRECATED) {
          scan_warn2(lexical_parser_,
                     _("using deprecated keyword %s on line %d"), item->name,
                     lexical_parser_->line_no);
          // only warning
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_.StoreResource(item->type, lexical_parser_, item,
                                      resource_item_index,
                                      parser_pass_number_)) {
          if (my_config_.store_res_) {
            my_config_.store_res_(lexical_parser_, item, resource_item_index,
                                  parser_pass_number_);
          }
        }
      } else {
        Dmsg2(900, "config_level_=%d id=%s\n", config_level_,
              lexical_parser_->str);
        Dmsg1(900, "Keyword = %s\n", lexical_parser_->str);
        scan_err1(lexical_parser_,
                  _("Keyword \"%s\" not permitted in this resource.\n"
                    "Perhaps you left the trailing brace off of the "
                    "previous resource."),
                  lexical_parser_->str);
        return ERROR;
      }
      return GET_NEXT_TOKEN;
    }
    case BCT_EOB:
      config_level_--;
      state = ParseState::kInit;
      Dmsg0(900, "BCT_EOB => define new resource\n");
      if (!static_resource_->resource_name_) {
        scan_err0(lexical_parser_, _("Name not specified for resource"));
        return ERROR;
      }
      /* save resource */
      if (!my_config_.SaveResourceCb_(rcode_, resource_items_,
                                      parser_pass_number_)) {
        scan_err0(lexical_parser_, _("SaveResource failed"));
        return ERROR;
      }
      return GET_NEXT_TOKEN;

    case BCT_EOL:
      return GET_NEXT_TOKEN;

    default:
      scan_err2(lexical_parser_,
                _("unexpected token %d %s in resource definition"), token,
                lex_tok_to_str(token));
      return ERROR;
  }
  return GET_NEXT_TOKEN;
}

int ConfigParserStateMachine::ParserStateNone(int token)
{
  switch (token) {
    case BCT_EOL:
    case BCT_UTF8_BOM:
      return GET_NEXT_TOKEN;
    case BCT_UTF16_BOM:
      scan_err0(lexical_parser_,
                _("Currently we cannot handle UTF-16 source files. "
                  "Please convert the conf file to UTF-8\n"));
      return ERROR;
    default:
      if (token != BCT_IDENTIFIER) {
        scan_err1(lexical_parser_,
                  _("Expected a Resource name identifier, got: %s"),
                  lexical_parser_->str);
        return ERROR;
      }
      break;
  }

  resource_table_ = my_config_.GetResourceTable(lexical_parser_->str);

  if (resource_table_ && resource_table_->items) {
    resource_items_ = resource_table_->items;
    state = ParseState::kResource;
    rcode_ = resource_table_->rcode;
    my_config_.InitResource(rcode_, resource_items_, parser_pass_number_,
                            resource_table_->ResourceSpecificInitializer);
    ASSERT(resource_table_->static_resource_);
    static_resource_ = *resource_table_->static_resource_;
    ASSERT(static_resource_);
  }

  if (state == ParseState::kInit) {
    scan_err1(lexical_parser_, _("expected resource name, got: %s"),
              lexical_parser_->str);
    return ERROR;
  }
  return NEXT_STATE;
}

bool ConfigParserStateMachine::InitParserPass(const char* cf,
                                              void* caller_ctx,
                                              LEX*& lexical_parser_,
                                              LEX_ERROR_HANDLER* ScanError,
                                              LEX_WARNING_HANDLER* scan_warning,
                                              int parser_pass_number_)
{
  Dmsg1(900, "ParseConfig parser_pass_number_ %d\n", parser_pass_number_);
  if ((lexical_parser_ = lex_open_file(lexical_parser_, cf, ScanError,
                                       scan_warning)) == nullptr) {
    my_config_.lex_error(cf, ScanError, scan_warning);
    return false;
  }
  LexSetErrorHandlerErrorType(lexical_parser_, my_config_.err_type_);
  lexical_parser_->error_counter = 0;
  lexical_parser_->caller_ctx = caller_ctx;
  return true;
}

void ConfigParserStateMachine::DumpResourcesAfterSecondPass()
{
  if (debug_level >= 900 && parser_pass_number_ == 2) {
    for (int i = my_config_.r_first_; i <= my_config_.r_last_; i++) {
      my_config_.DumpResourceCb_(i,
                                 my_config_.res_head_[i - my_config_.r_first_],
                                 PrintMessage, nullptr, false, false);
    }
  }
}

bool ConfigurationParser::ParseConfigFile(const char* config_file_name,
                                          void* caller_ctx,
                                          LEX_ERROR_HANDLER* ScanError,
                                          LEX_WARNING_HANDLER* scan_warning)
{
  ConfigParserStateMachine state_machine(*this);

  Dmsg1(900, "Enter ParseConfigFile(%s)\n", config_file_name);

  for (state_machine.parser_pass_number_ = 1;
       state_machine.parser_pass_number_ <= 2;
       state_machine.parser_pass_number_++) {
    if (!state_machine.InitParserPass(
            config_file_name, caller_ctx, state_machine.lexical_parser_,
            ScanError, scan_warning, state_machine.parser_pass_number_)) {
      return false;
    }

    state_machine.ParseAllTokens();

    if (state_machine.state != ParseState::kInit) {
      scan_err0(state_machine.lexical_parser_,
                _("End of conf file reached with unclosed resource."));
      return false;
    }

    state_machine.DumpResourcesAfterSecondPass();

    if (state_machine.lexical_parser_->error_counter > 0) { return false; }

    state_machine.lexical_parser_ = LexCloseFile(state_machine.lexical_parser_);
  }  // for (state_machine.parser_pass_number_..

  Dmsg0(900, "Leave ParseConfigFile()\n");
  return true;
}

bool ConfigurationParser::AppendToResourcesChain(BareosResource* new_resource,
                                                 int rcode)
{
  int rindex = rcode - r_first_;

  if (!new_resource->resource_name_) {
    Emsg1(M_ERROR, 0,
          _("Name item is required in %s resource, but not found.\n"),
          resources_[rindex].name);
    return false;
  }

  if (!res_head_[rindex]) {
    res_head_[rindex] = new_resource;
    Dmsg3(900, "Inserting first %s res: %s index=%d\n", ResToStr(rcode),
          new_resource->resource_name_, rindex);
  } else {  // append
    BareosResource* last = nullptr;
    BareosResource* current = res_head_[rindex];
    do {
      if (bstrcmp(current->resource_name_, new_resource->resource_name_)) {
        Emsg2(M_ERROR, 0,
              _("Attempt to define second %s resource named \"%s\" is not "
                "permitted.\n"),
              resources_[rindex].name, new_resource->resource_name_);
        return false;
      }
      last = current;
      current = last->next_;
    } while (current);
    last->next_ = new_resource;
    Dmsg3(900, _("Inserting %s res: %s index=%d\n"), ResToStr(rcode),
          new_resource->resource_name_, rindex);
  }
  return true;
}

int ConfigurationParser::GetResourceTableIndex(int resource_type)
{
  int rindex = -1;

  if ((resource_type >= r_first_) && (resource_type <= r_last_)) {
    rindex = resource_type = r_first_;
  }

  return rindex;
}

ResourceTable* ConfigurationParser::GetResourceTable(int resource_type)
{
  ResourceTable* result = nullptr;
  int rindex = GetResourceTableIndex(resource_type);

  if (rindex >= 0) { result = &resources_[rindex]; }

  return result;
}

ResourceTable* ConfigurationParser::GetResourceTable(
    const char* resource_type_name)
{
  ResourceTable* result = nullptr;
  int i;

  for (i = 0; resources_[i].name; i++) {
    if (Bstrcasecmp(resources_[i].name, resource_type_name)) {
      result = &resources_[i];
    }
  }

  return result;
}

int ConfigurationParser::GetResourceItemIndex(ResourceItem* resource_items_,
                                              const char* item)
{
  int result = -1;
  int i;

  for (i = 0; resource_items_[i].name; i++) {
    if (Bstrcasecmp(resource_items_[i].name, item)) {
      result = i;
      break;
    }
  }

  return result;
}

ResourceItem* ConfigurationParser::GetResourceItem(
    ResourceItem* resource_items_,
    const char* item)
{
  ResourceItem* result = nullptr;
  int i = -1;

  i = GetResourceItemIndex(resource_items_, item);
  if (i >= 0) { result = &resource_items_[i]; }

  return result;
}

const char* ConfigurationParser::GetDefaultConfigDir()
{
#if defined(HAVE_WIN32)
  HRESULT hr;
  static char szConfigDir[MAX_PATH + 1] = {0};

  if (!p_SHGetFolderPath) {
    bstrncpy(szConfigDir, DEFAULT_CONFIGDIR, sizeof(szConfigDir));
    return szConfigDir;
  }

  if (szConfigDir[0] == '\0') {
    hr = p_SHGetFolderPath(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0,
                           szConfigDir);

    if (SUCCEEDED(hr)) {
      bstrncat(szConfigDir, "\\Bareos", sizeof(szConfigDir));
    } else {
      bstrncpy(szConfigDir, DEFAULT_CONFIGDIR, sizeof(szConfigDir));
    }
  }

  return szConfigDir;
#else
  return CONFDIR;
#endif
}

#ifdef HAVE_SETENV
static inline void set_env(const char* key, const char* value)
{
  setenv(key, value, 1);
}
#elif HAVE_PUTENV
static inline void set_env(const char* key, const char* value)
{
  PoolMem env_string;

  Mmsg(env_string, "%s=%s", key, value);
  putenv(strdup(env_string.c_str()));
}
#else
static inline void set_env(const char* key, const char* value) {}
#endif

bool ConfigurationParser::GetConfigFile(PoolMem& full_path,
                                        const char* config_dir,
                                        const char* config_filename)
{
  bool found = false;

  if (!PathIsDirectory(config_dir)) { return false; }

  if (config_filename) {
    full_path.strcpy(config_dir);
    if (PathAppend(full_path, config_filename)) {
      if (PathExists(full_path)) {
        config_dir_ = config_dir;
        found = true;
      }
    }
  }

  return found;
}

bool ConfigurationParser::GetConfigIncludePath(PoolMem& full_path,
                                               const char* config_dir)
{
  bool found = false;

  if (!config_include_dir_.empty()) {
    /*
     * Set full_path to the initial part of the include path,
     * so it can be used as result, even on errors.
     * On success, full_path will be overwritten with the full path.
     */
    full_path.strcpy(config_dir);
    PathAppend(full_path, config_include_dir_.c_str());
    if (PathIsDirectory(full_path)) {
      config_dir_ = config_dir;
      /*
       * Set full_path to wildcard path.
       */
      if (GetPathOfResource(full_path, nullptr, nullptr, nullptr, true)) {
        use_config_include_dir_ = true;
        found = true;
      }
    }
  }

  return found;
}

/*
 * Returns false on error
 *         true  on OK, with full_path set to where config file should be
 */
bool ConfigurationParser::FindConfigPath(PoolMem& full_path)
{
  bool found = false;
  PoolMem config_dir;
  PoolMem config_path_file;

  if (cf_.empty()) {
    /*
     * No path is given, so use the defaults.
     */
    found = GetConfigFile(full_path, GetDefaultConfigDir(),
                          config_default_filename_.c_str());
    if (!found) {
      config_path_file.strcpy(full_path);
      found = GetConfigIncludePath(full_path, GetDefaultConfigDir());
    }
    if (!found) {
      Jmsg2(nullptr, M_ERROR, 0,
            _("Failed to read config file at the default locations "
              "\"%s\" (config file path) and \"%s\" (config include "
              "directory).\n"),
            config_path_file.c_str(), full_path.c_str());
    }
  } else if (PathExists(cf_.c_str())) {
    /*
     * Path is given and exists.
     */
    if (PathIsDirectory(cf_.c_str())) {
      found = GetConfigFile(full_path, cf_.c_str(),
                            config_default_filename_.c_str());
      if (!found) {
        config_path_file.strcpy(full_path);
        found = GetConfigIncludePath(full_path, cf_.c_str());
      }
      if (!found) {
        Jmsg3(nullptr, M_ERROR, 0,
              _("Failed to find configuration files under directory \"%s\". "
                "Did look for \"%s\" (config file path) and \"%s\" (config "
                "include directory).\n"),
              cf_.c_str(), config_path_file.c_str(), full_path.c_str());
      }
    } else {
      full_path.strcpy(cf_.c_str());
      PathGetDirectory(config_dir, full_path);
      config_dir_ = config_dir.c_str();
      found = true;
    }
  } else if (config_default_filename_.empty()) {
    /*
     * Compatibility with older versions.
     * If config_default_filename_ is not set,
     * cf_ may contain what is expected in config_default_filename_.
     */
    found = GetConfigFile(full_path, GetDefaultConfigDir(), cf_.c_str());
    if (!found) {
      Jmsg2(nullptr, M_ERROR, 0,
            _("Failed to find configuration files at \"%s\" and \"%s\".\n"),
            cf_.c_str(), full_path.c_str());
    }
  } else {
    Jmsg1(nullptr, M_ERROR, 0, _("Failed to read config file \"%s\"\n"),
          cf_.c_str());
  }

  if (found) { set_env("BAREOS_CFGDIR", config_dir_.c_str()); }

  return found;
}

BareosResource** ConfigurationParser::SaveResources()
{
  int num = r_last_ - r_first_ + 1;
  BareosResource** res =
      (BareosResource**)malloc(num * sizeof(BareosResource*));

  for (int i = 0; i < num; i++) {
    res[i] = res_head_[i];
    res_head_[i] = nullptr;
  }

  return res;
}

bool ConfigurationParser::RemoveResource(int rcode, const char* name)
{
  int rindex = rcode - r_first_;
  BareosResource* last;

  /*
   * Remove resource from list.
   *
   * Note: this is intended for removing a resource that has just been added,
   * but proven to be incorrect (added by console command "configure add").
   * For a general approach, a check if this resource is referenced by other
   * resources must be added. If it is referenced, don't remove it.
   */
  last = nullptr;
  for (BareosResource* res = res_head_[rindex]; res; res = res->next_) {
    if (bstrcmp(res->resource_name_, name)) {
      if (!last) {
        Dmsg2(900,
              _("removing resource %s, name=%s (first resource in list)\n"),
              ResToStr(rcode), name);
        res_head_[rindex] = res->next_;
      } else {
        Dmsg2(900, _("removing resource %s, name=%s\n"), ResToStr(rcode), name);
        last->next_ = res->next_;
      }
      res->next_ = nullptr;
      FreeResourceCb_(res, rcode);
      return true;
    }
    last = res;
  }

  /*
   * Resource with this name not found
   */
  return false;
}

void ConfigurationParser::DumpResources(void sendit(void* sock,
                                                    const char* fmt,
                                                    ...),
                                        void* sock,
                                        bool hide_sensitive_data)
{
  for (int i = r_first_; i <= r_last_; i++) {
    if (res_head_[i - r_first_]) {
      DumpResourceCb_(i, res_head_[i - r_first_], sendit, sock,
                      hide_sensitive_data, false);
    }
  }
}

bool ConfigurationParser::GetPathOfResource(PoolMem& path,
                                            const char* component,
                                            const char* resourcetype,
                                            const char* name,
                                            bool set_wildcards)
{
  PoolMem rel_path(PM_FNAME);
  PoolMem directory(PM_FNAME);
  PoolMem resourcetype_lowercase(resourcetype);
  resourcetype_lowercase.toLower();

  if (!component) {
    if (!config_include_dir_.empty()) {
      component = config_include_dir_.c_str();
    } else {
      return false;
    }
  }

  if (resourcetype_lowercase.strlen() <= 0) {
    if (set_wildcards) {
      resourcetype_lowercase.strcpy("*");
    } else {
      return false;
    }
  }

  if (!name) {
    if (set_wildcards) {
      name = "*";
    } else {
      return false;
    }
  }

  path.strcpy(config_dir_.c_str());
  rel_path.bsprintf(config_include_naming_format_.c_str(), component,
                    resourcetype_lowercase.c_str(), name);
  PathAppend(path, rel_path);

  return true;
}

bool ConfigurationParser::GetPathOfNewResource(PoolMem& path,
                                               PoolMem& extramsg,
                                               const char* component,
                                               const char* resourcetype,
                                               const char* name,
                                               bool error_if_exists,
                                               bool create_directories)
{
  PoolMem rel_path(PM_FNAME);
  PoolMem directory(PM_FNAME);
  PoolMem resourcetype_lowercase(resourcetype);
  resourcetype_lowercase.toLower();

  if (!GetPathOfResource(path, component, resourcetype, name, false)) {
    return false;
  }

  PathGetDirectory(directory, path);

  if (create_directories) { PathCreate(directory); }

  if (!PathExists(directory)) {
    extramsg.bsprintf("Resource config directory \"%s\" does not exist.\n",
                      directory.c_str());
    return false;
  }

  /*
   * Store name for temporary file in extramsg.
   * Can be used, if result is true.
   * Otherwise it contains an error message.
   */
  extramsg.bsprintf("%s.tmp", path.c_str());

  if (!error_if_exists) { return true; }

  /*
   * File should not exists, as it is going to be created.
   */
  if (PathExists(path)) {
    extramsg.bsprintf("Resource config file \"%s\" already exists.\n",
                      path.c_str());
    return false;
  }

  if (PathExists(extramsg)) {
    extramsg.bsprintf(
        "Temporary resource config file \"%s.tmp\" already exists.\n",
        path.c_str());
    return false;
  }

  return true;
}
