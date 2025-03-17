/* Copyright (C) 2009-2022 Greenbone AG
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file manage_sql_nvts.c
 * @brief GVM management layer: NVTs
 *
 * The NVT parts of the GVM management layer.
 */

/**
 * @brief Enable extra GNU functions.
 */
#define _GNU_SOURCE         /* See feature_test_macros(7) */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>

#include <gvm/base/nvti.h>
#include "glibconfig.h"
#include "manage.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gvm/util/jsonpull.h>
#include <gvm/base/cvss.h>

#include "manage_sql_nvts.h"
#include "manage_preferences.h"
#include "manage_sql.h"
#include "manage_sql_configs.h"
#include "manage_sql_secinfo.h"
#include "sql.h"
#include "utils.h"

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "md manage"


/* Headers from backend specific manage_xxx.c file. */

void
create_tables_nvt (const gchar *);


/* NVT related global options */

/**
 * @brief Max number of rows inserted per statement.
 */
static int vt_ref_insert_size = VT_REF_INSERT_SIZE_DEFAULT;

/**
 * @brief Max number of rows inserted per statement.
 */
static int vt_sev_insert_size = VT_SEV_INSERT_SIZE_DEFAULT;


/* NVT's. */

/**
 * @brief Set the VT ref insert size.
 *
 * @param new_size  New size.
 */
void
set_vt_ref_insert_size (int new_size)
{
  if (new_size < 0)
    vt_ref_insert_size = 0;
  else
    vt_ref_insert_size = new_size;
}

/**
 * @brief Set the VT severity insert size.
 *
 * @param new_size  New size.
 */
void
set_vt_sev_insert_size (int new_size)
{
  if (new_size < 0)
    vt_sev_insert_size = 0;
  else
    vt_sev_insert_size = new_size;
}

/**
 * @brief Ensures the sanity of nvts cache in DB.
 */
void
check_db_nvts ()
{
  /* Ensure the nvti cache update flag exists and is clear. */
  if (sql_int ("SELECT count(*) FROM %s.meta"
               " WHERE name = 'update_nvti_cache';",
               sql_schema ()))
    sql ("UPDATE %s.meta SET value = 0 WHERE name = 'update_nvti_cache';",
         sql_schema ());
  else
    sql ("INSERT INTO %s.meta (name, value)"
         " VALUES ('update_nvti_cache', 0);",
         sql_schema ());
}

/**
 * @brief Get the name of an NVT.
 *
 * @param[in]  nvt  NVT.
 *
 * @return Freshly allocated name of NVT if possible, else NULL.
 */
char *
manage_nvt_name (nvt_t nvt)
{
  return sql_string ("SELECT name FROM nvts WHERE id = %llu;", nvt);
}

/**
 * @brief Get the name of an NVT given its OID.
 *
 * @param[in]  oid  OID of NVT.
 *
 * @return Name of NVT if possible, else NULL.
 */
char *
nvt_name (const char *oid)
{
  gchar *quoted_oid = sql_quote (oid);
  char *ret = sql_string ("SELECT name FROM nvts WHERE oid = '%s' LIMIT 1;",
                          quoted_oid);
  g_free (quoted_oid);
  return ret;
}

/**
 * @brief Return feed version of the plugins in the plugin cache.
 *
 * @return Feed version of plugins if the plugins are cached, else NULL.
 */
char*
nvts_feed_version ()
{
  return sql_string ("SELECT value FROM %s.meta"
                     " WHERE name = 'nvts_feed_version';",
                     sql_schema ());
}

/**
 * @brief Return feed version of the plugins as seconds since epoch.
 *
 * @return Feed version in seconds since epoch of plugins.
 */
time_t
nvts_feed_version_epoch ()
{
  gchar *feed_version;
  struct tm tm;

  feed_version = nvts_feed_version ();

  if (feed_version == NULL)
    return 0;

  memset (&tm, 0, sizeof (struct tm));
  strptime (feed_version, "%Y%m%d%H%M%S", &tm);

  g_free (feed_version);

  return mktime (&tm);
}

/**
 * @brief Set the feed version of the plugins in the plugin cache.
 *
 * @param[in]  feed_version  New feed version.
 *
 * Also queue an update to the nvti cache.
 */
