/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

        Authors:    Domas Mituzas, Facebook ( domas at fb dot com )
                    Mark Leith, Oracle Corporation (mark dot leith at oracle dot com)
                    Andrew Hutchings, MariaDB Foundation (andrew at mariadb dot org)
                    Max Bubenick, Percona RDBA (max dot bubenick at percona dot com)
                    David Ducos, Percona (david dot ducos at percona dot com)
*/




#include <mysql.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <math.h>
#include "mydumper_start_dump.h"
#include "server_detect.h"
#include "regex.h"
#include "mydumper_common.h"
#include "mydumper_jobs.h"
#include "mydumper_database.h"
#include "mydumper_working_thread.h"
#include "mydumper_write.h"
#include "mydumper_masquerade.h"
#include "mydumper_global.h"
#include "connection.h"
#include "mydumper_arguments.h"

const gchar *insert_statement=INSERT;
guint statement_size = 1000000;
guint complete_insert = 0;
guint chunk_filesize = 0;
gboolean load_data = FALSE;
gboolean csv = FALSE;
gboolean clickhouse = FALSE;
gboolean include_header = FALSE;
const gchar *fields_enclosed_by=NULL;
gchar *fields_escaped_by=NULL;
gchar *fields_terminated_by=NULL;
gchar *lines_starting_by=NULL;
gchar *lines_terminated_by=NULL;
gchar *statement_terminated_by=NULL;
const gchar *fields_enclosed_by_ld=NULL;
gchar *lines_starting_by_ld=NULL;
gchar *lines_terminated_by_ld=NULL;
gchar *statement_terminated_by_ld=NULL;
gchar *fields_terminated_by_ld=NULL;
gboolean insert_ignore = FALSE;
gboolean replace = FALSE;
gboolean hex_blob = FALSE;



void message_dumping_data_short(struct table_job *tj){
  g_mutex_lock(innodb_table->mutex);
  g_mutex_lock(non_innodb_table->mutex);
  g_message("Thread %d: %s%s%s.%s%s%s [ %"G_GINT64_FORMAT"%% ] | Tables: %u/%u",
                    tj->td->thread_id,
                    identifier_quote_character_str, tj->dbt->database->name, identifier_quote_character_str, identifier_quote_character_str, tj->dbt->table, identifier_quote_character_str,
                    tj->dbt->rows_total!=0?100*tj->dbt->rows/tj->dbt->rows_total:0, g_list_length(innodb_table->list)+g_list_length(non_innodb_table->list),g_hash_table_size(all_dbts));
  g_mutex_unlock(innodb_table->mutex);
  g_mutex_unlock(non_innodb_table->mutex);
}

void message_dumping_data_long(struct table_job *tj){
  g_mutex_lock(innodb_table->mutex);
  g_mutex_lock(non_innodb_table->mutex);
  g_message("Thread %d: dumping data from %s%s%s.%s%s%s%s%s%s%s%s%s%s%s%s%s into %s | Completed: %"G_GINT64_FORMAT"%% | Remaining tables: %u / %u",
                    tj->td->thread_id,
                    identifier_quote_character_str, tj->dbt->database->name, identifier_quote_character_str, identifier_quote_character_str, tj->dbt->table, identifier_quote_character_str,
                    tj->partition?" ":"",tj->partition?tj->partition:"",
                     (tj->where->len || where_option   || tj->dbt->where) ? " WHERE " : "" , tj->where->len ? tj->where->str : "",
                     (tj->where->len && where_option )                    ? " AND "   : "" ,   where_option ?   where_option : "",
                    ((tj->where->len || where_option ) && tj->dbt->where) ? " AND "   : "" , tj->dbt->where ? tj->dbt->where : "",
                    order_by_primary_key && tj->dbt->primary_key_separated_by_comma ? " ORDER BY " : "", order_by_primary_key && tj->dbt->primary_key_separated_by_comma ? tj->dbt->primary_key_separated_by_comma : "",
                    tj->rows->filename, tj->dbt->rows_total!=0?100*tj->dbt->rows/tj->dbt->rows_total:0, g_list_length(innodb_table->list)+g_list_length(non_innodb_table->list),g_hash_table_size(all_dbts));
  g_mutex_unlock(innodb_table->mutex);
  g_mutex_unlock(non_innodb_table->mutex);
}

void (*message_dumping_data)(struct table_job *tj);

