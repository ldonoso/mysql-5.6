#ifndef FIELD_INCLUDED
#define FIELD_INCLUDED

/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mysqld.h"                             /* system_charset_info */
#include "table.h"                              /* TABLE */
#include "sql_string.h"                         /* String */
#include "my_decimal.h"                         /* my_decimal */
#include "sql_error.h"                          /* Sql_condition */
#include "mysql_version.h"                      /* FRM_VER */
#include "../fbson/FbsonDocument.h"
#include "../fbson/FbsonJsonParser.h"
#include "../fbson/FbsonUtil.h"
#include "../fbson/FbsonUpdater.h"

/*

Field class hierarchy


Field (abstract)
|
+--Field_bit
|  +--Field_bit_as_char
|  
+--Field_num (abstract)
|  |  +--Field_real (asbstract)
|  |     +--Field_decimal
|  |     +--Field_float
|  |     +--Field_double
|  |
|  +--Field_new_decimal
|  +--Field_short
|  +--Field_medium
|  +--Field_long
|  +--Field_longlong
|  +--Field_tiny
|     +--Field_year
|
+--Field_str (abstract)
|  +--Field_longstr
|  |  +--Field_string
|  |  +--Field_varstring
|  |  +--Field_blob
|  |     +--Field_geom
|  |     +--Field_document
|  |
|  +--Field_null
|  +--Field_enum
|     +--Field_set
|
+--Field_temporal (abstract)
   +--Field_time_common (abstract)
   |  +--Field_time
   |  +--Field_timef
   |
   +--Field_temporal_with_date (abstract)
      +--Field_newdate
      +--Field_temporal_with_date_and_time (abstract)
         +--Field_timestamp
         +--Field_datetime
         +--Field_temporal_with_date_and_timef (abstract)
            +--Field_timestampf
            +--Field_datetimef
*/


class Send_field;
class Protocol;
class Create_field;
class Relay_log_info;
class Field;
class Save_in_field_args;
enum enum_check_fields
{
  CHECK_FIELD_IGNORE,
  CHECK_FIELD_WARN,
  CHECK_FIELD_ERROR_FOR_NULL
};


enum Derivation
{
  DERIVATION_IGNORABLE= 6,
  DERIVATION_NUMERIC= 5,
  DERIVATION_COERCIBLE= 4,
  DERIVATION_SYSCONST= 3,
  DERIVATION_IMPLICIT= 2,
  DERIVATION_NONE= 1,
  DERIVATION_EXPLICIT= 0
};

/**
  Status when storing a value in a field or converting from one
  datatype to another. The values should be listed in order of
  increasing seriousness so that if two type_conversion_status
  variables are compared, the bigger one is most serious.
*/
enum type_conversion_status
{
  /// Storage/conversion went fine.
  TYPE_OK= 0,
  /**
    A minor problem when converting between temporal values, e.g.
    if datetime is converted to date the time information is lost.
  */
  TYPE_NOTE_TIME_TRUNCATED,
  /**
    Value outside min/max limit of datatype. The min/max value is
    stored by Field::store() instead (if applicable)
  */
  TYPE_WARN_OUT_OF_RANGE,
  /**
    Value was stored, but something was cut. What was cut is
    considered insignificant enough to only issue a note. Example:
    trying to store a number with 5 decimal places into a field that
    can only store 3 decimals. The number rounded to 3 decimal places
    should be stored. Another example: storing the string "foo " into
    a VARCHAR(3). The string "foo" is stored in this case, so only
    whitespace is cut.
  */
  TYPE_NOTE_TRUNCATED,
  /**
    Value was stored, but something was cut. What was cut is
    considered significant enough to issue a warning. Example: storing
    the string "foo" into a VARCHAR(2). The string "fo" is stored in
    this case. Another example: storing the string "2010-01-01foo"
    into a DATE. The garbage in the end of the string is cut in this
    case.
  */
  TYPE_WARN_TRUNCATED,
  /// Trying to store NULL in a NOT NULL field.
  TYPE_ERR_NULL_CONSTRAINT_VIOLATION,
  /**
    Store/convert incompatible values, like converting "foo" to a
    date.
  */
  TYPE_ERR_BAD_VALUE,
  /// Out of memory
  TYPE_ERR_OOM
};


#define STORAGE_TYPE_MASK 7
#define COLUMN_FORMAT_MASK 7
#define COLUMN_FORMAT_SHIFT 3

#define my_charset_numeric      my_charset_latin1
#define MY_REPERTOIRE_NUMERIC   MY_REPERTOIRE_ASCII

struct st_cache_field;
type_conversion_status field_conv(Field *to,Field *from);

inline uint get_enum_pack_length(int elements)
{
  return elements < 256 ? 1 : 2;
}

inline uint get_set_pack_length(int elements)
{
  uint len= (elements + 7) / 8;
  return len > 4 ? 8 : len;
}

inline type_conversion_status
decimal_err_to_type_conv_status(int dec_error)
{
  if (dec_error & E_DEC_OOM)
    return TYPE_ERR_OOM;

  if (dec_error & (E_DEC_DIV_ZERO | E_DEC_BAD_NUM))
    return TYPE_ERR_BAD_VALUE;

  if (dec_error & E_DEC_TRUNCATED)
    return TYPE_NOTE_TRUNCATED;

  if (dec_error & E_DEC_OVERFLOW)
    return TYPE_WARN_OUT_OF_RANGE;

  if (dec_error == E_DEC_OK)
    return TYPE_OK;

  // impossible
  DBUG_ASSERT(false);
  return TYPE_ERR_BAD_VALUE;
}

/**
  Convert warnings returned from str_to_time() and str_to_datetime()
  to their corresponding type_conversion_status codes.
*/
inline type_conversion_status
time_warning_to_type_conversion_status(const int warn)
{
  if (warn & MYSQL_TIME_NOTE_TRUNCATED)
    return TYPE_NOTE_TIME_TRUNCATED;

  if (warn & MYSQL_TIME_WARN_OUT_OF_RANGE)
    return TYPE_WARN_OUT_OF_RANGE;

  if (warn & MYSQL_TIME_WARN_TRUNCATED)
    return TYPE_NOTE_TRUNCATED;

  if (warn & (MYSQL_TIME_WARN_ZERO_DATE | MYSQL_TIME_WARN_ZERO_IN_DATE))
    return TYPE_ERR_BAD_VALUE;

  if (warn & MYSQL_TIME_WARN_INVALID_TIMESTAMP)
    // date was fine but pointed to daylight saving time switch gap
    return TYPE_OK;

  DBUG_ASSERT(!warn);
  return TYPE_OK;
}

#define ASSERT_COLUMN_MARKED_FOR_READ      \
DBUG_ASSERT(!table || (!table->read_set || \
                       bitmap_is_set(table->read_set, field_index)))
#define ASSERT_COLUMN_MARKED_FOR_WRITE \
DBUG_ASSERT(!table || \
            table->in_use->variables.binlog_row_image \
             != BINLOG_ROW_IMAGE_FULL || \
            (!table->write_set || bitmap_is_set(table->write_set, field_index)))


/**
  Tests if field type is temporal, i.e. represents
  DATE, TIME, DATETIME or TIMESTAMP types in SQL.
     
  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal
  @retval false  If field type is not temporal
*/
inline bool is_temporal_type(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    return true;
  default:
    return false;
  }
}

/**
  Tests if field type is an integer

  @param type Field type, as returned by field->type()

  @returns true if integer type, false otherwise
*/
inline bool is_integer_type(enum_field_types type) {
  switch (type) {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
    return true;
  default:
    return false;
  }
}

/**
  Tests if field real type is temporal, i.e. represents
  all existing implementations of
  DATE, TIME, DATETIME or TIMESTAMP types in SQL.

  @param type    Field real type, as returned by field->real_type()
  @retval true   If field real type is temporal
  @retval false  If field real type is not temporal
*/
inline bool is_temporal_real_type(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_TIMESTAMP2:
  case MYSQL_TYPE_DATETIME2:
    return true;
  default:
    return is_temporal_type(type);
  }
}


/**
  Tests if field type is temporal and has time part,
  i.e. represents TIME, DATETIME or TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal type with time part.
  @retval false  If field type is not temporal type with time part.
*/
inline bool is_temporal_type_with_time(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  default:
    return false;
  }
}


/**
  Tests if field type is temporal and has date part,
  i.e. represents DATE, DATETIME or TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal type with date part.
  @retval false  If field type is not temporal type with date part.
*/
inline bool is_temporal_type_with_date(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  default:
    return false;
  }
}


/**
  Tests if field type is temporal and has date and time parts,
  i.e. represents DATETIME or TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->type().
  @retval true   If field type is temporal type with date and time parts.
  @retval false  If field type is not temporal type with date and time parts.
*/
inline bool is_temporal_type_with_date_and_time(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
    return true;
  default:
    return false;
  }
}


/**
  Tests if field real type can have "DEFAULT CURRENT_TIMESTAMP",
  i.e. represents TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->real_type().
  @retval true   If field real type can have "DEFAULT CURRENT_TIMESTAMP".
  @retval false  If field real type can not have "DEFAULT CURRENT_TIMESTAMP".
*/
inline bool real_type_with_now_as_default(enum_field_types type)
{
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_TIMESTAMP2 ||
    type == MYSQL_TYPE_DATETIME || type == MYSQL_TYPE_DATETIME2;
}


/**
  Tests if field real type can have "ON UPDATE CURRENT_TIMESTAMP",
  i.e. represents TIMESTAMP types in SQL.

  @param type    Field type, as returned by field->real_type().
  @retval true   If field real type can have "ON UPDATE CURRENT_TIMESTAMP".
  @retval false  If field real type can not have "ON UPDATE CURRENT_TIMESTAMP".
*/
inline bool real_type_with_now_on_update(enum_field_types type)
{
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_TIMESTAMP2 ||
    type == MYSQL_TYPE_DATETIME || type == MYSQL_TYPE_DATETIME2;
}


/**
   Recognizer for concrete data type (called real_type for some reason),
   returning true if it is one of the TIMESTAMP types.
*/
inline bool is_timestamp_type(enum_field_types type)
{
  return type == MYSQL_TYPE_TIMESTAMP || type == MYSQL_TYPE_TIMESTAMP2;
}


/**
  Convert temporal real types as retuned by field->real_type()
  to field type as returned by field->type().
  
  @param real_type  Real type.
  @retval           Field type.
*/
inline enum_field_types real_type_to_type(enum_field_types real_type)
{
  switch (real_type)
  {
  case MYSQL_TYPE_TIME2:
    return MYSQL_TYPE_TIME;
  case MYSQL_TYPE_DATETIME2:
    return MYSQL_TYPE_DATETIME;
  case MYSQL_TYPE_TIMESTAMP2:
    return MYSQL_TYPE_TIMESTAMP;
  case MYSQL_TYPE_NEWDATE:
    return MYSQL_TYPE_DATE;
  /* Note: NEWDECIMAL is a type, not only a real_type */
  default: return real_type;
  }
}


/**
   Copies an integer value to a format comparable with memcmp(). The
   format is characterized by the following:

   - The sign bit goes first and is unset for negative values.
   - The representation is big endian.

   The function template can be instantiated to copy from little or
   big endian values.

   @tparam Is_big_endian True if the source integer is big endian.

   @param to          Where to write the integer.
   @param to_length   Size in bytes of the destination buffer.
   @param from        Where to read the integer.
   @param from_length Size in bytes of the source integer
   @param is_unsigned True if the source integer is an unsigned value.
*/
template<bool Is_big_endian>
void copy_integer(uchar *to, int to_length,
                  const uchar* from, int from_length,
                  bool is_unsigned)
{
  if (Is_big_endian)
  {
    if (is_unsigned)
      to[0]= from[0];
    else
      to[0]= (char)(from[0] ^ 128); // Reverse the sign bit.
    memcpy(to + 1, from + 1, to_length - 1);
  }
  else
  {
    const int sign_byte= from[from_length - 1];
    if (is_unsigned)
      to[0]= sign_byte;
    else
      to[0]= static_cast<char>(sign_byte ^ 128); // Reverse the sign bit.
    for (int i= 1, j= from_length - 2; i < to_length; ++i, --j)
      to[i]= from[j];
  }
}


/* A document key in the dot separated document path */
class Document_key :public Sql_alloc
{
public:
  Document_key(const char* str, int len):
      string(str), length(len)
  {
    DBUG_ASSERT(str);
    DBUG_ASSERT(strlen(str) == (uint)len);
   // Generate the index for it if possible
   char *p = NULL;
   index = strtol(str, &p, 10);
   if (p != str + len)
     index = -1;
 }
  bool operator!=(const Document_key& rsh) const
  {
    return !(*this == rsh);
  }
  bool operator==(const Document_key& rsh) const
  {
    return (length == rsh.length &&
            index == rsh.index &&
            0 == strncmp(string, rsh.string, length));
  }
  /*
     when the string is pure number then it can be an array index,
     index will be the converted value, otherwise, index will be -1
  */
  const char* string;
  int length;
  int index;

private:
  Document_key();
};

/*
  Document path is generated by some rule pre-defined. For example,
  when we have the document path doc.a.b.c the name will be generated
  like `doc`.`a`.`b`.`c`. This can be improved later, like we use an
  invisible character to separate them like:
  SEP doc SEP a SEP b SEP c. Because now, it may cause some problem
  when there is back tick in the document path.
  The purpose of this function is to check whether short_name is a
  prefix of the long_name. It can be any name in MySQL, such as alias name,
  field name and so on.

  @param long_name      The name of an item or field
  @param short_name     The name of an item or field
  @retval false short_name is not a prefix of long name
          true short_name is a prefix of long name
 */
inline bool check_name_match(const char *long_name,
                                    const char *short_name)
{
  DBUG_ASSERT(long_name && short_name);

  bool prefix_q = false;
  /*
    Document path now is guaranteed to start with '`', but for ordinary name
    it's supposed not starting with '`'. It may bring some problem and should
    be fixed later.
  */
  if(*short_name != '`' && *long_name == '`')
  {
    long_name++;
    prefix_q = true;
  }
  int long_name_len = strlen(long_name);
  int short_name_len = strlen(short_name);

  if(short_name_len > long_name_len)
    return false;
  if(prefix_q)
  {
    if(long_name[short_name_len] != '`')
      return false;
    if(long_name[short_name_len + 1] != 0 &&
       long_name[short_name_len + 1] != '.')
      return false;
  }else if(long_name[short_name_len] != 0 &&
           long_name[short_name_len] != '.')
    return false;

  return 0 == system_charset_info->coll->
    strnncoll(system_charset_info,
              (const uchar*)long_name, long_name_len,
              (const uchar*)short_name, short_name_len,
              true);
}

/* Generate document path full name */
const char *gen_document_path_full_name(
  MEM_ROOT *mem_root,
  const char* field_name,
  List<Document_key>& document_path_keys);

class Field
{
  Field(const Item &);				/* Prevent use of these */
  void operator=(Field &);
public:
  /*
    A field can be a sub field of another field. For example,
    a.b.c is a sub field of a.b. So we can't just compare the
    name of the field to check whether it's the one that we
    want.
  */
  virtual bool check_field_name_match(const char *name)
  {
    return (0 == my_strcasecmp(system_charset_info,
                               field_name,
                               name));
  }
  bool has_insert_default_function() const
  {
    return unireg_check == TIMESTAMP_DN_FIELD ||
      unireg_check == TIMESTAMP_DNUN_FIELD;
  }

  bool has_update_default_function() const
  {
    return unireg_check == TIMESTAMP_UN_FIELD ||
      unireg_check == TIMESTAMP_DNUN_FIELD;
  }

  /* To do: inherit Sql_alloc and get these for free */
  static void *operator new(size_t size) throw ()
  { return sql_alloc(size); }
  static void *operator new(size_t size, MEM_ROOT *mem_root) throw () {
    return alloc_root(mem_root, size);
  }
  static void operator delete(void *ptr, MEM_ROOT *mem_root)
  { DBUG_ASSERT(false); /* never called */ }

  static void operator delete(void *ptr_arg, size_t size) throw()
  { TRASH(ptr_arg, size); }

  uchar		*ptr;			// Position to field in record

protected:
  /**
     Byte where the @c NULL bit is stored inside a record. If this Field is a
     @c NOT @c NULL field, this member is @c NULL.
  */
  uchar		*null_ptr;

public:
  /*
    Note that you can use table->in_use as replacement for current_thd member 
    only inside of val_*() and store() members (e.g. you can't use it in cons)
  */
  TABLE *table;                                 // Pointer for table
  TABLE *orig_table;                            // Pointer to original table
  const char	**table_name, *field_name;
  LEX_STRING	comment;
  /* Field is part of the following keys */
  key_map key_start;                /* Keys that starts with this field */
  key_map part_of_key;              /* All keys that includes this field */
  key_map part_of_key_not_clustered;/* ^ but only for non-clustered keys */
  key_map part_of_sortkey;          /* ^ but only keys usable for sorting */
  /* 
    We use three additional unireg types for TIMESTAMP to overcome limitation 
    of current binary format of .frm file. We'd like to be able to support 
    NOW() as default and on update value for such fields but unable to hold 
    this info anywhere except unireg_check field. This issue will be resolved
    in more clean way with transition to new text based .frm format.
    See also comment for Field_timestamp::Field_timestamp().
    Both BLOB types and DOCUMENT type share BLOB_FIELD.
  */
  enum utype  { NONE,DATE,SHIELD,NOEMPTY,CASEUP,PNR,BGNR,PGNR,YES,NO,REL,
		CHECK,EMPTY,UNKNOWN_FIELD,CASEDN,NEXT_NUMBER,INTERVAL_FIELD,
                BIT_FIELD, TIMESTAMP_OLD_FIELD, CAPITALIZE, BLOB_FIELD,
                TIMESTAMP_DN_FIELD, TIMESTAMP_UN_FIELD, TIMESTAMP_DNUN_FIELD};
  enum geometry_type
  {
    GEOM_GEOMETRY = 0, GEOM_POINT = 1, GEOM_LINESTRING = 2, GEOM_POLYGON = 3,
    GEOM_MULTIPOINT = 4, GEOM_MULTILINESTRING = 5, GEOM_MULTIPOLYGON = 6,
    GEOM_GEOMETRYCOLLECTION = 7
  };
  enum document_type
  {
    DOC_NONE = 0,
    DOC_DOCUMENT = 1, // this is a regular document