void
set_nvts_feed_version (const char *feed_version)
{
  gchar* quoted = sql_quote (feed_version);
  sql ("DELETE FROM %s.meta WHERE name = 'nvts_feed_version';",
       sql_schema ());
  sql ("INSERT INTO %s.meta (name, value)"
       " VALUES ('nvts_feed_version', '%s');",
       sql_schema (),
       quoted);
  g_free (quoted);
}

/**
 * @brief Find an NVT given an identifier.
 *
 * @param[in]   oid  An NVT identifier.
 * @param[out]  nvt  NVT return, 0 if successfully failed to find task.
 *
 * @return FALSE on success (including if failed to find NVT), TRUE on error.
 */
gboolean
find_nvt (const char* oid, nvt_t* nvt)
{
  gchar *quoted_oid;
  int ret;

  quoted_oid = sql_quote (oid);
  ret = sql_int64 (nvt,
                   "SELECT id FROM nvts WHERE oid = '%s';",
                   quoted_oid);
  g_free (quoted_oid);
  switch (ret)
    {
      case 0:
        break;
      case 1:        /* Too few rows in result of query. */
        *nvt = 0;
        break;
      default:       /* Programming error. */
        assert (0);
      case -1:
        return TRUE;
        break;
    }

  return FALSE;
}

/**
 * @brief Initialise an NVT iterator.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  get         GET data.
 * @param[in]  name        Name of the info
 *
 * @return 0 success, 1 failed to find NVT, 2 failed to find filter,
 *         -1 error.
 */
int
init_nvt_info_iterator (iterator_t* iterator, get_data_t *get, const char *name)
{
  static const char *filter_columns[] = NVT_INFO_ITERATOR_FILTER_COLUMNS;
  static column_t columns[] = NVT_ITERATOR_COLUMNS;
  gchar *clause = NULL;
  int ret;

  if (get->id)
    {
      gchar *quoted = sql_quote (get->id);
      clause = g_strdup_printf (" AND uuid = '%s'", quoted);
      g_free (quoted);
    }
  else if (name)
    {
      gchar *quoted = sql_quote (name);
      clause = g_strdup_printf (" AND name = '%s'", quoted);
      g_free (quoted);
      /* The entry is specified by name, so filtering just gets in the way. */
      g_free (get->filter);
      get->filter = NULL;
    }

  ret = init_get_iterator (iterator,
                           "nvt",
                           get,
                           /* Columns. */
                           columns,
                           /* Columns for trashcan. */
                           NULL,
                           filter_columns,
                           0,
                           NULL,
                           clause,
                           0);

  g_free (clause);
  return ret;
}

/**
 * @brief Initialise an NVT iterator not limited to a name.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  get         GET data.
 *
 * @return 0 success, 1 failed to find NVT, 2 failed to find filter,
 *         -1 error.
 */
int
init_nvt_info_iterator_all (iterator_t* iterator, get_data_t *get)
{
  return init_nvt_info_iterator(iterator, get, NULL);
}

/**
 * @brief Get NVT iterator SELECT columns.
 *
 * @return SELECT columns
 */
static gchar *
nvt_iterator_columns ()
{
  static column_t select_columns[] = NVT_ITERATOR_COLUMNS;
  static gchar *columns = NULL;
  if (columns == NULL)
    columns = columns_build_select (select_columns);
  return columns;
}

/**
 * @brief Get NVT iterator SELECT columns.
 *
 * @return SELECT columns
 */
static gchar *
nvt_iterator_columns_nvts ()
{
  static column_t select_columns[] = NVT_ITERATOR_COLUMNS_NVTS;
  static gchar *columns = NULL;
  if (columns == NULL)
    columns = columns_build_select (select_columns);
  return columns;
}

/**
 * @brief Count number of nvt.
 *
 * @param[in]  get  GET params.
 *
 * @return Total number of cpes in filtered set.
 */
int
nvt_info_count (const get_data_t *get)
{
  static const char *extra_columns[] = NVT_INFO_ITERATOR_FILTER_COLUMNS;
  static column_t columns[] = NVT_ITERATOR_COLUMNS;
  return count ("nvt", get, columns, NULL, extra_columns, 0, 0, 0,
                FALSE);
}