void initialize_write(){
  if (verbose > 3)
    message_dumping_data=&message_dumping_data_long;
  else
    message_dumping_data=&message_dumping_data_short;

  // rows chunks have precedence over chunk_filesize
  if (starting_chunk_step_size > 0 && chunk_filesize > 0) {
//    chunk_filesize = 0;
//    g_warning("--chunk-filesize disabled by --rows option");
    g_warning("We are going to chunk by row and by filesize when possible");
  }

  g_assert(fields_enclosed_by); // initialized in detect_quote_character()

  if(fields_enclosed_by_ld && strlen(fields_enclosed_by_ld)>1)
    m_critical("--fields-enclosed-by must be a single character");
  if(fields_escaped_by && strlen(fields_escaped_by)>1)
    m_critical("--fields-escaped-by must be a single character");


  switch (output_format){
		case CLICKHOUSE:
		case SQL_INSERT:
      if (fields_enclosed_by_ld)
				fields_enclosed_by= fields_enclosed_by_ld;

      if (fields_terminated_by_ld==NULL)
        fields_terminated_by=g_strdup(",");
      else if (g_strcmp0(fields_terminated_by_ld, "\\t")==0)
        fields_terminated_by=g_strdup("\t");
      else
        fields_terminated_by=replace_escaped_strings(g_strdup(fields_terminated_by_ld));

      if (!lines_starting_by_ld)
        lines_starting_by=g_strdup("(");
      else
        lines_starting_by=replace_escaped_strings(g_strdup(lines_starting_by_ld));

      if (!lines_terminated_by_ld)
        lines_terminated_by=g_strdup(")\n");
      else
        lines_terminated_by=replace_escaped_strings(g_strdup(lines_terminated_by_ld));

      if (!statement_terminated_by_ld)
        statement_terminated_by=g_strdup(";\n");
      else
        statement_terminated_by=replace_escaped_strings(g_strdup(statement_terminated_by_ld));

			break;
		case LOAD_DATA:
      if (!fields_enclosed_by_ld){
				fields_enclosed_by= "";
        fields_enclosed_by_ld= fields_enclosed_by;
			}else
        fields_enclosed_by= fields_enclosed_by_ld;

      if (fields_escaped_by){
        if (strcmp(fields_escaped_by,"\\")==0)
          fields_escaped_by=g_strdup("\\\\");
      }else
        fields_escaped_by=g_strdup("\\\\");

      if (fields_terminated_by_ld==NULL){
        fields_terminated_by=g_strdup("\t");
        fields_terminated_by_ld=g_strdup("\\t");
      }else if (g_strcmp0(fields_terminated_by_ld, "\\t")==0){
        fields_terminated_by=g_strdup("\t");
        fields_terminated_by_ld=g_strdup("\\t");
      }else
        fields_terminated_by=replace_escaped_strings(g_strdup(fields_terminated_by_ld));

			if (!lines_starting_by_ld){
        lines_starting_by=g_strdup("");
        lines_starting_by_ld=lines_starting_by;
      }else
        lines_starting_by=replace_escaped_strings(g_strdup(lines_starting_by_ld));

      if (!lines_terminated_by_ld){
        lines_terminated_by=g_strdup("\n");
        lines_terminated_by_ld=g_strdup("\\n");
      }else
        lines_terminated_by=replace_escaped_strings(g_strdup(lines_terminated_by_ld));

      if (!statement_terminated_by_ld){
        statement_terminated_by=g_strdup("");
        statement_terminated_by_ld=statement_terminated_by;
      }else
        statement_terminated_by=replace_escaped_strings(g_strdup(statement_terminated_by_ld));

			break;
		case CSV:
			if (!fields_enclosed_by_ld){
        fields_enclosed_by= "\"";
        fields_enclosed_by_ld= fields_enclosed_by;
      }else
        fields_enclosed_by= fields_enclosed_by_ld;

			if (fields_escaped_by){
        if (strcmp(fields_escaped_by,"\\")==0)
          fields_escaped_by=g_strdup("\\\\");
      }else
        fields_escaped_by=g_strdup("\\\\");

      if (fields_terminated_by_ld==NULL){
        fields_terminated_by=g_strdup(",");
        fields_terminated_by_ld=fields_terminated_by;
      }else if (g_strcmp0(fields_terminated_by_ld, "\\t")==0){
        fields_terminated_by=g_strdup("\t");
        fields_terminated_by_ld=g_strdup("\\t");
      }else
        fields_terminated_by=replace_escaped_strings(g_strdup(fields_terminated_by_ld));

      if (!lines_starting_by_ld){
        lines_starting_by=g_strdup("");
        lines_starting_by_ld=lines_starting_by;
      }else
        lines_starting_by=replace_escaped_strings(g_strdup(lines_starting_by_ld));

      if (!lines_terminated_by_ld){
        lines_terminated_by=g_strdup("\n");
        lines_terminated_by_ld=g_strdup("\\n");
      }else
        lines_terminated_by=replace_escaped_strings(g_strdup(lines_terminated_by_ld));

      if (!statement_terminated_by_ld){
        statement_terminated_by=g_strdup("");
        statement_terminated_by_ld=statement_terminated_by;
      }else
        statement_terminated_by=replace_escaped_strings(g_strdup(statement_terminated_by_ld));

			break;
	}


  if ( insert_ignore && replace ){
    m_error("You can't use --insert-ignore and --replace at the same time");
  }

  if (insert_ignore)
    insert_statement=INSERT_IGNORE;

  if (replace)
    insert_statement=REPLACE;
}

void finalize_write(){
  g_free(fields_terminated_by);
  g_free(lines_starting_by);
  g_free(lines_terminated_by);
  g_free(statement_terminated_by);
}