    // the following type values are used for index ref keys
    // where we store raw values in document field for index scan
    DOC_PATH_TINY,
    DOC_PATH_INT,
    DOC_PATH_DOUBLE,
    DOC_PATH_STRING
  };
  enum imagetype { itRAW, itMBR};

  utype		unireg_check;
  uint32	field_length;		// Length of field
  uint32	flags;
  uint16        field_index;            // field number in fields array
  uchar		null_bit;		// Bit used to test null bit
  /**
     If true, this field was created in create_tmp_field_from_item from a NULL
     value. This means that the type of the field is just a guess, and the type
     may be freely coerced to another type.

     @see create_tmp_field_from_item
     @see Item_type_holder::get_real_type

   */
  bool is_created_from_null_item;

  Field(uchar *ptr_arg,uint32 length_arg,uchar *null_ptr_arg,
        uchar null_bit_arg, utype unireg_check_arg,
        const char *field_name_arg);
  virtual ~Field() {}

  virtual type_conversion_status store_document_value(
    fbson::FbsonValue *val,
    const CHARSET_INFO *cs)
  {
    return TYPE_ERR_BAD_VALUE;
  }
  /* Store functions returns 1 on overflow and -1 on fatal error */
  virtual type_conversion_status store(const char *to, uint length,
                                       const CHARSET_INFO *cs)=0;
  virtual type_conversion_status store(double nr)=0;
  virtual type_conversion_status store(longlong nr, bool unsigned_val)=0;
  virtual type_conversion_status set_delete(){ return TYPE_ERR_BAD_VALUE; }
  /**
    Store a temporal value in packed longlong format into a field.
    The packed value is compatible with TIME_to_longlong_time_packed(),
    TIME_to_longlong_date_packed() or TIME_to_longlong_datetime_packed().
    Note, the value must be properly rounded or truncated according
    according to field->decimals().

    @param  nr  temporal value in packed longlong format.
    @retval false on success
    @retval true  on error
  */
  virtual type_conversion_status store_packed(longlong nr)
  {
    return store(nr, 0);
  }
  virtual type_conversion_status store_decimal(const my_decimal *d)=0;
  /**
    Store MYSQL_TIME value with the given amount of decimal digits
    into a field.

    Note, the "dec" parameter represents number of digits of the Item
    that previously created the MYSQL_TIME value. It's needed when we
    store the value into a CHAR/VARCHAR/TEXT field to display
    the proper amount of fractional digits.
    For other field types the "dec" value does not matter and is ignored.

    @param ltime   Time, date or datetime value.
    @param dec     Number of decimals in ltime.
    @retval false  on success
    @retval true   on error
  */
  virtual type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  /**
    Store MYSQL_TYPE value into a field when the number of fractional
    digits is not important or is not know.

    @param ltime   Time, date or datetime value.
    @retval false   on success
    @retval true   on error
  */
  type_conversion_status store_time(MYSQL_TIME *ltime)
  {
    return store_time(ltime, 0);
  }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *cs,
                               enum_check_fields check_level);
  virtual double val_real(void)=0;
  virtual longlong val_int(void)=0;
  /**
    Returns TIME value in packed longlong format.
    This method should not be called for non-temporal types.
    Temporal field types override the default method.
  */
  virtual longlong val_time_temporal()
  {
    DBUG_ASSERT(0);
    return 0;
  }
  /**
    Returns DATE/DATETIME value in packed longlong format.
    This method should not be called for non-temporal types.
    Temporal field types override the default method.
  */
  virtual longlong val_date_temporal()
  {
    DBUG_ASSERT(0);
    return 0;
  }
  /**
    Returns "native" packed longlong representation of
    a TIME or DATE/DATETIME field depending on field type.
  */
  longlong val_temporal_by_field_type()
  {
    // Return longlong TIME or DATETIME representation, depending on field type
    if (type() == MYSQL_TYPE_TIME)
      return val_time_temporal();
    DBUG_ASSERT(is_temporal_with_date());
    return val_date_temporal();
  }
  virtual my_decimal *val_decimal(my_decimal *)= 0;
  inline String *val_str(String *str) { return val_str(str, str); }
  /*
     val_str(buf1, buf2) gets two buffers and should use them as follows:
     if it needs a temp buffer to convert result to string - use buf1
       example Field_tiny::val_str()
     if the value exists as a string already - use buf2
       example Field_string::val_str()
     consequently, buf2 may be created as 'String buf;' - no memory
     will be allocated for it. buf1 will be allocated to hold a
     value if it's too small. Using allocated buffer for buf2 may result in
     an unnecessary free (and later, may be an alloc).
     This trickery is used to decrease a number of malloc calls.
  */
  virtual String *val_str(String*,String *)=0;
  /*
   * Since we have added a document type in MySQL, here the function is
   * used to extract the value from fields. It works similarly as val_str,
   * val_int ...
   * Get the FbsonValue from a field. There are some codes to be
   * implemented to convert all the type fo document type.
   */
  virtual fbson::FbsonValue *val_document_value(
    String*,
    String*)
  {
    return nullptr;
  }

  /*
    This function return the doc_value of the field in the table.
    It's only used by the wildcards in item_cmpfunc.cc and should be
    removed soon, such as
       select * from doc where doc.a._.b = 123;
    It can be removed later with better design of how wildcard works.
  */
  virtual fbson::FbsonValue *val_root_document_value(String*, String*)
  {
    return nullptr;
  }
  String *val_int_as_str(String *val_buffer, my_bool unsigned_flag);

  /*
   str_needs_quotes() returns TRUE if the value returned by val_str() needs
   to be quoted when used in constructing an SQL query.
  */
  virtual bool str_needs_quotes() { return FALSE; }
  virtual Item_result result_type () const=0;
  /**
    Returns Item_result type of a field when it appears
    in numeric context such as:
      SELECT time_column + 1;
      SELECT SUM(time_column);
    Examples:
    - a column of type TIME, DATETIME, TIMESTAMP act as INT.
    - a column of type TIME(1), DATETIME(1), TIMESTAMP(1)
      act as DECIMAL with 1 fractional digits.
  */
  virtual Item_result numeric_context_result_type() const
  {
    return result_type();
  }
  virtual Item_result cmp_type () const { return result_type(); }
  virtual Item_result cast_to_int_type () const { return result_type(); }
  static bool type_can_have_key_part(enum_field_types);
  static enum_field_types field_type_merge(enum_field_types, enum_field_types);
  static Item_result result_merge_type(enum_field_types);
  virtual bool eq(Field *field)
  {
    return (ptr == field->ptr && null_ptr == field->null_ptr &&
            null_bit == field->null_bit && field->type() == type());
  }
  virtual bool eq_def(Field *field);
  
  /*
    pack_length() returns size (in bytes) used to store field data in memory
    (i.e. it returns the maximum size of the field in a row of the table,
    which is located in RAM).
  */
  virtual uint32 pack_length() const { return (uint32) field_length; }

  /*
    pack_length_in_rec() returns size (in bytes) used to store field data on
    storage (i.e. it returns the maximal size of the field in a row of the
    table, which is located on disk).
  */
  virtual uint32 pack_length_in_rec() const { return pack_length(); }
  virtual bool compatible_field_size(uint metadata, Relay_log_info *rli,
                                     uint16 mflags, int *order);
  virtual uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field::pack_length_from_metadata");
    DBUG_RETURN(field_metadata);
  }
  virtual uint row_pack_length() const { return 0; }
  virtual int save_field_metadata(uchar *first_byte)
  { return do_save_field_metadata(first_byte); }

  /*
    data_length() return the "real size" of the data in memory.
  */
  virtual uint32 data_length() { return pack_length(); }
  virtual uint32 sort_length() const { return pack_length(); }

  /**
     Get the maximum size of the data in packed format.

     @return Maximum data length of the field when packed using the
     Field::pack() function.
   */
  virtual uint32 max_data_length() const {
    return pack_length();
  };

  virtual type_conversion_status reset(void)
  {
    memset(ptr, 0, pack_length());
    return TYPE_OK;
  }
  virtual void reset_fields() {}
  /**
    Returns timestamp value in "struct timeval" format.
    This method is used in "SELECT UNIX_TIMESTAMP(field)"
    to avoid conversion from timestamp to MYSQL_TIME and back.
  */
  virtual bool get_timestamp(struct timeval *tm, int *warnings);
  /**
    Stores a timestamp value in timeval format in a field.
   
   @note 
   - store_timestamp(), get_timestamp() and store_time() do not depend on
   timezone and always work "in UTC".

   - The default implementation of this interface expects that storing the
   value will not fail. For most Field descendent classes, this is not the
   case. However, this interface is only used when the function
   CURRENT_TIMESTAMP is used as a column default expression, and currently we
   only allow TIMESTAMP and DATETIME columns to be declared with this as the
   column default. Hence it is enough that the classes implementing columns
   with these types either override this interface, or that
   store_time(MYSQL_TIME*, uint8) does not fail.

   - The column types above interpret decimals() to mean the scale of the
   fractional seconds.
   
   - We also have the limitation that the scale of a column must be the same as
   the scale of the CURRENT_TIMESTAMP. I.e. we only allow 
   
   @code
   
   [ TIMESTAMP | DATETIME ] (n) [ DEFAULT | ON UPDATE ] CURRENT_TIMESTAMP (n)

   @endcode

   Since this interface relies on the caller to truncate the value according to this
   Field's scale, it will work with all constructs that we currently allow.
  */
  virtual void store_timestamp(const timeval *tm) { DBUG_ASSERT(false); }

  /**
     Interface for legacy code. Newer code uses the store_timestamp(const
     timeval*) interface.

     @param timestamp A TIMESTAMP value in the my_time_t format.
  */
  void store_timestamp(my_time_t sec)
  {
    struct timeval tm;
    tm.tv_sec= sec;
    tm.tv_usec= 0;
    store_timestamp(&tm);
  }
  virtual void set_default()
  {
    if (has_insert_default_function())
    {
      evaluate_insert_default_function();
      return;
    }

    my_ptrdiff_t l_offset= (my_ptrdiff_t) (table->s->default_values -
					  table->record[0]);
    memcpy(ptr, ptr + l_offset, pack_length());
    if (real_maybe_null())
      *null_ptr= ((*null_ptr & (uchar) ~null_bit) |
		  (null_ptr[l_offset] & null_bit));
  }


  /**
     Evaluates the @c INSERT default function and stores the result in the
     field. If no such function exists for the column, or the function is not
     valid for the column's data type, invoking this function has no effect.
  */
  void evaluate_insert_default_function();


  /**
     Evaluates the @c UPDATE default function, if one exists, and stores the
     result in the record buffer. If no such function exists for the column,
     or the function is not valid for the column's data type, invoking this
     function has no effect.
  */
  void evaluate_update_default_function();
  virtual bool binary() const { return 1; }
  virtual bool zero_pack() const { return 1; }
  virtual enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  virtual uint32 key_length() const { return pack_length(); }
  virtual enum_field_types type() const =0;
  virtual enum_field_types real_type() const { return type(); }
  virtual enum_field_types binlog_type() const
  {
    /*
      Binlog stores field->type() as type code by default.
      This puts MYSQL_TYPE_STRING in case of CHAR, VARCHAR, SET and ENUM,
      with extra data type details put into metadata.

      We cannot store field->type() in case of temporal types with
      fractional seconds: TIME(n), DATETIME(n) and TIMESTAMP(n),
      because binlog records with MYSQL_TYPE_TIME, MYSQL_TYPE_DATETIME
      type codes do not have metadata.
      So for temporal data types with fractional seconds we'll store
      real_type() type codes instead, i.e.
      MYSQL_TYPE_TIME2, MYSQL_TYPE_DATETIME2, MYSQL_TYPE_TIMESTAMP2,
      and put precision into metatada.

      Note: perhaps binlog should eventually be modified to store
      real_type() instead of type() for all column types.
    */
    return type();
  }
  inline  int cmp(const uchar *str) { return cmp(ptr,str); }
  virtual int cmp_max(const uchar *a, const uchar *b, uint max_len)
    { return cmp(a, b); }
  virtual int cmp(const uchar *,const uchar *)=0;
  virtual int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0L)
  { return memcmp(a,b,pack_length()); }
  virtual int cmp_offset(uint row_offset)
  { return cmp(ptr,ptr+row_offset); }
  virtual int cmp_binary_offset(uint row_offset)
  { return cmp_binary(ptr, ptr+row_offset); };
  virtual int key_cmp(const uchar *a,const uchar *b)
  { return cmp(a, b); }
  virtual int key_cmp(const uchar *str, uint length)
  { return cmp(ptr,str); }
  virtual uint decimals() const { return 0; }
  /*
    Caller beware: sql_type can change str.Ptr, so check
    ptr() to see if it changed if you are using your own buffer
    in str and restore it with set() if needed
  */
  virtual void sql_type(String &str) const =0;

  bool is_temporal() const
  { return is_temporal_type(type()); }

  bool is_temporal_with_date() const
  { return is_temporal_type_with_date(type()); }

  bool is_temporal_with_time() const
  { return is_temporal_type_with_time(type()); }

  bool is_temporal_with_date_and_time() const
  { return is_temporal_type_with_date_and_time(type()); }

  virtual bool is_null(my_ptrdiff_t row_offset= 0) const
  {
    /*
      if the field is NULLable, it returns NULLity based
      on null_ptr[row_offset] value. Otherwise it returns
      NULL flag depending on TABLE::null_row value.

      The table may have been marked as containing only NULL values
      for all fields if it is a NULL-complemented row of an OUTER JOIN
      or if the query is an implicitly grouped query (has aggregate
      functions but no GROUP BY clause) with no qualifying rows. If
      this is the case (in which TABLE::null_row is true) and the
      field is not nullable, the field is considered to be NULL.

      Do not change the order of testing. Fields may be associated
      with a TABLE object without being part of the current row.
      For NULL value check to work for these fields, they must
      have a valid null_ptr, and this pointer must be checked before
      TABLE::null_row. 

    */
    return real_maybe_null() ?
      MY_TEST(null_ptr[row_offset] & null_bit) : table->null_row;
  }

  virtual bool is_real_null(my_ptrdiff_t row_offset= 0) const
  { return real_maybe_null() ? MY_TEST(null_ptr[row_offset] & null_bit) : false; }

  bool is_null_in_record(const uchar *record) const
  { return real_maybe_null() ? MY_TEST(record[null_offset()] & null_bit) : false; }

  virtual void set_null(my_ptrdiff_t row_offset= 0)
  {
    if (real_maybe_null())
      null_ptr[row_offset]|= null_bit;
  }

  virtual void set_notnull(my_ptrdiff_t row_offset= 0)
  {
    if (real_maybe_null())
      null_ptr[row_offset]&= (uchar) ~null_bit;
  }

  virtual bool maybe_null(void) const
  { return real_maybe_null() || table->maybe_null; }

  /// @return true if this field is NULL-able, false otherwise.
  virtual bool real_maybe_null(void) const
  { return null_ptr != 0; }

  uint null_offset(const uchar *record) const
  { return (uint) (null_ptr - record); }

  uint null_offset() const
  { return null_offset(table->record[0]); }

  void set_null_ptr(uchar *p_null_ptr, uint p_null_bit)
  {
    null_ptr= p_null_ptr;
    null_bit= p_null_bit;
  }

  enum {
    LAST_NULL_BYTE_UNDEF= 0
  };

  /*
    Find the position of the last null byte for the field.

    SYNOPSIS
      last_null_byte()

    DESCRIPTION
      Return a pointer to the last byte of the null bytes where the
      field conceptually is placed.

    RETURN VALUE
      The position of the last null byte relative to the beginning of
      the record. If the field does not use any bits of the null
      bytes, the value 0 (LAST_NULL_BYTE_UNDEF) is returned.
   */
  size_t last_null_byte() const {
    size_t bytes= do_last_null_byte();
    DBUG_PRINT("debug", ("last_null_byte() ==> %ld", (long) bytes));
    DBUG_ASSERT(bytes <= table->s->null_bytes);
    return bytes;
  }

  virtual void make_field(Send_field *);

  /**
    Writes a copy of the current value in the record buffer, suitable for
    sorting using byte-by-byte comparison. Integers are always in big-endian
    regardless of hardware architecture. At most length bytes are written
    into the buffer.

    @param buff The buffer, assumed to be at least length bytes.

    @param length Number of bytes to write.
  */
  virtual void make_sort_key(uchar *buff, uint length) = 0;

  /*
    Make sort key as a given type, this is for document path only, e.g.
    ORDER BY doc.a.b.c AS int, in which doc.a.b.c is a document path.
  */
  virtual void make_sort_key_as_type(uchar *buff, uint length,
                                     enum_field_types as_type)
  {
    DBUG_ASSERT(0);
  }
  virtual bool optimize_range(uint idx, uint part);
  /*
    This should be true for fields which, when compared with constant
    items, can be casted to longlong. In this case we will at 'fix_fields'
    stage cast the constant items to longlongs and at the execution stage
    use field->val_int() for comparison.  Used to optimize clauses like
    'a_column BETWEEN date_const, date_const'.
  */
  virtual bool can_be_compared_as_longlong() const { return false; }
  virtual void free() {}
  virtual Field *new_field(MEM_ROOT *root,
                           TABLE *new_table,
                           bool keep_type,
                           // If it's called from an item with alias
                           bool from_alias = false
                          );
  virtual Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                               uchar *new_ptr, uchar *new_null_ptr,
                               uint new_null_bit);

  Field *new_key_field(MEM_ROOT *root,
                       TABLE *new_table,
                       uchar *new_ptr)
  {
    return new_key_field(root,
                         new_table,
                         new_ptr,
                         null_ptr,
                         null_bit);
  }

  /**
     Makes a shallow copy of the Field object.
     
     @note This member function must be overridden in all concrete
     subclasses. Several of the Field subclasses are concrete even though they
     are not leaf classes, so the compiler will not always catch this.

     @retval NULL If memory allocation failed.
  */ 
  virtual Field *clone() const =0;

  /**
     Makes a shallow copy of the Field object.
     
     @note This member function must be overridden in all concrete
     subclasses. Several of the Field subclasses are concrete even though they
     are not leaf classes, so the compiler will not always catch this.
     
     @param mem_root MEM_ROOT to use for memory allocation.
     @retval NULL If memory allocation failed.
   */
  virtual Field *clone(MEM_ROOT *mem_root) const =0;
  inline void move_field(uchar *ptr_arg,uchar *null_ptr_arg,uchar null_bit_arg)
  {
    ptr=ptr_arg; null_ptr=null_ptr_arg; null_bit=null_bit_arg;
  }
  inline void move_field(uchar *ptr_arg) { ptr=ptr_arg; }
  virtual void move_field_offset(my_ptrdiff_t ptr_diff)
  {
    ptr=ADD_TO_PTR(ptr,ptr_diff, uchar*);
    if (null_ptr)
      null_ptr=ADD_TO_PTR(null_ptr,ptr_diff,uchar*);
  }
  virtual void get_image(uchar *buff, uint length, const CHARSET_INFO *cs)
    { memcpy(buff,ptr,length); }
  virtual void set_image(const uchar *buff,uint length,
                         const CHARSET_INFO *cs)
    { memcpy(ptr,buff,length); }


  /*
    Copy a field part into an output buffer.

    SYNOPSIS
      Field::get_key_image()
      buff   [out] output buffer
      length       output buffer size
      type         itMBR for geometry blobs, otherwise itRAW

    DESCRIPTION
      This function makes a copy of field part of size equal to or
      less than "length" parameter value.
      For fields of string types (CHAR, VARCHAR, TEXT) the rest of buffer
      is padded by zero byte.

    NOTES
      For variable length character fields (i.e. UTF-8) the "length"
      parameter means a number of output buffer bytes as if all field
      characters have maximal possible size (mbmaxlen). In the other words,
      "length" parameter is a number of characters multiplied by
      field_charset->mbmaxlen.

    RETURN
      Number of copied bytes (excluding padded zero bytes -- see above).
  */

  virtual uint get_key_image(uchar *buff, uint length, imagetype type)
  {
    get_image(buff, length, &my_charset_bin);
    return length;
  }
  virtual void set_key_image(const uchar *buff,uint length)
    { set_image(buff,length, &my_charset_bin); }
  inline longlong val_int_offset(uint row_offset)
    {
      ptr+=row_offset;
      longlong tmp=val_int();
      ptr-=row_offset;
      return tmp;
    }
  inline longlong val_int(const uchar *new_ptr)
  {
    uchar *old_ptr= ptr;
    longlong return_value;
    ptr= (uchar*) new_ptr;
    return_value= val_int();
    ptr= old_ptr;
    return return_value;
  }
  inline String *val_str(String *str, const uchar *new_ptr)
  {
    uchar *old_ptr= ptr;
    ptr= (uchar*) new_ptr;
    val_str(str);
    ptr= old_ptr;
    return str;
  }
  virtual bool send_binary(Protocol *protocol);

  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  /**
     @overload Field::pack(uchar*, const uchar*, uint, bool)
  */
  uchar *pack(uchar *to, const uchar *from)
  {
    DBUG_ENTER("Field::pack");
    uchar *result= this->pack(to, from, UINT_MAX, table->s->db_low_byte_first);
    DBUG_RETURN(result);
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first);
  /**
     @overload Field::unpack(uchar*, const uchar*, uint, bool)
  */
  const uchar *unpack(uchar* to, const uchar *from)
  {
    DBUG_ENTER("Field::unpack");
    const uchar *result= unpack(to, from, 0U, table->s->db_low_byte_first);
    DBUG_RETURN(result);
  }

  virtual uint packed_col_length(const uchar *to, uint length)
  { return length;}
  virtual uint max_packed_col_length(uint max_length)
  { return max_length;}

  uint offset(uchar *record)
  {
    return (uint) (ptr - record);
  }
  void copy_from_tmp(int offset);
  uint fill_cache_field(struct st_cache_field *copy);
  virtual bool get_date(MYSQL_TIME *ltime,uint fuzzydate);
  virtual bool get_time(MYSQL_TIME *ltime);
  virtual const CHARSET_INFO *charset(void) const { return &my_charset_bin; }
  virtual const CHARSET_INFO *charset_for_protocol(void) const
  { return binary() ? &my_charset_bin : charset(); }
  virtual const CHARSET_INFO *sort_charset(void) const { return charset(); }
  virtual bool has_charset(void) const { return FALSE; }
  /*
    match_collation_to_optimize_range() is to distinguish in
    range optimizer (see opt_range.cc) between real string types:
      CHAR, VARCHAR, TEXT
    and the other string-alike types with result_type() == STRING_RESULT:
      DATE, TIME, DATETIME, TIMESTAMP
    We need it to decide whether to test if collation of the operation
    matches collation of the field (needed only for real string types).
    QQ: shouldn't DATE/TIME types have their own XXX_RESULT types eventually?
  */
  virtual bool match_collation_to_optimize_range() const { return false; };
  virtual enum Derivation derivation(void) const
  { return DERIVATION_IMPLICIT; }
  virtual uint repertoire(void) const { return MY_REPERTOIRE_UNICODE30; }
  virtual void set_derivation(enum Derivation derivation_arg) { }
  bool set_warning(Sql_condition::enum_warning_level, unsigned int code,
                   int cuted_increment) const;
  inline bool check_overflow(int op_result)
  {
    return (op_result == E_DEC_OVERFLOW);
  }
  inline bool check_truncated(int op_result)
  {
    return (op_result == E_DEC_TRUNCATED);
  }
  bool warn_if_overflow(int op_result);
  void init(TABLE *table_arg)
  {
    orig_table= table= table_arg;
    table_name= &table_arg->alias;
  }

  /* maximum possible display length */
  virtual uint32 max_display_length()= 0;

  /**
    Whether a field being created is compatible with a existing one.

    Used by the ALTER TABLE code to evaluate whether the new definition
    of a table is compatible with the old definition so that it can
    determine if data needs to be copied over (table data change).
  */
  virtual uint is_equal(Create_field *new_field);
  /* convert decimal to longlong with overflow check */
  longlong convert_decimal2longlong(const my_decimal *val, bool unsigned_flag,
                                    bool *has_overflow);
  /* The max. number of characters */
  virtual uint32 char_length() const
  {
    return field_length / charset()->mbmaxlen;
  }

  virtual geometry_type get_geometry_type()
  {
    /* shouldn't get here. */
    DBUG_ASSERT(0);
    return GEOM_GEOMETRY;
  }

  virtual document_type get_document_type()
  {
    /* shouldn't get here. */
    DBUG_ASSERT(0);
    return DOC_NONE;
  }