/**
 * @brief Count number of nvts created or modified after a given time.
 *
 * @param[in]  get            GET params.
 * @param[in]  count_time     Time NVTs must be created or modified after.
 * @param[in]  get_modified   Whether to get the modification time.
 *
 * @return Total number of nvts in filtered set.
 */
int
nvt_info_count_after (const get_data_t *get, time_t count_time,
                      gboolean get_modified)
{
  static const char *filter_columns[] = NVT_INFO_ITERATOR_FILTER_COLUMNS;
  static column_t columns[] = NVT_ITERATOR_COLUMNS;
  gchar *extra_where;
  int ret;

  if (get_modified)
    extra_where = g_strdup_printf (" AND modification_time > %ld"
                                   " AND creation_time <= %ld",
                                   count_time,
                                   count_time);
  else
    extra_where = g_strdup_printf (" AND creation_time > %ld",
                                   count_time);

  ret = count ("nvt", get, columns, NULL, filter_columns, 0, 0, extra_where,
               FALSE);

  g_free (extra_where);
  return ret;
}

/**
 * @brief Return SQL for selecting NVT's of a config from one family.
 *
 * @param[in]  config      Config.
 * @param[in]  family      Family to limit selection to.
 * @param[in]  ascending   Whether to sort ascending or descending.
 * @param[in]  sort_field  Field to sort on, or NULL for "nvts.id".
 *
 * @return Freshly allocated SELECT statement on success, or NULL on error.
 */