GString *append_load_data_columns(GString *statement, MYSQL_FIELD *fields, guint num_fields){
  guint i = 0;
  GString *str=g_string_new("SET ");
  gboolean appendable=FALSE;
  for (i = 0; i < num_fields; ++i) {
    if (i > 0) {
      g_string_append_c(statement, ',');
    }
    if (fields[i].type == MYSQL_TYPE_JSON){
      g_string_append_c(statement,'@');
      g_string_append(statement, fields[i].name);
      if (str->len > 4)
        g_string_append_c(str, ',');
      g_string_append(str,identifier_quote_character_str);
      g_string_append(str,fields[i].name);
      g_string_append(str,identifier_quote_character_str);
      g_string_append(str,"=CONVERT(@");
      g_string_append(str, fields[i].name);
      g_string_append(str, " USING UTF8MB4)");
      appendable=TRUE;
    }else if (hex_blob && fields[i].type == MYSQL_TYPE_BLOB){
      g_string_append_c(statement,'@');
      g_string_append(statement, fields[i].name);
      if (str->len > 4)
        g_string_append_c(str, ',');
      g_string_append(str,identifier_quote_character_str);
      g_string_append(str,fields[i].name);
      g_string_append(str,identifier_quote_character_str);
      g_string_append(str,"=UNHEX(@");
      g_string_append(str, fields[i].name);
      g_string_append(str, ")");
      appendable=TRUE;
    }else{
      g_string_append(statement, identifier_quote_character_str);
      g_string_append(statement, fields[i].name);
      g_string_append(statement, identifier_quote_character_str);
    }
  }
  if (appendable)
    return str;
  else{
    g_string_free(str,TRUE);
    return NULL;
  }
}

void append_columns (GString *statement, MYSQL_FIELD *fields, guint num_fields){
  guint i = 0;
  for (i = 0; i < num_fields; ++i) {
    if (i > 0) {
      g_string_append_c(statement, ',');
    }
    g_string_append_c(statement,identifier_quote_character);
    g_string_append(statement, fields[i].name);    
    g_string_append_c(statement,identifier_quote_character);
  }
}

void build_insert_statement(struct db_table * dbt, MYSQL_FIELD *fields, guint num_fields){
  GString * i_s=g_string_new(insert_statement);
  g_string_append(i_s, " INTO ");
  g_string_append_c(i_s, identifier_quote_character);
  g_string_append(i_s, dbt->table);
  g_string_append_c(i_s, identifier_quote_character);

  if (dbt->columns_on_insert){
    g_string_append(i_s, " (");
    g_string_append(i_s, dbt->columns_on_insert);
    g_string_append(i_s, ")");
  }else{
    if (dbt->complete_insert) {
      g_string_append(i_s, " (");
      append_columns(i_s,fields,num_fields);
      g_string_append(i_s, ")");
    }
  } 
  g_string_append(i_s, " VALUES");
  dbt->insert_statement=i_s;
}

gboolean real_write_data(int file, float *filesize, GString *data) {
  size_t written = 0;
  ssize_t r = 0;
  gboolean second_write_zero = FALSE;
  while (written < data->len) {
    r=write(file, data->str + written, data->len);
    if (r < 0) {
      g_critical("Couldn't write data to a file: %s", strerror(errno));
      errors++;
      return FALSE;
    }
    if ( r == 0 ) {
      if (second_write_zero){
        g_critical("Couldn't write data to a file: %s", strerror(errno));
        errors++;
        return FALSE;
      }
      second_write_zero=TRUE;
    }else{
      second_write_zero=FALSE;
    }
    written += r;
  }
  *filesize+=written;
  return TRUE;
}


gboolean write_data(int file, GString *data) {
  float f=0;
  return real_write_data(file, &f, data);
}

void initialize_load_data_statement_suffix(struct db_table *dbt, MYSQL_FIELD * fields, guint num_fields){
  gchar *character_set=set_names_str != NULL ? set_names_str : dbt->character_set /* "BINARY"*/;
  GString *load_data_suffix=g_string_sized_new(statement_size);
  g_string_append_printf(load_data_suffix, "%s' INTO TABLE %s%s%s ", exec_per_thread_extension, identifier_quote_character_str, dbt->table, identifier_quote_character_str);
  if (character_set && strlen(character_set)!=0)
    g_string_append_printf(load_data_suffix, "CHARACTER SET %s ",character_set);
  if (fields_terminated_by_ld)
    g_string_append_printf(load_data_suffix, "FIELDS TERMINATED BY '%s' ",fields_terminated_by_ld);
  if (fields_enclosed_by_ld)
    g_string_append_printf(load_data_suffix, "ENCLOSED BY '%s' ",fields_enclosed_by_ld);
  if (fields_escaped_by)
    g_string_append_printf(load_data_suffix, "ESCAPED BY '%s' ",fields_escaped_by);
  g_string_append(load_data_suffix, "LINES ");
  if (lines_starting_by_ld)
    g_string_append_printf(load_data_suffix, "STARTING BY '%s' ",lines_starting_by_ld);
  g_string_append_printf(load_data_suffix, "TERMINATED BY '%s' ", lines_terminated_by_ld);
  if (include_header)
    g_string_append(load_data_suffix, "IGNORE 1 LINES ");
  g_string_append_printf(load_data_suffix, "(");
  if (dbt->columns_on_insert){
    g_string_append(load_data_suffix,dbt->columns_on_insert);
    g_string_append(load_data_suffix,")");
  }else{
    GString * set_statement=append_load_data_columns(load_data_suffix,fields,num_fields);
    g_string_append(load_data_suffix,")");
    if (set_statement != NULL){
      g_string_append(load_data_suffix,set_statement->str);
      g_string_free(set_statement,TRUE);
    }
  }
  g_string_append(load_data_suffix,";\n");
  dbt->load_data_suffix=load_data_suffix;
}