#ifndef DBUG_OFF
  /* Print field value into debug trace, in NULL-aware way. */
  void dbug_print()
  {
    if (is_real_null())
      fprintf(DBUG_FILE, "NULL");
    else
    {
      char buf[256];
      String str(buf, sizeof(buf), &my_charset_bin);
      str.length(0);
      String *pstr;
      pstr= val_str(&str);
      fprintf(DBUG_FILE, "'%s'", pstr->c_ptr_safe());
    }
  }
#endif

  ha_storage_media field_storage_type() const
  {
    return (ha_storage_media)
      ((flags >> FIELD_FLAGS_STORAGE_MEDIA) & 3);
  }

  void set_storage_type(ha_storage_media storage_type_arg)
  {
    DBUG_ASSERT(field_storage_type() == HA_SM_DEFAULT);
    flags |= (storage_type_arg << FIELD_FLAGS_STORAGE_MEDIA);
  }

  column_format_type column_format() const
  {
    return (column_format_type)
      ((flags >> FIELD_FLAGS_COLUMN_FORMAT) & 3);
  }

  void set_column_format(column_format_type column_format_arg)
  {
    DBUG_ASSERT(column_format() == COLUMN_FORMAT_TYPE_DEFAULT);
    flags |= (column_format_arg << FIELD_FLAGS_COLUMN_FORMAT);
  }

  /* Validate the value stored in a field */
  virtual type_conversion_status validate_stored_val(THD *thd)
  { return TYPE_OK; }

  /* Hash value */
  virtual void hash(ulong *nr, ulong *nr2);

/**
  Checks whether a string field is part of write_set.

  @return
    FALSE  - If field is not char/varchar/....
           - If field is char/varchar/.. and is not part of write set.
    TRUE   - If field is char/varchar/.. and is part of write set.
*/
  virtual bool is_updatable() const { return FALSE; }

  friend int cre_myisam(char * name, TABLE *form, uint options,
			ulonglong auto_increment_value);
  friend class Copy_field;
  friend class Item_avg_field;
  friend class Item_std_field;
  friend class Item_sum_num;
  friend class Item_sum_sum;
  friend class Item_sum_str;
  friend class Item_sum_count;
  friend class Item_sum_avg;
  friend class Item_sum_std;
  friend class Item_sum_min;
  friend class Item_sum_max;
  friend class Item_func_group_concat;

private:
  /*
    Primitive for implementing last_null_byte().

    SYNOPSIS
      do_last_null_byte()

    DESCRIPTION
      Primitive for the implementation of the last_null_byte()
      function. This represents the inheritance interface and can be
      overridden by subclasses.
   */
  virtual size_t do_last_null_byte() const;

/**
   Retrieve the field metadata for fields.

   This default implementation returns 0 and saves 0 in the metadata_ptr
   value.

   @param   metadata_ptr   First byte of field metadata

   @returns 0 no bytes written.
*/
  virtual int do_save_field_metadata(uchar *metadata_ptr)
  { return 0; }

protected:
  static void handle_int16(uchar *to, const uchar *from,
                           bool low_byte_first_from, bool low_byte_first_to)
  {
    int16 val;
#ifdef WORDS_BIGENDIAN
    if (low_byte_first_from)
      val = sint2korr(from);
    else
#endif
      shortget(val, from);

#ifdef WORDS_BIGENDIAN
    if (low_byte_first_to)
      int2store(to, val);
    else
#endif
      shortstore(to, val);
  }

  static void handle_int24(uchar *to, const uchar *from,
                           bool low_byte_first_from, bool low_byte_first_to)
  {
    int32 val;
#ifdef WORDS_BIGENDIAN
    if (low_byte_first_from)
      val = sint3korr(from);
    else
#endif
      val= (from[0] << 16) + (from[1] << 8) + from[2];

#ifdef WORDS_BIGENDIAN
    if (low_byte_first_to)
      int2store(to, val);
    else
#endif
    {
      to[0]= 0xFF & (val >> 16);
      to[1]= 0xFF & (val >> 8);
      to[2]= 0xFF & val;
    }
  }

  /*
    Helper function to pack()/unpack() int32 values
  */
  static void handle_int32(uchar *to, const uchar *from,
                           bool low_byte_first_from, bool low_byte_first_to)
  {
    int32 val;
#ifdef WORDS_BIGENDIAN
    if (low_byte_first_from)
      val = sint4korr(from);
    else
#endif
      longget(val, from);

#ifdef WORDS_BIGENDIAN
    if (low_byte_first_to)
      int4store(to, val);
    else
#endif
      longstore(to, val);
  }

  /*
    Helper function to pack()/unpack() int64 values
  */
  static void handle_int64(uchar* to, const uchar *from,
                           bool low_byte_first_from, bool low_byte_first_to)
  {
    int64 val;
#ifdef WORDS_BIGENDIAN
    if (low_byte_first_from)
      val = sint8korr(from);
    else
#endif
      longlongget(val, from);

#ifdef WORDS_BIGENDIAN
    if (low_byte_first_to)
      int8store(to, val);
    else
#endif
      longlongstore(to, val);
  }

  uchar *pack_int16(uchar *to, const uchar *from, bool low_byte_first_to)
  {
    handle_int16(to, from, table->s->db_low_byte_first, low_byte_first_to);
    return to  + sizeof(int16);
  }

  const uchar *unpack_int16(uchar* to, const uchar *from,
                            bool low_byte_first_from)
  {
    handle_int16(to, from, low_byte_first_from, table->s->db_low_byte_first);
    return from + sizeof(int16);
  }

  uchar *pack_int24(uchar *to, const uchar *from, bool low_byte_first_to)
  {
    handle_int24(to, from, table->s->db_low_byte_first, low_byte_first_to);
    return to + 3;
  }

  const uchar *unpack_int24(uchar* to, const uchar *from,
                            bool low_byte_first_from)
  {
    handle_int24(to, from, low_byte_first_from, table->s->db_low_byte_first);
    return from + 3;
  }

  uchar *pack_int32(uchar *to, const uchar *from, bool low_byte_first_to)
  {
    handle_int32(to, from, table->s->db_low_byte_first, low_byte_first_to);
    return to  + sizeof(int32);
  }

  const uchar *unpack_int32(uchar* to, const uchar *from,
                            bool low_byte_first_from)
  {
    handle_int32(to, from, low_byte_first_from, table->s->db_low_byte_first);
    return from + sizeof(int32);
  }

  uchar *pack_int64(uchar* to, const uchar *from, bool low_byte_first_to)
  {
    handle_int64(to, from, table->s->db_low_byte_first, low_byte_first_to);
    return to + sizeof(int64);
  }

  const uchar *unpack_int64(uchar* to, const uchar *from,
                            bool low_byte_first_from)
  {
    handle_int64(to, from, low_byte_first_from, table->s->db_low_byte_first);
    return from + sizeof(int64);
  }

};


class Field_num :public Field {
public:
  const uint8 dec;
  bool zerofill,unsigned_flag;	// Purify cannot handle bit fields
  Field_num(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const char *field_name_arg,
            uint8 dec_arg, bool zero_arg, bool unsigned_arg);
  Item_result result_type () const { return REAL_RESULT; }
  enum Derivation derivation(void) const { return DERIVATION_NUMERIC; }
  uint repertoire(void) const { return MY_REPERTOIRE_NUMERIC; }
  const CHARSET_INFO *charset(void) const { return &my_charset_numeric; }
  void prepend_zeros(String *value);
  void add_zerofill_and_unsigned(String &res) const;
  friend class Create_field;
  uint decimals() const { return (uint) dec; }
  bool eq_def(Field *field);
  type_conversion_status store_decimal(const my_decimal *);
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  my_decimal *val_decimal(my_decimal *);
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  bool get_time(MYSQL_TIME *ltime);
  uint is_equal(Create_field *new_field);
  uint row_pack_length() const { return pack_length(); }
  uint32 pack_length_from_metadata(uint field_metadata) {
    uint32 length= pack_length();
    DBUG_PRINT("result", ("pack_length_from_metadata(%d): %u",
                          field_metadata, length));
    return length;
  }
  type_conversion_status check_int(const CHARSET_INFO *cs,
                                   const char *str, int length,
                                   const char *int_end, int error);
  type_conversion_status get_int(const CHARSET_INFO *cs,
                                 const char *from, uint len,
                                 longlong *rnd, ulonglong unsigned_max,
                                 longlong signed_min, longlong signed_max);
};


class Field_str :public Field {
protected:
  const CHARSET_INFO *field_charset;
  enum Derivation field_derivation;
public:
  Field_str(uchar *ptr_arg,uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg, utype unireg_check_arg,
	    const char *field_name_arg, const CHARSET_INFO *charset);
  Item_result result_type () const { return STRING_RESULT; }
  Item_result numeric_context_result_type() const
  { 
    return REAL_RESULT; 
  }
  uint decimals() const { return NOT_FIXED_DEC; }
  void make_field(Send_field *field);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val)=0;
  type_conversion_status store_decimal(const my_decimal *);
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *cs)=0;
  uint repertoire(void) const
  {
    return my_charset_repertoire(field_charset);
  }
  const CHARSET_INFO *charset(void) const { return field_charset; }
  void set_charset(const CHARSET_INFO *charset_arg)
  { field_charset= charset_arg; }
  enum Derivation derivation(void) const { return field_derivation; }
  virtual void set_derivation(enum Derivation derivation_arg)
  { field_derivation= derivation_arg; }
  bool binary() const { return field_charset == &my_charset_bin; }
  uint32 max_display_length() { return field_length; }
  friend class Create_field;
  virtual bool str_needs_quotes() { return TRUE; }
  uint is_equal(Create_field *new_field);
};


/* base class for Field_string, Field_varstring and Field_blob */

class Field_longstr :public Field_str
{
protected:
  type_conversion_status report_if_important_data(const char *ptr,
                                                  const char *end,
                                                  bool count_spaces) const;
  type_conversion_status
    check_string_copy_error(const char *well_formed_error_pos,
                            const char *cannot_convert_error_pos,
                            const char *from_end_pos,
                            const char *end,
                            bool count_spaces,
                            const CHARSET_INFO *cs) const;
public:
  Field_longstr(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                uchar null_bit_arg, utype unireg_check_arg,
                const char *field_name_arg, const CHARSET_INFO *charset_arg)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, charset_arg)
    {}

  type_conversion_status store_decimal(const my_decimal *d);
  uint32 max_data_length() const;
  bool is_updatable() const
  {
    DBUG_ASSERT(table && table->write_set);
    return bitmap_is_set(table->write_set, field_index);
  }
};

/* base class for float and double and decimal (old one) */
class Field_real :public Field_num {
public:
  my_bool not_fixed;

  Field_real(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg, utype unireg_check_arg,
             const char *field_name_arg,
             uint8 dec_arg, bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
               field_name_arg, dec_arg, zero_arg, unsigned_arg),
    not_fixed(dec_arg >= NOT_FIXED_DEC)
    {}
  type_conversion_status store_decimal(const my_decimal *);
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  my_decimal *val_decimal(my_decimal *);
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  bool get_time(MYSQL_TIME *ltime);
  bool truncate(double *nr, double max_length);
  uint32 max_display_length() { return field_length; }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first);
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length, bool low_byte_first);
};


class Field_decimal :public Field_real {
public:
  Field_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
		uchar null_bit_arg,
		enum utype unireg_check_arg, const char *field_name_arg,
		uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {}
  enum_field_types type() const { return MYSQL_TYPE_DECIMAL;}
  enum ha_base_keytype key_type() const
  { return zerofill ? HA_KEYTYPE_BINARY : HA_KEYTYPE_NUM; }
  type_conversion_status reset(void);
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  void overflow(bool negative);
  bool zero_pack() const { return 0; }
  void sql_type(String &str) const;
  Field_decimal *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_DECIMAL);
    return new (mem_root) Field_decimal(*this);
  }
  Field_decimal *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_DECIMAL);
    return new Field_decimal(*this);
  }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first)
  {
    return Field::unpack(to, from, param_data, low_byte_first);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length, bool low_byte_first)
  {
    return Field::pack(to, from, max_length, low_byte_first);
  }
};


/* New decimal/numeric field which use fixed point arithmetic */
class Field_new_decimal :public Field_num {
private:
  int do_save_field_metadata(uchar *first_byte);
public:

  /* The maximum number of decimal digits can be stored */
  uint precision;
  uint bin_size;
  /*
    Constructors take max_length of the field as a parameter - not the
    precision as the number of decimal digits allowed.
    So for example we need to count length from precision handling
    CREATE TABLE ( DECIMAL(x,y)) 
  */
  Field_new_decimal(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg, const char *field_name_arg,
                    uint8 dec_arg, bool zero_arg, bool unsigned_arg);
  Field_new_decimal(uint32 len_arg, bool maybe_null_arg,
                    const char *field_name_arg, uint8 dec_arg,
                    bool unsigned_arg);
  enum_field_types type() const { return MYSQL_TYPE_NEWDECIMAL;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  Item_result result_type () const { return DECIMAL_RESULT; }
  type_conversion_status reset(void);
  type_conversion_status store_value(const my_decimal *decimal_value);
  void set_value_on_overflow(my_decimal *decimal_value, bool sign);
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  type_conversion_status store_decimal(const my_decimal *);
  double val_real(void);
  longlong val_int(void);
  my_decimal *val_decimal(my_decimal *);
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  bool get_time(MYSQL_TIME *ltime);
  String *val_str(String*, String *);
  int cmp(const uchar *, const uchar *);
  void make_sort_key(uchar *buff, uint length);
  bool zero_pack() const { return 0; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return field_length; }
  uint32 pack_length() const { return (uint32) bin_size; }
  uint pack_length_from_metadata(uint field_metadata);
  uint row_pack_length() const { return pack_length(); }
  bool compatible_field_size(uint field_metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  uint is_equal(Create_field *new_field);
  Field_new_decimal *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_NEWDECIMAL);
    return new (mem_root) Field_new_decimal(*this);
  }
  Field_new_decimal *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_NEWDECIMAL);
    return new Field_new_decimal(*this);
  }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first);
  static Field *create_from_item (Item *);
};


class Field_tiny :public Field_num {
public:
  Field_tiny(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg,
	       0, zero_arg,unsigned_arg)
    {}
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types type() const { return MYSQL_TYPE_TINY;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_BINARY : HA_KEYTYPE_INT8; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void) { ptr[0]=0; return TYPE_OK; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return 1; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 4; }
  Field_tiny *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_TINY);
    return new (mem_root) Field_tiny(*this);
  }
  Field_tiny *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_TINY);
    return new Field_tiny(*this);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length, bool low_byte_first)
  {
    *to= *from;
    return to + 1;
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first)
  {
    *to= *from;
    return from + 1;
  }
};


class Field_short :public Field_num {
public:
  Field_short(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const char *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg,
	       0, zero_arg,unsigned_arg)
    {}
  Field_short(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	      bool unsigned_arg)
    :Field_num((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
	       NONE, field_name_arg, 0, 0, unsigned_arg)
    {}
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types type() const { return MYSQL_TYPE_SHORT;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_USHORT_INT : HA_KEYTYPE_SHORT_INT;}
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void) { ptr[0]=ptr[1]=0; return TYPE_OK; }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return 2; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 6; }
  Field_short *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_SHORT);
    return new (mem_root) Field_short(*this);
  }
  Field_short *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_SHORT);
    return new Field_short(*this);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length, bool low_byte_first)
  {
    return pack_int16(to, from, low_byte_first);
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first)
  {
    return unpack_int16(to, from, low_byte_first);
  }
};

class Field_medium :public Field_num {
public:
  Field_medium(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const char *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg,
	       0, zero_arg,unsigned_arg)
    {}
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types type() const { return MYSQL_TYPE_INT24;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_UINT24 : HA_KEYTYPE_INT24; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=0;
    return TYPE_OK;
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return 3; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 8; }
  Field_medium *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_INT24);
    return new (mem_root) Field_medium(*this);
  }
  Field_medium *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_INT24);
    return new Field_medium(*this);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length, bool low_byte_first)
  {
    return Field::pack(to, from, max_length, low_byte_first);
  }

  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first)
  {
    return Field::unpack(to, from, param_data, low_byte_first);
  }
};


class Field_long :public Field_num {
public:

  static const int PACK_LENGTH= 4;

  Field_long(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg,
	       0, zero_arg,unsigned_arg)
    {}
  Field_long(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	     bool unsigned_arg)
    :Field_num((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
	       NONE, field_name_arg,0,0,unsigned_arg)
    {}
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types type() const { return MYSQL_TYPE_LONG;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_ULONG_INT : HA_KEYTYPE_LONG_INT; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=0;
    return TYPE_OK;
  }
  double val_real(void);
  longlong val_int(void);
  bool send_binary(Protocol *protocol);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return PACK_LENGTH; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return MY_INT32_NUM_DECIMAL_DIGITS; }
  Field_long *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_LONG);
    return new (mem_root) Field_long(*this);
  }
  Field_long *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_LONG);
    return new Field_long(*this);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length MY_ATTRIBUTE((unused)),
                      bool low_byte_first)
  {
    return pack_int32(to, from, low_byte_first);
  }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data MY_ATTRIBUTE((unused)),
                              bool low_byte_first)
  {
    return unpack_int32(to, from, low_byte_first);
  }
};


#ifdef HAVE_LONG_LONG
class Field_longlong :public Field_num {
public:
  static const int PACK_LENGTH= 8;

  Field_longlong(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const char *field_name_arg,
	      bool zero_arg, bool unsigned_arg)
    :Field_num(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg,
	       0, zero_arg,unsigned_arg)
    {}
  Field_longlong(uint32 len_arg,bool maybe_null_arg,
		 const char *field_name_arg,
		  bool unsigned_arg)
    :Field_num((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0,0,
	       NONE, field_name_arg,0,0,unsigned_arg)
    {}
  enum Item_result result_type () const { return INT_RESULT; }
  enum_field_types type() const { return MYSQL_TYPE_LONGLONG;}
  enum ha_base_keytype key_type() const
    { return unsigned_flag ? HA_KEYTYPE_ULONGLONG : HA_KEYTYPE_LONGLONG; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=ptr[4]=ptr[5]=ptr[6]=ptr[7]=0;
    return TYPE_OK;
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return PACK_LENGTH; }
  void sql_type(String &str) const;
  bool can_be_compared_as_longlong() const { return true; }
  uint32 max_display_length() { return 20; }
  Field_longlong *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_LONGLONG);
    return new (mem_root) Field_longlong(*this);
  }
  Field_longlong *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_LONGLONG);
    return new Field_longlong(*this);
  }
  virtual uchar *pack(uchar* to, const uchar *from,
                      uint max_length  MY_ATTRIBUTE((unused)),
                      bool low_byte_first)
  {
    return pack_int64(to, from, low_byte_first);
  }
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data MY_ATTRIBUTE((unused)),
                              bool low_byte_first)
  {
    return unpack_int64(to, from, low_byte_first);
  }
};
#endif


class Field_float :public Field_real {
public:
  Field_float(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	      uchar null_bit_arg,
	      enum utype unireg_check_arg, const char *field_name_arg,
              uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {}
  Field_float(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	      uint8 dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {}
  enum_field_types type() const { return MYSQL_TYPE_FLOAT;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_FLOAT; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void)
  {
    memset(ptr, 0, sizeof(float));
    return TYPE_OK;
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return sizeof(float); }
  uint row_pack_length() const { return pack_length(); }
  void sql_type(String &str) const;
  Field_float *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_FLOAT);
    return new (mem_root) Field_float(*this);
  }
  Field_float *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_FLOAT);
    return new Field_float(*this);
  }
private:
  int do_save_field_metadata(uchar *first_byte);
};


class Field_double :public Field_real {
public:
  Field_double(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const char *field_name_arg,
	       uint8 dec_arg,bool zero_arg,bool unsigned_arg)
    :Field_real(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                unireg_check_arg, field_name_arg,
                dec_arg, zero_arg, unsigned_arg)
    {}
  Field_double(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	       uint8 dec_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {}
  Field_double(uint32 len_arg, bool maybe_null_arg, const char *field_name_arg,
	       uint8 dec_arg, my_bool not_fixed_arg)
    :Field_real((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "" : 0, (uint) 0,
                NONE, field_name_arg, dec_arg, 0, 0)
    {not_fixed= not_fixed_arg; }
  enum_field_types type() const { return MYSQL_TYPE_DOUBLE;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_DOUBLE; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status reset(void)
  {
    memset(ptr, 0, sizeof(double));
    return TYPE_OK;
  }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return sizeof(double); }
  uint row_pack_length() const { return pack_length(); }
  void sql_type(String &str) const;
  Field_double *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_DOUBLE);
    return new (mem_root) Field_double(*this);
  }
  Field_double *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_DOUBLE);
    return new Field_double(*this);
  }
private:
  int do_save_field_metadata(uchar *first_byte);
};


/* Everything saved in this will disappear. It will always return NULL */

class Field_null :public Field_str {
  static uchar null[1];
public:
  Field_null(uchar *ptr_arg, uint32 len_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     const CHARSET_INFO *cs)
    :Field_str(ptr_arg, len_arg, null, 1,
	       unireg_check_arg, field_name_arg, cs)
    {}
  enum_field_types type() const { return MYSQL_TYPE_NULL;}
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *cs)
  {
    null[0]= 1;
    return TYPE_OK;
  }
  type_conversion_status store(double nr)   { null[0]=1; return TYPE_OK; }
  type_conversion_status store(longlong nr, bool unsigned_val)
  {
    null[0]=1;
    return TYPE_OK;
  }
  type_conversion_status store_decimal(const my_decimal *d)
  {
    null[0]=1;
    return TYPE_OK;
  }
  type_conversion_status reset(void)       { return TYPE_OK; }
  double val_real(void)		{ return 0.0;}
  longlong val_int(void)	{ return 0;}
  my_decimal *val_decimal(my_decimal *) { return 0; }
  String *val_str(String *value,String *value2)
  { value2->length(0); return value2;}
  int cmp(const uchar *a, const uchar *b) { return 0;}
  void make_sort_key(uchar *buff, uint length)  {}
  uint32 pack_length() const { return 0; }
  void sql_type(String &str) const;
  uint32 max_display_length() { return 4; }
  Field_null *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_NULL);
    return new (mem_root) Field_null(*this);
  }
  Field_null *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_NULL);
    return new Field_null(*this);
  }
};


/*
  Abstract class for TIME, DATE, DATETIME, TIMESTAMP
  with and without fractional part.
*/
class Field_temporal :public Field {
protected:
  uint8 dec; // Number of fractional digits

  /**
    Adjust number of decimal digits from NOT_FIXED_DEC to DATETIME_MAX_DECIMALS
  */
  uint8 normalize_dec(uint8 dec_arg)
  { return dec_arg == NOT_FIXED_DEC ? DATETIME_MAX_DECIMALS : dec_arg; }

  /**
    Low level routine to store a MYSQL_TIME value into a field.
    The value must be already properly rounded or truncated
    and checked for being a valid TIME/DATE/DATETIME value.

    @param IN   ltime   MYSQL_TIME value.
    @param OUT  error   Error flag vector, set in case of error.
    @retval     false   In case of success.
    @retval     true    In case of error.
  */
  virtual type_conversion_status store_internal(const MYSQL_TIME *ltime,
                                                int *error)= 0;

  /**
    Low level routine to store a MYSQL_TIME value into a field
    with rounding according to the field decimals() value.

    @param IN   ltime   MYSQL_TIME value.
    @param OUT  error   Error flag vector, set in case of error.
    @retval     false   In case of success.
    @retval     true    In case of error.    
  */
  virtual type_conversion_status store_internal_with_round(MYSQL_TIME *ltime,
                                                           int *warnings)= 0;

  /**
    Store a temporal value in lldiv_t into a field,
    with rounding according to the field decimals() value.

    @param IN   lld     Temporal value.
    @param OUT  warning Warning flag vector.
    @retval     false   In case of success.
    @retval     true    In case of error.    
  */
  type_conversion_status store_lldiv_t(const lldiv_t *lld, int *warning);

  /**
    Convert a string to MYSQL_TIME, according to the field type.

    @param IN   str     String
    @param IN   len     String length
    @param IN   cs      String character set
    @param OUT  ltime   The value is stored here
    @param OUT  status  Conversion status
    @retval     false   Conversion went fine, ltime contains a valid time
    @retval     true    Conversion failed, ltime was reset and contains nothing
  */
  virtual bool convert_str_to_TIME(const char *str, uint len,
                                   const CHARSET_INFO *cs,
                                   MYSQL_TIME *ltime, 
                                   MYSQL_TIME_STATUS *status)= 0;
  /**
    Convert a number with fractional part with nanosecond precision
    into MYSQL_TIME, according to the field type. Nanoseconds
    are rounded to milliseconds and added to ltime->second_part.

    @param IN   nr            Number
    @param IN   unsigned_val  SIGNED/UNSIGNED flag
    @param IN   nanoseconds   Fractional part in nanoseconds
    @param OUT  ltime         The value is stored here
    @param OUT  status        Conversion status
    @retval     false         On success
    @retval     true          On error
  */
  virtual type_conversion_status convert_number_to_TIME(longlong nr,
                                                        bool unsigned_val,
                                                        int nanoseconds,
                                                        MYSQL_TIME *ltime,
                                                        int *warning)= 0;

  /**
    Convert an integer number into MYSQL_TIME, according to the field type.

    @param IN   nr            Number
    @param IN   unsigned_val  SIGNED/UNSIGNED flag
    @param OUT  ltime         The value is stored here
    @retval     false         On success
    @retval     true          On error
  */
  longlong convert_number_to_datetime(longlong nr, bool unsigned_val,
                                      MYSQL_TIME *ltime, int *warning);

  /**
    Set a warning according to warning bit flag vector.
    Multiple warnings are possible at the same time.
    Every warning in the bit vector is set by an individual
    set_datetime_warning() call.

    @param str      Warning parameter
    @param warnings Warning bit flag
  */
  void set_warnings(ErrConvString str, int warnings);

  /**
    Flags that are passed as "flag" argument to
    check_date(), number_to_datetime(), str_to_datetime().

    Flags depend on the session sql_mode settings, such as
    MODE_NO_ZERO_DATE, MODE_NO_ZERO_IN_DATE.
    Also, Field_newdate, Field_datetime, Field_datetimef add TIME_FUZZY_DATE
    to the session sql_mode settings, to allow relaxed date format,
    while Field_timestamp, Field_timestampf do not.

    @param  thd  THD
    @retval      sql_mode flags mixed with the field type flags.
  */
  virtual ulonglong date_flags(const THD *thd)
  {
    return 0;
  }
  /**
    Flags that are passed as "flag" argument to
    check_date(), number_to_datetime(), str_to_datetime().
    Similar to the above when we don't have a THD value.
  */
  inline ulonglong date_flags()
  {
    return date_flags(table ? table->in_use : current_thd);
  }

  /**
    Set a single warning using make_truncated_value_warning().
    
    @param IN  level           Warning level (error, warning, note)
    @param IN  code            Warning code
    @param IN  str             Warning parameter
    @param IN  ts_type         Timestamp type (time, date, datetime, none)
    @param IN  cuted_inctement Incrementing of cut field counter
  */
  void set_datetime_warning(Sql_condition::enum_warning_level level, uint code,
                            ErrConvString str,
                            timestamp_type ts_type, int cuted_increment);
public:
  /**
    Constructor for Field_temporal
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param len_arg           Number of characters in the integer part.
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal(uchar *ptr_arg,
                 uchar *null_ptr_arg, uchar null_bit_arg,
                 enum utype unireg_check_arg, const char *field_name_arg,
                 uint32 len_arg, uint8 dec_arg)
    :Field(ptr_arg,
           len_arg + ((dec= normalize_dec(dec_arg)) ? dec + 1 : 0),
           null_ptr_arg, null_bit_arg,
           unireg_check_arg, field_name_arg)
    { flags|= BINARY_FLAG; }
  /**
    Constructor for Field_temporal
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param len_arg           Number of characters in the integer part.
    @param dec_arg           Number of second fraction digits, 0..6
  */
  Field_temporal(bool maybe_null_arg, const char *field_name_arg,
                 uint32 len_arg, uint8 dec_arg)
    :Field((uchar *) 0, 
           len_arg + ((dec= normalize_dec(dec_arg)) ? dec + 1 : 0),
           maybe_null_arg ? (uchar *) "" : 0, 0,
           NONE, field_name_arg)
    { flags|= BINARY_FLAG; }
  virtual Item_result result_type() const { return STRING_RESULT; }
  virtual uint32 max_display_length() { return field_length; }
  virtual bool str_needs_quotes() { return TRUE; }
  virtual uint is_equal(Create_field *new_field);
  Item_result numeric_context_result_type() const
  {
    return dec ? DECIMAL_RESULT : INT_RESULT;
  }
  enum Item_result cmp_type() const { return INT_RESULT; }
  enum Derivation derivation() const { return DERIVATION_NUMERIC; }
  uint repertoire() const { return MY_REPERTOIRE_NUMERIC; }
  const CHARSET_INFO *charset() const { return &my_charset_numeric; }
  bool can_be_compared_as_longlong() const { return true; }
  bool binary() const { return true; }
  type_conversion_status store(const char *str, uint len,
                               const CHARSET_INFO *cs);
  type_conversion_status store_decimal(const my_decimal *decimal);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store(double nr);
  double val_real() // FSP-enable types redefine it.
  {
    return (double) val_int();
  }
  my_decimal *val_decimal(my_decimal *decimal_value); // FSP types redefine it
};


/**
  Abstract class for types with date
  with optional time, with or without fractional part:
  DATE, DATETIME, DATETIME(N), TIMESTAMP, TIMESTAMP(N).
*/
class Field_temporal_with_date :public Field_temporal {
protected:
  /**
    Low level function to get value into MYSQL_TIME,
    without checking for being valid.
  */
  virtual bool get_date_internal(MYSQL_TIME *ltime)= 0;

  /**
    Get value into MYSQL_TIME and check TIME_NO_ZERO_DATE flag.
    @retval   True on error: we get a zero value but flags disallow zero dates.
    @retval   False on success.
  */
  bool get_internal_check_zero(MYSQL_TIME *ltime, uint fuzzydate);
  