static gchar *
select_config_nvts (const config_t config, const char* family, int ascending,
                    const char* sort_field)
{
  gchar *quoted_selector, *quoted_family, *sql;
  char *selector;

  selector = config_nvt_selector (config);
  if (selector == NULL)
    /* The config should always have a selector. */
    return NULL;

  quoted_selector = sql_quote (selector);
  free (selector);

  quoted_family = sql_quote (family);

  if (config_nvts_growing (config))
    {
      int constraining;

      /* The number of NVT's can increase. */

      constraining = config_families_growing (config);

      if (constraining)
        {
          /* Constraining the universe. */

          if (sql_int ("SELECT COUNT(*) FROM nvt_selectors WHERE name = '%s';",
                       quoted_selector)
              == 1)
            /* There is one selector, it should be the all selector. */
            sql = g_strdup_printf
                   ("SELECT %s"
                    " FROM nvts WHERE family = '%s'"
                    " ORDER BY %s %s;",
                    nvt_iterator_columns (),
                    quoted_family,
                    sort_field ? sort_field : "name",
                    ascending ? "ASC" : "DESC");
          else
            {
              /* There are multiple selectors. */

              if (sql_int ("SELECT COUNT(*) FROM nvt_selectors"
                           " WHERE name = '%s' AND exclude = 1"
                           " AND type = "
                           G_STRINGIFY (NVT_SELECTOR_TYPE_FAMILY)
                           " AND family_or_nvt = '%s'"
                           ";",
                           quoted_selector,
                           quoted_family))
                /* The family is excluded, just iterate the NVT includes. */
                sql = g_strdup_printf
                       ("SELECT %s"
                        " FROM nvts, nvt_selectors"
                        " WHERE"
                        " nvts.family = '%s'"
                        " AND nvt_selectors.name = '%s'"
                        " AND nvt_selectors.family = '%s'"
                        " AND nvt_selectors.type = "
                        G_STRINGIFY (NVT_SELECTOR_TYPE_NVT)
                        " AND nvt_selectors.exclude = 0"
                        " AND nvts.oid = nvt_selectors.family_or_nvt"
                        " ORDER BY %s %s;",
                        nvt_iterator_columns_nvts (),
                        quoted_family,
                        quoted_selector,
                        quoted_family,
                        sort_field ? sort_field : "nvts.name",
                        ascending ? "ASC" : "DESC");
              else
                /* The family is included.
                 *
                 * Iterate all NVT's minus excluded NVT's. */
                sql = g_strdup_printf
                       ("SELECT %s"
                        " FROM nvts"
                        " WHERE family = '%s'"
                        " EXCEPT"
                        " SELECT %s"
                        " FROM nvt_selectors, nvts"
                        " WHERE"
                        " nvts.family = '%s'"
                        " AND nvt_selectors.name = '%s'"
                        " AND nvt_selectors.family = '%s'"
                        " AND nvt_selectors.type = "
                        G_STRINGIFY (NVT_SELECTOR_TYPE_NVT)
                        " AND nvt_selectors.exclude = 1"
                        " AND nvts.oid = nvt_selectors.family_or_nvt"
                        " ORDER BY %s %s;",
                        nvt_iterator_columns (),
                        quoted_family,
                        nvt_iterator_columns_nvts (),
                        quoted_family,
                        quoted_selector,
                        quoted_family,
                        /* This works around "ERROR: missing FROM-clause" from
                         * Postgres when using nvts.name. */
                        sort_field && strcmp (sort_field, "nvts.name")
                         ? sort_field : "3", /* 3 is nvts.name. */
                        ascending ? "ASC" : "DESC");
            }
        }
      else
        {
          int all;

          /* Generating from empty. */

          all = sql_int ("SELECT COUNT(*) FROM nvt_selectors"
                         " WHERE name = '%s' AND exclude = 0"
                         " AND type = "
                         G_STRINGIFY (NVT_SELECTOR_TYPE_FAMILY)
                         " AND family_or_nvt = '%s';",
                         quoted_selector,
                         quoted_family);

          if (all)
            /* There is a family include for this family. */
            sql = g_strdup_printf
                   ("SELECT %s"
                    " FROM nvts"
                    " WHERE family = '%s'"
                    " EXCEPT"
                    " SELECT %s"
                    " FROM nvt_selectors, nvts"
                    " WHERE"
                    " nvts.family = '%s'"
                    " AND nvt_selectors.name = '%s'"
                    " AND nvt_selectors.family = '%s'"
                    " AND nvt_selectors.type = "
                    G_STRINGIFY (NVT_SELECTOR_TYPE_NVT)
                    " AND nvt_selectors.exclude = 1"
                    " AND nvts.oid = nvt_selectors.family_or_nvt"
                    " ORDER BY %s %s;",
                    nvt_iterator_columns (),
                    quoted_family,
                    nvt_iterator_columns_nvts (),
                    quoted_family,
                    quoted_selector,
                    quoted_family,
                    /* This works around "ERROR: missing FROM-clause" from
                     * Postgres when using nvts.name. */
                    sort_field && strcmp (sort_field, "nvts.name")
                     ? sort_field : "3", /* 3 is nvts.name. */
                    ascending ? "ASC" : "DESC");
          else
            sql = g_strdup_printf
                   (" SELECT %s"
                    " FROM nvt_selectors, nvts"
                    " WHERE"
                    " nvts.family = '%s'"
                    " AND nvt_selectors.name = '%s'"
                    " AND nvt_selectors.family = '%s'"
                    " AND nvt_selectors.type = "
                    G_STRINGIFY (NVT_SELECTOR_TYPE_NVT)
                    " AND nvt_selectors.exclude = 0"
                    " AND nvts.oid = nvt_selectors.family_or_nvt"
                    " ORDER BY %s %s;",
                    nvt_iterator_columns_nvts (),
                    quoted_family,
                    quoted_selector,
                    quoted_family,
                    sort_field ? sort_field : "nvts.name",
                    ascending ? "ASC" : "DESC");
        }
    }
  else
    {
      /* The number of NVT's is static.  Assume a simple list of NVT
       * includes. */

      sql = g_strdup_printf
             ("SELECT %s"
              " FROM nvt_selectors, nvts"
              " WHERE nvts.family = '%s'"
              " AND nvt_selectors.exclude = 0"
              " AND nvt_selectors.type = " G_STRINGIFY (NVT_SELECTOR_TYPE_NVT)
              " AND nvt_selectors.name = '%s'"
              " AND nvts.oid = nvt_selectors.family_or_nvt"
              " ORDER BY %s %s;",
              nvt_iterator_columns_nvts (),
              quoted_family,
              quoted_selector,
              sort_field ? sort_field : "nvts.id",
              ascending ? "ASC" : "DESC");
    }

  g_free (quoted_selector);
  g_free (quoted_family);

  return sql;
}

/**
 * @brief Initialise an NVT iterator.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  nvt         NVT to iterate over, all if 0.
 * @param[in]  config      Config to limit selection to.  NULL for all NVTs.
 *                         Overridden by \arg nvt.
 * @param[in]  family      Family to limit selection to.  NULL for all NVTs.
 *                         Overridden by \arg config.
 * @param[in]  category    Category to limit selection to.  NULL for all.
 * @param[in]  ascending   Whether to sort ascending or descending.
 * @param[in]  sort_field  Field to sort on, or NULL for "id".
 */