void initialize_clickhouse_statement_suffix(struct db_table *dbt, MYSQL_FIELD * fields, guint num_fields){
  gchar *character_set=set_names_str != NULL ? set_names_str : dbt->character_set /* "BINARY"*/;
  dbt->load_data_suffix=g_string_sized_new(statement_size);
  g_string_append_printf(dbt->load_data_suffix, "%s' INTO TABLE %s%s%s ", exec_per_thread_extension, identifier_quote_character_str, dbt->table, identifier_quote_character_str);
  if (character_set && strlen(character_set)!=0)
    g_string_append_printf(dbt->load_data_suffix, "CHARACTER SET %s ",character_set);
  if (fields_terminated_by_ld)
    g_string_append_printf(dbt->load_data_suffix, "FIELDS TERMINATED BY '%s' ",fields_terminated_by_ld);
  if (fields_enclosed_by_ld)
    g_string_append_printf(dbt->load_data_suffix, "ENCLOSED BY '%s' ",fields_enclosed_by_ld);
  if (fields_escaped_by)
    g_string_append_printf(dbt->load_data_suffix, "ESCAPED BY '%s' ",fields_escaped_by);
  g_string_append(dbt->load_data_suffix, "LINES ");
  if (lines_starting_by_ld)
    g_string_append_printf(dbt->load_data_suffix, "STARTING BY '%s' ",lines_starting_by_ld);
  g_string_append_printf(dbt->load_data_suffix, "TERMINATED BY '%s' ", lines_terminated_by_ld);
  if (include_header)
    g_string_append(dbt->load_data_suffix, "IGNORE 1 LINES ");
  g_string_append_printf(dbt->load_data_suffix, "(");
  if (dbt->columns_on_insert){
    g_string_append(dbt->load_data_suffix,dbt->columns_on_insert);
    g_string_append(dbt->load_data_suffix,")");
  }else{
    GString * set_statement=append_load_data_columns(dbt->load_data_suffix,fields,num_fields);
    g_string_append(dbt->load_data_suffix,")");
    if (set_statement != NULL){
      g_string_append(dbt->load_data_suffix,set_statement->str);
      g_string_free(set_statement,TRUE);
    }
  }
  g_string_append(dbt->load_data_suffix,";\n");
}

void initialize_load_data_header(struct db_table *dbt, MYSQL_FIELD *fields, guint num_fields){
  dbt->load_data_header = g_string_sized_new(statement_size);
  guint i = 0;
  for (i = 0; i < num_fields-1; ++i) {
    g_string_append(dbt->load_data_header,fields_enclosed_by);
    g_string_append(dbt->load_data_header,fields[i].name);
    g_string_append(dbt->load_data_header,fields_enclosed_by);
    g_string_append(dbt->load_data_header,fields_terminated_by);
  }
  g_string_append(dbt->load_data_header,fields_enclosed_by);
  g_string_append(dbt->load_data_header,fields[i].name);
  g_string_append(dbt->load_data_header,fields_enclosed_by);
  g_string_append(dbt->load_data_header,lines_terminated_by);
}

gboolean write_statement(int load_data_file, float *filessize, GString *statement, struct db_table * dbt){
  if (!real_write_data(load_data_file, filessize, statement)) {
    g_critical("Could not write out data for %s.%s", dbt->database->name, dbt->table);
    return FALSE;
  }
  g_string_set_size(statement, 0);
  return TRUE;
}

void write_load_data_statement(struct table_job * tj){
  GString *statement = g_string_sized_new(statement_size);
  char * basename=g_path_get_basename(tj->rows->filename);
  initialize_sql_statement(statement);
  g_string_append_printf(statement, "%s%s%s", LOAD_DATA_PREFIX, basename, tj->dbt->load_data_suffix->str);
  if (!write_data(tj->sql->file, statement)) {
    g_critical("Could not write out data for %s.%s", tj->dbt->database->name, tj->dbt->table);
  }
}

void write_clickhouse_statement(struct table_job * tj){
  GString *statement = g_string_sized_new(statement_size);
  char * basename=g_path_get_basename(tj->rows->filename);
  initialize_sql_statement(statement);
  g_string_append_printf(statement, "%s INTO %s%s%s FROM INFILE '%s' FORMAT MySQLDump;", insert_statement, identifier_quote_character_str, tj->dbt->table, identifier_quote_character_str, basename); // , tj->dbt->load_data_suffix->str);
  if (!write_data(tj->sql->file, statement)) {
    g_critical("Could not write out data for %s.%s", tj->dbt->database->name, tj->dbt->table);
  }
}

gboolean write_header(struct table_job * tj){
  if (tj->dbt->load_data_header && !write_data(tj->rows->file, tj->dbt->load_data_header)) {
    g_critical("Could not write header for %s.%s", tj->dbt->database->name, tj->dbt->table);
    return FALSE;
  }
  return TRUE;
}