  type_conversion_status convert_number_to_TIME(longlong nr, bool unsigned_val,
                                                int nanoseconds,
                                                MYSQL_TIME *ltime,
                                                int *warning);
  bool convert_str_to_TIME(const char *str, uint len, const CHARSET_INFO *cs,
                           MYSQL_TIME *ltime, MYSQL_TIME_STATUS *status);
  type_conversion_status store_internal_with_round(MYSQL_TIME *ltime,
                                                   int *warnings);
public:
  /**
    Constructor for Field_temporal
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param len_arg           Number of characters in the integer part.
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal_with_date(uchar *ptr_arg, uchar *null_ptr_arg,
                           uchar null_bit_arg,
                           enum utype unireg_check_arg,
                           const char *field_name_arg,
                           uint8 int_length_arg, uint8 dec_arg)
    :Field_temporal(ptr_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg,
                    int_length_arg, dec_arg)
    { }
  /**
    Constructor for Field_temporal
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param len_arg           Number of characters in the integer part.
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal_with_date(bool maybe_null_arg, const char *field_name_arg,
                           uint int_length_arg, uint8 dec_arg)
    :Field_temporal((uchar*) 0, maybe_null_arg ? (uchar*) "": 0, 0,
                    NONE, field_name_arg, int_length_arg, dec_arg)
    { }
  bool send_binary(Protocol *protocol);
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  String *val_str(String *, String *);
  longlong val_time_temporal();
  longlong val_date_temporal();
  bool get_time(MYSQL_TIME *ltime)
  {
    return get_date(ltime, TIME_FUZZY_DATE);
  }
  /* Validate the value stored in a field */
  virtual type_conversion_status validate_stored_val(THD *thd);
};


/**
  Abstract class for types with date and time,
  with or without fractional part:
  DATETIME, DATETIME(N), TIMESTAMP, TIMESTAMP(N).
*/
class Field_temporal_with_date_and_time :public Field_temporal_with_date {
private:
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    if (decimals())
    {
      *metadata_ptr= decimals();
      return 1;
    }
    return 0;
  }
protected:
  /**
     Initialize flags for TIMESTAMP DEFAULT CURRENT_TIMESTAMP / ON UPDATE
     CURRENT_TIMESTAMP columns.
     
     @todo get rid of TIMESTAMP_FLAG and ON_UPDATE_NOW_FLAG.
  */
  void init_timestamp_flags();
  /**
    Store "struct timeval" value into field.
    The value must be properly rounded or truncated according
    to the number of fractional second digits.
  */
  virtual void store_timestamp_internal(const struct timeval *tm)= 0;
  bool convert_TIME_to_timestamp(THD *thd, const MYSQL_TIME *ltime,
                                 struct timeval *tm, int *error);

public:
  /**
    Constructor for Field_temporal_with_date_and_time
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal_with_date_and_time(uchar *ptr_arg, uchar *null_ptr_arg,
                                    uchar null_bit_arg,
                                    enum utype unireg_check_arg,
                                    const char *field_name_arg,
                                    uint8 dec_arg)
    :Field_temporal_with_date(ptr_arg, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg,
                              MAX_DATETIME_WIDTH, dec_arg)
    { }
  void store_timestamp(const struct timeval *tm);
};


/**
  Abstract class for types with date and time, with fractional part:
  DATETIME, DATETIME(N), TIMESTAMP, TIMESTAMP(N).
*/
class Field_temporal_with_date_and_timef :
  public Field_temporal_with_date_and_time {
private:
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    *metadata_ptr= decimals();
    return 1;
  }
public:
  /**
    Constructor for Field_temporal_with_date_and_timef
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal_with_date_and_timef(uchar *ptr_arg, uchar *null_ptr_arg,
                                     uchar null_bit_arg,
                                     enum utype unireg_check_arg,
                                     const char *field_name_arg,
                                     uint8 dec_arg)
    :Field_temporal_with_date_and_time(ptr_arg, null_ptr_arg, null_bit_arg,
                                       unireg_check_arg, field_name_arg,
                                       dec_arg)
    { }
  /**
    Constructor for Field_temporal_with_date_and_timef
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_temporal_with_date_and_timef(bool maybe_null_arg,
                                     const char *field_name_arg,
                                     uint8 dec_arg)
    :Field_temporal_with_date_and_time((uchar *) 0,
                                       maybe_null_arg ? (uchar*) "" : 0, 0,
                                       NONE, field_name_arg, dec_arg)
    { }

  uint decimals() const { return dec; }
  const CHARSET_INFO *sort_charset() const { return &my_charset_bin; }
  void make_sort_key(uchar *to, uint length) { memcpy(to, ptr, length); }
  int cmp(const uchar *a_ptr, const uchar *b_ptr)
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
  uint row_pack_length() const { return pack_length(); }
  double val_real();
  longlong val_int();
  my_decimal *val_decimal(my_decimal *decimal_value);
};


/*
  Field implementing TIMESTAMP data type without fractional seconds.
  We will be removed eventually.
*/
class Field_timestamp :public Field_temporal_with_date_and_time {
protected:
  ulonglong date_flags(const THD *thd);
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
  bool get_date_internal(MYSQL_TIME *ltime);
  void store_timestamp_internal(const struct timeval *tm);
public:
  static const int PACK_LENGTH= 4;
  Field_timestamp(uchar *ptr_arg, uint32 len_arg,
                  uchar *null_ptr_arg, uchar null_bit_arg,
		  enum utype unireg_check_arg, const char *field_name_arg);
  Field_timestamp(bool maybe_null_arg, const char *field_name_arg);
  enum_field_types type() const { return MYSQL_TYPE_TIMESTAMP;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_ULONG_INT; }
  type_conversion_status store_packed(longlong nr);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=0;
    return TYPE_OK;
  }
  longlong val_int(void);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return PACK_LENGTH; }
  void sql_type(String &str) const;
  bool zero_pack() const { return 0; }
  /* Get TIMESTAMP field value as seconds since begging of Unix Epoch */
  bool get_timestamp(struct timeval *tm, int *warnings);
  bool get_date(MYSQL_TIME *ltime,uint fuzzydate);
  Field_timestamp *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIMESTAMP);
    return new (mem_root) Field_timestamp(*this);
  }
  Field_timestamp *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIMESTAMP);
    return new Field_timestamp(*this);
  }
  uchar *pack(uchar *to, const uchar *from,
              uint max_length MY_ATTRIBUTE((unused)), bool low_byte_first)
  {
    return pack_int32(to, from, low_byte_first);
  }
  const uchar *unpack(uchar* to, const uchar *from,
                      uint param_data MY_ATTRIBUTE((unused)),
                      bool low_byte_first)
  {
    return unpack_int32(to, from, low_byte_first);
  }
  /* Validate the value stored in a field */
  virtual type_conversion_status validate_stored_val(THD *thd);
};


/*
  Field implementing TIMESTAMP(N) data type, where N=0..6.
*/
class Field_timestampf :public Field_temporal_with_date_and_timef {
protected:
  bool get_date_internal(MYSQL_TIME *ltime);
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
  ulonglong date_flags(const THD *thd);
  void store_timestamp_internal(const struct timeval *tm);
public:
  static const int PACK_LENGTH= 8;
  /**
    Field_timestampf constructor
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param share             Table share.
    @param dec_arg           Number of fractional second digits, 0..6.
  */
  Field_timestampf(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                   enum utype unireg_check_arg, const char *field_name_arg,
                   uint8 dec_arg);
  /**
    Field_timestampf constructor
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of fractional second digits, 0..6.
  */
  Field_timestampf(bool maybe_null_arg, const char *field_name_arg,
                   uint8 dec_arg);
  Field_timestampf *clone(MEM_ROOT *mem_root) const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIMESTAMP);
    return new (mem_root) Field_timestampf(*this);
  }
  Field_timestampf *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIMESTAMP);
    return new Field_timestampf(*this);
  }

  enum_field_types type() const { return MYSQL_TYPE_TIMESTAMP; }
  enum_field_types real_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_TIMESTAMP2; }
  bool zero_pack() const { return 0; }

  uint32 pack_length() const
  {
    return my_timestamp_binary_length(dec);
  }
  virtual uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_timestampf::pack_length_from_metadata");
    uint tmp= my_timestamp_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }

  type_conversion_status reset();
  type_conversion_status store_packed(longlong nr);
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  void sql_type(String &str) const;

  bool get_timestamp(struct timeval *tm, int *warnings);
  /* Validate the value stored in a field */
  virtual type_conversion_status validate_stored_val(THD *thd);
};


class Field_year :public Field_tiny {
public:
  Field_year(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	     uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg)
    :Field_tiny(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
		unireg_check_arg, field_name_arg, 1, 1)
    {}
  enum_field_types type() const { return MYSQL_TYPE_YEAR;}
  type_conversion_status store(const char *to,uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  void sql_type(String &str) const;
  bool can_be_compared_as_longlong() const { return true; }
  Field_year *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_YEAR);
    return new (mem_root) Field_year(*this);
  }
  Field_year *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_YEAR);
    return new Field_year(*this);
  }
};


class Field_newdate :public Field_temporal_with_date {
protected:
  static const int PACK_LENGTH= 3;
  ulonglong date_flags(const THD *thd);
  bool get_date_internal(MYSQL_TIME *ltime);
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);

public:
  Field_newdate(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                enum utype unireg_check_arg, const char *field_name_arg)
    :Field_temporal_with_date(ptr_arg, null_ptr_arg, null_bit_arg,
                              unireg_check_arg, field_name_arg,
                              MAX_DATE_WIDTH, 0)
    { }
  Field_newdate(bool maybe_null_arg, const char *field_name_arg)
    :Field_temporal_with_date((uchar *) 0, maybe_null_arg ? (uchar *) "" : 0,
                              0, NONE, field_name_arg, MAX_DATE_WIDTH, 0)
    { }
  enum_field_types type() const { return MYSQL_TYPE_DATE;}
  enum_field_types real_type() const { return MYSQL_TYPE_NEWDATE; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_UINT24; }
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=0;
    return TYPE_OK;
  }
  type_conversion_status store_packed(longlong nr);
  longlong val_int(void);
  longlong val_time_temporal();
  longlong val_date_temporal();
  String *val_str(String*,String *);
  bool send_binary(Protocol *protocol);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return PACK_LENGTH; }
  void sql_type(String &str) const;
  bool zero_pack() const { return 1; }
  bool get_date(MYSQL_TIME *ltime,uint fuzzydate);
  Field_newdate *clone(MEM_ROOT *mem_root) const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATE);
    DBUG_ASSERT(real_type() == MYSQL_TYPE_NEWDATE);
    return new (mem_root) Field_newdate(*this);
  }
  Field_newdate *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATE);
    DBUG_ASSERT(real_type() == MYSQL_TYPE_NEWDATE);
    return new Field_newdate(*this);
  }
};


/**
  Abstract class for TIME and TIME(N).
*/
class Field_time_common :public Field_temporal {
protected:
  bool convert_str_to_TIME(const char *str, uint len, const CHARSET_INFO *cs,
                           MYSQL_TIME *ltime, MYSQL_TIME_STATUS *status);
  /**
    @todo: convert_number_to_TIME returns conversion status through
    two different interfaces: return value and warning. It should be
    refactored to only use return value.
   */
  type_conversion_status convert_number_to_TIME(longlong nr, bool unsigned_val,
                                                int nanoseconds,
                                                MYSQL_TIME *ltime,
                                                int *warning);
  /**
    Low-level function to store MYSQL_TIME value.
    The value must be rounded or truncated according to decimals().
  */
  virtual type_conversion_status store_internal(const MYSQL_TIME *ltime,
                                                int *error)= 0;
  /**
    Function to store time value.
    The value is rounded according to decimals().
  */
  virtual type_conversion_status store_internal_with_round(MYSQL_TIME *ltime,
                                                           int *warnings);
public:
  /**
    Constructor for Field_time_common
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_time_common(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                    enum utype unireg_check_arg, const char *field_name_arg,
                    uint8 dec_arg)
    :Field_temporal(ptr_arg, null_ptr_arg, null_bit_arg,
                    unireg_check_arg, field_name_arg,
                    MAX_TIME_WIDTH, dec_arg)
    { }
  /**
    Constructor for Field_time_common
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_time_common(bool maybe_null_arg, const char *field_name_arg,
                    uint8 dec_arg)
    :Field_temporal((uchar *) 0, maybe_null_arg ? (uchar *) "" : 0, 0,
                    NONE, field_name_arg, MAX_TIME_WIDTH, dec_arg)
    { }
  type_conversion_status store_time(MYSQL_TIME *ltime, uint8 dec);
  String *val_str(String*, String *);
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  longlong val_date_temporal();
  bool send_binary(Protocol *protocol);
};


/*
  Field implementing TIME data type without fractional seconds.
  We will be removed eventually.
*/
class Field_time :public Field_time_common {
protected:
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
public:
  Field_time(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg)
    :Field_time_common(ptr_arg, null_ptr_arg, null_bit_arg,
                       unireg_check_arg, field_name_arg, 0)
    { }
  Field_time(bool maybe_null_arg, const char *field_name_arg)
    :Field_time_common((uchar *) 0, maybe_null_arg ? (uchar *) "" : 0, 0,
                       NONE, field_name_arg, 0)
    { }
  enum_field_types type() const { return MYSQL_TYPE_TIME;}
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_INT24; }
  type_conversion_status store_packed(longlong nr);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=0;
    return TYPE_OK;
  }
  longlong val_int(void);
  longlong val_time_temporal();
  bool get_time(MYSQL_TIME *ltime);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return 3; }
  void sql_type(String &str) const;
  bool zero_pack() const { return 1; }
  Field_time *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIME);
    return new (mem_root) Field_time(*this);
  }
  Field_time *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIME);
    return new Field_time(*this);
  }
};


/*
  Field implementing TIME(N) data type, where N=0..6.
*/
class Field_timef :public Field_time_common {
private:
  int do_save_field_metadata(uchar *metadata_ptr)
  {
    *metadata_ptr= decimals();
    return 1;
  }
protected:
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
public:
  /**
    Constructor for Field_timef
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_timef(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
              enum utype unireg_check_arg, const char *field_name_arg,
              uint8 dec_arg)
    :Field_time_common(ptr_arg, null_ptr_arg, null_bit_arg,
                       unireg_check_arg, field_name_arg, dec_arg)
  { }
  /**
    Constructor for Field_timef
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_timef(bool maybe_null_arg, const char *field_name_arg, uint8 dec_arg)
    :Field_time_common((uchar *) 0, maybe_null_arg ? (uchar *) "" : 0, 0,
                       NONE, field_name_arg, dec_arg)
  { }
  Field_timef *clone(MEM_ROOT *mem_root) const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIME);
    return new (mem_root) Field_timef(*this);
  }
  Field_timef *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_TIME);
    return new Field_timef(*this);
  }
  uint decimals() const { return dec; }
  enum_field_types type() const { return MYSQL_TYPE_TIME;}
  enum_field_types real_type() const { return MYSQL_TYPE_TIME2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_TIME2; }
  type_conversion_status store_packed(longlong nr);
  type_conversion_status reset();
  double val_real();
  longlong val_int();
  longlong val_time_temporal();
  bool get_time(MYSQL_TIME *ltime);
  my_decimal *val_decimal(my_decimal *);
  uint32 pack_length() const
  {
    return my_time_binary_length(dec);
  }
  virtual uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_timef::pack_length_from_metadata");
    uint tmp= my_time_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  uint row_pack_length() const { return pack_length(); }
  void sql_type(String &str) const;
  bool zero_pack() const { return 1; }
  const CHARSET_INFO *sort_charset(void) const { return &my_charset_bin; }
  void make_sort_key(uchar *to, uint length) { memcpy(to, ptr, length); }
  int cmp(const uchar *a_ptr, const uchar *b_ptr)
  {
    return memcmp(a_ptr, b_ptr, pack_length());
  }
};


/*
  Field implementing DATETIME data type without fractional seconds.
  We will be removed eventually.
*/
class Field_datetime :public Field_temporal_with_date_and_time {
protected:
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
  bool get_date_internal(MYSQL_TIME *ltime);
  ulonglong date_flags(const THD *thd);
  void store_timestamp_internal(const struct timeval *tm);

public:
  static const int PACK_LENGTH= 8;

  /**
     DATETIME columns can be defined as having CURRENT_TIMESTAMP as the
     default value on inserts or updates. This constructor accepts a
     unireg_check value to initialize the column default expressions.

     The implementation of function defaults is heavily entangled with the
     binary .frm file format. The @c utype @c enum is part of the file format
     specification but is declared a member of the Field class.

     Four distinct unireg_check values are used for DATETIME columns to
     distinguish various cases of DEFAULT or ON UPDATE values. These values are:

     - TIMESTAMP_DN_FIELD - means DATETIME DEFAULT CURRENT_TIMESTAMP.

     - TIMESTAMP_UN_FIELD - means DATETIME DEFAULT <default value> ON UPDATE
     CURRENT_TIMESTAMP, where <default value> is an implicit or explicit
     expression other than CURRENT_TIMESTAMP or any synonym thereof
     (e.g. NOW().)

     - TIMESTAMP_DNUN_FIELD - means DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE
     CURRENT_TIMESTAMP.

     - NONE - means that the column has neither DEFAULT CURRENT_TIMESTAMP, nor
     ON UPDATE CURRENT_TIMESTAMP
   */
  Field_datetime(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                 enum utype unireg_check_arg, const char *field_name_arg)
    :Field_temporal_with_date_and_time(ptr_arg, null_ptr_arg, null_bit_arg,
                                       unireg_check_arg, field_name_arg, 0)
    {}
  Field_datetime(bool maybe_null_arg, const char *field_name_arg)
    :Field_temporal_with_date_and_time((uchar *) 0,
                                       maybe_null_arg ? (uchar *) "" : 0,
                                       0, NONE, field_name_arg, 0)
    {}
  enum_field_types type() const { return MYSQL_TYPE_DATETIME;}
#ifdef HAVE_LONG_LONG
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_ULONGLONG; }
#endif
  using Field_temporal_with_date_and_time::store; // Make -Woverloaded-virtual
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_packed(longlong nr);
  type_conversion_status reset(void)
  {
    ptr[0]=ptr[1]=ptr[2]=ptr[3]=ptr[4]=ptr[5]=ptr[6]=ptr[7]=0;
    return TYPE_OK;
  }
  longlong val_int(void);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return PACK_LENGTH; }
  void sql_type(String &str) const;
  bool zero_pack() const { return 1; }
  bool get_date(MYSQL_TIME *ltime,uint fuzzydate);
  Field_datetime *clone(MEM_ROOT *mem_root) const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATETIME);
    return new (mem_root) Field_datetime(*this);
  }
  Field_datetime *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATETIME);
    return new Field_datetime(*this);
  }
  uchar *pack(uchar* to, const uchar *from,
              uint max_length MY_ATTRIBUTE((unused)), bool low_byte_first)
  {
    return pack_int64(to, from, low_byte_first);
  }
  const uchar *unpack(uchar* to, const uchar *from,
                      uint param_data MY_ATTRIBUTE((unused)),
                      bool low_byte_first)
  {
    return unpack_int64(to, from, low_byte_first);
  }
};


