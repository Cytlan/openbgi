/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * json.h - a small JSON reader for the configuration files of the
 *          reimplementation (games.json); not part of the original engine.
 *          Implemented in src/core/json.c.
 *
 * The whole document is parsed into a tree of nodes.  Object members hang
 * off `child` with their `key` set, array elements off `child` without a
 * key; siblings are linked through `next`.  Strings are UTF-8 (escapes
 * decoded, \uXXXX surrogate pairs included).
 */
#ifndef BGI_JSON_H_
#define BGI_JSON_H_

#include "bgi/common.h"

typedef enum JsonType
{
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} JsonType_t;

typedef struct JsonNode
{
	JsonType_t type;
	char* key;              // member name (objects), NULL otherwise
	char* str;              // JSON_STRING: the decoded text, UTF-8
	double num;             // JSON_NUMBER, JSON_BOOL (0 / 1)
	struct JsonNode* child; // first member / element
	struct JsonNode* next;  // next sibling
} JsonNode_t;

/* parse `len` bytes of text (a UTF-8 byte order mark and '//' or slash-star
 * comments are allowed) into a tree; NULL on a syntax error, with the offset
 * of the error in *err when given (0 on success).  Json_Free the result. */
JsonNode_t* Json_Parse(const char* text, size_t len, size_t* err);
// free a node with its children and every sibling after it; NULL is allowed
void Json_Free(JsonNode_t* n);

// the member `key` of an object; NULL when obj is not an object or has no such member
const JsonNode_t* Json_Get(const JsonNode_t* obj, const char* key);
// the string member `key`, or def when it is missing or not a string
const char* Json_String(const JsonNode_t* obj, const char* key, const char* def);
// the number (or bool, as 0 / 1) member `key`, or def when missing or another type
double Json_Number(const JsonNode_t* obj, const char* key, double def);
// the elements of an array / members of an object (0 for NULL)
int Json_Count(const JsonNode_t* arr);

#endif // BGI_JSON_H_