/*
guint64 get_estimated_remaining_chunks_on_dbt(struct db_table *dbt){
  GList *l=dbt->chunks;
  guint64 total=0;
  while (l!=NULL){
    total+=((union chunk_step *)(l->data))->integer_step.estimated_remaining_steps;
    l=l->next;
  }
  return total;
}
*/

guint64 get_estimated_remaining_of(struct MList *mlist){
  g_mutex_lock(mlist->mutex);
  GList *tl=mlist->list;
  guint64 total=0;
  while (tl!=NULL){
    total+=((struct db_table *)(tl->data))->estimated_remaining_steps;
    tl=tl->next;
  }
  g_mutex_unlock(mlist->mutex);
  return total;
}

guint64 get_estimated_remaining_of_all_chunks(){
  return get_estimated_remaining_of(non_innodb_table) + get_estimated_remaining_of(innodb_table);
}

void write_load_data_column_into_string( MYSQL *conn, gchar **column, MYSQL_FIELD field, gulong length, struct thread_data_buffers *buffers){
    if (!*column) {
      g_string_append(buffers->statement, "\\N");
    } else if ( field.type == MYSQL_TYPE_BLOB && hex_blob ) {
      g_string_set_size(buffers->escaped, length * 2 + 1);
      mysql_hex_string(buffers->escaped->str,*column,length);
      g_string_append(buffers->statement,buffers->escaped->str);
    }else if (field.type != MYSQL_TYPE_LONG && field.type != MYSQL_TYPE_LONGLONG  && field.type != MYSQL_TYPE_INT24  && field.type != MYSQL_TYPE_SHORT ){
      g_string_append(buffers->statement,fields_enclosed_by);
      // this will reserve the memory needed if the current size is not enough.
      g_string_set_size(buffers->escaped, length * 2 + 1);
      unsigned long new_length = mysql_real_escape_string(conn, buffers->escaped->str, *column, length);
      new_length++;
      //g_string_set_size(escaped, new_length);
      m_replace_char_with_char('\\',*fields_escaped_by,buffers->escaped->str, new_length);
      m_escape_char_with_char(*fields_terminated_by, *fields_escaped_by, buffers->escaped->str, new_length);
      g_string_append(buffers->statement,buffers->escaped->str);
      g_string_append(buffers->statement,fields_enclosed_by);
    }else
      g_string_append(buffers->statement, *column);
}

void write_sql_column_into_string( MYSQL *conn, gchar **column, MYSQL_FIELD field, gulong length, struct thread_data_buffers *buffers){
    if (!*column) {
      g_string_append(buffers->statement, "NULL");
    } else if (field.flags & NUM_FLAG) {
      g_string_append(buffers->statement, *column);
    } else if ( length == 0){
      g_string_append_c(buffers->statement,*fields_enclosed_by);
      g_string_append_c(buffers->statement,*fields_enclosed_by);
    } else if ( field.type == MYSQL_TYPE_BLOB && hex_blob ) {
      g_string_set_size(buffers->escaped, length * 2 + 1);
      g_string_append(buffers->statement,"0x");
      mysql_hex_string(buffers->escaped->str,*column,length);
      g_string_append(buffers->statement,buffers->escaped->str);
    } else {
      /* We reuse buffers for string escaping, growing is expensive just at
 *        * the beginning */
      g_string_set_size(buffers->escaped, length * 2 + 1);
      mysql_real_escape_string(conn, buffers->escaped->str, *column, length);
      if (field.type == MYSQL_TYPE_JSON)
        g_string_append(buffers->statement, "CONVERT(");
      g_string_append_c(buffers->statement, *fields_enclosed_by);
      g_string_append(buffers->statement, buffers->escaped->str);
      g_string_append_c(buffers->statement, *fields_enclosed_by);
      if (field.type == MYSQL_TYPE_JSON)
        g_string_append(buffers->statement, " USING UTF8MB4)");
    }
}



void write_column_into_string_with_terminated_by(MYSQL *conn, gchar * row, MYSQL_FIELD field, gulong length, struct thread_data_buffers *buffers, void write_column_into_string(MYSQL *, gchar **, MYSQL_FIELD , gulong ,struct thread_data_buffers *), struct function_pointer * f, gchar * terminated_by){
  gchar *column=NULL;
  gulong rlength=length;
//  g_string_set_size(buffers->column,0);
  if (row)
    column=row;
  if (f){
   if (f->is_pre){
     write_column_into_string( conn, &(column), field, rlength, buffers);
     column=f->function(&(buffers->statement->str), &rlength, f);
     g_string_printf(buffers->statement,"%s",column);
   }else{
     column=f->function(&(column), &rlength, f);
     write_column_into_string( conn, &(column), field, rlength, buffers);
   }
  }else{
    write_column_into_string( conn, &(column), field, rlength, buffers);
  }
//  g_string_append(buffers->row, buffers->column->str);
  g_string_append(buffers->statement, terminated_by);

  if (!column && column != row)
    g_free(column);
}