void
init_nvt_iterator (iterator_t* iterator, nvt_t nvt, config_t config,
                   const char* family, const char *category, int ascending,
                   const char* sort_field)
{
  assert ((nvt && family) == 0);

  if (nvt)
    {
      gchar* sql;
      sql = g_strdup_printf ("SELECT %s"
                             " FROM nvts WHERE id = %llu;",
                             nvt_iterator_columns (),
                             nvt);
      init_iterator (iterator, "%s", sql);
      g_free (sql);
    }
  else if (config)
    {
      gchar* sql;
      if (family == NULL) abort ();
      sql = select_config_nvts (config, family, ascending, sort_field);
      if (sql)
        {
          init_iterator (iterator, "%s", sql);
          g_free (sql);
        }
      else
        init_iterator (iterator,
                       "SELECT %s"
                       " FROM nvts LIMIT 0;",
                       nvt_iterator_columns ());
    }
  else if (family)
    {
      gchar *quoted_family = sql_quote (family);
      init_iterator (iterator,
                     "SELECT %s"
                     " FROM nvts"
                     " WHERE family = '%s'"
                     " ORDER BY %s %s;",
                     nvt_iterator_columns (),
                     quoted_family,
                     sort_field ? sort_field : "name",
                     ascending ? "ASC" : "DESC");
      g_free (quoted_family);
    }
  else if (category)
    {
      gchar *quoted_category;
      quoted_category = sql_quote (category);
      init_iterator (iterator,
                     "SELECT %s"
                     " FROM nvts"
                     " WHERE category = '%s'"
                     " ORDER BY %s %s;",
                     nvt_iterator_columns (),
                     quoted_category,
                     sort_field ? sort_field : "name",
                     ascending ? "ASC" : "DESC");
      g_free (quoted_category);
    }
  else
    init_iterator (iterator,
                   "SELECT %s"
                   " FROM nvts"
                   " ORDER BY %s %s;",
                   nvt_iterator_columns (),
                   sort_field ? sort_field : "name",
                   ascending ? "ASC" : "DESC");
}

/**
 * @brief Initialise an NVT iterator, for NVTs of a certain CVE.
 *
 * @param[in]  iterator    Iterator.
 * @param[in]  cve         CVE name.
 * @param[in]  ascending   Whether to sort ascending or descending.
 * @param[in]  sort_field  Field to sort on, or NULL for "id".
 */
void
init_cve_nvt_iterator (iterator_t* iterator, const char *cve, int ascending,
                       const char* sort_field)
{
  init_iterator (iterator,
                 "SELECT %s"
                 " FROM nvts"
                 " WHERE cve %s '%%%s, %%'"
                 "    OR cve %s '%%%s'"
                 " ORDER BY %s %s;",
                 nvt_iterator_columns (),
                 sql_ilike_op (),
                 cve ? cve : "",
                 sql_ilike_op (),
                 cve ? cve : "",
                 sort_field ? sort_field : "name",
                 ascending ? "ASC" : "DESC");
}

/**
 * @brief Get the OID from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return OID, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_oid, GET_ITERATOR_COLUMN_COUNT);

/**
 * @brief Get the name from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Name, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_name, GET_ITERATOR_COLUMN_COUNT + 2);

/**
 * @brief Get the tag from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Tag, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_tag, GET_ITERATOR_COLUMN_COUNT + 4);

/**
 * @brief Get the category from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Category.
 */
int
nvt_iterator_category (iterator_t* iterator)
{
  int ret;
  if (iterator->done) return -1;
  ret = iterator_int (iterator, GET_ITERATOR_COLUMN_COUNT + 5);
  return ret;
}