/*
  Field implementing DATETIME(N) data type, where N=0..6.
*/
class Field_datetimef :public Field_temporal_with_date_and_timef {
protected:
  bool get_date_internal(MYSQL_TIME *ltime);
  type_conversion_status store_internal(const MYSQL_TIME *ltime, int *error);
  ulonglong date_flags(const THD *thd);
  void store_timestamp_internal(const struct timeval *tm);

public:
  /**
    Constructor for Field_datetimef
    @param ptr_arg           See Field definition
    @param null_ptr_arg      See Field definition
    @param null_bit_arg      See Field definition
    @param unireg_check_arg  See Field definition
    @param field_name_arg    See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_datetimef(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
                 enum utype unireg_check_arg, const char *field_name_arg,
                 uint8 dec_arg)
    :Field_temporal_with_date_and_timef(ptr_arg, null_ptr_arg, null_bit_arg,
                                        unireg_check_arg, field_name_arg,
                                        dec_arg)
    {}
  /**
    Constructor for Field_datetimef
    @param maybe_null_arg    See Field definition
    @param field_name_arg    See Field definition
    @param len_arg           See Field definition
    @param dec_arg           Number of second fraction digits, 0..6.
  */
  Field_datetimef(bool maybe_null_arg, const char *field_name_arg,
                  uint8 dec_arg)
    :Field_temporal_with_date_and_timef((uchar *) 0,
                                        maybe_null_arg ? (uchar *) "" : 0, 0,
                                        NONE, field_name_arg, dec_arg)
    {}
  Field_datetimef *clone(MEM_ROOT *mem_root) const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATETIME);
    return new (mem_root) Field_datetimef(*this);
  }
  Field_datetimef *clone() const
  {
    DBUG_ASSERT(type() == MYSQL_TYPE_DATETIME);
    return new Field_datetimef(*this);
  }

  enum_field_types type() const { return MYSQL_TYPE_DATETIME;}
  enum_field_types real_type() const { return MYSQL_TYPE_DATETIME2; }
  enum_field_types binlog_type() const { return MYSQL_TYPE_DATETIME2; }
  uint32 pack_length() const
  {
    return my_datetime_binary_length(dec);
  }
  virtual uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_ENTER("Field_datetimef::pack_length_from_metadata");
    uint tmp= my_datetime_binary_length(field_metadata);
    DBUG_RETURN(tmp);
  }
  bool zero_pack() const { return 1; }

  type_conversion_status store_packed(longlong nr);
  type_conversion_status reset();
  longlong val_date_temporal();
  bool get_date(MYSQL_TIME *ltime, uint fuzzydate);
  void sql_type(String &str) const;
};


class Field_string :public Field_longstr {
 public:
  bool can_alter_field_type;
  Field_string(uchar *ptr_arg, uint32 len_arg,uchar *null_ptr_arg,
	       uchar null_bit_arg,
	       enum utype unireg_check_arg, const char *field_name_arg,
	       const CHARSET_INFO *cs)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, cs),
     can_alter_field_type(1) {};
  Field_string(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
               const CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
     can_alter_field_type(1) {};

  enum_field_types type() const
  {
    return ((can_alter_field_type && orig_table &&
             orig_table->s->db_create_options & HA_OPTION_PACK_RECORD &&
	     field_length >= 4) &&
            orig_table->s->frm_version < FRM_VER_TRUE_VARCHAR ?
	    MYSQL_TYPE_VAR_STRING : MYSQL_TYPE_STRING);
  }
  bool match_collation_to_optimize_range() const { return true; }
  enum ha_base_keytype key_type() const
    { return binary() ? HA_KEYTYPE_BINARY : HA_KEYTYPE_TEXT; }
  bool zero_pack() const { return 0; }
  type_conversion_status reset(void)
  {
    charset()->cset->fill(charset(),(char*) ptr, field_length,
                          (has_charset() ? ' ' : 0));
    return TYPE_OK;
  }
  type_conversion_status store(const char *to,uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(longlong nr, bool unsigned_val);
  /* QQ: To be deleted */
  type_conversion_status store(double nr) { return Field_str::store(nr); }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  void sql_type(String &str) const;
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first);
  uint pack_length_from_metadata(uint field_metadata)
  {
    DBUG_PRINT("debug", ("field_metadata: 0x%04x", field_metadata));
    if (field_metadata == 0)
      return row_pack_length();
    return (((field_metadata >> 4) & 0x300) ^ 0x300) + (field_metadata & 0x00ff);
  }
  bool compatible_field_size(uint field_metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  uint row_pack_length() const { return field_length; }
  int pack_cmp(const uchar *a,const uchar *b,uint key_length,
               my_bool insert_or_update);
  int pack_cmp(const uchar *b,uint key_length,my_bool insert_or_update);
  uint packed_col_length(const uchar *to, uint length);
  uint max_packed_col_length(uint max_length);
  enum_field_types real_type() const { return MYSQL_TYPE_STRING; }
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  Field *new_field(MEM_ROOT *root,
                   TABLE *new_table,
                   bool keep_type,
                   bool from_alias = false);
  Field_string *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(real_type() == MYSQL_TYPE_STRING);
    return new (mem_root) Field_string(*this);
  }
  Field_string *clone() const {
    DBUG_ASSERT(real_type() == MYSQL_TYPE_STRING);
    return new Field_string(*this);
  }
  virtual uint get_key_image(uchar *buff,uint length, imagetype type);
private:
  int do_save_field_metadata(uchar *first_byte);
};


class Field_varstring :public Field_longstr {
public:
  /*
    If this field is created for a document path key with string type.
  */
  bool document_key_field = false;
  /*
    The maximum space available in a Field_varstring, in bytes. See
    length_bytes.
  */
  static const uint MAX_SIZE;
  /* Store number of bytes used to store length (1 or 2) */
  uint32 length_bytes;

  Field_varstring(uchar *ptr_arg,
                  uint32 len_arg, uint length_bytes_arg,
                  uchar *null_ptr_arg, uchar null_bit_arg,
		  enum utype unireg_check_arg, const char *field_name_arg,
		  TABLE_SHARE *share, const CHARSET_INFO *cs)
    :Field_longstr(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
                   unireg_check_arg, field_name_arg, cs),
     length_bytes(length_bytes_arg)
  {
    share->varchar_fields++;
  }
  Field_varstring(uint32 len_arg,bool maybe_null_arg,
                  const char *field_name_arg,
                  TABLE_SHARE *share, const CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
     length_bytes(len_arg < 256 ? 1 :2)
  {
    share->varchar_fields++;
  }

  enum_field_types type() const { return MYSQL_TYPE_VARCHAR; }
  bool match_collation_to_optimize_range() const { return true; }
  enum ha_base_keytype key_type() const;
  uint row_pack_length() const { return field_length; }
  bool zero_pack() const { return 0; }
  type_conversion_status reset(void)
  {
    memset(ptr, 0, field_length+length_bytes);
    return TYPE_OK;
  }
  uint32 pack_length() const { return (uint32) field_length+length_bytes; }
  uint32 key_length() const { return (uint32) field_length; }
  uint32 sort_length() const
  {
    return (uint32) field_length + (field_charset == &my_charset_bin ?
                                    length_bytes : 0);
  }
  type_conversion_status store(const char *to,uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(longlong nr, bool unsigned_val);
  /* QQ: To be deleted */
  type_conversion_status store(double nr) { return Field_str::store(nr); }
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp_max(const uchar *, const uchar *, uint max_length);
  int cmp(const uchar *a,const uchar *b)
  {
    return cmp_max(a, b, ~0L);
  }
  void make_sort_key(uchar *buff, uint length);
  uint get_key_image(uchar *buff,uint length, imagetype type);
  void set_key_image(const uchar *buff,uint length);
  void sql_type(String &str) const;
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  virtual const uchar *unpack(uchar* to, const uchar *from,
                              uint param_data, bool low_byte_first);
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0L);
  int key_cmp(const uchar *,const uchar*);
  int key_cmp(const uchar *str, uint length);
  uint packed_col_length(const uchar *to, uint length);
  uint max_packed_col_length(uint max_length);
  uint32 data_length();
  enum_field_types real_type() const { return MYSQL_TYPE_VARCHAR; }
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  Field *new_field(MEM_ROOT *root,
                   TABLE *new_table,
                   bool keep_type,
                   bool from_alias = false);
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uchar *new_null_ptr,
                       uint new_null_bit);
  Field_varstring *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_VARCHAR);
    DBUG_ASSERT(real_type() == MYSQL_TYPE_VARCHAR);
    return new (mem_root) Field_varstring(*this);
  }
  Field_varstring *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_VARCHAR);
    DBUG_ASSERT(real_type() == MYSQL_TYPE_VARCHAR);
    return new Field_varstring(*this);
  }
  uint is_equal(Create_field *new_field);
  void hash(ulong *nr, ulong *nr2);
private:
  int do_save_field_metadata(uchar *first_byte);
};


class Field_blob :public Field_longstr {
  /**
    Copy value to memory storage.
  */
  type_conversion_status store_to_mem(const char *from, uint length,
                                      const CHARSET_INFO *cs,
                                      uint max_length,
                                      Blob_mem_storage *blob_storage);
protected:
  virtual type_conversion_status store_internal(const char *from, uint length,
                                                const CHARSET_INFO *cs);
  /**
    The number of bytes used to represent the length of the blob.
  */
  uint packlength;
  
  /**
    The 'value'-object is a cache fronting the storage engine.
  */
  String value;

  /**
    Backup String for table's blob fields.
  */
  String m_blob_backup;

  /**
    Store ptr and length.
  */
  void store_ptr_and_length(const char *from, uint32 length)
  {
    store_length(length);
    bmove(ptr + packlength, &from, sizeof(char *));
  }
  
public:
  Field_blob(uchar *ptr_arg, uchar *null_ptr_arg, uchar null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length, const CHARSET_INFO *cs);
  Field_blob(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
             const CHARSET_INFO *cs)
    :Field_longstr((uchar*) 0, len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs),
    packlength(4)
  {
    flags|= BLOB_FLAG;
  }
  Field_blob(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	     const CHARSET_INFO *cs, bool set_packlength)
    :Field_longstr((uchar*) 0,len_arg, maybe_null_arg ? (uchar*) "": 0, 0,
                   NONE, field_name_arg, cs)
  {
    flags|= BLOB_FLAG;
    packlength= 4;
    if (set_packlength)
    {
      uint32 l_char_length= len_arg/cs->mbmaxlen;
      packlength= l_char_length <= 255 ? 1 :
                  l_char_length <= 65535 ? 2 :
                  l_char_length <= 16777215 ? 3 : 4;
    }
  }
  Field_blob(uint32 packlength_arg)
    :Field_longstr((uchar*) 0, 0, (uchar*) "", 0, NONE, "temp", system_charset_info),
    packlength(packlength_arg) {}
  /* Note that the default copy constructor is used, in clone() */
  enum_field_types type() const { return MYSQL_TYPE_BLOB;}
  bool match_collation_to_optimize_range() const { return true; }
  enum ha_base_keytype key_type() const
    { return binary() ? HA_KEYTYPE_VARBINARY2 : HA_KEYTYPE_VARTEXT2; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*,String *);
  my_decimal *val_decimal(my_decimal *);
  int cmp_max(const uchar *, const uchar *, uint max_length);
  int cmp(const uchar *a,const uchar *b)
    { return cmp_max(a, b, ~0L); }
  int cmp(const uchar *a, uint32 a_length, const uchar *b, uint32 b_length);
  int cmp_binary(const uchar *a,const uchar *b, uint32 max_length=~0L);
  int key_cmp(const uchar *,const uchar*);
  int key_cmp(const uchar *str, uint length);
  uint32 key_length() const { return 0; }
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const
  { return (uint32) (packlength + portable_sizeof_char_ptr); }

  /**
     Return the packed length without the pointer size added. 

     This is used to determine the size of the actual data in the row
     buffer.

     @returns The length of the raw data itself without the pointer.
  */
  uint32 pack_length_no_ptr() const
  { return (uint32) (packlength); }
  uint row_pack_length() const { return pack_length_no_ptr(); }
  uint32 sort_length() const;
  virtual uint32 max_data_length() const
  {
    return (uint32) (((ulonglong) 1 << (packlength*8)) -1);
  }
  type_conversion_status reset(void)
  {
    memset(ptr, 0, packlength+sizeof(uchar*));
    return TYPE_OK;
  }
  void reset_fields() { memset(&value, 0, sizeof(value)); }
  uint32 get_field_buffer_size(void) { return value.alloced_length(); }
#ifndef WORDS_BIGENDIAN
  static
#endif
  void store_length(uchar *i_ptr, uint i_packlength, uint32 i_number, bool low_byte_first);
  void store_length(uchar *i_ptr, uint i_packlength, uint32 i_number)
  {
    store_length(i_ptr, i_packlength, i_number, table->s->db_low_byte_first);
  }
  inline void store_length(uint32 number)
  {
    store_length(ptr, packlength, number);
  }
  inline uint32 get_length(uint row_offset= 0)
  { return get_length(ptr+row_offset, this->packlength, table->s->db_low_byte_first); }
  static uint32 get_length(const uchar *ptr, uint packlength,
                           bool low_byte_first);
  uint32 get_length(const uchar *ptr_arg)
  { return get_length(ptr_arg, this->packlength, table->s->db_low_byte_first); }
  void put_length(uchar *pos, uint32 length);
  inline void get_ptr(uchar **str)
    {
      memcpy(str, ptr+packlength, sizeof(uchar*));
    }
  inline void get_ptr(uchar **str, uint row_offset)
    {
      memcpy(str, ptr+packlength+row_offset, sizeof(char*));
    }
  inline void set_ptr(uchar *length, uchar *data)
    {
      memcpy(ptr,length,packlength);
      memcpy(ptr+packlength, &data,sizeof(char*));
    }
  void set_ptr_offset(my_ptrdiff_t ptr_diff, uint32 length, const uchar *data)
    {
      uchar *ptr_ofs= ADD_TO_PTR(ptr,ptr_diff,uchar*);
      store_length(ptr_ofs, packlength, length);
      memcpy(ptr_ofs+packlength, &data, sizeof(char*));
    }
  inline void set_ptr(uint32 length, uchar *data)
    {
      set_ptr_offset(0, length, data);
    }
  uint get_key_image(uchar *buff,uint length, imagetype type);
  void set_key_image(const uchar *buff,uint length);
  void sql_type(String &str) const;
  inline bool copy()
  {
    uchar *tmp;
    get_ptr(&tmp);
    if (value.copy((char*) tmp, get_length(), charset()))
    {
      Field_blob::reset();
      return 1;
    }
    tmp=(uchar*) value.ptr();
    memcpy(ptr+packlength, &tmp, sizeof(char*));
    return 0;
  }
  Field_blob *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_BLOB);
    return new (mem_root) Field_blob(*this);
  }
  Field_blob *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_BLOB);
    return new Field_blob(*this);
  }
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              uint param_data, bool low_byte_first);
  uint packed_col_length(const uchar *col_ptr, uint length);
  uint max_packed_col_length(uint max_length);
  void free() {
    value.free();
    m_blob_backup.free();
  }
  inline void clear_temporary() { memset(&value, 0, sizeof(value)); }
  friend type_conversion_status field_conv(Field *to,Field *from);
  bool has_charset(void) const
  { return charset() == &my_charset_bin ? FALSE : TRUE; }
  uint32 max_display_length();
  uint32 char_length() const;
  uint is_equal(Create_field *new_field);
  inline bool in_read_set() { return bitmap_is_set(table->read_set, field_index); }
  inline bool in_write_set() { return bitmap_is_set(table->write_set, field_index); }

  /**
    Backup data stored in 'value' into the m_blob_backup
  */
  bool backup_blob_field() {
    value.swap(m_blob_backup);
    return false;
  }

private:
  int do_save_field_metadata(uchar *first_byte);
};


#ifdef HAVE_SPATIAL
class Field_geom :public Field_blob {
  virtual type_conversion_status store_internal(const char *from, uint length,
                                                const CHARSET_INFO *cs);
public:
  enum geometry_type geom_type;

  Field_geom(uchar *ptr_arg, uchar *null_ptr_arg, uint null_bit_arg,
	     enum utype unireg_check_arg, const char *field_name_arg,
	     TABLE_SHARE *share, uint blob_pack_length,
	     enum geometry_type geom_type_arg)
     :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg, 
                 field_name_arg, share, blob_pack_length, &my_charset_bin)
  { geom_type= geom_type_arg; }
  Field_geom(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
	     TABLE_SHARE *share, enum geometry_type geom_type_arg)
    :Field_blob(len_arg, maybe_null_arg, field_name_arg, &my_charset_bin)
  { geom_type= geom_type_arg; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_VARBINARY2; }
  enum_field_types type() const { return MYSQL_TYPE_GEOMETRY; }
  bool match_collation_to_optimize_range() const { return false; }
  void sql_type(String &str) const;
  using Field_blob::store;
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_decimal(const my_decimal *);

  /**
    Non-nullable GEOMETRY types cannot have defaults,
    but the underlying blob must still be reset.
   */
  type_conversion_status reset(void)
  {
    type_conversion_status res= Field_blob::reset();
    if (res != TYPE_OK)
      return res;
    return maybe_null() ? TYPE_OK : TYPE_ERR_NULL_CONSTRAINT_VIOLATION;
  }

  geometry_type get_geometry_type() { return geom_type; };
  Field_geom *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(type() == MYSQL_TYPE_GEOMETRY);
    return new (mem_root) Field_geom(*this);
  }
  Field_geom *clone() const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_GEOMETRY);
    return new Field_geom(*this);
  }
  uint is_equal(Create_field *new_field);
};
#endif /*HAVE_SPATIAL*/

class Document_path_iterator;