void write_row_into_string(MYSQL *conn, struct db_table * dbt, MYSQL_ROW row, MYSQL_FIELD *fields, gulong *lengths, guint num_fields, struct thread_data_buffers * buffers, void write_column_into_string(MYSQL *, gchar **, MYSQL_FIELD , gulong , struct thread_data_buffers*)){
  guint i = 0;
  g_string_append(buffers->statement, lines_starting_by);
  struct function_pointer ** f = dbt->anonymized_function;

  for (i = 0; i < num_fields-1; i++) {
    write_column_into_string_with_terminated_by(conn, row[i], fields[i], lengths[i], buffers, write_column_into_string,f==NULL?NULL:f[i], fields_terminated_by);
  }
  write_column_into_string_with_terminated_by(conn, row[i], fields[i], lengths[i], buffers, write_column_into_string,f==NULL?NULL:f[i], lines_terminated_by);
}

void update_dbt_rows(struct db_table * dbt, guint64 num_rows){
  g_mutex_lock(dbt->rows_lock);
  dbt->rows+=num_rows;
  g_mutex_unlock(dbt->rows_lock);
}


void initiliaze_load_data_files(struct table_job * tj, struct db_table * dbt){

  m_close(tj->td->thread_id, tj->sql->file, g_strdup(tj->sql->filename), 1, dbt);
  m_close(tj->td->thread_id, tj->rows->file, g_strdup(tj->rows->filename), 1, dbt);
  tj->sql->file=0;
  tj->rows->file=0;

  g_free(tj->sql->filename);
  g_free(tj->rows->filename);

  tj->sql->filename=NULL;
  tj->rows->filename=NULL;

  if (update_files_on_table_job(tj)){
    write_load_data_statement(tj);
    write_header(tj);
  }
}


void initiliaze_clickhouse_files(struct table_job * tj, struct db_table * dbt){

  m_close(tj->td->thread_id, tj->sql->file, g_strdup(tj->sql->filename), 1, dbt);
  m_close(tj->td->thread_id, tj->rows->file, g_strdup(tj->rows->filename), 1, dbt);
  tj->sql->file=0;
  tj->rows->file=0;

  g_free(tj->sql->filename);
  g_free(tj->rows->filename);

  tj->sql->filename=NULL;
  tj->rows->filename=NULL;

  if (update_files_on_table_job(tj)){
    write_clickhouse_statement(tj);
    write_header(tj);
  }
}