/**
 * @brief Get the family from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Family, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_family, GET_ITERATOR_COLUMN_COUNT + 6);

/**
 * @brief Get the cvss_base from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Cvss_base, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_cvss_base, GET_ITERATOR_COLUMN_COUNT + 7);

/**
 * @brief Get the qod from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return QoD, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_qod, GET_ITERATOR_COLUMN_COUNT + 10);

/**
 * @brief Get the qod_type from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return QoD type, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_qod_type, GET_ITERATOR_COLUMN_COUNT + 11);

/**
 * @brief Get the solution_type from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Solution Type, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_solution_type, GET_ITERATOR_COLUMN_COUNT + 12);

/**
 * @brief Get the solution from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Solution, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_solution, GET_ITERATOR_COLUMN_COUNT + 14);

/**
 * @brief Get the summary from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Summary, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_summary, GET_ITERATOR_COLUMN_COUNT + 15);

/**
 * @brief Get the insight from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Insight, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_insight, GET_ITERATOR_COLUMN_COUNT + 16);

/**
 * @brief Get the affected from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Affected, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_affected, GET_ITERATOR_COLUMN_COUNT + 17);

/**
 * @brief Get the impact from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Impact, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_impact, GET_ITERATOR_COLUMN_COUNT + 18);

/**
 * @brief Get the detection from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Detection, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_detection, GET_ITERATOR_COLUMN_COUNT + 19);

/**
 * @brief Get the solution method from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Solution method, or NULL if iteration is complete.  Freed by
 *         cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_solution_method, GET_ITERATOR_COLUMN_COUNT + 20);

/**
 * @brief Get the EPSS score selected by severity from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The EPSS score.
 */
double
nvt_iterator_epss_score (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 21);
  return ret;
}

/**
 * @brief Get the EPSS percentile selected by severity from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The EPSS percentile.
 */
double
nvt_iterator_epss_percentile (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 22);
  return ret;
}

/**
 * @brief Get the CVE of the EPSS score by severity from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return CVE-ID of the EPSS score, or NULL if iteration is complete.
 *         Freed by cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_epss_cve, GET_ITERATOR_COLUMN_COUNT + 23);

/**
 * @brief Get the maximum severity of CVEs with EPSS info from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The severity score.
 */
double
nvt_iterator_epss_severity (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 24);
  return ret;
}

/**
 * @brief Get whether the NVT has a severity for the max severity EPSS score.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Whether the severity exists.
 */
gboolean
nvt_iterator_has_epss_severity (iterator_t* iterator)
{
  gboolean ret;
  if (iterator->done) return -1;
  ret = iterator_string (iterator, GET_ITERATOR_COLUMN_COUNT + 24) != NULL;
  return ret;
}

/**
 * @brief Get the maximum EPSS score from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The maximum EPSS score.
 */
double
nvt_iterator_max_epss_score (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 25);
  return ret;
}

/**
 * @brief Get the maximum EPSS percentile from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The maximum EPSS percentile.
 */
double
nvt_iterator_max_epss_percentile (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 26);
  return ret;
}

/**
 * @brief Get the CVE of the maximum EPSS score from an NVT iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return CVE-ID of the maximum EPSS score, or NULL if iteration is complete.
 *         Freed by cleanup_iterator.
 */
DEF_ACCESS (nvt_iterator_max_epss_cve, GET_ITERATOR_COLUMN_COUNT + 27);

/**
 * @brief Get the severity of the maximum EPSS score from an NVT iterator.
 * @param[in]  iterator  Iterator.
 *
 * @return The severity score.
 */
double
nvt_iterator_max_epss_severity (iterator_t* iterator)
{
  double ret;
  if (iterator->done) return -1;
  ret = iterator_double (iterator, GET_ITERATOR_COLUMN_COUNT + 28);
  return ret;
}

/**
 * @brief Get whether the NVT has a severity for the max EPSS score.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return Whether the severity exists.
 */
gboolean
nvt_iterator_has_max_epss_severity (iterator_t* iterator)
{
  gboolean ret;
  if (iterator->done) return -1;
  ret = iterator_string (iterator, GET_ITERATOR_COLUMN_COUNT + 28) != NULL;
  return ret;
}

/**
 * @brief Get the default timeout of an NVT.
 *
 * @param[in]  oid  The OID of the NVT to get the timeout of.
 *
 * @return  Newly allocated string of the timeout in seconds or NULL.
 */
char *
nvt_default_timeout (const char* oid)
{
  return sql_string ("SELECT value FROM nvt_preferences"
                     " WHERE name = '%s:0:entry:timeout'",
                     oid);
}

/**
 * @brief Get the family of an NVT.
 *
 * @param[in]  oid  The OID of the NVT.
 *
 * @return Newly allocated string of the family, or NULL.
 */