class Field_document :public Field_blob {
  bool validate(const char *from, uint length, const CHARSET_INFO *cs);
  void push_error_invalid(const char *from,
                          const fbson::FbsonErrInfo *err_info = nullptr);
  void push_error_too_big();
  virtual type_conversion_status store_internal(const char *from, uint length,
                                                const CHARSET_INFO *cs);

  type_conversion_status update_json(const fbson::FbsonValue *val,
                                     const CHARSET_INFO *cs,
                                     Document_path_iterator& key_path,
                                     Field_document *from);
  type_conversion_status prepare_update(const CHARSET_INFO *cs,
                                        char **buff);
protected:
  const Field_document *get_inner_field() const {
    return const_cast<Field_document*>(this)->get_inner_field(); }
  uint key_len; // The length of the key defined in the index
public:
  friend class Document_path_iterator;
  enum document_type doc_type;
  /*
    Used to indicate the number of the prefix of document path in
    this field. Because we forbid storing something not array or
    object in root and sometimes, in temptable, when the column doesn't
    contain the document_path and also with alias doesn't contain
    prefix_document_path, we have to use this variable to know whether
    store something in the root is valid.

    Here is some examples:
    1. select addr from (select doc.addr as addr from t1) as tmp;
       In this query, for the field in the temp table：
           prefix_path_num = 1
           document_key_path = <>
           prefix_document_key_path = <>
       and the field in the select will be exactly the one in the
       temp table. When alias was set, prefix_path_num is not consistent
       with the number of elements in prefix_document_key_path.
    2. select doc.addr from (select doc.addr from t1) as tmp;
       In this query, for the field in the temp table：
           prefix_path_num = 1
           document_key_path = <>
           prefix_document_key_path = <addr>
       and the field in the select will be exactly the one in the
       temp table. Without alias, prefix_path_num is consistent with
       the number of elements in prefix_document_key_path.
    3. select doc.addr.state from (select doc.addr from t1) as tmp;
       In this query, for the field in the temp table:
           prefix_path_num = 1
           document_key_path = <>
           prefix_document_key_path = <addr>
       and the field in the select will be
           prefix_path_num = 1
           document_key_path = <state>
           prefix_document_key_path = <addr>
     4. select addr.zipcode from (select doc.addr as addr from t1) as tmp
       In this query, for the field in the temp table:
           prefix_path_num = 1
           document_key_path = <>
           prefix_document_key_path = <>
       and the field in the select will be
           prefix_path_num = 1
           document_key_path = <zipcode>
           prefix_document_key_path = <>
     When the prefix_path_num = 0, it means that it's a field defined
     in the original table. And for the document column, we only allow
     Object or Array to be stored in it.
     But if it stands for the field in the document column, we can store
     anything valid in Fbson in it.
  */
  int prefix_path_num;
  uint doc_key_prefix_len;// stores the length of prefix index
  Save_in_field_args *update_args; // store the partial update arguments

  /* array to hold pointers to document path keys.
   * The purpose is similar to the key_start map, but for document keys
   * we need to specifically look at the key part, as there could be
   * document path key part could be different.
   */
  DOCUMENT_PATH_KEY_PART_INFO* document_path_key_start[MAX_KEY];

  Field_document(uchar *ptr_arg, uchar *null_ptr_arg, uint null_bit_arg,
                 enum utype unireg_check_arg, const char *field_name_arg,
                 TABLE_SHARE *share, uint blob_pack_length)
      :Field_blob(ptr_arg, null_ptr_arg, null_bit_arg, unireg_check_arg,
                  field_name_arg, share, blob_pack_length, &my_charset_bin),
       key_len(0), doc_type(DOC_DOCUMENT), prefix_path_num(0),
       doc_key_prefix_len(0), update_args(nullptr), inner_field(nullptr)
    {
      init();
    }
  Field_document(uint32 len_arg,bool maybe_null_arg, const char *field_name_arg,
                 TABLE_SHARE *share)
      :Field_blob(len_arg, maybe_null_arg, field_name_arg, &my_charset_bin),
       key_len(0), doc_type(DOC_DOCUMENT), prefix_path_num(0),
       doc_key_prefix_len(0), update_args(nullptr), inner_field(nullptr)
    {
      init();
    }
  /*
    Create a Field_document by a document path.

    @param mem_root
    @param document The field where this Field_document will reference to.
    @param doc_path The document path that will will be uesed to access the
                    data in wrapped document.
    @param type The type of the Field_document if defined.
    @param key_len The length of the key defined in the index.
  */
  Field_document(MEM_ROOT *mem_root,
                 Field_document *document,
                 List<Document_key> &doc_path,
                 enum_field_types type,
                 uint key_length);

  /*
    Create a Field_document by a document path, but skip the first skip_num
    nodes in doc_path. Because, the document path defined in document,
    can be a prefix of doc_path in the parameter.
    skip_num denotes where is offset of the doc_path that is not part of
    the one defined in document.
  */
  Field_document(MEM_ROOT *mem_root,
                 Field_document *document,
                 List<Document_key> &doc_path,
                 int skip_num);

  /*
    Check whether this field is created based on the other field, such as
    a document path.
  */
  bool is_derived() const
  {
    return real_field() != this;
  }

  /*
    When we create a key_field, for document path it's not always the
    same type as the document. Because in document path we can have
    any type valid in JSON and in the index we also need to specify the
    type of the field. So we need to create the right field for the key
    according to the type defined in the index.
  */
  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uchar *new_null_ptr,
                       uint new_null_bit)
  {
    DBUG_ASSERT(validate_doc_type());

    Field *field = nullptr;
    switch(doc_type)
    {
      case DOC_PATH_DOUBLE:
        field = new (root) Field_double(new_ptr,
                                        key_len,
                                        new_null_ptr,
                                        new_null_bit,
                                        unireg_check,
                                        field_name,
                                        1,
                                        true,
                                        true);
        break;
      case DOC_PATH_STRING:
      {
        /* Key segments are always packed with a 2-byte length
           (HA_KEY_BLOB_LENGTH) prefix. See mi_rkey for details.
        */
        field= new (root) Field_varstring(new_ptr, key_len,
                                          HA_KEY_BLOB_LENGTH,
                                          new_null_ptr,
                                          new_null_bit,
                                          Field::NONE,
                                          field_name,
                                          new_table->s,
                                          charset());
        ((Field_varstring*)field)->document_key_field = true;
        break;
      }
      case DOC_PATH_INT:
        field = new (root) Field_longlong(new_ptr,
                                          key_len,
                                          new_null_ptr,
                                          new_null_bit,
                                          unireg_check,
                                          field_name,
                                          true,
                                          false);
        break;
      case DOC_PATH_TINY:
        field = new (root) Field_tiny(new_ptr,
                                      key_len,
                                      new_null_ptr,
                                      new_null_bit,
                                      unireg_check,
                                      field_name,
                                      true,
                                      false);
        break;
      default:
        DBUG_ASSERT(false);
        break;
    }
    // We need to set the table.
    field->init(new_table);
    return field;
  }
  bool check_field_name_match(const char *name)
  {
    return ::check_name_match(name, field_name);
  }
  void set_null(my_ptrdiff_t row_offset = 0);
  void set_notnull(my_ptrdiff_t row_offset = 0)
  {
    if(real_field() == this)
      return Field_blob::set_notnull(row_offset);
    return
      real_field()->set_notnull(row_offset);
  }
  /*
    This function return the field where the data is stored in.
   */
  Field_document *real_field()
  {
    return get_inner_field() != nullptr ?
      get_inner_field()->real_field() :
      this;
  }
  const Field_document *real_field() const
  {
    return get_inner_field() != nullptr ?
      get_inner_field()->real_field() :
      this;
  }
  /*
    Check if it is null as a blob, during which only flags will be
    checked.
  */
  bool is_null_as_blob(my_ptrdiff_t row_offset= 0) const;
  /*
    Check if the document is null, during which both flags and data
    will be checked.
  */
  bool is_null(my_ptrdiff_t row_offset= 0) const;
  enum_field_types type() const { return MYSQL_TYPE_DOCUMENT; }
  bool match_collation_to_optimize_range() const { return false; }
  uint get_key_image(uchar *buff, uint length, imagetype type);
  void set_key_image(const uchar *buff, uint length) {
    Field_blob::set_key_image(buff, length);
  }
  int key_cmp(const uchar *,const uchar*);
  int key_cmp(const uchar *str, uint length);
  uint32 key_length() const;
  void sql_type(String &str) const;
  using Field_blob::store;

  int document_path_prefix(List<Document_key> &key_list);
  type_conversion_status store_document_value(
    fbson::FbsonValue *val,
    const CHARSET_INFO *cs);
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_decimal(const my_decimal *nr);
  type_conversion_status set_delete(); // Delete the node in the partial update
  Field_document *new_field(MEM_ROOT *root,
                            TABLE *new_table,
                            bool keep_type,
                            bool from_alias);
  /* Copy the document path back to the caller. It's used when we
     initialize an item using a field.
   */
  void get_document_path(List<Document_key> &key_path);

  bool maybe_null(void) const
  {
    return get_inner_field() == nullptr ?
      Field_blob::maybe_null() : // It's the column
      true; // It's a document path
  }

  void make_field(Send_field *field);
  // Store for the doc_path
  double val_real(void);
  longlong val_int(void);
  my_decimal *val_decimal(my_decimal *);
  String *val_str(String*,String *);
  fbson::FbsonValue *val_document_value(String*, String*);
  fbson::FbsonValue *val_root_document_value(String*, String*);
  String *val_doc(String*, String*);
  void make_sort_key(uchar *buff, uint length);
  void make_sort_key_as_type(uchar *buff,
                             uint length,
                             enum_field_types as_type);
  bool eq(Field *field);
  type_conversion_status reset(void);

  enum ha_base_keytype key_type() const
  {
    DBUG_ASSERT(validate_doc_type());

    switch (doc_type)
    {
    case DOC_PATH_TINY:
      return HA_KEYTYPE_INT8;
    case DOC_PATH_INT:
      return HA_KEYTYPE_LONGLONG;
    case DOC_PATH_DOUBLE:
      return HA_KEYTYPE_DOUBLE;
    case DOC_PATH_STRING:
      return HA_KEYTYPE_VARBINARY2;
    case DOC_DOCUMENT:
      // A document key cannot be built on a document column
      // directly but if a type needs to be returned
      return HA_KEYTYPE_VARBINARY2;
    default:
      break;
    }
    DBUG_ASSERT(false); // should never reach here
    return HA_KEYTYPE_END;
  }

  document_type get_document_type()
  {
    DBUG_ASSERT(doc_type >= DOC_DOCUMENT);
    return doc_type;
  }
  // Get the true doc_type of the field.
  document_type real_doc_type()
  {
    // Sometimes, the data is not read from the index,
    // then it should be a document type
    return !is_from_index() ?
      DOC_DOCUMENT :
    // Or it should be the type defined in the index.
      get_document_type();
  }

  bool is_doc_type_string()
  {
    DBUG_ASSERT(validate_doc_type());
    return (doc_type == DOC_PATH_STRING);
  }

  // If the doc_type of this document is basic type,
  // e.g. TINY/INT/DOUBLE/STRING, which indicates
  // that this is a derived field and there is an
  // index on it.
  bool is_doc_type_basic()
  {
    DBUG_ASSERT(validate_doc_type());
    return (doc_type != DOC_DOCUMENT);
  }

  void set_document_type(enum_field_types type)
  {
    switch (type)
    {
      case MYSQL_TYPE_TINY: doc_type = DOC_PATH_TINY; break;
      case MYSQL_TYPE_LONGLONG: doc_type = DOC_PATH_INT; break;
      case MYSQL_TYPE_DOUBLE: doc_type = DOC_PATH_DOUBLE; break;
      case MYSQL_TYPE_STRING: doc_type = DOC_PATH_STRING; break;
      case MYSQL_TYPE_DOCUMENT: doc_type = DOC_DOCUMENT; break;
      default: DBUG_ASSERT(false); //not supported
    }
  }

  bool validate_doc_type() const
  {
    switch (doc_type)
    {
    case DOC_PATH_TINY:
      return (key_len == 1);
    case DOC_PATH_INT:
      return (key_len == sizeof(int64_t));
    case DOC_PATH_DOUBLE:
      return (key_len == sizeof(double));
    case DOC_PATH_STRING:
      return (key_len > 0);
    case DOC_DOCUMENT:
      return (key_len == 0);
    default:
      break;
    }
    DBUG_ASSERT(false); //not supported
    return false;
  }

  Item_result result_type() const
  {
    switch (doc_type)
    {
      case DOC_PATH_TINY:
      case DOC_PATH_INT:
        return INT_RESULT;

      case DOC_PATH_DOUBLE:
        return REAL_RESULT;

      case DOC_PATH_STRING:
      case DOC_DOCUMENT:
        return STRING_RESULT;

      default:
        break;
    }
    // Should never reach here.
    DBUG_ASSERT(false);
    return STRING_RESULT;
  }

  /* This overwrites the base version.
   * Note: document column should always nullable.
   * We keep the document path logic below (which is no longer needed) for now
   * - If the field is a document path, it can always be nullable. For example,
   *   when document path is in the select, prefix_path_num is non-zero. When
   *   document path used in partial update, update_args is non-null. When
   *   document path is used in ORDER BY, get_inner_field() is non-null
   */
  bool real_maybe_null(void) const
  {
    // document column should always be nullable
    DBUG_ASSERT(Field_blob::real_maybe_null());
    return Field_blob::real_maybe_null() ||
           (prefix_path_num || update_args || get_inner_field());
  }

  bool is_real_null(my_ptrdiff_t row_offset= 0) const
  {
    return Field_blob::real_maybe_null() && is_null(row_offset);
  }

  bool is_real_null_as_blob(my_ptrdiff_t row_offset= 0) const
  {
    if (Field_blob::real_maybe_null())
      return MY_TEST(null_ptr[row_offset] & null_bit);
    return false;
  }

  Field_document *clone(MEM_ROOT *mem_root) const
  {
    return new (mem_root) Field_document(*this);
  }

  Field_document *clone() const
  {
    return new Field_document(*this);
  }

  // Usually this value is get from max_data_length(). This is used to
  // get the max data length of Field_document when using the SQL function
  // fbson().
  uint32 static max_data_length_static() {
    return (1 << 24) - 1;
  }
  fbson::FbsonValue *get_fbson_value();
  void set_prefix_document_path(List<Document_key> &pre);
  void reset_as_blob()
  {
    Field_blob::reset();
  }
protected:
  /*
    This variable is the place where document path is stored.
    With this variable, we know where to extract the document from the data.
    For the field defined in the table, it's always empty.
  */
  List<Document_key> document_key_path;
  /*
    This variable is the place where the prefix of a path is stored.
    This variable only solve the problem of name resolution. For the field
    not from the temp table, it's always empty.
   */
  List<Document_key> prefix_document_key_path;

  // This is the field where the document path in this class referencing to.
  Field_document *inner_field;
  int document_path_prefix_helper(List_iterator<Document_key> &key_iter);
  /*
    This function returns the field where the document path defined
    in this class reference to. It's probably not the field where the
    data is stored as.
   */
  virtual Field_document *get_inner_field() { return inner_field; }
private:
  // Return whether the data is read from the index.
  bool is_from_index() const
  {
    return doc_type != DOC_DOCUMENT &&
      /*
        This is necessary, because sometimes, even when the index is defined,
        it may not read from it. For example:
        select doc.id from t1 using document keys where doc.string = 123;
      */
      const_cast<Field_document*>(this)->real_field()->table->key_read;
  }
  void init()
  {
    flags|= DOCUMENT_FLAG;
    packlength = 3;
    memset(document_path_key_start, 0,
           sizeof(DOCUMENT_PATH_KEY_PART_INFO*)*MAX_KEY);
  }
  fbson::FbsonErrType update_node(fbson::FbsonUpdater &updater,
                                  Document_path_iterator &iter,
                                  const fbson::FbsonValue *val,
                                  Save_in_field_args *args);
  int64 read_bigendian(char *src, unsigned len);

  uint get_key_image_bool(uchar *buff, fbson::FbsonValue *pval);
  uint get_key_image_int(uchar *buff, fbson::FbsonValue *pval);
  uint get_key_image_double(uchar *buff, fbson::FbsonValue *pval);
  uint get_key_image_text(uchar *buff, uint length, fbson::FbsonValue *pval);
  template<class T>
  uint get_key_image_numT(T &val, fbson::FbsonValue *pval);
};
/*
  This class is for iterating of the document key in the
  Field_document class. Because, in the field document class,
  there maybe inner field inside it. So the original iterator
  is not so convenient here.
 */
class Document_path_iterator : public List_iterator<Document_key>
{
public:
  Document_path_iterator(Field_document *doc_field)
  {
    // We store a stack for it.
    while(doc_field)
    {
      fields.push(doc_field);
      doc_field = doc_field->get_inner_field();
    }
    init_iterator();
  }
  Document_key *operator++(int)
  {
    Document_key *key =
      (*static_cast<List_iterator<Document_key>*>(this))++;
    if(nullptr == key)
    {
      if(init_iterator())
        key = (*static_cast<List_iterator<Document_key>*>(this))++;
    }
    return key;
  }
private:
  bool init_iterator()
  {
    if(fields.size() == 0)
      return false;
    Field_document *doc = fields.top();
    fields.pop();
    init(doc->document_key_path);
    return true;
  }
private:
  std::stack<Field_document*> fields;
};


/*
  This class is for referene of the Field_document. The
  only difference is the way the to get the inner_field.
  In this class, the inner_field is accessed through the
  referenced item.
*/
class Field_document_ref : public Field_document
{
public:
  Field_document_ref(MEM_ROOT *root,
                     Item_field **inner,
                     List<Document_key> &doc_path,
                     int skip_num);
protected:
  Field_document *get_inner_field();
private:
  Item_field **inner_ref;
};