void write_result_into_file(MYSQL *conn, MYSQL_RES *result, struct table_job * tj){
	struct db_table * dbt = tj->dbt;
	guint num_fields = mysql_num_fields(result);
  MYSQL_FIELD *fields = mysql_fetch_fields(result);
  MYSQL_ROW row;
  int data_buffer_pos=0;
  struct thread_data_buffers *thread_data_buffer = &tj->td->thread_data_buffers[data_buffer_pos];
  g_string_set_size(thread_data_buffer->statement,0);
  //g_string_set_size(thread_data_buffer->row,0);
  g_string_set_size(thread_data_buffer->escaped,0);
  gulong *lengths = NULL;
  guint64 num_rows=0;
  guint64 num_rows_st = 0;
  void (*write_column_into_string)(MYSQL *, gchar **, MYSQL_FIELD , gulong , struct thread_data_buffers*) = write_sql_column_into_string;
  tj->st_in_file=0;
  g_async_queue_push(tj->td->write_buffer_queue, tj);

  switch (output_format){
    case LOAD_DATA:
    case CSV:
  		write_column_into_string=write_load_data_column_into_string;
    	if (dbt->load_data_suffix==NULL){
        g_mutex_lock(dbt->chunks_mutex);
        if (dbt->load_data_suffix==NULL){
          initialize_load_data_statement_suffix(tj->dbt, fields, num_fields);
        if (include_header)
          initialize_load_data_header(tj->dbt, fields, num_fields);
        }
        g_mutex_unlock(dbt->chunks_mutex);
      }
      if (update_files_on_table_job(tj)){
        write_load_data_statement(tj);
        write_header(tj);
      }
	  	break;
    case CLICKHOUSE:
      if (tj->rows->file == 0){
        update_files_on_table_job(tj);
      }
			if (dbt->load_data_suffix==NULL){
        g_mutex_lock(dbt->chunks_mutex);
        if (dbt->load_data_suffix==NULL)
          initialize_clickhouse_statement_suffix(tj->dbt, fields, num_fields);
  			g_mutex_unlock(dbt->chunks_mutex);
      }
			if (dbt->insert_statement==NULL){
        g_mutex_lock(dbt->chunks_mutex);
        if (dbt->insert_statement==NULL)
          build_insert_statement(dbt, fields, num_fields);
        g_mutex_unlock(dbt->chunks_mutex);
      }
      if (!tj->st_in_file){
        initialize_sql_statement(thread_data_buffer->statement);
				write_clickhouse_statement(tj);
			}
      g_string_append(thread_data_buffer->statement, dbt->insert_statement->str);
      break;
		case SQL_INSERT:
      if (tj->rows->file == 0){
        update_files_on_table_job(tj);
  		}
      if (dbt->insert_statement==NULL){
        g_mutex_lock(dbt->chunks_mutex);
        if (dbt->insert_statement==NULL)
          build_insert_statement(dbt, fields, num_fields);
        g_mutex_unlock(dbt->chunks_mutex);
      }
	  	if (!tj->st_in_file)
  	  	initialize_sql_statement(thread_data_buffer->statement);
  		g_string_append(thread_data_buffer->statement, dbt->insert_statement->str);
	  	break;
	}

  message_dumping_data(tj);

  GDateTime *from = g_date_time_new_now_local();
  guint st_responded=0;
	while ((row = mysql_fetch_row(result))) {
    lengths = mysql_fetch_lengths(result);
    if (num_rows>0 && (output_format == SQL_INSERT || output_format == CLICKHOUSE))
      g_string_append_c(thread_data_buffer->statement, ',');
    num_rows++;
    // prepare row into statement_row
		write_row_into_string(conn, dbt, row, fields, lengths, num_fields, thread_data_buffer, write_column_into_string);

		// if row exceeded statement_size then FLUSH buffer to disk
		if (thread_data_buffer->statement->len > statement_size){
      if (num_rows_st == 0) {
//        g_string_append(thread_data_buffer->statement, thread_data_buffer->row->str);
//        g_string_set_size(thread_data_buffer->row, 0);
        g_warning("Row bigger than statement_size for %s.%s", dbt->database->name,
                dbt->table);
      }
      g_string_append(thread_data_buffer->statement, statement_terminated_by);
//      if (!write_statement(tj->rows->file, &(tj->filesize), thread_data_buffer->statement, dbt)) {
//        return;
//      }

//      enqueue_statement_to_write(tj,thread_data_buffer);
      g_async_queue_push(tj->td->write_buffer_queue, GINT_TO_POINTER(1));
//      g_async_queue_push(tj->write_buffer_response_queue, GINT_TO_POINTER(1));
//      tj->filesize+=thread_data_buffer->statement->len;
      tj->st_in_file++;
      data_buffer_pos++;
      if (data_buffer_pos == MAX_WRITE_BUFFER_PER_THREAD)
        data_buffer_pos=0;
      gpointer p=g_async_queue_try_pop(tj->write_buffer_response_queue);
      if (p){
        st_responded++;
      }
      if (tj->st_in_file - st_responded >= MAX_WRITE_BUFFER_PER_THREAD){
        p=g_async_queue_pop(tj->write_buffer_response_queue);
        st_responded++;
      }

      thread_data_buffer = &tj->td->thread_data_buffers[data_buffer_pos];
      g_string_set_size(thread_data_buffer->statement, 0);
      update_dbt_rows(dbt, num_rows);
			num_rows=0;
			num_rows_st=0;
    // initilize buffer if needed (INSERT INTO)
      if (output_format == SQL_INSERT || output_format == CLICKHOUSE){
				g_string_append(thread_data_buffer->statement, dbt->insert_statement->str);
			}
      GDateTime *to = g_date_time_new_now_local();
      GTimeSpan diff=g_date_time_difference(to,from)/G_TIME_SPAN_SECOND;
      if (diff > 4){
        g_date_time_unref(from);
        from=to;
        message_dumping_data(tj);
      }

			check_pause_resume(tj->td);
      if (shutdown_triggered) {
        return;
      }
  		// if file size exceeded limit, we need to rotate
	  	if (dbt->chunk_filesize && (guint)ceil((float)tj->filesize / 1024 / 1024) >
              dbt->chunk_filesize){
  			tj->sub_part++;
// sync this with write buffer thread to be safe to close and open files
        //tj->write_buffer_pos
        while (tj->st_in_file > st_responded){
          g_async_queue_pop(tj->write_buffer_response_queue);
          st_responded++;
        }
        switch (output_format){
  			  case LOAD_DATA:
	  			case CSV:
		  			initiliaze_load_data_files(tj, dbt);
            break;
  				case CLICKHOUSE:
    				initiliaze_clickhouse_files(tj, dbt);
		  			break;
			  	case SQL_INSERT:
            m_close(tj->td->thread_id, tj->rows->file, tj->rows->filename, 1, dbt);
            tj->rows->file=0;
            update_files_on_table_job(tj);
			    	initialize_sql_statement(thread_data_buffer->statement);
            g_string_append(thread_data_buffer->statement, dbt->insert_statement->str);
	  			  break;
        }
        tj->st_in_file = 0;
        st_responded=0;
        tj->filesize = 0;			
      }
    }
		//
		// write row to buffer
//    if (num_rows_st && (output_format == SQL_INSERT || output_format == CLICKHOUSE))
//      g_string_append_c(thread_data_buffer->statement, ',');
//    g_string_append(thread_data_buffer->statement, thread_data_buffer->row->str);
//		if (thread_data_buffer->row->len>0){
      num_rows_st++;
//    }
//    g_string_set_size(thread_data_buffer->row, 0);
  }
  update_dbt_rows(dbt, num_rows);
  if (num_rows_st > 0 && thread_data_buffer->statement->len > 0){
    if (output_format == SQL_INSERT || output_format == CLICKHOUSE)
			g_string_append(thread_data_buffer->statement, statement_terminated_by);
//    if (!write_statement(tj->rows->file, &(tj->filesize), thread_data_buffer->statement, dbt)) {
//      return;
//    }
		tj->st_in_file++;
    g_async_queue_push(tj->td->write_buffer_queue, GINT_TO_POINTER(1));
  }
//  g_string_free(statement, TRUE);
//  g_string_free(escaped, TRUE);
//  g_string_free(statement_row, TRUE);	
  g_async_queue_push(tj->td->write_buffer_queue, GINT_TO_POINTER(2));
  while (tj->st_in_file > st_responded){
    g_async_queue_pop(tj->write_buffer_response_queue);
    st_responded++;
  }

  return;
}