char *
nvt_family (const char *oid)
{
  gchar *quoted_oid;
  char *ret;

  quoted_oid = sql_quote (oid);
  ret = sql_string ("SELECT family FROM nvts WHERE oid = '%s' LIMIT 1;",
                    quoted_oid);
  g_free (quoted_oid);
  return ret;
}

/**
 * @brief Get the number of NVTs in one or all families.
 *
 * @param[in]  family  Family name.  NULL for all families.
 *
 * @return Number of NVTs in family, or total number of nvts.
 */
int
family_nvt_count (const char *family)
{
  gchar *quoted_family;

  if (family == NULL)
    {
      static int nvt_count = -1;
      if (nvt_count == -1)
        nvt_count = sql_int ("SELECT COUNT(*) FROM nvts"
                             " WHERE family != 'Credentials';");
      return nvt_count;
    }

  quoted_family = sql_quote (family);
  int ret = sql_int ("SELECT COUNT(*) FROM nvts WHERE family = '%s';",
                     quoted_family);
  g_free (quoted_family);
  return ret;
}

/**
 * @brief Get the number of families.
 *
 * @return Total number of families.
 */
int
family_count ()
{
  return sql_int ("SELECT COUNT(distinct family) FROM nvts"
                  " WHERE family != 'Credentials';");
}

/**
 * @brief Initialise an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 * @param[in]  oid       OID of NVT.
 */
void
init_nvt_severity_iterator (iterator_t* iterator, const char *oid)
{
  gchar *quoted_oid;
  quoted_oid = sql_quote (oid ? oid : "");

  init_iterator (iterator,
                 "SELECT type, origin, iso_time(date), score, value"
                 " FROM vt_severities"
                 " WHERE vt_oid = '%s'",
                 quoted_oid);

  g_free (quoted_oid);
}

/**
 * @brief Gets the type from an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The type of the severity.
 */
DEF_ACCESS (nvt_severity_iterator_type, 0)

/**
 * @brief Gets the origin from an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The origin of the severity.
 */
DEF_ACCESS (nvt_severity_iterator_origin, 1);

/**
 * @brief Gets the date from an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The date of the severity in ISO time format.
 */
DEF_ACCESS (nvt_severity_iterator_date, 2);

/**
 * @brief Gets the score from an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The score of the severity.
 */
double
nvt_severity_iterator_score (iterator_t *iterator)
{
  return iterator_double (iterator, 3);
}

/**
 * @brief Gets the value from an NVT severity iterator.
 *
 * @param[in]  iterator  Iterator.
 *
 * @return The value of the severity in ISO time format.
 */
DEF_ACCESS (nvt_severity_iterator_value, 4);

/**
 * @brief Check VTs feed version status
 *
 * @return 0 VTs feed current, 1 VT update needed, -1 error.
 */
int
nvts_feed_version_status ()
{
#if OPENVASD
  return nvts_feed_version_status_internal_openvasd (NULL, NULL);
#else
  return nvts_feed_version_status_internal_osp (get_osp_vt_update_socket (),
                                                NULL,
                                                NULL);
#endif
}

/**
 * @brief Update VTs via OSP or openvasd.
 *
 * Expect to be called in the child after a fork.
 *
 * @param[in]  update_socket  Socket to use to contact ospd-openvas scanner.
 *
 * @return 0 success, -1 error, 1 VT integrity check failed.
 */
int
manage_update_nvt_cache (const gchar *update_socket)
{
  int ret;

#if OPENVASD
      ret = manage_update_nvt_cache_openvasd ();
#else
      ret = manage_update_nvt_cache_osp (update_socket);
#endif

  return ret;
}

/**
 * @brief Sync NVTs if newer NVTs are available.
 *
 * @param[in]  fork_update_nvt_cache  Function to do the update.
 *
 * @return PID of the forked process handling the VTs sync, -1 on error.
 */
pid_t
manage_sync_nvts (int (*fork_update_nvt_cache) (pid_t*))
{
  pid_t child_pid = -1;
  fork_update_nvt_cache (&child_pid);
  return child_pid;
}

/**
 * @brief Update or rebuild NVT db.
 *
 * Caller must get the lock.
 *
 * @param[in]  update  0 rebuild, else update.
 *
 * @return 0 success, -1 error, -1 no osp update socket, -2 could not connect
 *         to update socket, -3 failed to get scanner version
 */