class Field_enum :public Field_str {
protected:
  uint packlength;
public:
  TYPELIB *typelib;
  Field_enum(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
             uchar null_bit_arg,
             enum utype unireg_check_arg, const char *field_name_arg,
             uint packlength_arg,
             TYPELIB *typelib_arg,
             const CHARSET_INFO *charset_arg)
    :Field_str(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
	       unireg_check_arg, field_name_arg, charset_arg),
    packlength(packlength_arg),typelib(typelib_arg)
  {
    flags|=ENUM_FLAG;
  }
  Field *new_field(MEM_ROOT *root,
                   TABLE *new_table,
                   bool keep_type,
                   bool from_alias);
  enum_field_types type() const { return MYSQL_TYPE_STRING; }
  bool match_collation_to_optimize_range() const { return false; }
  enum Item_result cmp_type () const { return INT_RESULT; }
  enum Item_result cast_to_int_type () const { return INT_RESULT; }
  enum ha_base_keytype key_type() const;
  type_conversion_status store(const char *to,uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  double val_real(void);
  my_decimal *val_decimal(my_decimal *decimal_value);
  longlong val_int(void);
  String *val_str(String*,String *);
  int cmp(const uchar *,const uchar *);
  void make_sort_key(uchar *buff, uint length);
  uint32 pack_length() const { return (uint32) packlength; }
  void store_type(ulonglong value);
  void sql_type(String &str) const;
  enum_field_types real_type() const { return MYSQL_TYPE_ENUM; }
  uint pack_length_from_metadata(uint field_metadata)
  { return (field_metadata & 0x00ff); }
  uint row_pack_length() const { return pack_length(); }
  virtual bool zero_pack() const { return 0; }
  bool optimize_range(uint idx, uint part) { return 0; }
  bool eq_def(Field *field);
  bool has_charset(void) const { return TRUE; }
  /* enum and set are sorted as integers */
  CHARSET_INFO *sort_charset(void) const { return &my_charset_bin; }
  Field_enum *clone(MEM_ROOT *mem_root) const {
    DBUG_ASSERT(real_type() == MYSQL_TYPE_ENUM);
    return new (mem_root) Field_enum(*this);
  }
  Field_enum *clone() const { 
    DBUG_ASSERT(real_type() == MYSQL_TYPE_ENUM);
    return new Field_enum(*this);
  }
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              uint param_data, bool low_byte_first);

private:
  int do_save_field_metadata(uchar *first_byte);
  uint is_equal(Create_field *new_field);
};


class Field_set :public Field_enum {
public:
  Field_set(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
	    uchar null_bit_arg,
	    enum utype unireg_check_arg, const char *field_name_arg,
	    uint32 packlength_arg,
	    TYPELIB *typelib_arg, const CHARSET_INFO *charset_arg)
    :Field_enum(ptr_arg, len_arg, null_ptr_arg, null_bit_arg,
		    unireg_check_arg, field_name_arg,
                packlength_arg,
                typelib_arg,charset_arg),
      empty_set_string("", 0, charset_arg)
    {
      flags= (flags & ~ENUM_FLAG) | SET_FLAG;
    }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr)
  {
    return Field_set::store((longlong) nr, FALSE);
  }
  type_conversion_status store(longlong nr, bool unsigned_val);
  virtual bool zero_pack() const { return 1; }
  String *val_str(String*,String *);
  void sql_type(String &str) const;
  enum_field_types real_type() const { return MYSQL_TYPE_SET; }
  bool has_charset(void) const { return TRUE; }
  Field_set *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(real_type() == MYSQL_TYPE_SET);
    return new (mem_root) Field_set(*this);
  }
  Field_set *clone() const {
    DBUG_ASSERT(real_type() == MYSQL_TYPE_SET);
    return new Field_set(*this);
  }
private:
  const String empty_set_string;
};


/*
  Note:
    To use Field_bit::cmp_binary() you need to copy the bits stored in
    the beginning of the record (the NULL bytes) to each memory you
    want to compare (where the arguments point).

    This is the reason:
    - Field_bit::cmp_binary() is only implemented in the base class
      (Field::cmp_binary()).
    - Field::cmp_binary() currenly use pack_length() to calculate how
      long the data is.
    - pack_length() includes size of the bits stored in the NULL bytes
      of the record.
*/
class Field_bit :public Field {
public:
  uchar *bit_ptr;     // position in record where 'uneven' bits store
  uchar bit_ofs;      // offset to 'uneven' high bits
  uint bit_len;       // number of 'uneven' high bits
  uint bytes_in_rec;
  Field_bit(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
            uchar null_bit_arg, uchar *bit_ptr_arg, uchar bit_ofs_arg,
            enum utype unireg_check_arg, const char *field_name_arg);
  enum_field_types type() const { return MYSQL_TYPE_BIT; }
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BIT; }
  uint32 key_length() const { return (uint32) (field_length + 7) / 8; }
  uint32 max_data_length() const { return (field_length + 7) / 8; }
  uint32 max_display_length() { return field_length; }
  Item_result result_type () const { return INT_RESULT; }
  type_conversion_status reset(void)
  {
    memset(ptr, 0, bytes_in_rec);
    if (bit_ptr && (bit_len > 0))  // reset odd bits among null bits
      clr_rec_bits(bit_ptr, bit_ofs, bit_len);
    return TYPE_OK;
  }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr);
  type_conversion_status store(longlong nr, bool unsigned_val);
  type_conversion_status store_decimal(const my_decimal *);
  double val_real(void);
  longlong val_int(void);
  String *val_str(String*, String *);
  virtual bool str_needs_quotes() { return TRUE; }
  my_decimal *val_decimal(my_decimal *);
  int cmp(const uchar *a, const uchar *b)
  {
    DBUG_ASSERT(ptr == a || ptr == b);
    if (ptr == a)
      return Field_bit::key_cmp(b, bytes_in_rec+MY_TEST(bit_len));
    else
      return Field_bit::key_cmp(a, bytes_in_rec+MY_TEST(bit_len)) * -1;
  }
  int cmp_binary_offset(uint row_offset)
  { return cmp_offset(row_offset); }
  int cmp_max(const uchar *a, const uchar *b, uint max_length);
  int key_cmp(const uchar *a, const uchar *b)
  { return cmp_binary((uchar *) a, (uchar *) b); }
  int key_cmp(const uchar *str, uint length);
  int cmp_offset(uint row_offset);
  void get_image(uchar *buff, uint length, const CHARSET_INFO *cs)
  { get_key_image(buff, length, itRAW); }   
  void set_image(const uchar *buff,uint length, const CHARSET_INFO *cs)
  { Field_bit::store((char *) buff, length, cs); }
  uint get_key_image(uchar *buff, uint length, imagetype type);
  void set_key_image(const uchar *buff, uint length)
  { Field_bit::store((char*) buff, length, &my_charset_bin); }
  void make_sort_key(uchar *buff, uint length)
  { get_key_image(buff, length, itRAW); }
  uint32 pack_length() const { return (uint32) (field_length + 7) / 8; }
  uint32 pack_length_in_rec() const { return bytes_in_rec; }
  uint pack_length_from_metadata(uint field_metadata);
  uint row_pack_length() const
  { return (bytes_in_rec + ((bit_len > 0) ? 1 : 0)); }
  bool compatible_field_size(uint metadata, Relay_log_info *rli,
                             uint16 mflags, int *order_var);
  void sql_type(String &str) const;
  virtual uchar *pack(uchar *to, const uchar *from,
                      uint max_length, bool low_byte_first);
  virtual const uchar *unpack(uchar *to, const uchar *from,
                              uint param_data, bool low_byte_first);
  virtual void set_default();

  Field *new_key_field(MEM_ROOT *root, TABLE *new_table,
                       uchar *new_ptr, uchar *new_null_ptr,
                       uint new_null_bit);
  void set_bit_ptr(uchar *bit_ptr_arg, uchar bit_ofs_arg)
  {
    bit_ptr= bit_ptr_arg;
    bit_ofs= bit_ofs_arg;
  }
  bool eq(Field *field)
  {
    return (Field::eq(field) &&
            bit_ptr == ((Field_bit *)field)->bit_ptr &&
            bit_ofs == ((Field_bit *)field)->bit_ofs);
  }
  uint is_equal(Create_field *new_field);
  void move_field_offset(my_ptrdiff_t ptr_diff)
  {
    Field::move_field_offset(ptr_diff);
    bit_ptr= ADD_TO_PTR(bit_ptr, ptr_diff, uchar*);
  }
  void hash(ulong *nr, ulong *nr2);
  Field_bit *clone(MEM_ROOT *mem_root) const { 
    DBUG_ASSERT(type() == MYSQL_TYPE_BIT);
    return new (mem_root) Field_bit(*this);
  }
  Field_bit *clone() const {
    DBUG_ASSERT(type() == MYSQL_TYPE_BIT);
    return new Field_bit(*this);
  }
private:
  virtual size_t do_last_null_byte() const;
  int do_save_field_metadata(uchar *first_byte);
};


/**
  BIT field represented as chars for non-MyISAM tables.

  @todo The inheritance relationship is backwards since Field_bit is
  an extended version of Field_bit_as_char and not the other way
  around. Hence, we should refactor it to fix the hierarchy order.
 */
class Field_bit_as_char: public Field_bit {
public:
  Field_bit_as_char(uchar *ptr_arg, uint32 len_arg, uchar *null_ptr_arg,
                    uchar null_bit_arg,
                    enum utype unireg_check_arg, const char *field_name_arg);
  enum ha_base_keytype key_type() const { return HA_KEYTYPE_BINARY; }
  type_conversion_status store(const char *to, uint length,
                               const CHARSET_INFO *charset);
  type_conversion_status store(double nr) { return Field_bit::store(nr); }
  type_conversion_status store(longlong nr, bool unsigned_val)
  { return Field_bit::store(nr, unsigned_val); }
  void sql_type(String &str) const;
  Field_bit_as_char *clone(MEM_ROOT *mem_root) const { 
    return new (mem_root) Field_bit_as_char(*this);
  }
  Field_bit_as_char *clone() const { return new Field_bit_as_char(*this); }
};


/*
  Create field class for CREATE TABLE
*/

class Create_field :public Sql_alloc
{
public:
  const char *field_name;
  const char *change;			// If done with alter table
  const char *after;			// Put column after this one
  LEX_STRING comment;			// Comment for field

  /**
     The declared default value, if any, otherwise NULL. Note that this member
     is NULL if the default is a function. If the column definition has a
     function declared as the default, the information is found in
     Create_field::unireg_check.
     
     @see Create_field::unireg_check
  */
  Item *def;
  enum	enum_field_types sql_type;
  /*
    At various stages in execution this can be length of field in bytes or
    max number of characters. 
  */
  ulong length;
  /*
    The value of `length' as set by parser: is the number of characters
    for most of the types, or of bytes for BLOBs or numeric types.
  */
  uint32 char_length;
  uint  decimals, flags, pack_length, key_length;
  Field::utype unireg_check;
  TYPELIB *interval;			// Which interval to use
  TYPELIB *save_interval;               // Temporary copy for the above
                                        // Used only for UCS2 intervals
  List<String> interval_list;
  const CHARSET_INFO *charset;
  Field::geometry_type geom_type;
  Field::document_type document_type;	// Sub-type of DOCUMENT
  Field *field;				// For alter table

  uint8 row,col,sc_length,interval_id;	// For rea_create_table
  uint	offset,pack_flag;
  Create_field() :after(NULL) {}
  Create_field(Field *field, Field *orig_field);
  /* Used to make a clone of this object for ALTER/CREATE TABLE */
  Create_field *clone(MEM_ROOT *mem_root) const
    { return new (mem_root) Create_field(*this); }
  void create_length_to_internal_length(void);

  /* Init for a tmp table field. To be extended if need be. */
  void init_for_tmp_table(enum_field_types sql_type_arg,
                          uint32 max_length, uint32 decimals,
                          bool maybe_null, bool is_unsigned,
                          uint pack_length = ~0U);

  bool init(THD *thd, const char *field_name, enum_field_types type,
            const char *length, const char *decimals, uint type_modifier,
            Item *default_value, Item *on_update_value, LEX_STRING *comment,
            const char *change, List<String> *interval_list,
            const CHARSET_INFO *cs, uint uint_geom_type);

  ha_storage_media field_storage_type() const
  {
    return (ha_storage_media)
      ((flags >> FIELD_FLAGS_STORAGE_MEDIA) & 3);
  }

  column_format_type column_format() const
  {
    return (column_format_type)
      ((flags >> FIELD_FLAGS_COLUMN_FORMAT) & 3);
  }
};


/*
  A class for sending info to the client
*/

class Send_field :public Sql_alloc {
 public:
  const char *db_name;
  const char *table_name,*org_table_name;
  const char *col_name,*org_col_name;
  ulong length;
  uint charsetnr, flags, decimals;
  enum_field_types type;
  Send_field() {}
};


/*
  A class for quick copying data to fields
*/

class Copy_field :public Sql_alloc {
  /**
    Convenience definition of a copy function returned by
    get_copy_func.
  */
  typedef type_conversion_status Copy_func(Copy_field*);
  Copy_func *get_copy_func(Field *to, Field *from);
public:
  uchar *from_ptr,*to_ptr;
  uchar *from_null_ptr,*to_null_ptr;
  my_bool *null_row;
  uint	from_bit,to_bit;
  /**
    Number of bytes in the fields pointed to by 'from_ptr' and
    'to_ptr'. Usually this is the number of bytes that are copied from
    'from_ptr' to 'to_ptr'.

    For variable-length fields (VARCHAR), the first byte(s) describe
    the actual length of the text. For VARCHARs with length 
       < 256 there is 1 length byte 
       >= 256 there is 2 length bytes
    Thus, if from_field is VARCHAR(10), from_length (and in most cases
    to_length) is 11. For VARCHAR(1024), the length is 1026. @see
    Field_varstring::length_bytes

    Note that for VARCHARs, do_copy() will be do_varstring*() which
    only copies the length-bytes (1 or 2) + the actual length of the
    text instead of from/to_length bytes. @see get_copy_func()
  */
  uint from_length,to_length;
  Field *from_field,*to_field;
  String tmp;					// For items

  Copy_field() {}
  ~Copy_field() {}
  void set(Field *to,Field *from,bool save);	// Field to field 
  void set(uchar *to,Field *from);		// Field to string
  type_conversion_status (*do_copy)(Copy_field *);
  type_conversion_status (*do_copy2)(Copy_field *);// Used to handle null values
};


Field *make_field(TABLE_SHARE *share, uchar *ptr, uint32 field_length,
		  uchar *null_pos, uchar null_bit,
		  uint pack_flag, enum_field_types field_type,
		  const CHARSET_INFO *cs,
		  Field::geometry_type geom_type,
		  Field::utype unireg_check,
		  TYPELIB *interval, const char *field_name);
uint pack_length_to_packflag(uint type);
enum_field_types get_blob_type_from_length(ulong length);
uint32 calc_pack_length(enum_field_types type,uint32 length);
type_conversion_status set_field_to_null(Field *field);
type_conversion_status set_field_to_null_with_conversions(Field *field,
                                                          bool no_conversions);

/*
  The following are for the interface with the .frm file
*/

#define FIELDFLAG_DECIMAL		1
#define FIELDFLAG_BINARY		1	// Shares same flag
#define FIELDFLAG_NUMBER		2
#define FIELDFLAG_ZEROFILL		4
#define FIELDFLAG_PACK			120	// Bits used for packing
#define FIELDFLAG_INTERVAL		256     // mangled with decimals!
#define FIELDFLAG_BITFIELD		512	// mangled with decimals!
#define FIELDFLAG_BLOB			1024	// mangled with decimals!
#define FIELDFLAG_GEOM			2048    // mangled with decimals!

#define FIELDFLAG_TREAT_BIT_AS_CHAR     4096    /* use Field_bit_as_char */

#define FIELDFLAG_DOCUMENT		8192	// mangled with decimals!

#define FIELDFLAG_RIGHT_FULLSCREEN	16384
#define FIELDFLAG_FORMAT_NUMBER		16384	// predit: ###,,## in output
#define FIELDFLAG_NO_DEFAULT		16384   /* sql */
#define FIELDFLAG_SUM			((uint) 32768)// predit: +#fieldflag
#define FIELDFLAG_MAYBE_NULL		((uint) 32768)// sql
#define FIELDFLAG_HEX_ESCAPE		((uint) 0x10000)
#define FIELDFLAG_PACK_SHIFT		3
#define FIELDFLAG_DEC_SHIFT		8
#define FIELDFLAG_MAX_DEC		31
#define FIELDFLAG_NUM_SCREEN_TYPE	0x7F01
#define FIELDFLAG_ALFA_SCREEN_TYPE	0x7800

#define MTYP_TYPENR(type) (type & 127)	/* Remove bits from type */

#define f_is_dec(x)		((x) & FIELDFLAG_DECIMAL)
#define f_is_num(x)		((x) & FIELDFLAG_NUMBER)
#define f_is_zerofill(x)	((x) & FIELDFLAG_ZEROFILL)
#define f_is_packed(x)		((x) & FIELDFLAG_PACK)
#define f_packtype(x)		(((x) >> FIELDFLAG_PACK_SHIFT) & 15)
#define f_decimals(x)		((uint8) (((x) >> FIELDFLAG_DEC_SHIFT) & FIELDFLAG_MAX_DEC))
#define f_is_alpha(x)		(!f_is_num(x))
#define f_is_binary(x)          ((x) & FIELDFLAG_BINARY) // 4.0- compatibility
#define f_is_enum(x)            (((x) & (FIELDFLAG_INTERVAL | FIELDFLAG_NUMBER)) == FIELDFLAG_INTERVAL)
#define f_is_bitfield(x)        (((x) & (FIELDFLAG_BITFIELD | FIELDFLAG_NUMBER)) == FIELDFLAG_BITFIELD)
#define f_is_blob(x)		(((x) & (FIELDFLAG_BLOB | FIELDFLAG_NUMBER)) == FIELDFLAG_BLOB)
#define f_is_geom(x)		(((x) & (FIELDFLAG_GEOM | FIELDFLAG_NUMBER)) == FIELDFLAG_GEOM)
#define f_is_document(x)	(((x) & (FIELDFLAG_DOCUMENT | FIELDFLAG_NUMBER)) == FIELDFLAG_DOCUMENT)
#define f_is_equ(x)		((x) & (1+2+FIELDFLAG_PACK+31*256))
#define f_settype(x)		(((int) x) << FIELDFLAG_PACK_SHIFT)
#define f_maybe_null(x)		(x & FIELDFLAG_MAYBE_NULL)
#define f_no_default(x)		(x & FIELDFLAG_NO_DEFAULT)
#define f_bit_as_char(x)        ((x) & FIELDFLAG_TREAT_BIT_AS_CHAR)
#define f_is_hex_escape(x)      ((x) & FIELDFLAG_HEX_ESCAPE)

#endif /* FIELD_INCLUDED */
