/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.
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
 * Kern Sibbald, January MM, September MM
 */
#define NEED_JANSSON_NAMESPACE 1
#include "include/bareos.h"
#include "console/console_globals.h"
#include "console/console_conf.h"
#include "lib/alist.h"
#include "lib/json.h"
#include "lib/resource_item.h"
#include "lib/output_formatter.h"
#include "lib/tls_resource_items.h"

namespace console {

static BareosResource* sres_head[R_LAST - R_FIRST + 1];
static BareosResource** res_head = sres_head;

static bool SaveResource(int type, ResourceItem* items, int pass);
static void FreeResource(BareosResource* sres, int type);
static void DumpResource(int type,
                         BareosResource* reshdr,
                         void sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose);

/* the first configuration pass uses this static memory */
static DirectorResource res_dir;
static ConsoleResource res_cons;

/* clang-format off */

static ResourceItem cons_items[] = {
  { "Name", CFG_TYPE_NAME, ITEM(res_cons, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, "The name of this resource." },
  { "Description", CFG_TYPE_STR, ITEM(res_cons, description_), 0, 0, NULL, NULL, NULL },
  { "RcFile", CFG_TYPE_DIR, ITEM(res_cons, rc_file), 0, 0, NULL, NULL, NULL },
  { "HistoryFile", CFG_TYPE_DIR, ITEM(res_cons, history_file), 0, 0, NULL, NULL, NULL },
  { "HistoryLength", CFG_TYPE_PINT32, ITEM(res_cons, history_length), 0, CFG_ITEM_DEFAULT, "100", NULL, NULL },
  { "Password", CFG_TYPE_MD5PASSWORD, ITEM(res_cons, password), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "Director", CFG_TYPE_STR, ITEM(res_cons, director), 0, 0, NULL, NULL, NULL },
  { "HeartbeatInterval", CFG_TYPE_TIME, ITEM(res_cons, heartbeat_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL },
  TLS_COMMON_CONFIG(res_cons),
  TLS_CERT_CONFIG(res_cons),
  {nullptr, 0, {nullptr}, nullptr, 0, 0, nullptr, nullptr, nullptr}
};

static ResourceItem dir_items[] = {
  { "Name", CFG_TYPE_NAME, ITEM(res_dir, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "Description", CFG_TYPE_STR, ITEM(res_dir, description_), 0, 0, NULL, NULL, NULL },
  { "DirPort", CFG_TYPE_PINT32, ITEM(res_dir, DIRport), 0, CFG_ITEM_DEFAULT, DIR_DEFAULT_PORT, NULL, NULL },
  { "Address", CFG_TYPE_STR, ITEM(res_dir, address), 0, 0, NULL, NULL, NULL },
  { "Password", CFG_TYPE_MD5PASSWORD, ITEM(res_dir, password_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "HeartbeatInterval", CFG_TYPE_TIME, ITEM(res_dir, heartbeat_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL },
  TLS_COMMON_CONFIG(res_dir),
  TLS_CERT_CONFIG(res_dir),
  {nullptr, 0, {nullptr}, nullptr, 0, 0, nullptr, nullptr, nullptr}
};

static ResourceTable resources[] = {
  { "Console", cons_items, R_CONSOLE, sizeof(ConsoleResource),
      [] (){ return new(&res_cons) ConsoleResource(); }, &res_cons },
  { "Director", dir_items, R_DIRECTOR, sizeof(DirectorResource),
      [] (){ return new(&res_dir) DirectorResource(); }, &res_dir },
  {nullptr, nullptr, 0, 0, nullptr, nullptr}
};

/* clang-format on */


static void DumpResource(int type,
                         BareosResource* res,
                         void sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose)
{
  PoolMem buf;
  bool recurse = true;

  if (!res) {
    sendit(sock, _("Warning: no \"%s\" resource (%d) defined.\n"),
           my_config->ResToStr(type), type);
    return;
  }
  if (type < 0) {  // no recursion
    type = -type;
    recurse = false;
  }

  switch (type) {
    default:
      res->PrintConfig(buf, *my_config);
      break;
  }
  sendit(sock, "%s", buf.c_str());

  if (recurse && res->next_) {
    DumpResource(type, res->next_, sendit, sock, hide_sensitive_data, verbose);
  }
}

static void FreeResource(BareosResource* res, int type)
{
  if (!res) return;

  BareosResource* next_resource = (BareosResource*)res->next_;

  if (res->resource_name_) {
    free(res->resource_name_);
    res->resource_name_ = nullptr;
  }
  if (res->description_) {
    free(res->description_);
    res->description_ = nullptr;
  }

  switch (type) {
    case R_CONSOLE: {
      ConsoleResource* p = dynamic_cast<ConsoleResource*>(res);
      if (p->rc_file) { free(p->rc_file); }
      if (p->history_file) { free(p->history_file); }
      if (p->tls_cert_.allowed_certificate_common_names_) {
        p->tls_cert_.allowed_certificate_common_names_->destroy();
        free(p->tls_cert_.allowed_certificate_common_names_);
      }
      delete p;
      break;
    }
    case R_DIRECTOR: {
      DirectorResource* p = dynamic_cast<DirectorResource*>(res);
      if (p->address) { free(p->address); }
      if (p->tls_cert_.allowed_certificate_common_names_) {
        p->tls_cert_.allowed_certificate_common_names_->destroy();
        free(p->tls_cert_.allowed_certificate_common_names_);
      }
      delete p;
      break;
    }
    default:
      printf(_("Unknown resource type %d\n"), type);
      break;
  }
  if (next_resource) { FreeResource(next_resource, type); }
}

static bool SaveResource(int type, ResourceItem* items, int pass)
{
  int rindex = type - R_FIRST;
  int i;
  int error = 0;

  // Ensure that all required items are present
  for (i = 0; items[i].name; i++) {
    if (items[i].flags & CFG_ITEM_REQUIRED) {
      if (!BitIsSet(i, items[i].static_resource->item_present_)) {
        Emsg2(M_ABORT, 0,
              _("%s item is required in %s resource, but not found.\n"),
              items[i].name, resources[rindex].name);
      }
    }
  }

  // save previously discovered pointers into dynamic memory
  if (pass == 2) {
    switch (type) {
      case R_CONSOLE: {
        ConsoleResource* p = dynamic_cast<ConsoleResource*>(
            my_config->GetResWithName(R_CONSOLE, res_cons.resource_name_));
        if (!p) {
          Emsg1(M_ABORT, 0, _("Cannot find Console resource %s\n"),
                res_cons.resource_name_);
        } else {
          p->tls_cert_.allowed_certificate_common_names_ =
              res_cons.tls_cert_.allowed_certificate_common_names_;
        }
        break;
      }
      case R_DIRECTOR: {
        DirectorResource* p = dynamic_cast<DirectorResource*>(
            my_config->GetResWithName(R_DIRECTOR, res_dir.resource_name_));
        if (!p) {
          Emsg1(M_ABORT, 0, _("Cannot find Director resource %s\n"),
                res_dir.resource_name_);
        } else {
          p->tls_cert_.allowed_certificate_common_names_ =
              p->tls_cert_.allowed_certificate_common_names_;
        }
        break;
      }
      default:
        Emsg1(M_ERROR, 0, _("Unknown resource type %d\n"), type);
        error = 1;
        break;
    }

    return (error == 0);
  }

  if (!error) {
    BareosResource* new_resource = nullptr;
    switch (resources[rindex].rcode) {
      case R_DIRECTOR: {
        DirectorResource* p = new DirectorResource;
        *p = res_dir;
        new_resource = p;
        break;
      }
      case R_CONSOLE: {
        ConsoleResource* p = new ConsoleResource;
        *p = res_cons;
        new_resource = p;
        break;
      }
      default:
        Emsg1(M_ERROR_TERM, 0, "Unknown resource type: %d\n",
              resources[rindex].rcode);
        return false;
    }
    my_config->AppendToResourcesChain(new_resource, type);
  }
  return (error == 0);
}

static void ConfigReadyCallback(ConfigurationParser& my_config)
{
  std::map<int, std::string> map{{R_DIRECTOR, "R_DIRECTOR"},
                                 {R_CONSOLE, "R_CONSOLE"}};
  my_config.InitializeQualifiedResourceNameTypeConverter(map);
}

ConfigurationParser* InitConsConfig(const char* configfile, int exit_code)
{
  ConfigurationParser* config = new ConfigurationParser(
      configfile, nullptr, nullptr, nullptr, nullptr, nullptr, exit_code,
      R_FIRST, R_LAST, resources, res_head, default_config_filename.c_str(),
      "bconsole.d", ConfigReadyCallback, SaveResource, DumpResource,
      FreeResource);
  if (config) { config->r_own_ = R_CONSOLE; }
  return config;
}

#ifdef HAVE_JANSSON
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  InitializeJson();

  json_t* json = json_object();
  json_object_set_new(json, "format-version", json_integer(2));
  json_object_set_new(json, "component", json_string("bconsole"));
  json_object_set_new(json, "version", json_string(VERSION));

  json_t* json_resource_object = json_object();
  json_object_set(json, "resource", json_resource_object);
  json_t* bconsole = json_object();
  json_object_set(json_resource_object, "bconsole", bconsole);

  ResourceTable* resources = my_config->resources_;
  for (; resources->name; ++resources) {
    json_object_set(bconsole, resources->name, json_items(resources->items));
  }

  PmStrcat(buffer, json_dumps(json, JSON_INDENT(2)));
  json_decref(json);

  return true;
}
#else
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  PmStrcat(buffer, "{ \"success\": false, \"message\": \"not available\" }");
  return false;
}
#endif
} /* namespace console */