int
update_or_rebuild_nvts (int update)
{
#if OPENVASD
   return update_or_rebuild_nvts_openvasd (update);
#else
   return update_or_rebuild_nvts_osp (update);
#endif
}

/**
 * @brief Rebuild NVT db.
 *
 * @param[in]  log_config  Log configuration.
 * @param[in]  database    Location of manage database.
 *
 * @return 0 success, 1 VT integrity check failed, -1 error,
 *         -2 database is too old,
 *         -3 database needs to be initialised from server,
 *         -5 database is too new, -6 sync active.
 */
int
manage_rebuild (GSList *log_config, const db_conn_info_t *database)
{
  int ret;
  static lockfile_t lockfile;

  g_info ("   Rebuilding NVTs.");

  switch (feed_lockfile_lock_timeout (&lockfile))
    {
      case 1:
        printf ("A feed sync is already running.\n");
        return -6;
      case -1:
        printf ("Error getting sync lock.\n");
        return -1;
    }

  ret = manage_option_setup (log_config, database,
                             0 /* avoid_db_check_inserts */);
  if (ret)
    {
      feed_lockfile_unlock (&lockfile);
      return ret;
    }

  sql_begin_immediate ();
  ret = update_or_rebuild_nvts (0);

  switch (ret)
    {
      case 0:
        sql_commit ();
        break;
      case -1:
        printf ("No OSP VT update socket found."
                " Use --osp-vt-update or change the 'OpenVAS Default'"
                " scanner to use the main ospd-openvas socket.\n");
        sql_rollback ();
        break;
      case -2:
        printf ("Failed to connect to OSP VT update socket.\n");
        sql_rollback ();
        break;
      case -3:
        printf ("Failed to get scanner_version.\n");
        sql_rollback ();
        break;
      default:
        printf ("Failed to update or rebuild nvts.\n");
        sql_rollback ();
        break;
    }

  if (ret == 0)
    update_scap_extra ();

  feed_lockfile_unlock (&lockfile);
  manage_option_cleanup ();

  return ret;
}

/**
 * @brief Dump the string used to calculate the VTs verification hash
 *  to stdout.
 *
 * @param[in]  log_config  Log configuration.
 * @param[in]  database    Location of manage database.
 *
 * @return 0 success, -1 error, -2 database is too old,
 *         -3 database needs to be initialised from server,
 *         -5 database is too new, -6 sync active.
 */
int
manage_dump_vt_verification (GSList *log_config,
                             const db_conn_info_t *database)
{
  int ret;
  static lockfile_t lockfile;
  char *verification_str;

  switch (feed_lockfile_lock_timeout (&lockfile))
    {
      case 1:
        printf ("A feed sync is already running.\n");
        return -6;
      case -1:
        printf ("Error getting sync lock.\n");
        return -1;
    }

  ret = manage_option_setup (log_config, database,
                             0 /* avoid_db_check_inserts */);
  if (ret)
    {
      feed_lockfile_unlock (&lockfile);
      return ret;
    }

  verification_str = sql_string ("SELECT vts_verification_str ();");
  printf ("%s\n", verification_str);

  feed_lockfile_unlock (&lockfile);
  manage_option_cleanup ();

  return 0;
}

/**
 * @brief Cleans up NVT related id sequences likely to run out.
 *
 * @return 0 success, -1 error.
 */
int
cleanup_nvt_sequences () {
  g_info ("Cleaning up NVT related id sequences...");
  sql_begin_immediate ();

  if (cleanup_ids_for_table ("nvts"))
    {
      sql_rollback ();
      return -1;
    }
  g_info ("Updating nvt references in tags to new row ids");
  sql ("UPDATE tag_resources"
       " SET resource = (SELECT id FROM nvts WHERE uuid = resource_uuid)"
       " WHERE resource_type = 'nvt';");
  sql ("UPDATE tag_resources_trash"
       " SET resource = (SELECT id FROM nvts WHERE uuid = resource_uuid)"
       " WHERE resource_type = 'nvt';");

  if (cleanup_ids_for_table ("vt_refs"))
    {
      sql_rollback ();
      return -1;
    }

  sql_commit ();
  return 0;
}