/* Do actual data chunk reading/writing magic */
void write_table_job_into_file(struct table_job * tj){
  MYSQL *conn = tj->td->thrconn;
  MYSQL_RES *result = NULL;
  char *query = NULL;

  /* Ghm, not sure if this should be statement_size - but default isn't too big
   * for now */
  /* Poor man's database code */
  query = g_strdup_printf(
      "SELECT %s %s FROM %s%s%s.%s%s%s %s %s %s %s %s %s %s %s %s %s %s",
      is_mysql_like() ? "/*!40001 SQL_NO_CACHE */" : "",
      tj->dbt->columns_on_select?tj->dbt->columns_on_select:tj->dbt->select_fields->str,
      identifier_quote_character_str,tj->dbt->database->name, identifier_quote_character_str, identifier_quote_character_str, tj->dbt->table, identifier_quote_character_str, tj->partition?tj->partition:"",
       (tj->where->len || where_option   || tj->dbt->where) ? "WHERE"  : "" , tj->where->len ? tj->where->str : "",
       (tj->where->len && where_option )                    ? "AND"    : "" ,   where_option ?   where_option : "",
      ((tj->where->len || where_option ) && tj->dbt->where) ? "AND"    : "" , tj->dbt->where ? tj->dbt->where : "",
      order_by_primary_key && tj->dbt->primary_key_separated_by_comma ? " ORDER BY " : "", order_by_primary_key && tj->dbt->primary_key_separated_by_comma ? tj->dbt->primary_key_separated_by_comma : "",
      tj->dbt->limit ?  "LIMIT" : "", tj->dbt->limit ? tj->dbt->limit : ""
  );
  if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
    if (!it_is_a_consistent_backup){
      g_warning("Thread %d: Error dumping table (%s.%s) data: %s\nQuery: %s", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table,
                mysql_error(conn), query);

      if (mysql_ping(tj->td->thrconn)) {
        m_connect(tj->td->thrconn);
        execute_gstring(tj->td->thrconn, set_session);
      }
      g_warning("Thread %d: Retrying last failed executed statement", tj->td->thread_id);

      if (mysql_query(conn, query) || !(result = mysql_use_result(conn))) {
       // ERROR 1146
        if (success_on_1146 && mysql_errno(conn) == 1146) {
          g_warning("Thread %d: Error dumping table (%s.%s) data: %s\nQuery: %s", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table,
                  mysql_error(conn), query);
        } else {
          g_critical("Thread %d: Error dumping table (%s.%s) data: %s\nQuery: %s ", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table,
                     mysql_error(conn), query);
          errors++;
        }
        goto cleanup;
      }
    }else{
      if (success_on_1146 && mysql_errno(conn) == 1146) {
        g_warning("Thread %d: Error dumping table (%s.%s) data: %s\nQuery: %s", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table,
                  mysql_error(conn), query);
      } else {
        g_critical("Thread %d: Error dumping table (%s.%s) data: %s\nQuery: %s ", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table,
                   mysql_error(conn), query);
        errors++;
      }
      goto cleanup;
    }
  }

  /* Poor man's data dump code */
    write_result_into_file(conn, result, tj);

  if (mysql_errno(conn)) {
    g_critical("Thread %d: Could not read data from %s.%s to write on %s at byte %.0f: %s", tj->td->thread_id, tj->dbt->database->name, tj->dbt->table, tj->rows->filename, tj->filesize,
               mysql_error(conn));
    errors++;
    if (mysql_ping(tj->td->thrconn)) {
      if (!it_is_a_consistent_backup){
        g_warning("Thread %d: Reconnecting due errors", tj->td->thread_id);
        m_connect(tj->td->thrconn);
        execute_gstring(tj->td->thrconn, set_session);
      }
    } 
 }

cleanup:
  g_free(query);

  if (result) {
    mysql_free_result(result);
  }
}

void * write_buffer_thread(GAsyncQueue *write_buffer_queue){
  gboolean cont;
  struct table_job * tj=NULL;
  while (TRUE){
    // init job only
    tj = (struct table_job *) g_async_queue_pop(write_buffer_queue);
    cont=GPOINTER_TO_INT(g_async_queue_pop(write_buffer_queue))==1;
    while ( cont ){
      if (!write_statement(tj->rows->file, &(tj->filesize), tj->td->thread_data_buffers[tj->write_buffer_pos].statement, tj->dbt)) {
        return NULL;
      }
      // buffers to write
      g_async_queue_push(tj->write_buffer_response_queue, GINT_TO_POINTER(tj->write_buffer_pos + 1) );
      if (tj->write_buffer_pos < MAX_WRITE_BUFFER_PER_THREAD - 1)
        tj->write_buffer_pos++;
      else
        tj->write_buffer_pos=0;
      cont=GPOINTER_TO_INT(g_async_queue_pop(write_buffer_queue))==1;
    }

  }
  return NULL;
}



